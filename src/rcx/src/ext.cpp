// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#include "rcx/ext.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "odb/db.h"
#include "odb/dbObject.h"
#include "odb/dbWireCodec.h"
#include "odb/normalizeWireConnectivity.h"
#include "parse.h"
#include "rcx/extMeasureRC.h"
#include "rcx/extModelGen.h"
#include "rcx/extPattern.h"
#include "rcx/extRCap.h"
#include "rcx/ext_options.h"
#include "rcx/multiChipExtractor.h"
#include "rcx/multiChipSpefWriter.h"
#include "rcx/routingPreserver.h"
#include "utl/Logger.h"

namespace rcx {

using utl::Logger;
using utl::RCX;

namespace {

// A dbFill has no electrical owner.  For the deliberately bounded fill
// profile, expose each exact rectangle to OpenRCX as a temporary routed path,
// then eliminate the temporary conductor from the extracted capacitance
// matrix.  The elimination is the zero-net-charge Schur complement, not the
// (incorrect) approximation of grounding fill.
struct FillReduction
{
  int neighbours = 0;
  int couplings = 0;
};

class FloatingFill
{
 public:
  FloatingFill(odb::dbBlock* block,
               odb::dbFill* fill,
               size_t index,
               Logger* logger)
      : block_(block), logger_(logger)
  {
    odb::Rect rect;
    fill->getRect(rect);
    if (rect.dx() <= 0 || rect.dy() <= 0) {
      logger_->error(RCX,
                     542,
                     "Explicit fill rectangle has non-positive dimensions.");
    }
    odb::dbTechLayer* layer = fill->getTechLayer();
    if (layer == nullptr
        || layer->getType() != odb::dbTechLayerType::ROUTING) {
      logger_->error(RCX,
                     543,
                     "The bounded floating-fill profile requires a routing "
                     "layer rectangle.");
    }

    const bool horizontal = rect.dx() >= rect.dy();
    const int fill_width = horizontal ? rect.dy() : rect.dx();

    constexpr const char* base_name = "__rcx_floating_fill__";
    std::string name = std::string(base_name) + std::to_string(index);
    for (int suffix = 1; block_->findNet(name.c_str()) != nullptr; suffix++) {
      name = std::string(base_name) + std::to_string(index) + "_"
             + std::to_string(suffix);
    }

    net_ = odb::dbNet::create(block_, name.c_str());
    if (net_ == nullptr) {
      logger_->error(RCX, 541, "Could not create the temporary fill net.");
    }
    net_->setSigType(odb::dbSigType::SIGNAL);

    odb::dbTechLayerRule* layer_rule = nullptr;
    if (fill_width != layer->getWidth()) {
      const std::string rule_name = name + "_ndr";
      ndr_ = odb::dbTechNonDefaultRule::create(block_, rule_name.c_str());
      if (ndr_ == nullptr) {
        odb::dbNet::destroy(net_);
        net_ = nullptr;
        logger_->error(RCX,
                       550,
                       "Could not create a temporary fill-width rule.");
      }
      layer_rule = odb::dbTechLayerRule::create(ndr_, layer);
      if (layer_rule == nullptr) {
        odb::dbNet::destroy(net_);
        net_ = nullptr;
        odb::dbTechNonDefaultRule::destroyBlockRule(ndr_);
        ndr_ = nullptr;
        logger_->error(RCX,
                       551,
                       "Could not create a temporary fill layer rule.");
      }
      layer_rule->setWidth(fill_width);
    }

    odb::dbWire* wire = odb::dbWire::create(net_);
    odb::dbWireEncoder encoder;
    encoder.begin(wire);
    if (fill->maskNumber() != 0) {
      encoder.setColor(fill->maskNumber());
    }
    if (layer_rule == nullptr) {
      encoder.newPath(layer, odb::dbWireType::ROUTED);
    } else {
      encoder.newPath(layer, odb::dbWireType::ROUTED, layer_rule);
    }
    if (horizontal) {
      const int y = rect.yMin() + rect.dy() / 2;
      encoder.addPoint(rect.xMin(), y, 0);
      encoder.addPoint(rect.xMax(), y, 0);
    } else {
      const int x = rect.xMin() + rect.dx() / 2;
      encoder.addPoint(x, rect.yMin(), 0);
      encoder.addPoint(x, rect.yMax(), 0);
    }
    encoder.end();

    odb::dbWireShapeItr shapes;
    shapes.begin(wire);
    odb::dbShape shape;
    odb::dbShape extra_shape;
    const bool has_shape = shapes.next(shape);
    const odb::Rect wire_rect = has_shape ? shape.getBox() : odb::Rect();
    if (!has_shape || shapes.next(extra_shape) || wire_rect.xMin() != rect.xMin()
        || wire_rect.yMin() != rect.yMin()
        || wire_rect.xMax() != rect.xMax()
        || wire_rect.yMax() != rect.yMax()) {
      odb::dbNet::destroy(net_);
      net_ = nullptr;
      if (ndr_ != nullptr) {
        odb::dbTechNonDefaultRule::destroyBlockRule(ndr_);
        ndr_ = nullptr;
      }
      logger_->error(RCX,
                     552,
                     "Could not represent a dbFill rectangle exactly as a "
                     "temporary routed shape.");
    }
  }

