// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

#pragma once

#include <map>
#include <memory>
#include <vector>

#include "boost/property_tree/json_parser.hpp"
#include "fin/density_check.h"
#include "odb/PtrSetMap.h"
#include "odb/db.h"
#include "odb/geom.h"
#include "utl/Logger.h"

namespace fin {

struct DensityFillLayerConfig;
class DensityBudget;
class Graphics;

// Per-layer density targets and window geometry that steer fill insertion.
// The windows themselves are measured by DensityCheck (the one measurement
// core, shared with the check_metal_density signoff command) so that fill and
// signoff can never disagree about what a window is or what it contains.
struct DensityTarget
{
  bool enabled = false;
  int window = 0;  // DBU
  int step = 0;    // DBU
  // A NEGATIVE bound means "not supplied", the same convention the density
  // engine uses.  Fill must never substitute a bound the caller did not give:
  // inventing min = 0.0 makes every window look satisfied (fill silently does
  // nothing), and inventing max = 1.0 removes the overshoot cap entirely.
  double min_density = -1.0;
  double max_density = -1.0;

  bool hasMin() const { return min_density >= 0.0; }
  bool hasMax() const { return max_density >= 0.0; }
};

// Extra keep-out held around coupling-sensitive nets while filling.  Fill
// shapes add sidewall capacitance to whatever they run beside, so nets that
// cannot absorb it are given a halo larger than the plain fill spacing.
struct CouplingRelief
{
  odb::PtrSet<odb::dbNet> nets;
  int halo = 0;  // DBU
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

 private:
  void loadConfig(const char* cfg_filename, odb::dbTech* tech);
  void readAndExpandLayers(odb::dbTech* tech,
                           boost::property_tree::ptree& tree);
  void fillLayer(odb::dbBlock* block,
                 odb::dbTechLayer* layer,
                 const odb::Rect& fill_bounds,
                 const DensityTarget& target,
                 const CouplingRelief& coupling,
                 const DensityCheckResult& measured);

  odb::dbDatabase* db_;
  odb::PtrMap<odb::dbTechLayer, DensityFillLayerConfig> layers_;
  std::unique_ptr<Graphics> graphics_;
  utl::Logger* logger_;
};

}  // namespace fin
