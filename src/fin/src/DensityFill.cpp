// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

#include "DensityFill.h"

#include "DensityCheck.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "boost/lexical_cast.hpp"
#include "boost/polygon/polygon.hpp"
#include "graphics.h"
#include "odb/db.h"
#include "odb/dbShape.h"
#include "odb/dbTypes.h"
#include "odb/geom.h"
#include "polygon.h"
#include "utl/Logger.h"

namespace fin {

using utl::FIN;

namespace pt = boost::property_tree;

using odb::dbBlock;
using odb::dbChip;
using odb::dbDatabase;
using odb::dbFill;
using odb::dbInstShapeItr;
using odb::dbShape;
using odb::dbTech;
using odb::dbTechLayer;
using odb::dbTechLayerDir;
using odb::dbTechVia;
using odb::dbVia;
using odb::dbWire;
using odb::dbWireShapeItr;
using odb::Rect;

// The rules for OPC or non-OPC shapes on a layer from the JSON config
struct DensityFillShapesConfig
{
  // width & height of fill shapes to try (in order)
  std::vector<std::pair<int, int>> shapes;
  int space_to_fill;
  int space_to_non_fill;
  int space_line_end;
};

// The rules for a layer from the JSON config
struct DensityFillLayerConfig
{
  int space_to_outline;
  int num_masks;
  int opc_halo;
  bool has_opc;

