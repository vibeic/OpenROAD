// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

#include "fin/Finale.h"

#include "DensityCheck.h"
#include "DensityFill.h"
#include "odb/db.h"
#include "odb/geom.h"
#include "utl/Logger.h"

namespace fin {

////////////////////////////////////////////////////////////////

Finale::Finale(odb::dbDatabase* db, utl::Logger* logger)
    : db_(db), logger_(logger)
{
}

void Finale::setDebug()
{
  debug_ = true;
}

void Finale::densityFill(const char* rules_filename,
                         const odb::Rect& fill_area)
{
  densityFill(rules_filename, fill_area, false, 0, 0, 0.0, 1.0, {}, 0);
}

void Finale::densityFill(const char* rules_filename,
                         const odb::Rect& fill_area,
                         bool density_target,
                         int window,
                         int step,
                         double min_density,
                         double max_density,
                         const odb::PtrSet<odb::dbNet>& critical_nets,
                         int critical_halo)
{
  DensityTarget target;
  target.enabled = density_target;
  target.window = window;
  target.step = step;
  target.min_density = min_density;
  target.max_density = max_density;

  CouplingRelief coupling;
  coupling.nets = critical_nets;
  coupling.halo = critical_halo;

  DensityFill filler(db_, logger_, debug_);
  filler.fill(rules_filename, fill_area, target, coupling);
}

DensityCheckResult Finale::checkDensity(const odb::Rect& check_area,
                                        int window,
                                        int step,
                                        const DensityLimits& limits,
                                        const std::string& report_file)
{
  DensityCheck checker(db_, logger_);
  return checker.check(check_area, window, step, limits, report_file);
}

}  // namespace fin
