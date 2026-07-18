// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

%{
#include <fstream>
#include <sstream>
#include <string>

#include "ord/OpenRoad.hh"
#include "fin/Finale.h"
#include "odb/PtrSetMap.h"
#include "odb/db.h"
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
                 const odb::Rect& fill_area,
                 bool density_target,
                 int window,
                 int step,
                 double min_density,
                 double max_density,
                 const char* critical_net_names,
                 int critical_halo)
{
  auto *finale = ord::OpenRoad::openRoad()->getFinale();
  auto *block = ord::OpenRoad::openRoad()->getDb()->getChip()->getBlock();

  // critical_net_names is a space separated list; an empty string disables
  // coupling relief entirely.
  odb::PtrSet<odb::dbNet> critical_nets;
  std::string names(critical_net_names ? critical_net_names : "");
  size_t pos = 0;
  while (pos < names.size()) {
    size_t end = names.find(' ', pos);
    if (end == std::string::npos) {
      end = names.size();
    }
    std::string name = names.substr(pos, end - pos);
    if (!name.empty()) {
      odb::dbNet* net = block->findNet(name.c_str());
      if (net) {
        critical_nets.insert(net);
      } else {
        ord::OpenRoad::openRoad()->getLogger()->error(
            utl::FIN, 49, "Net {} not found.", name);
      }
    }
    pos = end + 1;
  }

  finale->densityFill(rules_filename, fill_area, density_target,
                      window, step, min_density, max_density,
                      critical_nets, critical_halo);
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

  // Window geometry is caller-supplied on BOTH commands, so a caller can drive
  // fill over one window and then measure over another -- fill would consider
  // the design done while signoff judges different geometry.  density_fill
  // records what it used; if that disagrees with what is being checked now,
  // say so loudly rather than let the two quietly diverge.
  auto *block = ord::OpenRoad::openRoad()->getDb()->getChip()->getBlock();
  auto *fill_w = odb::dbIntProperty::find(block, "fin_density_window");
  auto *fill_s = odb::dbIntProperty::find(block, "fin_density_step");
  if (fill_w != nullptr && fill_s != nullptr) {
    const int used_step = step > 0 ? step : window;
    if (fill_w->getValue() != window || fill_s->getValue() != used_step) {
      const double dbu
        = ord::OpenRoad::openRoad()->getDb()->getTech()->getDbUnitsPerMicron();
      logger->warn(utl::FIN, 52,
                   "Checking density over window/step {:.4f}/{:.4f} um but the "
                   "last density_fill was driven over {:.4f}/{:.4f} um; fill "
                   "satisfied different windows than this check measures.",
                   window / dbu, used_step / dbu,
                   fill_w->getValue() / dbu, fill_s->getValue() / dbu);
    }
  }

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

