// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, The OpenROAD Authors

#include "DensityCheck.h"

#include <array>
#include <fstream>
#include <string>
#include <vector>

#include "odb/db.h"
#include "odb/dbShape.h"
#include "polygon.h"
#include "utl/Logger.h"

namespace fin {

using odb::dbBlock;
using odb::dbFill;
using odb::dbInstShapeItr;
using odb::dbShape;
using odb::dbTechLayer;
using odb::dbTechLayerType;
using odb::dbVia;
using odb::dbWire;
using odb::dbWireShapeItr;
using odb::Rect;

namespace {

Polygon90 makeRect(int x_lo, int y_lo, int x_hi, int y_hi)
{
  using Pt = Polygon90::point_type;
  std::array<Pt, 4> pts
      = {Pt(x_lo, y_lo), Pt(x_hi, y_lo), Pt(x_hi, y_hi), Pt(x_lo, y_hi)};

  Polygon90 poly;
  poly.set(pts.begin(), pts.end());
  return poly;
}

// Add whatever part of `shape` lies on `layer` to the set.
void insertShape(const dbShape& shape,
                 Polygon90Set& polygon_set,
                 dbTechLayer* layer)
{
  const auto type = shape.getType();
  switch (type) {
    case dbShape::VIA:
    case dbShape::TECH_VIA: {
      // Only the via's metal landing pads contribute to metal density.
      std::vector<dbShape> boxes;
      dbShape::getViaBoxes(shape, boxes);
      for (const auto& box : boxes) {
        if (box.getTechLayer() == layer) {
          polygon_set.insert(
              makeRect(box.xMin(), box.yMin(), box.xMax(), box.yMax()));
        }
      }
      break;
    }
    case dbShape::SEGMENT:
    case dbShape::TECH_VIA_BOX:
    case dbShape::VIA_BOX:
      if (shape.getTechLayer() == layer) {
        polygon_set.insert(
            makeRect(shape.xMin(), shape.yMin(), shape.xMax(), shape.yMax()));
      }
      break;
  }
}

// The union of every piece of metal on `layer`: signal routing, special
// routing, instance pins & OBS, and dummy fill.  Fill is INCLUDED (unlike
// DensityFill's orNonFills, which deliberately excludes it) because the density
// a foundry deck measures is the density after fill.
Polygon90Set orAllMetal(dbBlock* block, dbTechLayer* layer)
{
  Polygon90Set metal;
  dbShape shape;

  dbWireShapeItr shapes;
  for (auto* net : block->getNets()) {
    dbWire* wire = net->getWire();
    if (wire != nullptr) {
      for (shapes.begin(wire); shapes.next(shape);) {
        insertShape(shape, metal, layer);
      }
    }
    std::vector<dbShape> via_shapes;
    for (auto* swire : net->getSWires()) {
      for (auto* sbox : swire->getWires()) {
        if (sbox->isVia()) {
          dbVia* via = sbox->getBlockVia();
          const Rect rect = sbox->getBox();
          dbShape via_shape;
          via_shape.setVia(via, rect);
          dbShape::getViaBoxes(via_shape, via_shapes);
          for (const auto& vs : via_shapes) {
            insertShape(vs, metal, layer);
          }
        } else if (sbox->getTechLayer() == layer) {
          metal.insert(
              makeRect(sbox->xMin(), sbox->yMin(), sbox->xMax(), sbox->yMax()));
        }
      }
    }
  }

  dbInstShapeItr insts(/* expand_vias */ false);
  for (auto* inst : block->getInsts()) {
    for (insts.begin(inst, dbInstShapeItr::ALL); insts.next(shape);) {
      insertShape(shape, metal, layer);
    }
  }

  for (auto* fill : block->getFills()) {
    if (fill->getTechLayer() != layer) {
      continue;
    }
    Rect rect;
    fill->getRect(rect);
    metal.insert(
        makeRect(rect.xMin(), rect.yMin(), rect.xMax(), rect.yMax()));
  }

  return metal;
}

}  // namespace

DensityCheck::DensityCheck(odb::dbDatabase* db, utl::Logger* logger)
    : db_(db), logger_(logger)
{
}

DensityCheckResult DensityCheck::check(const Rect& area,
                                       int window,
                                       int step,
                                       const DensityLimits& limits,
                                       const std::string& report_file)
{
  dbBlock* block = db_->getChip()->getBlock();
  const double dbu = block->getDbUnitsPerMicron();
  const double dbu2 = dbu * dbu;

  if (window <= 0) {
    logger_->error(utl::FIN, 20, "Density check window must be positive.");
  }
  if (step <= 0) {
    step = window;
  }

  std::vector<DensityWindow> measured;
  for (dbTechLayer* layer : db_->getTech()->getLayers()) {
    if (layer->getType() != dbTechLayerType::ROUTING) {
      continue;  // density rules are written against routing metal
    }
    const Polygon90Set metal = orAllMetal(block, layer);
    const std::string layer_name = layer->getName();

    for (int y = area.yMin(); y < area.yMax(); y += step) {
      for (int x = area.xMin(); x < area.xMax(); x += step) {
        // Clip the window to the check area so an edge window is measured
        // against its OWN (smaller) area rather than a full-size one.
        const int x1 = std::min(x + window, area.xMax());
        const int y1 = std::min(y + window, area.yMax());
        if (x1 <= x || y1 <= y) {
          continue;
        }

        Polygon90Set clipped = metal & makeRect(x, y, x1, y1);
        const double filled_dbu2
            = static_cast<double>(boost::polygon::area(clipped));

        DensityWindow w;
        w.layer = layer_name;
        w.filled_area_um2 = filled_dbu2 / dbu2;
        w.window_area_um2 = (static_cast<double>(x1 - x)
                             * static_cast<double>(y1 - y))
                            / dbu2;
        w.x0 = x / dbu;
        w.y0 = y / dbu;
        w.x1 = x1 / dbu;
        w.y1 = y1 / dbu;
        measured.push_back(std::move(w));
      }
    }
  }

  const DensityCheckResult results = classifyDensity(measured, limits);

  if (!report_file.empty()) {
    std::ofstream report(report_file);
    if (!report) {
      logger_->error(
          utl::FIN, 21, "Unable to open {} to write density report", report_file);
    }
    report << "Layer,X0,Y0,X1,Y1,FilledArea(um^2),WindowArea(um^2),Density,"
              "Min,Max,Status\n";
    for (const auto& w : results.windows) {
      const char* status = (w.min_limit < 0.0 && w.max_limit < 0.0) ? "NO_LIMIT"
                           : w.violated_min                        ? "UNDER"
                           : w.violated_max                        ? "OVER"
                                                                   : "OK";
      report << w.layer << "," << fmt::format("{:.4f}", w.x0) << ","
             << fmt::format("{:.4f}", w.y0) << ","
             << fmt::format("{:.4f}", w.x1) << ","
             << fmt::format("{:.4f}", w.y1) << ","
             << fmt::format("{:.6f}", w.filled_area_um2) << ","
             << fmt::format("{:.6f}", w.window_area_um2) << ","
             << fmt::format("{:.6f}", w.density) << ","
             << fmt::format("{:.4f}", w.min_limit) << ","
             << fmt::format("{:.4f}", w.max_limit) << "," << status << '\n';
    }
  }

  logger_->report("########## Metal density check ##########");
  logger_->report("Window / step       : {:.4f} / {:.4f} um",
                  window / dbu,
                  step / dbu);
  logger_->report("Windows measured    : {}", results.checked);
  logger_->report("With density band   : {}", results.limited);
  logger_->report("No band (skipped)   : {}", results.skipped_no_limit);
  if (results.any_limited) {
    logger_->report("Lowest density      : {:.6f} (layer {})",
                    results.min_density,
                    results.min_density_layer);
    logger_->report("Highest density     : {:.6f} (layer {})",
                    results.max_density,
                    results.max_density_layer);
  }
  logger_->report("Under-density       : {}", results.violations_min);
  logger_->report("Over-density        : {}", results.violations_max);
  logger_->report("Violations          : {}", results.violations);
  logger_->report("Verdict             : {}", results.pass() ? "PASS" : "FAIL");
  logger_->report("########################################");

  if (results.violations > 0) {
    logger_->warn(utl::FIN,
                  22,
                  "Metal density check FAILED: {} window(s) outside the "
                  "per-layer density band ({} under, {} over).",
                  results.violations,
                  results.violations_min,
                  results.violations_max);
  }

  logger_->metric("design__metal_density__violations", results.violations);
  logger_->metric("design__metal_density__min", results.min_density);
  logger_->metric("design__metal_density__max", results.max_density);

  return results;
}

}  // namespace fin
