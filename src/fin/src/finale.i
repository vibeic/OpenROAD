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

  // Window geometry is caller-supplied on BOTH commands, so a check can be
  // run over different windows than the fill was driven over -- or, more
  // insidiously, over exactly the same ones.
  //
  // DensityBudget rejects any fill shape that would push a budgeted window
  // past max_density, so after the fill EVERY budgeted window is within the
  // cap BY CONSTRUCTION.  Re-checking those same windows against that same cap
  // therefore cannot fail: it reads back the constraint instead of testing it.
  // Divergent geometry is the case that can actually find something -- on the
  // density_geometry_mismatch fixture, the grid the fill used measures
  // 0.399998 and PASSES while an offset grid measures 0.400200 and FAILS.
  auto *block = ord::OpenRoad::openRoad()->getDb()->getChip()->getBlock();
  auto *fill_w = odb::dbIntProperty::find(block, "fin_density_window");
  auto *fill_s = odb::dbIntProperty::find(block, "fin_density_step");
  auto *fill_m = odb::dbIntProperty::find(block, "fin_density_max_ppm");
  if (fill_w != nullptr && fill_s != nullptr) {
    const int used_step = step > 0 ? step : window;
    const double dbu
      = ord::OpenRoad::openRoad()->getDb()->getTech()->getDbUnitsPerMicron();
    const bool same_geometry
      = fill_w->getValue() == window && fill_s->getValue() == used_step;
    if (!same_geometry) {
      logger->warn(utl::FIN, 52,
                   "Checking density over window/step {:.4f}/{:.4f} um but the "
                   "last density_fill was driven over {:.4f}/{:.4f} um; the two "
                   "measure different windows.",
                   window / dbu, used_step / dbu,
                   fill_w->getValue() / dbu, fill_s->getValue() / dbu);
    } else if (fill_m != nullptr && fill_m->getValue() >= 0) {
      logger->warn(utl::FIN, 53,
                   "This check measures the same windows density_fill's budget "
                   "already constrained to {:.4f}, so it confirms the budget "
                   "rather than independently verifying density. Check at a "
                   "finer step to reach windows that straddle two filled "
                   "regions.",
                   fill_m->getValue() / 1e6);
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