  DensityFillShapesConfig opc;
  DensityFillShapesConfig non_opc;
};

// Make a boost polygon representing a rectangle
static Polygon90 makeRect(int x_lo, int y_lo, int x_hi, int y_hi)
{
  using Pt = Polygon90::point_type;
  std::array<Pt, 4> pts
      = {Pt(x_lo, y_lo), Pt(x_hi, y_lo), Pt(x_hi, y_hi), Pt(x_lo, y_hi)};

  Polygon90 poly;
  poly.set(pts.begin(), pts.end());
  return poly;
}

static double getValue(pt::ptree& tree)
{
  return boost::lexical_cast<double>(tree.data());
}

static double getValue(const char* key, pt::ptree& tree)
{
  return getValue(tree.get_child(key));
}

////////////////////////////////////////////////////////////////

DensityFill::DensityFill(dbDatabase* db, utl::Logger* logger, bool debug)
    : db_(db), logger_(logger)
{
  if (debug && Graphics::guiActive()) {
    graphics_ = std::make_unique<Graphics>();
  }
}

// must be in the .cpp due to forward decl
DensityFill::~DensityFill() = default;

// Converts the user's JSON configuration file in per layer
// DensityFillLayerConfig objects.
//
// In the configuration you can group layers together that have
// similar rules.  This method expands such groupings into per layer
// values.  It also translates from microns to DBU and layer names
// to dbTechLayer*.
void DensityFill::readAndExpandLayers(dbTech* tech, pt::ptree& tree)
{
  int dbu = tech->getDbUnitsPerMicron();

  auto& layers = tree.get_child("layers");
  for (auto& [name, layer] : layers) {
    DensityFillLayerConfig cfg;
    cfg.space_to_outline = getValue("space_to_outline", layer) * dbu;

    // non-OPC data
    {
      auto& non_opc = layer.get_child("non-opc");
      auto& scfg = cfg.non_opc;
      scfg.space_to_fill = getValue("space_to_fill", non_opc) * dbu;
      scfg.space_to_non_fill = getValue("space_to_non_fill", non_opc) * dbu;
      scfg.space_line_end = 0;  // n/a for non-OPC
      cfg.num_masks = non_opc.get_child("datatype").size();

      auto widths = non_opc.get_child("width");
      auto heights = non_opc.get_child("height");

      std::ranges::transform(widths,
                             heights,
                             std::back_inserter(scfg.shapes),
                             [dbu](auto& w, auto& h) {
                               return std::make_pair(getValue(w.second) * dbu,
                                                     getValue(h.second) * dbu);
                             });
    }

    // OPC data, if any
    auto opc_it = layer.find("opc");
    cfg.has_opc = opc_it != layer.not_found();
    if (cfg.has_opc) {
      auto& opc = layer.get_child("opc");
      auto& scfg = cfg.opc;
      cfg.opc_halo = getValue("halo", opc) * dbu;
      scfg.space_to_fill = getValue("space_to_fill", opc) * dbu;
      scfg.space_to_non_fill = getValue("space_to_non_fill", opc) * dbu;
      if (opc.find("space_line_end") != opc.not_found()) {
        scfg.space_line_end = getValue("space_line_end", opc) * dbu;
      } else {
        scfg.space_line_end = 0;
      }

      auto widths = opc.get_child("width");
      auto heights = opc.get_child("height");

      std::ranges::transform(widths,
                             heights,
                             std::back_inserter(scfg.shapes),
                             [dbu](auto& w, auto& h) {
                               return std::make_pair(getValue(w.second) * dbu,
                                                     getValue(h.second) * dbu);
                             });
    }

    auto it = layer.find("names");
    if (it != layer.not_found()) {
      // Expand names
      for (auto& [name, layer_name] : layer.get_child("names")) {
        auto tech_layer = tech->findLayer(layer_name.data().c_str());
        if (!tech_layer) {
          logger_->error(FIN,
                         1,
                         "Layer {} in names was not found.",
                         layer.get_child("name").data());
        }
        layers_[tech_layer] = cfg;
      }
    } else {
      // No expansion, just a single layer
      auto tech_layer = tech->findLayer(layer.get_child("name").data().c_str());
      if (!tech_layer) {
        logger_->error(
            FIN, 2, "Layer {} not found.", layer.get_child("name").data());
      }
      layers_[tech_layer] = std::move(cfg);
    }
  }
}

void DensityFill::loadConfig(const char* cfg_filename, dbTech* tech)
{
  // Read the json config file using Boost's property_tree
  pt::ptree tree;
  pt::json_parser::read_json(cfg_filename, tree);
  readAndExpandLayers(tech, tree);
}

// Insert into polygon_set any part of given shape on the given layer (shape may
// be a via)
static void insertShape(const dbShape& shape,
                        Polygon90Set& polygon_set,
                        dbTechLayer* layer)
{
  auto type = shape.getType();
  switch (type) {
    case dbShape::VIA:
    case dbShape::TECH_VIA: {
      dbTechLayer* top;
      dbTechLayer* bottom;
      if (type == dbShape::VIA) {
        dbVia* via = shape.getVia();
        top = via->getTopLayer();
        bottom = via->getBottomLayer();
      } else {
        dbTechVia* via = shape.getTechVia();
        top = via->getTopLayer();
        bottom = via->getBottomLayer();
      }

      if (top != layer && bottom != layer) {
        return;
      }
      std::vector<dbShape> boxes;
      dbShape::getViaBoxes(shape, boxes);
      for (auto& box : boxes) {
        if (box.getTechLayer() == layer) {
          polygon_set.insert(
              makeRect(box.xMin(), box.yMin(), box.xMax(), box.yMax()));
        }
      }
      break;
    }
    case dbShape::SEGMENT:
      if (shape.getTechLayer() == layer) {
        polygon_set.insert(
            makeRect(shape.xMin(), shape.yMin(), shape.xMax(), shape.yMax()));
      }
      break;
    case dbShape::TECH_VIA_BOX:
    case dbShape::VIA_BOX:
      if (shape.getTechLayer() == layer) {
        polygon_set.insert(
            makeRect(shape.xMin(), shape.yMin(), shape.xMax(), shape.yMax()));
      }
      break;
  }
}

// Build a polygon set out of all the non-fill shape on the given layer
// including wires, special wires, and instances' pins & OBS
static Polygon90Set orNonFills(dbBlock* block, dbTechLayer* layer)
{
  Polygon90Set non_fill;  // The result
  dbShape shape;          // Shared temp

  // Get shapes from regular wires
  dbWireShapeItr shapes;
  for (auto net : block->getNets()) {
    dbWire* wire = net->getWire();
    if (!wire) {
      continue;
    }
    for (shapes.begin(wire); shapes.next(shape);) {
      insertShape(shape, non_fill, layer);
    }
  }

  // Get shapes from special wires
  std::vector<dbShape> via_shapes;
  for (auto net : block->getNets()) {
    for (auto swire : net->getSWires()) {
      for (auto sbox : swire->getWires()) {
        if (sbox->isVia()) {
          dbVia* via = sbox->getBlockVia();
          Rect rect = sbox->getBox();
          shape.setVia(via, rect);
          dbShape::getViaBoxes(shape, via_shapes);
          for (auto& via_shape : via_shapes) {
            insertShape(via_shape, non_fill, layer);
          }
        } else if (sbox->getTechLayer() == layer) {
          non_fill.insert(
              makeRect(sbox->xMin(), sbox->yMin(), sbox->xMax(), sbox->yMax()));
        }
      }
    }
  }

  // Get shapes from instances
  dbInstShapeItr insts(/* expand_vias */ false);
  for (auto inst : block->getInsts()) {
    for (insts.begin(inst, dbInstShapeItr::ALL); insts.next(shape);) {
      insertShape(shape, non_fill, layer);
    }
  }

  return non_fill;
}

// Build a polygon set of the shapes of the given nets on the given layer.
// Used to hold fill away from coupling-sensitive (timing-critical) nets.
static Polygon90Set orNets(dbBlock* block,
                           dbTechLayer* layer,
                           const odb::PtrSet<odb::dbNet>& nets)
{
  Polygon90Set result;
  dbShape shape;

  dbWireShapeItr shapes;
  std::vector<dbShape> via_shapes;
  for (auto net : nets) {
    if (dbWire* wire = net->getWire()) {
      for (shapes.begin(wire); shapes.next(shape);) {
        insertShape(shape, result, layer);
      }
    }
    for (auto swire : net->getSWires()) {
      for (auto sbox : swire->getWires()) {
        if (sbox->isVia()) {
          dbVia* via = sbox->getBlockVia();
          Rect rect = sbox->getBox();
          shape.setVia(via, rect);
          dbShape::getViaBoxes(shape, via_shapes);
          for (auto& via_shape : via_shapes) {
            insertShape(via_shape, result, layer);
          }
        } else if (sbox->getTechLayer() == layer) {
          result.insert(
              makeRect(sbox->xMin(), sbox->yMin(), sbox->xMax(), sbox->yMax()));
        }
      }
    }
  }
  return result;
}

// Build a polygon set of the fill already present on the given layer.
static Polygon90Set orFills(dbBlock* block, dbTechLayer* layer)
{
  Polygon90Set fills;
  for (auto fill : block->getFills()) {
    if (fill->getTechLayer() != layer) {
      continue;
    }
    Rect rect;
    fill->getRect(rect);
    fills.insert(makeRect(rect.xMin(), rect.yMin(), rect.xMax(), rect.yMax()));
  }
  return fills;
}

// Charges fill shapes against a per-window metal-area budget so that inserting
// fill can never push a density window past max_density.  Windows overlap when
// step < window, so one shape is charged to every window it touches and is
// rejected unless *all* of them can absorb it.
//
// The windows and their starting metal area come from DensityCheck -- the same
// measurement the check_metal_density signoff uses -- so a window the budget
// believes is at 0.40 is a window signoff will also measure at 0.40.  Areas are
// um^2 to match that core.
class DensityBudget
{
 public:
  DensityBudget(std::vector<Rect> windows,
                std::vector<double> used_um2,
                std::vector<double> area_um2,
                double max_density,
                double dbu_per_um)
      : windows_(std::move(windows)),
        area_um2_(std::move(area_um2)),
        used_um2_(std::move(used_um2)),
        max_density_(max_density),
        dbu2_per_um2_(dbu_per_um * dbu_per_um)
  {
  }

