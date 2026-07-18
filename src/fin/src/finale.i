// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

%{
#include <fstream>
#include <sstream>
#include <string>

#include "ord/OpenRoad.hh"
#include "fin/Finale.h"
#include "utl/Logger.h"

%}

%include "../../Exception.i"

%inline %{

void
set_density_fill_debug_cmd()
{
  auto *finale = ord::OpenRoad::openRoad()->getFinale();
  finale->setDebug();
}

void
density_fill_cmd(const char* rules_filename,
                 const odb::Rect& fill_area)
{
  auto *finale = ord::OpenRoad::openRoad()->getFinale();
  finale->densityFill(rules_filename, fill_area);
}

int
check_metal_density_cmd(const odb::Rect& check_area,
                        int window,
                        int step,
                        double min_density,
                        double max_density,
                        const char* limits_file,
                        const char* report_file)
{
  auto *finale = ord::OpenRoad::openRoad()->getFinale();
  auto *logger = ord::OpenRoad::openRoad()->getLogger();

  fin::DensityLimits limits;
  limits.default_min = min_density;
  limits.default_max = max_density;
  if (limits_file != nullptr && limits_file[0] != '\0') {
    std::ifstream lf(limits_file);
    if (!lf) {
      logger->error(utl::FIN, 23, "Unable to open density limits file {}", limits_file);
    }
    std::string line;
    int nrows = 0;
    while (std::getline(lf, line)) {
      const auto hash = line.find('#');
      if (hash != std::string::npos) {
        line = line.substr(0, hash);
      }
      std::istringstream ss(line);
      std::string layer;
      double lo = -1.0, hi = -1.0;
      if (ss >> layer >> lo >> hi) {
        limits.per_layer[layer] = {lo, hi};
        nrows++;
      }
    }
    logger->info(utl::FIN, 24, "Loaded {} per-layer density band(s) from {}.", nrows, limits_file);
  }
  if (limits.empty()) {
    logger->warn(utl::FIN, 25,
                 "No metal density band supplied (-min_density / -max_density / "
                 "-limits_file); every window is reported as NO_LIMIT and the "
                 "check cannot fail.  Provide the PDK per-layer band for a real "
                 "signoff.");
  }

  const fin::DensityCheckResult res
      = finale->checkDensity(check_area, window, step, limits, report_file);
  return static_cast<int>(res.violations);
}

%} // inline

