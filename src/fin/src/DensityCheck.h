// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, The OpenROAD Authors

#pragma once

#include <string>

#include "fin/density_check.h"
#include "odb/db.h"
#include "odb/geom.h"
#include "utl/Logger.h"

namespace fin {

// Measures per-window, per-layer metal density over a design and hands the
// measurement to the pure rule engine in fin/density_check.h.
//
// The measured area is the UNION of every piece of metal on the layer -- signal
// routing, special (power) routing, instance pins and OBS, and any dummy fill
// already inserted -- clipped to the window, so overlapping shapes are counted
// once.  That is the same quantity a foundry density deck measures.
class DensityCheck
{
 public:
  DensityCheck(odb::dbDatabase* db, utl::Logger* logger);

  // window/step are in DBU.  A step <= 0 means "step = window" (tiled, no
  // overlap); a positive step smaller than window gives the sliding window the
  // stricter density rules require.
  DensityCheckResult check(const odb::Rect& area,
                           int window,
                           int step,
                           const DensityLimits& limits,
                           const std::string& report_file);

 private:
  odb::dbDatabase* db_ = nullptr;
  utl::Logger* logger_ = nullptr;
};

}  // namespace fin
