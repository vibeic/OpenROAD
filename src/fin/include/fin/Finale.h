// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

#pragma once

#include <string>

#include "fin/density_check.h"
#include "odb/db.h"
#include "odb/geom.h"
#include "utl/Logger.h"

namespace fin {

////////////////////////////////////////////////////////////////

class Finale
{
 public:
  Finale(odb::dbDatabase* db, utl::Logger* logger);

  void densityFill(const char* rules_filename, const odb::Rect& fill_area);

  // Metal density check (FL1): measure per-window, per-layer metal density over
  // check_area and flag every window outside the per-layer [min, max] band.
  // window/step are in DBU; a non-positive step means step = window.
  DensityCheckResult checkDensity(const odb::Rect& check_area,
                                  int window,
                                  int step,
                                  const DensityLimits& limits,
                                  const std::string& report_file);

  void setDebug();

 private:
  odb::dbDatabase* db_ = nullptr;
  utl::Logger* logger_ = nullptr;
  bool debug_ = false;
};

}  // namespace fin
