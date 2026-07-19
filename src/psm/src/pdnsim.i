// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022-2025, The OpenROAD Authors

%include "../../Exception.i"
%{
#include "ord/OpenRoad.hh"
#include "psm/pdnsim.h"
#include "psm/pdn_sizing.h"
#include "psm/decap_opt.h"
#include "sta/Scene.hh"

namespace ord {
psm::PDNSim*
getPDNSim();
}

namespace odb {
class dbNet;
}

using ord::getPDNSim;
using psm::PDNSim;
using sta::Scene;

#if TCL_MAJOR_VERSION < 9 && !defined(Tcl_Size)
  typedef int Tcl_Size;
#endif

%}

// OpenSTA swig rules
%include "tcl/StaTclTypes.i"

%typemap(in) psm::GeneratedSourceType {
  Tcl_Size length;
  const char *arg = Tcl_GetStringFromObj($input, &length);

  if (strcmp(arg, "BUMPS") == 0) {
    $1 = psm::GeneratedSourceType::kBumps;
  } else if (strcmp(arg, "FULL") == 0) {
    $1 = psm::GeneratedSourceType::kFull;
  } else if (strcmp(arg, "STRAPS") == 0) {
    $1 = psm::GeneratedSourceType::kStraps;
  } else {
    $1 = psm::GeneratedSourceType::kBumps;
  }
}

