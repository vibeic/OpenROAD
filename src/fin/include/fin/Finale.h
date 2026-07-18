// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

#pragma once

#include <set>

#include "odb/db.h"
#include "odb/geom.h"
#include "utl/Logger.h"

namespace fin {

////////////////////////////////////////////////////////////////

class Finale
{
 public:
  Finale(odb::dbDatabase* db, utl::Logger* logger);

  void densityFill(const char* rules_filename,
                   const odb::Rect& fill_area,
                   bool density_target,
                   int window,
                   int step,
                   double min_density,
                   double max_density,
                   const std::set<odb::dbNet*>& critical_nets,
                   int critical_halo);

  // Report metal-density windows outside [min_density, max_density].
  // Returns the number of violating windows.
  int checkDensity(const odb::Rect& area,
                   int window,
                   int step,
                   double min_density,
                   double max_density,
                   const char* layer_name);

  void setDebug();

 private:
  odb::dbDatabase* db_ = nullptr;
  utl::Logger* logger_ = nullptr;
  bool debug_ = false;
};

}  // namespace fin
