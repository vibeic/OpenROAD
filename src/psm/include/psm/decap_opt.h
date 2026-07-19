// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#pragma once

// Droop-driven decoupling-capacitance sizing and placement.
//
// PSM already has `insert_decap`, but it is driven by a capacitance TARGET the
// user has to invent ("give me 10 pF of decap"), not by the droop budget the
// designer actually signs off against.  This header supplies the missing
// inverse: how much decap a droop budget demands, and where it has to go.
//
// PHYSICS
// -------
// Take one aggregated die node fed from the ideal supply through the effective
// resistance R (on-die grid + package), carrying the on-die decap C, and hit
// with a switching-current step of amplitude I lasting T seconds.  Writing the
// droop d(t) = Vdd - v(t), KCL at the node gives
//     C * dd/dt + d / R = I,        d(0) = 0,
// whose solution is the first-order step response
//     d(t) = I * R * (1 - e^{-t / (R*C)}).
// d(t) rises monotonically, so the worst droop over the event is at t = T:
//     d_max = I * R * (1 - e^{-T / (R*C)}).
//
// Inverting for the capacitance that holds d_max down to a budget D:
//     1 - e^{-T/(R*C)} = D / (I*R)
//     T / (R*C)        = -ln(1 - D / (I*R))
//     C_req            = T / ( R * ln( I*R / (I*R - D) ) ).
//
// Two limits give the sanity anchors the unit test gates against:
//
//   * D >= I*R.  The DC droop alone already fits the budget, so the event
//     needs no decap at all: C_req = 0.
//
//   * R -> infinity (supply effectively disconnected during the event).  Then
//     D/(I*R) -> 0 and ln(1/(1-x)) -> x, so
//         C_req -> T / (R * D/(I*R)) = I * T / D,
//     which is just charge conservation Q = I*T = C*D.  Because a real finite
//     R lets the supply deliver part of the charge, the exact form always
//     needs LESS capacitance than this: C_req < I*T/D for every finite R.
//     That strict inequality is a gate, not a comment -- an implementation
//     that mixed the two up would violate it.
//
// The charge bound I*T/D is therefore the safe, R-free number to use when the
// per-region resistance is not separately known, which is the case for the
// allocator below (PSM reports a droop and a current per region, not an R).

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace psm {

// Closed-form droop of the single-node RC model at time t.
//   i : step-current amplitude [A]
//   r : effective resistance to the ideal supply [ohm]
//   c : decoupling capacitance on the node [F]
//   t : time since the event started [s]
inline double rcDroopAtTime(double i, double r, double c, double t)
{
  if (c <= 0.0) {
    // No capacitance: the node sits at its DC operating point immediately.
    return i * r;
  }
  return i * r * (1.0 - std::exp(-t / (r * c)));
}

// Charge-conservation bound: the capacitance that supplies the whole event
// charge I*T within a droop of D, with no help from the supply.  This is the
// R -> infinity limit of the exact form and is always conservative.
inline double chargeBoundDecap(double peak_current,
                               double event_duration,
                               double allowed_droop)
{
  if (allowed_droop <= 0.0) {
    return std::numeric_limits<double>::infinity();
  }
  return peak_current * event_duration / allowed_droop;
}

// What the caller knows about the droop event to be decoupled.
struct DecapSizingSpec
{
  double peak_current = 0.0;           // I [A] drawn during the event
  double event_duration = 0.0;         // T [s] the event lasts
  double effective_resistance = 0.0;   // R [ohm] to the ideal supply
  double allowed_droop = 0.0;          // D [V] budget
};

struct DecapSizingResult
{
  bool feasible = false;
  double required_cap = 0.0;      // [F] exact first-order RC answer
  double charge_bound_cap = 0.0;  // [F] conservative R-free bound I*T/D
  double dc_droop = 0.0;          // [V] I*R, the droop with no decap at all
  std::string limit_reason;       // populated when !feasible
};

// Solve for the decap that holds the event droop to spec.allowed_droop.
inline DecapSizingResult sizeDecapForDroop(const DecapSizingSpec& spec)
{
  DecapSizingResult result;
  result.dc_droop = spec.peak_current * spec.effective_resistance;
  result.charge_bound_cap = chargeBoundDecap(
      spec.peak_current, spec.event_duration, spec.allowed_droop);

  if (spec.allowed_droop <= 0.0) {
    result.feasible = false;
    result.required_cap = std::numeric_limits<double>::infinity();
    result.limit_reason = "allowed droop must be positive";
    return result;
  }

  if (spec.allowed_droop >= result.dc_droop) {
    // Even the un-decoupled steady state fits the budget.
    result.feasible = true;
    result.required_cap = 0.0;
    return result;
  }

  if (spec.effective_resistance <= 0.0) {
    result.feasible = false;
    result.required_cap = std::numeric_limits<double>::infinity();
    result.limit_reason = "effective resistance must be positive";
    return result;
  }

  // C = T / ( R * ln( I*R / (I*R - D) ) )
  const double ratio
      = result.dc_droop / (result.dc_droop - spec.allowed_droop);
  result.required_cap
      = spec.event_duration / (spec.effective_resistance * std::log(ratio));
  result.feasible = true;
  return result;
}

// One placement region competing for decap.  PSM reports a droop and a
// current per region; the site area it has left bounds how much decap can
// physically be dropped in.
struct DecapRegion
{
  std::string name;
  double droop = 0.0;         // [V] worst droop measured in this region
  double peak_current = 0.0;  // [A] switching current drawn in this region
  double capacity = 0.0;      // [F] most decap the free sites can hold
  double allocated = 0.0;     // [F] OUT: decap assigned to this region
  double shortfall = 0.0;     // [F] OUT: demand this region could not absorb
};

struct DecapAllocationResult
{
  bool feasible = false;      // true when every region's demand was met
  double total_required = 0.0;   // [F] summed demand
  double total_allocated = 0.0;  // [F] summed assignment
  double total_shortfall = 0.0;  // [F] summed unmet demand
  int regions_touched = 0;       // regions that needed any decap at all
};

// Allocate decap across regions to bring each region's droop to
// target_droop over an event of event_duration.
//
// Each region is sized independently with the EXACT first-order RC form.  PSM
// does not report a per-region resistance directly, but it does not need to:
// a region drawing peak_current and settling to `droop` has, by Ohm's law, an
// effective resistance R = droop / peak_current, which is exactly the R the
// closed form wants.  A region already at or below target gets nothing.
// Demand above a region's site capacity is reported as shortfall rather than
// silently dropped, so a capacity-starved floorplan surfaces as infeasible
// instead of as a quietly under-decoupled grid.
inline DecapAllocationResult allocateDecap(std::vector<DecapRegion>& regions,
                                           double target_droop,
                                           double event_duration)
{
  DecapAllocationResult result;

  for (DecapRegion& region : regions) {
    region.allocated = 0.0;
    region.shortfall = 0.0;

    if (target_droop <= 0.0 || region.droop <= target_droop
        || region.peak_current <= 0.0) {
      continue;
    }

    DecapSizingSpec spec;
    spec.peak_current = region.peak_current;
    spec.event_duration = event_duration;
    // R = droop / I recovers the region's effective supply resistance from
    // the measurement PSM already produced.
    spec.effective_resistance = region.droop / region.peak_current;
    spec.allowed_droop = target_droop;

    const double demand = sizeDecapForDroop(spec).required_cap;

    result.total_required += demand;
    result.regions_touched++;

    const double granted = std::min(demand, region.capacity);
    region.allocated = granted;
    region.shortfall = demand - granted;

    result.total_allocated += granted;
    result.total_shortfall += region.shortfall;
  }

  result.feasible = result.total_shortfall <= 0.0;
  return result;
}

}  // namespace psm