  FloatingFill(const FloatingFill&) = delete;
  FloatingFill& operator=(const FloatingFill&) = delete;

  ~FloatingFill()
  {
    if (net_ != nullptr) {
      odb::dbNet::destroy(net_);
    }
    if (ndr_ != nullptr) {
      odb::dbTechNonDefaultRule::destroyBlockRule(ndr_);
    }
  }

  FillReduction reduce()
  {
    const int corner_count = block_->getCornerCount();
    if (corner_count <= 0) {
      logger_->error(RCX,
                     544,
                     "Floating-fill extraction produced no process corners.");
    }

    std::vector<double> fill_ground(corner_count, 0.0);
    for (odb::dbCapNode* cap_node : net_->getCapNodes()) {
      for (int corner = 0; corner < corner_count; corner++) {
        fill_ground[corner] += cap_node->getCapacitance(corner);
      }
    }

    std::unordered_map<odb::dbCapNode*, std::vector<double>> neighbours;
    for (odb::dbCCSeg* cc : block_->getCCSegs()) {
      odb::dbCapNode* source = cc->getSourceCapNode();
      odb::dbCapNode* target = cc->getTargetCapNode();
      const bool source_is_fill = source->getNet() == net_;
      const bool target_is_fill = target->getNet() == net_;
      if (source_is_fill == target_is_fill) {
        continue;
      }
      odb::dbCapNode* other = source_is_fill ? target : source;
      auto it = neighbours.try_emplace(other, corner_count, 0.0).first;
      for (int corner = 0; corner < corner_count; corner++) {
        it->second[corner] += cc->getCapacitance(corner);
      }
    }

    if (neighbours.empty()) {
      return {};
    }

    std::vector<double> denominator = fill_ground;
    for (const auto& [node, capacitance] : neighbours) {
      for (int corner = 0; corner < corner_count; corner++) {
        denominator[corner] += capacitance[corner];
      }
    }
    for (int corner = 0; corner < corner_count; corner++) {
      if (!(denominator[corner] > 0.0)
          || !std::isfinite(denominator[corner])) {
        logger_->error(RCX,
                       546,
                       "Invalid floating-fill capacitance denominator {} at "
                       "corner {}.",
                       denominator[corner],
                       corner);
      }
    }

    std::vector<odb::dbCapNode*> nodes;
    nodes.reserve(neighbours.size());
    for (const auto& [node, capacitance] : neighbours) {
      nodes.push_back(node);
      for (int corner = 0; corner < corner_count; corner++) {
        const double added_ground = capacitance[corner]
                                    * fill_ground[corner]
                                    / denominator[corner];
        if (added_ground < 0.0 || !std::isfinite(added_ground)) {
          logger_->error(RCX,
                         547,
                         "Invalid reduced fill-to-ground capacitance {} at "
                         "corner {}.",
                         added_ground,
                         corner);
        }
        node->addCapacitance(added_ground, corner);
      }
    }

    int added_couplings = 0;
    for (size_t left = 0; left < nodes.size(); left++) {
      for (size_t right = left + 1; right < nodes.size(); right++) {
        std::vector<double> reduced_capacitance(corner_count, 0.0);
        bool nonzero = false;
        for (int corner = 0; corner < corner_count; corner++) {
          const double capacitance
              = neighbours.at(nodes[left])[corner]
                * neighbours.at(nodes[right])[corner] / denominator[corner];
          if (capacitance < 0.0 || !std::isfinite(capacitance)) {
            logger_->error(RCX,
                           548,
                           "Invalid reduced fill coupling {} at corner {}.",
                           capacitance,
                           corner);
          }
          reduced_capacitance[corner] = capacitance;
          nonzero = nonzero || capacitance > 0.0;
        }
        if (nonzero) {
          odb::dbCCSeg* reduced
              = odb::dbCCSeg::create(nodes[left], nodes[right], true);
          for (int corner = 0; corner < corner_count; corner++) {
            reduced->addCapacitance(reduced_capacitance[corner], corner);
          }
          added_couplings++;
        }
      }
    }

    return {static_cast<int>(neighbours.size()), added_couplings};
  }

