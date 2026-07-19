// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

#include "psm/pdnsim.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "debug_gui.h"
#include "dpl/Opendp.h"
#include "gui/gui.h"
#include "gui/heatMap.h"
#include "heatMap.h"
#include "ir_network.h"
#include "ir_solver.h"
#include "node.h"
#include "odb/db.h"
#include "odb/dbShape.h"
#include "odb/dbTypes.h"
#include "shape.h"
#include "sta/Graph.hh"
#include "sta/GraphDelayCalc.hh"
#include "sta/Liberty.hh"
#include "sta/Network.hh"
#include "sta/Scene.hh"
#include "sta/Transition.hh"
#include "sta/Units.hh"
#include "utl/Logger.h"

using odb::dbBlock;
using odb::dbSigType;

namespace psm {

PDNSim::PDNSim(utl::Logger* logger,
               odb::dbDatabase* db,
               sta::dbSta* sta,
               est::EstimateParasitics* estimate_parasitics,
               dpl::Opendp* opendp)
{
  db_ = db;
  sta_ = sta;
  estimate_parasitics_ = estimate_parasitics;
  opendp_ = opendp;
  logger_ = logger;
  heatmap_source_ = gui::registerHeatMapSource(
      "IR Drop", "IRDrop", "IRDrop", [this, sta, logger]() {
        return std::make_shared<IRDropDataSource>(this, sta, logger);
      });
}

PDNSim::~PDNSim() = default;

void PDNSim::setDebugGui(bool enable)
{
  debug_gui_enabled_ = enable;

  for (const auto& [net, solver] : solvers_) {
    solver->enableGui(debug_gui_enabled_);
  }

  gui::Gui::get()->registerDescriptor<Node*>(new NodeDescriptor(solvers_));
  gui::Gui::get()->registerDescriptor<ITermNode*>(
      new ITermNodeDescriptor(solvers_));
  gui::Gui::get()->registerDescriptor<BPinNode*>(
      new BPinNodeDescriptor(solvers_));
  gui::Gui::get()->registerDescriptor<Connection*>(
      new ConnectionDescriptor(solvers_));
}

void PDNSim::setNetVoltage(odb::dbNet* net, sta::Scene* corner, double voltage)
{
  auto& voltages = user_voltages_[net];
  voltages[corner] = voltage;
}

void PDNSim::setInstPower(odb::dbInst* inst, sta::Scene* corner, float power)
{
  auto& powers = user_powers_[inst];
  powers[corner] = power;
}

void PDNSim::analyzePowerGrid(odb::dbNet* net,
                              sta::Scene* corner,
                              GeneratedSourceType source_type,
                              const std::string& voltage_file,
                              bool use_prev_solution,
                              bool enable_em,
                              const std::string& em_file,
                              const std::string& error_file,
                              const std::string& voltage_source_file)
{
  if (!checkConnectivity(net, false, error_file, false)) {
    return;
  }

  last_net_ = net;
  last_corner_ = corner;
  auto* solver = getIRSolver(net, false);
  if (!use_prev_solution || !solver->hasSolution(corner)) {
    solver->solve(corner, source_type, voltage_source_file);
  } else {
    logger_->info(utl::PSM, 11, "Reusing previous solution");
  }
  solver->report(corner);

  if (heatmap_source_) {
    heatmap_source_->invalidateInstances();
  }

  if (enable_em) {
    solver->reportEM(corner);
    solver->writeEMFile(em_file, corner);
  }

  solver->writeInstanceVoltageFile(voltage_file, corner);
}

EMSignoffResult PDNSim::checkCurrentDensity(
    odb::dbNet* net,
    sta::Scene* corner,
    GeneratedSourceType source_type,
    const std::string& voltage_source_file,
    bool use_prev_solution,
    double default_limit,
    const std::string& limits_file,
    const std::string& report_file)
{
  if (!checkConnectivity(net, false, "", false)) {
    return EMSignoffResult{};
  }

  // Assemble the per-layer J-limits.  A per-layer file wins over the uniform
  // default for the layers it names; unnamed layers fall back to default_limit.
  EMLimits limits;
  limits.default_limit = default_limit;
  if (!limits_file.empty()) {
    std::ifstream lf(limits_file);
    if (!lf) {
      logger_->error(
          utl::PSM, 114, "Unable to open EM limits file {}", limits_file);
    }
    std::string line;
    int nrows = 0;
    while (std::getline(lf, line)) {
      // Strip a trailing comment and skip blank lines.
      const auto hash = line.find('#');
      if (hash != std::string::npos) {
        line = line.substr(0, hash);
      }
      std::istringstream ss(line);
      std::string layer;
      double value = 0.0;
      if (ss >> layer >> value) {
        limits.per_layer[layer] = value;
        nrows++;
      }
    }
    logger_->info(utl::PSM,
                  115,
                  "Loaded {} per-layer EM current-density limit(s) from {}.",
                  nrows,
                  limits_file);
  }

  if (limits.empty()) {
    logger_->warn(utl::PSM,
                  116,
                  "No EM current-density limit supplied (-em_limit / "
                  "-em_limits_file); every segment is reported as NO_LIMIT and "
                  "the check cannot fail.  Provide the PDK per-layer J-limits "
                  "for a real signoff.");
  }

  last_net_ = net;
  last_corner_ = corner;
  auto* solver = getIRSolver(net, false);
  if (!use_prev_solution || !solver->hasSolution(corner)) {
    solver->solve(corner, source_type, voltage_source_file);
  }

  return solver->checkCurrentDensity(corner, limits, report_file);
}

namespace {

// Parse "<layer> <avg|rms|peak> <A_per_um2>" rows into the three per-family
// tables.  Returns the number of rows consumed.
int loadSignalEMLimitsFile(const std::string& path,
                           SignalEMLimits& limits,
                           utl::Logger* logger)
{
  std::ifstream lf(path);
  if (!lf) {
    logger->error(
        utl::PSM, 121, "Unable to open signal EM limits file {}", path);
  }
  std::string line;
  int nrows = 0;
  while (std::getline(lf, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    std::istringstream ss(line);
    std::string layer, family;
    double value = 0.0;
    if (!(ss >> layer >> family >> value)) {
      continue;
    }
    if (family == "avg") {
      limits.avg.per_layer[layer] = value;
    } else if (family == "rms") {
      limits.rms.per_layer[layer] = value;
    } else if (family == "peak") {
      limits.peak.per_layer[layer] = value;
    } else {
      logger->error(utl::PSM,
                    122,
                    "Unknown signal EM limit family \"{}\" for layer {} in {}; "
                    "expected avg, rms or peak.",
                    family,
                    layer,
                    path);
    }
    nrows++;
  }
  return nrows;
}

// Parse "<net_name> <toggles_per_second>" rows.
int loadActivityFile(const std::string& path,
                     std::map<std::string, double>& activity,
                     utl::Logger* logger)
{
  std::ifstream af(path);
  if (!af) {
    logger->error(utl::PSM, 123, "Unable to open activity file {}", path);
  }
  std::string line;
  int nrows = 0;
  while (std::getline(af, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    std::istringstream ss(line);
    std::string net;
    double value = 0.0;
    if (ss >> net >> value) {
      activity[net] = value;
      nrows++;
    }
  }
  return nrows;
}

}  // namespace

SignalEMResult PDNSim::checkSignalEM(sta::Scene* corner,
                                     double supply_voltage,
                                     double toggle_rate,
                                     const std::string& activity_file,
                                     double avg_limit,
                                     double rms_limit,
                                     double peak_limit,
                                     const std::string& limits_file,
                                     const std::string& report_file)
{
  SignalEMLimits limits;
  limits.avg.default_limit = avg_limit;
  limits.rms.default_limit = rms_limit;
  limits.peak.default_limit = peak_limit;
  if (!limits_file.empty()) {
    const int nrows = loadSignalEMLimitsFile(limits_file, limits, logger_);
    logger_->info(utl::PSM,
                  124,
                  "Loaded {} per-layer signal EM current-density limit(s) "
                  "from {}.",
                  nrows,
                  limits_file);
  }
  if (limits.empty()) {
    logger_->warn(utl::PSM,
                  125,
                  "No signal EM current-density limit supplied (-avg_limit / "
                  "-rms_limit / -peak_limit / -em_limits_file); every segment "
                  "is reported as NO_LIMIT and the check cannot fail.  Provide "
                  "the PDK per-layer J-limits for a real signoff.");
  }

  std::map<std::string, double> per_net_activity;
  if (!activity_file.empty()) {
    const int nrows
        = loadActivityFile(activity_file, per_net_activity, logger_);
    logger_->info(
        utl::PSM, 126, "Loaded {} per-net activity value(s) from {}.",
        nrows, activity_file);
  }

  // The rail-to-rail swing.  Prefer what the user states; otherwise fall back
  // to the liberty nominal voltage.  Without a swing there is no charge per
  // transition, so the check reports nothing rather than guessing.
  if (!(supply_voltage > 0.0)) {
    sta::LibertyLibrary* lib = sta_->network()->defaultLibertyLibrary();
    if (lib != nullptr) {
      supply_voltage = lib->nominalVoltage();
    }
  }
  if (!(supply_voltage > 0.0)) {
    logger_->warn(utl::PSM,
                  127,
                  "No supply voltage available for signal EM (-supply_voltage "
                  "not given and no liberty nominal voltage); the check cannot "
                  "run.");
    return SignalEMResult{};
  }

  // Real load capacitances and transition times require a current delay calc.
  sta_->updateTiming(false);
  sta::Graph* graph = sta_->ensureGraph();
  sta::GraphDelayCalc* dcalc = sta_->graphDelayCalc();
  sta::dbNetwork* network = sta_->getDbNetwork();
  const sta::DcalcAPIndex dcalc_ap
      = corner->dcalcAnalysisPtIndex(sta::MinMax::max());

  odb::dbBlock* block = db_->getChip()->getBlock();
  const double dbu = block->getDbUnitsPerMicron();
  const double dbu2 = dbu * dbu;

  std::vector<SignalEMNet> nets;
  for (odb::dbNet* db_net : block->getNets()) {
    if (db_net->getSigType().isSupply() || db_net->isSpecial()) {
      continue;  // the power grid is EM3's job, not this check's
    }
    odb::dbWire* wire = db_net->getWire();
    if (wire == nullptr) {
      continue;  // unrouted: no geometry to check
    }

    SignalEMNet net;
    net.name = db_net->getName();

    // ---- drive conditions -------------------------------------------------
    // The driver pin carries the whole net's switching current.
    sta::Pin* drvr_pin = nullptr;
    for (odb::dbITerm* iterm : db_net->getITerms()) {
      if (iterm->getIoType() == odb::dbIoType::OUTPUT
          || iterm->getIoType() == odb::dbIoType::INOUT) {
        drvr_pin = network->dbToSta(iterm);
        break;
      }
    }
    if (drvr_pin == nullptr) {
      for (odb::dbBTerm* bterm : db_net->getBTerms()) {
        // A primary INPUT port drives the net inside the block.
        if (bterm->getIoType() == odb::dbIoType::INPUT
            || bterm->getIoType() == odb::dbIoType::INOUT) {
          drvr_pin = network->dbToSta(bterm);
          break;
        }
      }
    }
    if (drvr_pin != nullptr) {
      net.drive.cap_f = dcalc->loadCap(drvr_pin, corner, sta::MinMax::max());
      net.drive.supply_v = supply_voltage;
      const auto it = per_net_activity.find(net.name);
      net.drive.density_hz
          = (it != per_net_activity.end()) ? it->second : toggle_rate;

      // The transition time is the worst of the two edges at the driver.
      sta::Vertex* vertex = graph->pinDrvrVertex(drvr_pin);
      double transition = 0.0;
      if (vertex != nullptr) {
        for (const sta::RiseFall* rf : sta::RiseFall::range()) {
          const double slew
              = sta::delayAsFloat(graph->slew(vertex, rf, dcalc_ap));
          transition = std::max(transition, slew);
        }
      }
      net.drive.transition_s = transition;
    }

    // ---- routed geometry --------------------------------------------------
    odb::dbWireShapeItr itr;
    odb::dbShape shape;
    for (itr.begin(wire); itr.next(shape);) {
      SignalEMSegment seg;
      double area_dbu2 = 0.0;
      if (shape.isVia()) {
        // A via's cross-section is the sum of its cut areas.
        seg.is_via = true;
        std::vector<odb::dbShape> boxes;
        odb::dbShape::getViaBoxes(shape, boxes);
        for (const odb::dbShape& box : boxes) {
          odb::dbTechLayer* layer = box.getTechLayer();
          if (layer == nullptr
              || layer->getType() != odb::dbTechLayerType::CUT) {
            continue;
          }
          const odb::Rect r = box.getBox();
          area_dbu2 += static_cast<double>(r.dx()) * static_cast<double>(r.dy());
          seg.layer = layer->getName();
        }
      } else {
        odb::dbTechLayer* layer = shape.getTechLayer();
        if (layer == nullptr) {
          continue;
        }
        seg.layer = layer->getName();
        // A wire's cross-section is (width x metal THICKNESS).  When the PDK
        // omits THICKNESS no physical cross-section can be formed, so the
        // segment is left at area 0 and the engine skips it.
        uint32_t thickness = 0;
        if (layer->getThickness(thickness) && thickness != 0) {
          const odb::Rect r = shape.getBox();
          const double width = std::min(r.dx(), r.dy());
          area_dbu2 = width * static_cast<double>(thickness);
        }
      }
      const odb::Rect r = shape.getBox();
      seg.area_um2 = area_dbu2 / dbu2;
      seg.x0 = r.xMin() / dbu;
      seg.y0 = r.yMin() / dbu;
      seg.x1 = r.xMax() / dbu;
      seg.y1 = r.yMax() / dbu;
      net.segments.push_back(std::move(seg));
    }

    if (!net.segments.empty()) {
      nets.push_back(std::move(net));
    }
  }

  const SignalEMResult results = classifySignalEM(nets, limits);

  if (!report_file.empty()) {
    std::ofstream report(report_file);
    if (!report) {
      logger_->error(utl::PSM,
                     128,
                     "Unable to open {} to write signal EM report",
                     report_file);
    }
    report << "Net,Layer,Via,X0,Y0,X1,Y1,Area(um^2),"
              "Iavg(A),Irms(A),Ipeak(A),"
              "Javg(A/um^2),Jrms(A/um^2),Jpeak(A/um^2),"
              "LimitAvg,LimitRms,LimitPeak,WorstRatio,WorstMode,Status\n";
    for (const auto& s : results.segments) {
      const char* status = (s.limit_avg <= 0.0 && s.limit_rms <= 0.0
                            && s.limit_peak <= 0.0)
                               ? "NO_LIMIT"
                           : s.violated() ? "VIOLATED"
                                          : "OK";
      report << s.net << "," << s.layer << "," << (s.is_via ? 1 : 0) << ","
             << fmt::format("{:.4f}", s.x0) << ","
             << fmt::format("{:.4f}", s.y0) << ","
             << fmt::format("{:.4f}", s.x1) << ","
             << fmt::format("{:.4f}", s.y1) << ","
             << fmt::format("{:.4e}", s.area_um2) << ","
             << fmt::format("{:.3e}", s.i_avg) << ","
             << fmt::format("{:.3e}", s.i_rms) << ","
             << fmt::format("{:.3e}", s.i_peak) << ","
             << fmt::format("{:.3e}", s.j_avg) << ","
             << fmt::format("{:.3e}", s.j_rms) << ","
             << fmt::format("{:.3e}", s.j_peak) << ","
             << fmt::format("{:.3e}", s.limit_avg) << ","
             << fmt::format("{:.3e}", s.limit_rms) << ","
             << fmt::format("{:.3e}", s.limit_peak) << ","
             << fmt::format("{:.3f}", s.worstRatio()) << ","
             << signalEMModeName(s.worstMode()) << "," << status << '\n';
    }
  }

  logger_->report("########## Signal-net EM signoff ##########");
  logger_->report("Corner              : {}", corner->name());
  logger_->report("Supply swing        : {:3.3f} V", supply_voltage);
  logger_->report("Nets with routing   : {}", results.nets);
  logger_->report("Nets with drive     : {}", results.nets_driven);
  logger_->report("Segments checked    : {}", results.checked);
  logger_->report("With J-limit        : {}", results.limited);
  logger_->report("No drive (skipped)  : {}", results.skipped_no_drive);
  logger_->report("No geometry (skipped): {}", results.skipped_no_area);
  logger_->report("No J-limit (skipped): {}", results.skipped_no_limit);
  logger_->report("Worst J             : {:3.3e} A/um^2 ({} on layer {}, net {})",
                  results.worst_j,
                  signalEMModeName(results.worst_mode),
                  results.worst_layer.empty() ? "-" : results.worst_layer,
                  results.worst_net.empty() ? "-" : results.worst_net);
  logger_->report("Worst J-limit       : {:3.3e} A/um^2", results.worst_limit);
  logger_->report("Worst utilization   : {:3.2f} %", 100.0 * results.worst_ratio);
  logger_->report("Violations (avg)    : {}", results.violations_avg);
  logger_->report("Violations (rms)    : {}", results.violations_rms);
  logger_->report("Violations (peak)   : {}", results.violations_peak);
  logger_->report("Violations          : {}", results.violations);
  logger_->report("Verdict             : {}", results.pass() ? "PASS" : "FAIL");
  logger_->report("##########################################");

  if (results.violations > 0) {
    logger_->warn(utl::PSM,
                  129,
                  "Signal EM signoff FAILED: {} segment(s) exceed a per-layer "
                  "J-limit (worst {:.1f}% of the {} limit on {}, net {}).",
                  results.violations,
                  100.0 * results.worst_ratio,
                  signalEMModeName(results.worst_mode),
                  results.worst_layer.empty() ? "-" : results.worst_layer,
                  results.worst_net.empty() ? "-" : results.worst_net);
  }

  logger_->metric("design__signal_em__violations", results.violations);
  logger_->metric("design__signal_em__worst_ratio", results.worst_ratio);

  return results;
}

void PDNSim::analyzePowerGridDynamic(odb::dbNet* net,
                                     sta::Scene* corner,
                                     GeneratedSourceType source_type,
                                     const std::string& voltage_file,
                                     const std::string& error_file,
                                     const std::string& voltage_source_file,
                                     double period,
                                     int steps,
                                     int num_periods,
                                     double node_cap,
                                     double total_cap,
                                     double decap_cap,
                                     double current_duty,
                                     bool phase_spread,
                                     const std::string& current_profile,
                                     const std::string& vectored_profile,
                                     double package_r,
                                     double package_l)
{
  if (!checkConnectivity(net, false, error_file, false)) {
    return;
  }

  last_net_ = net;
  last_corner_ = corner;

  IRSolver::TransientSettings settings;
  settings.period = period;
  settings.steps = steps;
  settings.num_periods = num_periods;
  settings.node_cap = node_cap;
  settings.total_cap = total_cap;
  settings.decap_cap = decap_cap;
  settings.current_duty = current_duty;
  settings.phase_spread = phase_spread;
  settings.current_profile = current_profile;
  settings.vectored_profile = vectored_profile;
  settings.package_r = package_r;
  settings.package_l = package_l;

  auto* solver = getIRSolver(net, false);
  solver->solveTransient(corner, source_type, voltage_source_file, settings);
  solver->report(corner);
  solver->reportTransient(corner);

  if (heatmap_source_) {
    heatmap_source_->invalidateInstances();
  }

  solver->writeTransientVoltageFile(voltage_file, corner);
}

bool PDNSim::checkConnectivity(odb::dbNet* net,
                               bool floorplanning,
                               const std::string& error_file,
                               bool require_bterm)
{
  auto* solver = getIRSolver(net, floorplanning);
  const bool check = solver->check(require_bterm);
  solver->writeErrorFile(error_file);

  if (debug_gui_enabled_) {
    solver->enableGui(true);
  }

  if (logger_->debugCheck(utl::PSM, "stats", 1)) {
    solver->getNetwork()->reportStats();
  }

  if (check) {
    logger_->info(
        utl::PSM, 40, "All shapes on net {} are connected.", net->getName());
  } else {
    logger_->error(
        utl::PSM, 69, "Check connectivity failed on {}.", net->getName());
  }
  return check;
}

void PDNSim::writeSpiceNetwork(odb::dbNet* net,
                               sta::Scene* corner,
                               GeneratedSourceType source_type,
                               const std::string& spice_file,
                               const std::string& voltage_source_file)
{
  auto* solver = getIRSolver(net, false);
  solver->writeSpiceFile(source_type, spice_file, corner, voltage_source_file);
}

psm::IRSolver* PDNSim::getIRSolver(odb::dbNet* net, bool floorplanning)
{
  auto& solver = solvers_[net];
  if (solver == nullptr) {
    solver = std::make_unique<IRSolver>(net,
                                        floorplanning,
                                        sta_,
                                        estimate_parasitics_,
                                        logger_,
                                        user_voltages_,
                                        user_powers_,
                                        generated_source_settings_);
    addOwner(net->getBlock());
  }

  return solver.get();
}

void PDNSim::getIRDropForLayer(odb::dbNet* net,
                               odb::dbTechLayer* layer,
                               IRDropByPoint& ir_drop) const
{
  auto find_solver = solvers_.find(net);
  if (last_corner_ == nullptr || find_solver == solvers_.end()) {
    return;
  }
  ir_drop = find_solver->second->getIRDrop(layer, last_corner_);
}

void PDNSim::getIRDropForLayer(odb::dbNet* net,
                               sta::Scene* corner,
                               odb::dbTechLayer* layer,
                               IRDropByPoint& ir_drop) const
{
  auto find_solver = solvers_.find(net);
  if (find_solver == solvers_.end()) {
    return;
  }
  ir_drop = find_solver->second->getIRDrop(layer, corner);
}

double PDNSim::getWorstIRDrop(odb::dbNet* net) const
{
  auto find_solver = solvers_.find(net);
  if (last_corner_ == nullptr || find_solver == solvers_.end()) {
    return 0.0;
  }

  // The worst static IR drop is the largest drop seen on any routing layer.
  // Iterate the whole routing stack rather than a single layer so the number
  // matches the "Worst static IR drop" the solver reports.
  double worst = 0.0;
  odb::dbTech* tech = db_->getTech();
  for (odb::dbTechLayer* layer : tech->getLayers()) {
    if (layer->getType() != odb::dbTechLayerType::ROUTING) {
      continue;
    }
    IRDropByPoint ir_drop = find_solver->second->getIRDrop(layer, last_corner_);
    for (const auto& [point, drop] : ir_drop) {
      worst = std::max(worst, drop);
    }
  }
  return worst;
}

void PDNSim::setGeneratedSourceSettings(const GeneratedSourceSettings& settings)
{
  if (settings.bump_dx > 0) {
    generated_source_settings_.bump_dx = settings.bump_dx;
  }
  if (settings.bump_dy > 0) {
    generated_source_settings_.bump_dy = settings.bump_dy;
  }
  if (settings.bump_interval > 0) {
    generated_source_settings_.bump_interval = settings.bump_interval;
  }
  if (settings.bump_size > 0) {
    generated_source_settings_.bump_size = settings.bump_size;
  }
  if (settings.strap_track_pitch > 0) {
    generated_source_settings_.strap_track_pitch = settings.strap_track_pitch;
  }
  if (settings.resistance > 0) {
    generated_source_settings_.resistance = settings.resistance;
  }
}

void PDNSim::clearSolvers()
{
  solvers_.clear();
}

void PDNSim::inDbPostMoveInst(odb::dbInst*)
{
  clearSolvers();
}

void PDNSim::inDbNetDestroy(odb::dbNet*)
{
  clearSolvers();
}

void PDNSim::inDbBTermPostConnect(odb::dbBTerm*)
{
  clearSolvers();
}

void PDNSim::inDbBTermPostDisConnect(odb::dbBTerm*, odb::dbNet*)
{
  clearSolvers();
}

void PDNSim::inDbBPinCreate(odb::dbBPin*)
{
  clearSolvers();
}

void PDNSim::inDbBPinAddBox(odb::dbBox*)
{
  clearSolvers();
}

void PDNSim::inDbBPinRemoveBox(odb::dbBox*)
{
  clearSolvers();
}

void PDNSim::inDbBPinDestroy(odb::dbBPin*)
{
  clearSolvers();
}

void PDNSim::inDbSWireAddSBox(odb::dbSBox*)
{
  clearSolvers();
}

void PDNSim::inDbSWireRemoveSBox(odb::dbSBox*)
{
  clearSolvers();
}

void PDNSim::inDbSWirePostDestroySBoxes(odb::dbSWire*)
{
  clearSolvers();
}

// Functions of decap cells
void PDNSim::addDecapMaster(odb::dbMaster* decap_master, double decap_cap)
{
  opendp_->addDecapMaster(decap_master, decap_cap);
}

// Return the lowest layer of db_net route
odb::dbTechLayer* PDNSim::getLowestLayer(odb::dbNet* db_net)
{
  int min_layer_level = std::numeric_limits<int>::max();
  std::vector<odb::dbShape> via_boxes;
  for (odb::dbSWire* swire : db_net->getSWires()) {
    for (odb::dbSBox* s : swire->getWires()) {
      if (!s->isVia()) {
        odb::dbTechLayer* tech_layer = s->getTechLayer();
        min_layer_level
            = std::min(min_layer_level, tech_layer->getRoutingLevel());
      }
    }
  }
  return db_->getTech()->findRoutingLayer(min_layer_level);
}

odb::dbNet* PDNSim::findPowerNet(const char* net_name)
{
  dbBlock* block = db_->getChip()->getBlock();
  odb::dbNet* power_net = nullptr;
  // If net name is defined by user
  if (!std::string(net_name).empty()) {
    power_net = block->findNet(net_name);
    if (power_net == nullptr) {
      logger_->error(
          utl::PSM, 48, "Cannot find net {} in the design.", net_name);
    }
    // Check if net is supply
    if (!power_net->getSigType().isSupply()) {
      logger_->error(
          utl::PSM, 47, "{} is not a supply net.", power_net->getName());
    }
    return power_net;
  }
  // Otherwise find power net
  for (auto db_net : block->getNets()) {
    if (db_net->getSigType().isSupply()
        && db_net->getSigType() == dbSigType::POWER) {
      power_net = db_net;
      break;
    }
  }
  return power_net;
}

void PDNSim::insertDecapCells(double target, const char* net_name)
{
  // Get db_net
  odb::dbNet* db_net = findPowerNet(net_name);

  // Get lowest layer
  odb::dbTechLayer* tech_layer = getLowestLayer(db_net);

  IRDropByPoint ir_drops;
  getIRDropForLayer(db_net, tech_layer, ir_drops);

  if (ir_drops.empty()) {
    logger_->error(utl::PSM,
                   93,
                   "No IR drop data found. Run analyse_power_grid for net {} "
                   "before inserting decap cells.",
                   db_net->getName());
  }

  // call DPL to insert decap cells
  opendp_->insertDecapCells(target, ir_drops);
}

}  // namespace psm
