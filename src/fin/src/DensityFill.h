// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

#pragma once

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "boost/property_tree/json_parser.hpp"
#include "odb/PtrSetMap.h"
#include "odb/db.h"
#include "odb/geom.h"
#include "utl/Logger.h"

namespace fin {

struct DensityFillLayerConfig;
class DensityBudget;
class Graphics;

// Per-layer density targets and window geometry used both to steer fill
// insertion and to audit the result.  A window is a fixed-size square that
// slides over the area in `step` increments; only windows lying entirely
// inside the area are evaluated (if the area is smaller than one window a
// single window clamped to the area is used instead).
struct DensityTarget
{
  bool enabled = false;
  int window = 0;  // DBU
  int step = 0;    // DBU
  double min_density = 0.0;
  double max_density = 1.0;
};

// Extra keep-out held around coupling-sensitive nets while filling.  Fill
// shapes add sidewall capacitance to whatever they run beside, so nets that
// cannot absorb it are given a halo larger than the plain fill spacing.
struct CouplingRelief
{
  std::set<odb::dbNet*> nets;
  int halo = 0;  // DBU
};

// The measured metal density of one window.
struct DensityWindow
{
  odb::Rect bounds;
  int64_t metal_area = 0;   // DBU^2 of metal inside the window
  int64_t window_area = 0;  // DBU^2 of the window itself
  double density = 0.0;     // metal_area / window_area
};

////////////////////////////////////////////////////////////////

// This class inserts metal fill to meet density rules according
// to the specification in a user given JSON file.
class DensityFill
{
 public:
  DensityFill(odb::dbDatabase* db, utl::Logger* logger, bool debug);
  ~DensityFill();

  DensityFill(const DensityFill&) = delete;
  DensityFill& operator=(const DensityFill&) = delete;
  DensityFill(const DensityFill&&) = delete;
  DensityFill& operator=(const DensityFill&&) = delete;

  void fill(const char* cfg_filename,
            const odb::Rect& fill_area,
            const DensityTarget& target,
            const CouplingRelief& coupling);

  // Measure the metal density of `layer` over `area` in sliding windows.
  // Includes routing, special wires, instance shapes and any existing fill.
  std::vector<DensityWindow> measureDensity(odb::dbTechLayer* layer,
                                            const odb::Rect& area,
                                            int window,
                                            int step);

  // Measure every layer that has a routing direction and report windows
  // outside [min_density, max_density].  Returns the violation count.
  int checkDensity(const odb::Rect& area,
                   int window,
                   int step,
                   double min_density,
                   double max_density,
                   odb::dbTechLayer* only_layer);

 private:
  void loadConfig(const char* cfg_filename, odb::dbTech* tech);
  void readAndExpandLayers(odb::dbTech* tech,
                           boost::property_tree::ptree& tree);
  void fillLayer(odb::dbBlock* block,
                 odb::dbTechLayer* layer,
                 const odb::Rect& fill_bounds,
                 const DensityTarget& target,
                 const CouplingRelief& coupling);

  // Enumerate the window rectangles used by measurement and budgeting.
  static std::vector<odb::Rect> windowRects(const odb::Rect& area,
                                            int window,
                                            int step);

  odb::dbDatabase* db_;
  odb::PtrMap<odb::dbTechLayer, DensityFillLayerConfig> layers_;
  std::unique_ptr<Graphics> graphics_;
  utl::Logger* logger_;
};

}  // namespace fin