  // Charge `r` only if every window it touches stays within max_density.
  bool tryCharge(const Rect& r)
  {
    std::vector<std::pair<size_t, double>> pending;
    bool ok = true;
    forEachOverlap(r, [&](size_t i, double overlap_um2) {
      const double limit = max_density_ * area_um2_[i];
      if (used_um2_[i] + overlap_um2 > limit) {
        ok = false;
        return false;
      }
      pending.emplace_back(i, overlap_um2);
      return true;
    });
    if (!ok) {
      return false;
    }
    for (auto& [i, overlap_um2] : pending) {
      used_um2_[i] += overlap_um2;
    }
    return true;
  }

 private:
  // Calls fn(window_index, overlap_um2) for each window `r` intersects;
  // stops early if fn returns false.
  template <typename Fn>
  void forEachOverlap(const Rect& r, Fn fn)
  {
    for (size_t i = 0; i < windows_.size(); ++i) {
      const Rect& w = windows_[i];
      const int lx = std::max(w.xMin(), r.xMin());
      const int ly = std::max(w.yMin(), r.yMin());
      const int ux = std::min(w.xMax(), r.xMax());
      const int uy = std::min(w.yMax(), r.yMax());
      if (lx >= ux || ly >= uy) {
        continue;
      }
      const double overlap_um2
          = (static_cast<double>(ux - lx) * static_cast<double>(uy - ly))
            / dbu2_per_um2_;
      if (!fn(i, overlap_um2)) {
        return;
      }
    }
  }