%inline %{


void 
set_net_voltage_cmd(odb::dbNet* net, Scene* corner, double voltage)
{
  PDNSim* pdnsim = getPDNSim();
  pdnsim->setNetVoltage(net, corner, voltage);
}

void
analyze_power_grid_cmd(odb::dbNet* net, Scene* corner, psm::GeneratedSourceType type, const char* error_file, bool reuse_solution, bool enable_em, const char* em_file, const char* voltage_file, const char* voltage_source_file)
{
  PDNSim* pdnsim = getPDNSim();
  pdnsim->analyzePowerGrid(net, corner, type, voltage_file, reuse_solution, enable_em, em_file, error_file, voltage_source_file);
}

void
analyze_power_grid_dynamic_cmd(odb::dbNet* net, Scene* corner, psm::GeneratedSourceType type, const char* error_file, const char* voltage_file, const char* voltage_source_file, double period, int steps, int num_periods, double node_cap, double total_cap, double decap_cap, double current_duty, bool phase_spread, const char* current_profile, const char* vectored_profile, double package_r, double package_l)
{
  PDNSim* pdnsim = getPDNSim();
  pdnsim->analyzePowerGridDynamic(net, corner, type, voltage_file, error_file, voltage_source_file, period, steps, num_periods, node_cap, total_cap, decap_cap, current_duty, phase_spread, current_profile, vectored_profile, package_r, package_l);
}

int
check_current_density_cmd(odb::dbNet* net, Scene* corner, psm::GeneratedSourceType type, const char* voltage_source_file, bool reuse_solution, double default_limit, const char* limits_file, const char* report_file)
{
  PDNSim* pdnsim = getPDNSim();
  const psm::EMSignoffResult res = pdnsim->checkCurrentDensity(net, corner, type, voltage_source_file, reuse_solution, default_limit, limits_file, report_file);
  return static_cast<int>(res.violations);
}

int
check_signal_em_cmd(Scene* corner, double supply_voltage, double toggle_rate, const char* activity_file, double avg_limit, double rms_limit, double peak_limit, const char* limits_file, const char* report_file)
{
  PDNSim* pdnsim = getPDNSim();
  const psm::SignalEMResult res = pdnsim->checkSignalEM(corner, supply_voltage, toggle_rate, activity_file, avg_limit, rms_limit, peak_limit, limits_file, report_file);
  return static_cast<int>(res.violations);
}

void
add_decap_master(odb::dbMaster *master, float cap)
{
  PDNSim* pdnsim = getPDNSim();
  pdnsim->addDecapMaster(master, cap);
}

void
insert_decap_cmd(const float target, const char* net_name)
{
  PDNSim* pdnsim = getPDNSim();
  pdnsim->insertDecapCells(target, net_name);
}

bool
check_connectivity_cmd(odb::dbNet* net, bool floorplanning, const char* error_file, bool dont_require_bterm)
{
  PDNSim* pdnsim = getPDNSim();
  return pdnsim->checkConnectivity(net, floorplanning, error_file, !dont_require_bterm);
}

void
write_spice_file_cmd(odb::dbNet* net, Scene* corner, psm::GeneratedSourceType type, const char* file, const char* voltage_source_file)
{
  PDNSim* pdnsim = getPDNSim();
  return pdnsim->writeSpiceNetwork(net, corner, type, file, voltage_source_file);
}

void set_debug_gui(bool enable)
{
  PDNSim* pdnsim = getPDNSim();
  pdnsim->setDebugGui(enable);
}

void clear_solvers()
{
  PDNSim* pdnsim = getPDNSim();
  pdnsim->clearSolvers();
}

void set_source_settings(int bump_dx, int bump_dy, int bump_size, int bump_interval, int track_pitch, float resistance)
{
  PDNSim::GeneratedSourceSettings settings;
  settings.bump_dx = bump_dx;
  settings.bump_dy = bump_dy;
  settings.bump_size = bump_size;
  settings.bump_interval = bump_interval;
  settings.strap_track_pitch = track_pitch;
  settings.resistance = resistance;

  PDNSim* pdnsim = getPDNSim();
  pdnsim->setGeneratedSourceSettings(settings);
}

void
set_inst_power(odb::dbInst* inst, Scene* corner, float power)
{
  PDNSim* pdnsim = getPDNSim();
  pdnsim->setInstPower(inst, corner, power);
}

// Worst measured static IR drop [V] on `net` from the last analyze_power_grid.
double
get_worst_ir_drop_cmd(odb::dbNet* net)
{
  PDNSim* pdnsim = getPDNSim();
  return pdnsim->getWorstIRDrop(net);
}

// Analysis-driven strap sizing.  Given the droop PSM actually measured at the
// current strap width and the irreducible package/bump floor, return the strap
// width [m] that would meet the target droop.  Advisory: this computes the
// required geometry, it does not mutate the grid.  Returns +Inf when the
// target is at or below the package floor (unreachable by widening on-die
// metal); the TCL wrapper compares the result against max_width to detect the
// second infeasibility mode.  Each scalar of the PdnSizingResult is exposed by
// its own accessor so the SWIG boundary stays a plain double.
double
size_pdn_required_width_cmd(double measured_droop, double current_width, double irreducible_droop, double target_droop, double min_width, double max_width)
{
  return psm::resizeFromMeasuredDroop(measured_droop, current_width, irreducible_droop, target_droop, min_width, max_width).required_width;
}

double
size_pdn_achieved_droop_cmd(double measured_droop, double current_width, double irreducible_droop, double target_droop, double min_width, double max_width)
{
  return psm::resizeFromMeasuredDroop(measured_droop, current_width, irreducible_droop, target_droop, min_width, max_width).achieved_droop;
}

// Droop-driven decap sizing: the decoupling capacitance [F] that holds the
// event droop to the target.  Returns 0 when the DC droop already fits, +Inf
// when the budget is non-positive.
double
size_decap_required_cap_cmd(double peak_current, double event_duration, double effective_resistance, double target_droop)
{
  psm::DecapSizingSpec spec;
  spec.peak_current = peak_current;
  spec.event_duration = event_duration;
  spec.effective_resistance = effective_resistance;
  spec.allowed_droop = target_droop;
  return psm::sizeDecapForDroop(spec).required_cap;
}

// Conservative R-free charge bound I*T/D [F] for the same event.
double
decap_charge_bound_cmd(double peak_current, double event_duration, double target_droop)
{
  return psm::chargeBoundDecap(peak_current, event_duration, target_droop);
}

%} // inline
