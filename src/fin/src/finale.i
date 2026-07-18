// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

%{
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
            utl::FIN, 25, "Net {} not found.", name);
      }
    }
    pos = end + 1;
  }

  finale->densityFill(rules_filename, fill_area, density_target,
                      window, step, min_density, max_density,
                      critical_nets, critical_halo);
}

int
check_metal_density_cmd(const odb::Rect& area,
                        int window,
                        int step,
                        double min_density,
                        double max_density,
                        const char* layer_name)
{
  auto *finale = ord::OpenRoad::openRoad()->getFinale();
  return finale->checkDensity(area, window, step, min_density,
                              max_density, layer_name);
}

%} // inline