  std::vector<Rect> windows_;
  std::vector<double> area_um2_;
  std::vector<double> used_um2_;
  double max_density_;
  double dbu2_per_um2_;
};

static std::pair<int, int> getSpacing(dbTechLayer* layer,
                                      const DensityFillShapesConfig& cfg)
{
  bool is_horiz = layer->getDirection() == dbTechLayerDir::HORIZONTAL;
  int space_x = cfg.space_to_fill;
  int space_y = space_x;
  if (is_horiz) {
    space_x = std::max(space_x, cfg.space_line_end);
  } else {
    space_y = std::max(space_y, cfg.space_line_end);
  }

  return std::make_pair(space_x, space_y);
}

// Two different polygons might be less than min space apart and this
// can lead to DRVs when they are filled independently.  To avoid this
// we exclude a min-space area around each polygon.  This is somewhat
// conservative as we may not actually put a fill where a DRV would be
// caused but is much faster than updating the fill area after every
// polygon is filled.
static void prune(Polygon90Set& fill_area,
                  dbTechLayer* layer,
                  const DensityFillShapesConfig& cfg,
                  Graphics* graphics)
{
  auto [space_x, space_y] = getSpacing(layer, cfg);

  // From Boost on grow_and:
  //   Same as bloating non-overlapping regions and then applying self
  //   intersect to retain only the overlaps introduced by the bloat.
  Polygon90Set pruned(fill_area);
  grow_and(pruned, space_x, space_x, space_y, space_y);
  fill_area -= pruned;
}

// Fill a polygon (area) on the given layer using the given configuration.
// Num_masks is used to color the generated fills.
// filled_area, if given, is an OR of the generated fills without bloating
static void fillPolygon(const Polygon90& area,
                        dbTechLayer* layer,
                        dbBlock* block,
                        const DensityFillShapesConfig& cfg,
                        int num_masks,
                        bool needs_opc,
                        Graphics* graphics,
                        Polygon90Set* filled_area = nullptr,
                        DensityBudget* budget = nullptr)
{
  // Convert the area polygon to a polygon set as we will remove areas
  // filled by one fill shape from consideration by future shapes,
  // which may result in the polygon breaking apart into a set of
  // remaining polygons
  Polygon90Set fill_area;
  fill_area += area;

  bool is_horiz = layer->getDirection() == dbTechLayerDir::HORIZONTAL;
  auto [space_x, space_y] = getSpacing(layer, cfg);

  auto iter = cfg.shapes.begin();
  while (iter != cfg.shapes.end()) {
    auto [w, h] = *iter++;
    // Ensure the longer direction is in the preferred direction
    if ((is_horiz && w < h) || (!is_horiz && h < w)) {
      std::swap(w, h);
    }

    // Use a shrink/bloat cycle to remove any areas that are too small to fill
    // with this fill shape.  A benefit is that it helps break up big polygons
    // which makes it easier to fill them
    Polygon90Set pruned_fill_area = fill_area;

    int ew_sizing = w / 2 - 1;
    int ns_sizing = h / 2 - 1;
    shrink(pruned_fill_area, ew_sizing, ew_sizing, ns_sizing, ns_sizing);
    bloat(pruned_fill_area, ew_sizing, ew_sizing, ns_sizing, ns_sizing);

    // The polygon may break into parts that could be less than min-space
    // apart so prune the result.
    prune(pruned_fill_area, layer, cfg, graphics);

    if (graphics) {
      graphics->status("Fill Area for " + std::to_string(w) + " "
                       + std::to_string(h));
      graphics->drawPolygon90Set(pruned_fill_area);
    }

    Polygon90Set all_iter_fills;
    std::vector<Polygon90> sub_fill_areas;
    pruned_fill_area.get(sub_fill_areas);
    for (auto& sub_fill_area : sub_fill_areas) {
      Rectangle bounds;
      extents(bounds, sub_fill_area);

      // Tile a set of fills to cover the bounds.  (KLayout allows a
      // sweep on the origin of the tile set looking for maximum fill.
      // We could try that in the future.)
      Polygon90Set all_fills;
      for (int x = xl(bounds); x < xh(bounds); x += w + space_x) {
        for (int y = yl(bounds); y < yh(bounds); y += h + space_y) {
          all_fills.insert(makeRect(x, y, x + w, y + h));
        }
      }

      // Intersect fills with the sub area and keep only whole fill shapes
      Polygon90Set fills = all_fills & sub_fill_area;
      keep(fills, w * h, w * h, w - 1, w, h - 1, h);

      Polygon90Set tmp_fills(fills);
      all_iter_fills += bloat(tmp_fills, space_x, space_x, space_y, space_y);

      // Insert fills into the db
      std::vector<Rectangle> polygons;
      fills.get_rectangles(polygons);
      const int num_mask = std::max(num_masks, 1);
      int cnt = 0;
      for (auto& f : polygons) {
        int mask;
        if (num_mask == 1) {
          mask = 0;  // don't write a mask for single mask layers
        } else {
          mask = cnt++ % num_mask + 1;
        }
        auto x_lo = xl(f);
        auto y_lo = yl(f);
        auto x_hi = xh(f);
        auto y_hi = yh(f);
        // Skip shapes that would drive a density window over max_density.
        if (budget && !budget->tryCharge(Rect(x_lo, y_lo, x_hi, y_hi))) {
          continue;
        }
        dbFill::create(block, needs_opc, mask, layer, x_lo, y_lo, x_hi, y_hi);
        if (filled_area) {
          *filled_area += makeRect(x_lo, y_lo, x_hi, y_hi);
        }
      }
    }
    // Remove filled area from use by future shapes
    fill_area -= all_iter_fills;
  }
}

// Fill the given layer
void DensityFill::fillLayer(dbBlock* block,
                            dbTechLayer* layer,
                            const odb::Rect& fill_bounds_rect,
                            const DensityTarget& target,
                            const CouplingRelief& coupling,
                            const DensityCheckResult& measured)
{
  logger_->info(FIN, 3, "Filling layer {}.", layer->getConstName());

  Polygon90Set non_fill = orNonFills(block, layer);

  auto fill_bounds = makeRect(fill_bounds_rect.xMin(),
                              fill_bounds_rect.yMin(),
                              fill_bounds_rect.xMax(),
                              fill_bounds_rect.yMax());

  const DensityFillLayerConfig& cfg = layers_[layer];

  // Density-driven fill.  The windows and their current metal content come
  // from DensityCheck via `measured` -- the same measurement the
  // check_metal_density signoff runs -- so the windows this fill decides to
  // top up are exactly the windows signoff will later judge.
  //
  // Each bound is honoured only if the caller actually supplied it.  With no
  // min there is no notion of "short", so every fillable window stays in play;
  // with no max there is no budget.  Substituting a bound here would be the
  // same fail-safe leak the check guards against, one layer up.
  std::unique_ptr<DensityBudget> budget;
  Polygon90Set deficient_windows;
  const bool restrict_to_deficient = target.enabled && target.hasMin();
  if (target.enabled) {
    const double dbu = layer->getTech()->getDbUnitsPerMicron();
    const std::string layer_name = layer->getName();

    std::vector<Rect> budget_windows;
    std::vector<double> used_um2;
    std::vector<double> area_um2;
    int deficient = 0;
    int total = 0;
    for (const auto& w : measured.windows) {
      if (w.layer != layer_name) {
        continue;  // the measurement covers every routing layer at once
      }
      ++total;
      // Window bounds come back in microns; convert with rounding so the DBU
      // rect is bit-identical to the one the measurement clipped against.
      const Rect bounds(static_cast<int>(std::lround(w.x0 * dbu)),
                        static_cast<int>(std::lround(w.y0 * dbu)),
                        static_cast<int>(std::lround(w.x1 * dbu)),
                        static_cast<int>(std::lround(w.y1 * dbu)));
      budget_windows.push_back(bounds);
      used_um2.push_back(w.filled_area_um2);
      area_um2.push_back(w.window_area_um2);
      if (restrict_to_deficient && w.density < target.min_density) {
        ++deficient;
        deficient_windows += makeRect(
            bounds.xMin(), bounds.yMin(), bounds.xMax(), bounds.yMax());
      }
    }

    if (restrict_to_deficient) {
      logger_->info(FIN,
                    47,
                    "Layer {}: {} of {} density windows below min_density "
                    "{:.4f}.",
                    layer->getConstName(),
                    deficient,
                    total,
                    target.min_density);
      if (deficient == 0) {
        return;  // nothing on this layer is short
      }
    } else {
      logger_->info(FIN,
                    50,
                    "Layer {}: {} density windows, no min_density supplied so "
                    "every fillable window is in play.",
                    layer->getConstName(),
                    total);
    }

    if (target.hasMax()) {
      budget = std::make_unique<DensityBudget>(std::move(budget_windows),
                                               std::move(used_um2),
                                               std::move(area_um2),
                                               target.max_density,
                                               dbu);
    }
  }

  // Coupling relief: hold fill an extra distance away from timing-critical
  // nets so that filling cannot load them with sidewall capacitance.
  Polygon90Set critical_keepout;
  bool has_critical_keepout = false;
  if (!coupling.nets.empty() && coupling.halo > 0) {
    critical_keepout = orNets(block, layer, coupling.nets);
    if (!critical_keepout.empty()) {
      has_critical_keepout = true;
      critical_keepout = critical_keepout + coupling.halo;
      logger_->info(FIN,
                    48,
                    "Layer {}: holding fill {} DBU away from {} critical "
                    "nets.",
                    layer->getConstName(),
                    coupling.halo,
                    coupling.nets.size());
    }
  }

  std::vector<Polygon90> polygons;

  // Do non-OPC fill
  Polygon90Set fill_area
      = fill_bounds - (non_fill + cfg.non_opc.space_to_non_fill);

  if (has_critical_keepout) {
    fill_area -= critical_keepout;
  }

  // Restrict fill to the windows that actually need it.
  if (restrict_to_deficient) {
    fill_area = fill_area & deficient_windows;
  }

  if (graphics_) {
    graphics_->status("Non-OPC Area");
    graphics_->drawPolygon90Set(fill_area);
  }

  prune(fill_area, layer, cfg.non_opc, graphics_.get());

  fill_area.get(polygons);
  logger_->info(FIN, 9, "Filling {} areas with non-OPC fill.", polygons.size());

  Polygon90Set non_opc_fill_area;
  for (auto& polygon : polygons) {
    fillPolygon(polygon,
                layer,
                block,
                cfg.non_opc,
                cfg.num_masks,
                false,
                graphics_.get(),
                &non_opc_fill_area,
                budget.get());
  }
  logger_->info(FIN, 4, "Total fills: {}.", block->getFills().size());

  if (!cfg.has_opc) {
    return;
  }

  Polygon90Set opc_fill_area
      = fill_bounds - (non_fill + cfg.opc.space_to_non_fill)
        - (non_opc_fill_area + cfg.non_opc.space_to_fill);
  if (restrict_to_deficient) {
    opc_fill_area = opc_fill_area & deficient_windows;
  }
  if (has_critical_keepout) {
    opc_fill_area -= critical_keepout;
  }

  if (graphics_) {
    graphics_->status("OPC Area");
    graphics_->drawPolygon90Set(opc_fill_area);
  }

  prune(opc_fill_area, layer, cfg.opc, graphics_.get());

  polygons.clear();
  opc_fill_area.get(polygons);
  logger_->info(FIN, 5, "Filling {} areas with OPC fill.", polygons.size());
  for (auto& polygon : polygons) {
    fillPolygon(polygon,
                layer,
                block,
                cfg.opc,
                cfg.num_masks,
                true,
                graphics_.get(),
                nullptr,
                budget.get());
  }

  logger_->info(FIN, 6, "Total fills: {}.", block->getFills().size());

  if (graphics_) {
    graphics_->status("OPC Area");
    graphics_->drawPolygon90Set(opc_fill_area);
  }
}

// Fill the design according to the given cfg file
void DensityFill::fill(const char* cfg_filename,
                       const odb::Rect& fill_area,
                       const DensityTarget& target,
                       const CouplingRelief& coupling)
{
  dbTech* tech = db_->getTech();
  loadConfig(cfg_filename, tech);

  dbChip* chip = db_->getChip();
  dbBlock* block = chip->getBlock();

  // Measure once, for every routing layer, through the SAME core the
  // check_metal_density signoff uses.  Giving the layer a min band makes the
  // engine classify each window for us, so "which windows are short?" is
  // answered by the signoff rule rather than by a second opinion living here.
  DensityCheckResult measured;
  if (target.enabled) {
    // Hand the engine only the bounds the caller actually supplied; a bound
    // left at -1 stays "no bound" all the way down.  The per-window densities
    // come back either way, so an unlimited window is still measured.
    DensityLimits limits;
    limits.default_min = target.min_density;
    limits.default_max = target.max_density;

    // State the geometry this fill used.  The check prints the same line, so a
    // caller who passes different numbers to the two commands can SEE the
    // mismatch instead of getting a design that fill considers done and
    // signoff measures over other windows.
    const double dbu = tech->getDbUnitsPerMicron();
    logger_->info(FIN,
                  51,
                  "Density-driven fill window / step: {:.4f} / {:.4f} um.",
                  target.window / dbu,
                  (target.step > 0 ? target.step : target.window) / dbu);

    DensityCheck checker(db_, logger_);
    measured = checker.check(
        fill_area, target.window, target.step, limits, /* report_file */ "");

    // Record the geometry so check_metal_density can warn if it is later run
    // over different windows than the fill was driven by.
    if (auto* prop = odb::dbIntProperty::find(block, "fin_density_window")) {
      odb::dbProperty::destroy(prop);
    }
    if (auto* prop = odb::dbIntProperty::find(block, "fin_density_step")) {
      odb::dbProperty::destroy(prop);
    }
    odb::dbIntProperty::create(block, "fin_density_window", target.window);
    odb::dbIntProperty::create(
        block, "fin_density_step", target.step > 0 ? target.step : target.window);
  }

  for (dbTechLayer* layer : tech->getLayers()) {
    auto it = layers_.find(layer);
    if (it == layers_.end()) {
      logger_->warn(FIN, 10, "Skipping layer {}.", layer->getConstName());
      continue;
    }
    fillLayer(block, layer, fill_area, target, coupling, measured);
  }
}

}  // namespace fin
