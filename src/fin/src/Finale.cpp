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

void Finale::densityFill(const char* rules_filename, const odb::Rect& fill_area)
{
  DensityFill filler(db_, logger_, debug_);
  filler.fill(rules_filename, fill_area);
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