 private:
  odb::dbBlock* block_;
  Logger* logger_;
  odb::dbNet* net_ = nullptr;
  odb::dbTechNonDefaultRule* ndr_ = nullptr;
};

}  // namespace

Ext::Ext(odb::dbDatabase* db, Logger* logger, const char* spef_version)
    : _db(db),
      _ext(new extMain()),
      logger_(logger),
      spef_version_(spef_version),
      multi_chip_extractor_(std::make_unique<MultiChipExtractor>(db, logger)),
      multi_chip_spef_writer_(
          std::make_unique<MultiChipSpefWriter>(db, logger, spef_version))
{
  _ext->init(db, logger);
}

Ext::~Ext()
{
  delete _ext;
}

void Ext::setLogger(Logger* logger)
{
  if (!logger_) {
    logger_ = logger;
  }
}
void Ext::write_rules(const std::string& name, const std::string& file)
{
  _ext->setBlockFromChip(_db->getChip());
  _ext->writeRules(name.c_str(), file.c_str());
}
void Ext::bench_wires(const BenchWiresOptions& bwo)
{
  extMainOptions opt;

  opt._v1 = bwo.v1;
  opt._topDir = bwo.dir;
  opt._met_cnt = bwo.met_cnt;
  opt._met = bwo.met;
  opt._overDist = bwo.over_dist;
  opt._underDist = bwo.under_dist;
  opt._overMet = bwo.over_met;
  opt._over = bwo.Over;
  opt._underMet = bwo.under_met;
  opt._overUnder = bwo.over_under;
  opt._len = 1000 * bwo.len;
  opt._wireCnt = bwo.cnt;
  opt._name = bwo.block;

  opt._default_lef_rules = bwo.default_lef_rules;
  opt._nondefault_lef_rules = bwo.nondefault_lef_rules;

  opt._multiple_widths = bwo.multiple_widths;

  opt._diag = bwo.diag;
  opt._db_only = bwo.db_only;
  opt._gen_def_patterns = bwo.gen_def_patterns;

  opt._res_patterns = bwo.resPatterns;

  if (opt._gen_def_patterns) {
    opt._diag = true;
    opt._overUnder = true;
    opt._db_only = true;
    opt._over = true;
    opt._underMet = 0;
  }

  Parser parser(logger_);

  std::string w_list(bwo.w_list);
  std::string s_list(bwo.s_list);
  std::string th(bwo.th);
  std::string w(bwo.w);
  std::string s(bwo.s);
  std::string d(bwo.d);
  std::string grid_list(bwo.grid_list);

  if (!th.empty()) {
    parser.mkWords(bwo.th_list);
    parser.getDoubleArray(&opt._thicknessTable, 0);
    opt._thListFlag = true;
  } else {
    parser.mkWords(bwo.th);
    parser.getDoubleArray(&opt._thicknessTable, 0);
    opt._thListFlag = false;
  }
  opt._listsFlag = false;
  opt._wsListFlag = false;
  if (opt._default_lef_rules) {  // minWidth, minSpacing, minThickness, pitch
                                 // multiplied by grid_list
    // user sets default_flag and grid_list multipliers
    if (grid_list.empty()) {
      opt._gridTable.add(1.0);
    } else {
      parser.mkWords(grid_list.c_str());
      parser.getDoubleArray(&opt._gridTable, 0);
    }
    opt._listsFlag = true;
  } else if (opt._nondefault_lef_rules) {
    opt._listsFlag = true;
  } else if (!w_list.empty() && !s_list.empty()) {
    opt._listsFlag = true;
    opt._wsListFlag = true;
    parser.mkWords(w_list.c_str());
    parser.getDoubleArray(&opt._widthTable, 0);
    parser.mkWords(s_list.c_str());
    parser.getDoubleArray(&opt._spaceTable, 0);
  } else {
    parser.mkWords(w.c_str());
    parser.getDoubleArray(&opt._widthTable, 0);
    parser.mkWords(s.c_str());
    parser.getDoubleArray(&opt._spaceTable, 0);
    parser.mkWords(th.c_str());
    parser.getDoubleArray(&opt._thicknessTable, 0);
    parser.mkWords(d.c_str());
    parser.getDoubleArray(&opt._densityTable, 0);
  }
  // _ext->benchWires(&opt);
  if (opt._gen_def_patterns && opt._v1) {  // New patterns v1=true
    _ext->DefWires(&opt);
  } else {
    _ext->benchWires(&opt);
  }
}
void Ext::bench_wires_gen(const PatternOptions& opt)
{
  // printf("%s\n", opt.name);
  _ext->benchPatternsGen(opt);
}

void Ext::bench_verilog(const std::string& file)
{
  _ext->setBlockFromChip(_db->getChip());
  char* filename = (char*) file.c_str();
  if (!filename || !filename[0]) {
    logger_->error(RCX, 147, "bench_verilog: file is not defined!");
  }
  FILE* fp = fopen(filename, "w");
  if (fp == nullptr) {
    logger_->error(RCX, 378, "Can't open file {}", filename);
  }
  _ext->benchVerilog(fp);
}

void Ext::define_process_corner(int ext_model_index, const std::string& name)
{
  _ext->setBlockFromChip(_db->getChip());
  char* cornerName = _ext->addRCCorner(name.c_str(), ext_model_index);

  if (cornerName != nullptr) {
    logger_->info(RCX, 29, "Defined extraction corner {}", cornerName);
  }
}

void Ext::define_derived_corner(const std::string& name,
                                const std::string& process_corner_name,
                                float res_factor,
                                float cc_factor,
                                float gndc_factor)
{
  if (process_corner_name.empty()) {
    logger_->error(RCX, 30, "The original process corner name is required");
  }

  int model = _ext->getDbCornerModel(process_corner_name.c_str());

  char* cornerName = _ext->addRCCornerScaled(
      name.c_str(), model, res_factor, cc_factor, gndc_factor);

  if (cornerName != nullptr) {
    logger_->info(RCX, 31, "Defined Derived extraction corner {}", cornerName);
  }
}

void Ext::delete_corners()
{
  _ext->deleteCorners();
}

void Ext::get_corners(std::list<std::string>& corner_list)
{
  _ext->getCorners(corner_list);
}

void Ext::get_ext_db_corner(int& index, const std::string& name)
{
  index = _ext->getDbCornerIndex(name.c_str());

  if (index < 0) {
    logger_->warn(RCX, 148, "Extraction corner {} not found!", name.c_str());
  }
}

void Ext::extractMultiChip(const ExtractOptions& options)
{
  multi_chip_extractor_->run(options);
}

void Ext::setExtractionRulesFile(const std::string& rules_file)
{
  _ext->setExtractionRulesFile(rules_file);
}

void Ext::setExtractionRulesFile(const std::string& rules_file,
                                 const std::string& tech_name)
{
  odb::dbTech* tech = _db->findTech(tech_name.c_str());
  if (!tech) {
    logger_->error(RCX,
                   522,
                   "Could not set extraction rules file. Tech {} not found.",
                   tech_name);
  }

  multi_chip_extractor_->setExtractionRulesFile(tech, rules_file);
}

void Ext::setAssemblyExtractionRulesFile(
    const std::string& assembly_extraction_rules_file)
{
  multi_chip_extractor_->setAssemblyExtractionRulesFile(
      assembly_extraction_rules_file);
}

void Ext::extract(ExtractOptions options)
{
  _ext->setBlockFromChip(_db->getChip());
  odb::dbBlock* block = _ext->getBlock();

  const uint32_t fill_count = block->getFills().size();
  const bool whole_design
      = options.net == nullptr || options.net[0] == '\0';
  const bool supported_floating_fill
      = fill_count > 0 && fill_count <= 1024 && whole_design
        && options.coupling_threshold == 0.0;
  if (fill_count > 0 && !supported_floating_fill) {
    logger_->error(
        RCX,
        540,
        "The design contains {} explicit dbFill shape(s). OpenRCX currently "
        "supports at most 1024 floating routing-layer rectangles in "
        "whole-design extraction with coupling_threshold 0; refusing an "
        "unsupported fill profile rather than producing fill-blind "
        "parasitics.",
        fill_count);
  }

  // vibeic fork: see rcx::RoutingPreserver. Constructed before wire
  // normalization and destroyed when extract() returns, by any path.
  RoutingPreserver routing_preserver(logger_);
  routing_preserver.snapshot(block);

  odb::normalizeWireConnectivity(logger_, block);

  std::vector<std::unique_ptr<FloatingFill>> floating_fills;
  if (supported_floating_fill) {
    floating_fills.reserve(fill_count);
    size_t index = 0;
    for (odb::dbFill* fill : block->getFills()) {
      floating_fills.push_back(
          std::make_unique<FloatingFill>(block, fill, index++, logger_));
    }
  }

  if (options.ext_model_file != nullptr && options.ext_model_file[0] != '\0') {
    logger_->warn(RCX,
                  514,
                  "The ext_model_file option is deprecated. Use "
                  "set_extraction_rules_file command instead.");

    _ext->setExtractionRulesFile(options.ext_model_file);
  }

  const std::string& rules_file = _ext->getExtractionRulesFile();
  if (!rules_file.empty()) {
    options.ext_model_file = rules_file.c_str();
  }

  _ext->set_debug_nets(options.debug_net);

  _ext->_lef_res = options.lef_res;
  if (options.lef_rc) {
    if (!_ext->checkLayerResistance()) {
      return;
    }
    logger_->info(RCX, 375, "Using LEF RC values to extract!");
  }
  _ext->setExtractionOptions_v2(options);

  if (_ext->_v2) {
    _ext->makeBlockRCsegs_v2(options.net, options.ext_model_file);
  } else {
    _ext->setExtractionOptions(options);

    if (_ext->modelExists()) {
      odb::dbTech* tech = block->getTech();
      std::unique_ptr<extRCModel> rules_model = parseRules(
          tech, rules_file, _ext->getProcessCornerTable(), _ext->_v2, logger_);

      _ext->registerRulesModel(rules_model.release());
      _ext->setCornerCount();
      _ext->run();
    }
  }

  int neighbour_nodes = 0;
  int effective_couplings = 0;
  for (std::unique_ptr<FloatingFill>& fill : floating_fills) {
    const FillReduction reduction = fill->reduce();
    neighbour_nodes += reduction.neighbours;
    effective_couplings += reduction.couplings;
    fill.reset();
  }
  if (supported_floating_fill && neighbour_nodes == 0) {
    logger_->error(RCX,
                   545,
                   "The explicit fill profile has no extracted coupling to "
                   "a routed conductor; refusing a vacuous fill result.");
  }
  if (supported_floating_fill) {
    logger_->info(RCX,
                  549,
                  "Reduced {} floating dbFill conductor(s) with zero net "
                  "charge: {} cumulative neighbour node(s), {} effective "
                  "coupling(s), {} process corner(s).",
                  fill_count,
                  neighbour_nodes,
                  effective_couplings,
                  block->getCornerCount());
  }
}

void Ext::adjust_rc(float res_factor, float cc_factor, float gndc_factor)
{
  _ext->adjustRC(res_factor, cc_factor, gndc_factor);
}

void Ext::write_spef_nets(odb::dbObject* block,
                          bool flatten,
                          bool parallel,
                          int corner)
{
  _ext->setBlockFromChip(_db->getChip());
  _ext->write_spef_nets(flatten, parallel);
}

void Ext::write_spef(const SpefOptions& options)
{
  _ext->setBlockFromChip(_db->getChip());
  if (options.end) {
    _ext->writeSPEF(true);
    return;
  }
  const char* name = options.ext_corner_name;

  uint32_t netId = options.net_id;
  if (netId > 0) {
    _ext->writeSPEF(netId,
                    options.single_pi,
                    options.debug,
                    options.corner,
                    name,
                    spef_version_);
    return;
  }
  _ext->writeSPEF((char*) options.file,
                  (char*) options.nets,
                  options.no_name_map,
                  (char*) options.N,
                  options.term_junction_xy,
                  options.cap_units,
                  options.res_units,
                  options.gz,
                  options.stop_after_map,
                  options.w_clock,
                  options.w_conn,
                  options.w_cap,
                  options.w_cc_cap,
                  options.w_res,
                  options.no_c_num,
                  false,
                  options.single_pi,
                  options.no_backslash,
                  options.corner,
                  name,
                  spef_version_,
                  options.parallel);
}

void Ext::writeMultiChipSpef(const SpefOptions& options)
{
  multi_chip_spef_writer_->run(options);
}

void Ext::read_spef(ReadSpefOpts& opt)
{
  _ext->setBlockFromChip(_db->getChip());
  logger_->info(RCX, 1, "Reading SPEF file: {}", opt.file);

  bool stampWire = opt.stamp_wire;
  uint32_t testParsing = opt.test_parsing;

  Parser parser(logger_);
  char* filename = (char*) opt.file;
  if (!filename || !filename[0]) {
    logger_->error(RCX, 2, "Filename is not defined!");
  }
  parser.mkWords(filename);

  _ext->readSPEF(parser.get(0),
                 (char*) opt.net,
                 opt.force,
                 opt.r_conn,
                 (char*) opt.N,
                 opt.r_cap,
                 opt.r_cc_cap,
                 opt.r_res,
                 opt.cc_threshold,
                 opt.cc_ground_factor,
                 opt.length_unit,
                 opt.m_map,
                 opt.no_cap_num_collapse,
                 (char*) opt.cap_node_map_file,
                 opt.log,
                 opt.corner,
                 0.0,
                 0.0,
                 nullptr,
                 nullptr,
                 nullptr,
                 opt.db_corner_name,
                 opt.calibrate_base_corner,
                 opt.spef_corner,
                 opt.fix_loop,
                 opt.keep_loaded_corner,
                 stampWire,
                 testParsing,
                 opt.more_to_read,
                 false /*diff*/,
                 false /*calibrate*/,
                 opt.app_print_limit);

  for (int ii = 1; ii < parser.getWordCnt(); ii++) {
    _ext->readSPEFincr(parser.get(ii));
  }
}

void Ext::diff_spef(const DiffOptions& opt)
{
  _ext->setBlockFromChip(_db->getChip());
  std::string filename(opt.file);
  if (filename.empty()) {
    logger_->error(
        RCX, 380, "Filename is not defined to run diff_spef command!");
  }
  logger_->info(RCX, 19, "diffing spef {}", opt.file);

  Parser parser(logger_);
  parser.mkWords(opt.file);

  _ext->readSPEF(parser.get(0),
                 (char*) opt.net,
                 false /*force*/,
                 opt.r_conn,
                 nullptr /*N*/,
                 opt.r_cap,
                 opt.r_cc_cap,
                 opt.r_res,
                 -1.0,
                 0.0 /*cc_ground_factor*/,
                 1.0 /*length_unit*/,
                 opt.m_map,
                 false /*noCapNumCollapse*/,
                 nullptr /*capNodeMapFile*/,
                 opt.log,
                 opt.ext_corner,
                 opt.low_guard,
                 opt.upper_guard,
                 (char*) opt.exclude_net_subword,
                 (char*) opt.net_subword,
                 (char*) opt.rc_stats_file,
                 (char*) opt.db_corner_name,
                 nullptr,
                 opt.spef_corner,
                 0 /*fix_loop*/,
                 false /*keepLoadedCorner*/,
                 false /*stampWire*/,
                 opt.test_parsing,
                 false /*moreToRead*/,
                 true /*diff*/,
                 false /*calibrate*/,
                 0);
}

void Ext::calibrate(const std::string& spef_file,
                    const std::string& db_corner_name,
                    int corner,
                    int spef_corner,
                    bool m_map,
                    float upper_limit,
                    float lower_limit)
{
  if (spef_file.empty()) {
    logger_->error(RCX,
                   381,
                   "Filename for calibration is not defined. Define the "
                   "filename using -spef_file");
  }
  logger_->info(RCX, 21, "calibrate on spef file  {}", spef_file.c_str());
  Parser parser(logger_);
  parser.mkWords((char*) spef_file.c_str());
  _ext->calibrate(parser.get(0),
                  m_map,
                  upper_limit,
                  lower_limit,
                  db_corner_name.c_str(),
                  corner,
                  spef_corner);
}

bool Ext::gen_rcx_model(const std::string& spef_file_list,
                        const std::string& corner_list,
                        const std::string& out_file,
                        const std::string& comment,
                        const std::string& version,
                        int pattern)
{
  _ext->setBlockFromChip(_db->getChip());

  if (spef_file_list.empty()) {
    logger_->error(
        RCX, 144, "\nSpef List option -spef_file_list is required\n");
  }
  if (corner_list.empty()) {
    logger_->error(
        RCX, 145, "\nCorner List option -corner_list  is required\n");
  }

  Parser parser(logger_);
  int n = parser.mkWords(spef_file_list.c_str());

  std::list<std::string> file_list;
  for (int ii = 0; ii < n; ii++) {
    std::string name(parser.get(ii));
    file_list.push_back(name);
  }
  int n1 = parser.mkWords(corner_list.c_str());
  if (n != n1) {
    logger_->error(RCX, 150, "\nMismatch of number Corners and Spef Files\n");
  }

  std::list<std::string> corners_list;
  for (int ii = 0; ii < n; ii++) {
    std::string name(parser.get(ii));
    corners_list.push_back(name);
  }
  _ext->GenExtModel(file_list,
                    corners_list,
                    out_file.c_str(),
                    comment.c_str(),
                    version.c_str(),
                    pattern);
  return true;
}
bool Ext::define_rcx_corners(const std::string& corner_list)
{
  if (corner_list.empty()) {
    logger_->error(
        RCX, 146, "\nCorner List option -corner_list  is required\n");
  }

  _ext->setBlockFromChip(_db->getChip());

  Parser parser(logger_);
  int n1 = parser.mkWords(corner_list.c_str());
  for (int ii = 0; ii < n1; ii++) {
    const char* name = parser.get(ii);
    // char *cornerName = _ext->addRCCorner(name, ii);
    _ext->addRCCorner(name, ii);
  }
  return true;
}
bool Ext::rc_estimate(const std::string& ext_model_file,
                      const std::string& out_file_prefix)
{
  extRCModel* m = new extRCModel("MINTYPMAX", nullptr);

  double version = 0.0;
  std::list<std::string> corner_list
      = extModelGen::GetCornerNames(ext_model_file.c_str(), version, logger_);
  const auto extDbCnt = corner_list.size();

  uint32_t cornerTable[10];
  for (uint32_t ii = 0; ii < extDbCnt; ii++) {
    cornerTable[ii] = ii;
  }

  odb::dbTech* tech = _db->getTech();

  int dbunit = tech->getDbUnitsPerMicron();
  double dbFactor = 1;
  if (dbunit > 1000) {
    dbFactor = dbunit * 0.001;
  }

  if (!(m->readRules((char*) ext_model_file.c_str(),
                     false,
                     true,
                     true,
                     true,
                     true,
                     extDbCnt,
                     cornerTable,
                     dbFactor))) {
    fprintf(stderr, "Failed to parse %s\n", ext_model_file.c_str());
  }
  char buff[1000];
  sprintf(buff, "%s.estimate.wire.rc", out_file_prefix.c_str());
  m->calcMinMaxRC(tech, buff);

  delete m;

  return true;
}
bool Ext::get_model_corners(const std::string& ext_model_file, Logger* logger)
{
  double version = 0.0;
  std::list<std::string> corner_list
      = extModelGen::GetCornerNames(ext_model_file.c_str(), version, logger);
  // out_args->corner_list(corner_list);

  std::list<std::string>::iterator it;
  uint32_t cnt = 0;
  // notice(0, "List of Corners (%d) -- Model Version %g\n", corner_list.size(),
  // version);

  fprintf(stdout,
          "List of Corners (%d) -- Model Version %g\n",
          (int) corner_list.size(),
          version);
  for (it = corner_list.begin(); it != corner_list.end(); ++it) {
    const std::string& str = *it;
    // notice(0, "\t%d %s\n", cnt++, str.c_str())
    fprintf(stdout, "\t%d %s\n", cnt++, str.c_str());
  }
  return true;
}
}  // namespace rcx
