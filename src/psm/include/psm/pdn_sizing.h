// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#pragma once

// Analysis-driven power-grid sizing.
//
// PSM answers "what is the droop of THIS grid".  It does not answer the
// synthesis question the designer actually has: "how wide must the straps be
// so the droop meets my budget".  This header is that inverse.
//
// PHYSICS
// -------
// Consider one power strap of length L fed from an ideal supply, carrying a
// uniformly distributed load.  Let
//     Rs  = sheet resistance of the strap layer            [ohm/square]
//     w   = strap width                                    [m]
//     p   = strap pitch (the stripe of die each strap owns) [m]
//     J   = areal current density of the served logic      [A/m^2]
// so the per-unit-length rail resistance is r = Rs / w [ohm/m] and the
// per-unit-length load current is i = J * p [A/m].
//
// Single-end feed (fed at x = 0, open at x = L).  All the current drawn over
// [x, L] must flow through x, so I(x) = i * (L - x) and dV/dx = -r * I(x):
//     Vdrop(x) = integral_0^x r*i*(L-s) ds = r*i*(L*x - x^2/2),
// which is maximum at the open end:
//     Vdrop_max = r * i * L^2 / 2.
//
// Double-end feed (both ends held at Vdd).  By symmetry no current crosses the
// midpoint, so each half behaves as a single-end-fed rail of length L/2:
//     Vdrop_max = r * i * (L/2)^2 / 2 = r * i * L^2 / 8,
// located at the centre of the rail.
//
// Both cases are r*i*L^2 / k with k = 2 or k = 8.  These are the two classic
// hand-computable PDN numbers, and TestPdnSizing.cpp gates the closed form
// against an independent discrete ladder-network solve.
//
// WHY NAIVE SCALING IS WRONG
// --------------------------
// The distributed term above is proportional to 1/w, so it is tempting to
// repair a grid by scaling the width by (measured droop / target droop).  That
// is only correct when the ENTIRE droop is distributed.  Real supplies also
// carry an irreducible series term -- package lead, bump/C4 spreading, board
// -- which widening on-die metal cannot remove:
//     D(w) = D_irr + A / w,      A = Rs * i * L^2 / k.
// Solving D(w) = D_target gives
//     w_req = A / (D_target - D_irr),
// and the request is INFEASIBLE by widening alone once D_target <= D_irr: no
// amount of on-die metal can beat the package.  The naive law silently returns
// a finite width in that regime and under-delivers, which is exactly the
// failure TestPdnSizing.cpp pins as a proven-negative.

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace psm {

// How a strap is connected to the supply.
enum class RailFeed
{
  kSingleEnd,  // fed at one end only; worst droop at the far (open) end
  kDoubleEnd,  // fed at both ends; worst droop at the rail centre
};

// The geometric factor k in Vdrop_max = r*i*L^2 / k.
inline double railFeedFactor(RailFeed feed)
{
  return feed == RailFeed::kDoubleEnd ? 8.0 : 2.0;
}

// Closed-form worst-case droop of a uniformly-loaded resistive rail.
//   r_per_len : rail resistance per unit length [ohm/m]
//   i_per_len : load current per unit length    [A/m]
//   length    : rail length L                   [m]
inline double uniformRailDroop(double r_per_len,
                               double i_per_len,
                               double length,
                               RailFeed feed)
{
  return r_per_len * i_per_len * length * length / railFeedFactor(feed);
}

// Resistance per unit length of a strap of width w on a layer of sheet
// resistance Rs.  A run of length L spans L/w squares, so R = Rs*L/w and
// r = Rs/w.
inline double stripResistancePerLength(double sheet_res, double width)
{
  return sheet_res / width;
}

// What the caller knows about the grid it wants sized.
struct PdnSizingSpec
{
  double sheet_res = 0.0;        // Rs of the strap layer [ohm/square]
  double rail_length = 0.0;      // L [m]
  double strap_pitch = 0.0;      // p [m]
  double current_density = 0.0;  // J [A/m^2] drawn by the served logic
  RailFeed feed = RailFeed::kDoubleEnd;
  double target_droop = 0.0;  // droop budget [V]
  // Irreducible series resistance from the supply to the strap's feed points
  // (package + bump/C4 + board).  Widening on-die metal cannot reduce the
  // droop this produces.
  double series_resistance = 0.0;  // [ohm]
  // Manufacturable width window for the strap layer.
  double min_width = 0.0;                                   // [m]
  double max_width = std::numeric_limits<double>::max();  // [m]
};

// Outcome of a sizing request.
struct PdnSizingResult
{
  bool feasible = false;
  double required_width = 0.0;  // [m] width that meets target_droop
  // Droop predicted at required_width (== target_droop when the width is not
  // clamped by min_width).
  double achieved_droop = 0.0;  // [V]
  // The part of the droop that widening cannot remove (package/bump term).
  double irreducible_droop = 0.0;  // [V]
  // Total current the strap carries, J * p * L.
  double strap_current = 0.0;  // [A]
  std::string limit_reason;    // populated when !feasible
};

// Total current one strap carries: the areal density over the stripe it owns.
inline double strapCurrent(const PdnSizingSpec& spec)
{
  return spec.current_density * spec.strap_pitch * spec.rail_length;
}

// Solve for the strap width that meets spec.target_droop.
//
// Returns feasible = false (with limit_reason set) when the target is below
// the irreducible package droop, or when the required width exceeds
// max_width.  In both cases required_width still carries the value the
// physics demands, so the caller can report how far short the grid is.
inline PdnSizingResult sizeStrapWidthForDroop(const PdnSizingSpec& spec)
{
  PdnSizingResult result;

  const double i_tot = strapCurrent(spec);
  result.strap_current = i_tot;
  result.irreducible_droop = i_tot * spec.series_resistance;

  // A = Rs * i * L^2 / k, the numerator of the distributed 1/w term.
  const double i_per_len = spec.current_density * spec.strap_pitch;
  const double a_coeff = spec.sheet_res * i_per_len * spec.rail_length
                         * spec.rail_length / railFeedFactor(spec.feed);

  const double headroom = spec.target_droop - result.irreducible_droop;
  if (headroom <= 0.0) {
    // The package alone already burns the whole budget.  No on-die width can
    // fix this -- the caller needs more bumps or a lower-R package.
    result.feasible = false;
    result.required_width = std::numeric_limits<double>::infinity();
    result.achieved_droop = result.irreducible_droop;
    result.limit_reason
        = "target droop is at or below the irreducible package/bump droop; "
          "widening on-die straps cannot meet it";
    return result;
  }

  double width = a_coeff / headroom;

  // A grid that already meets target still has to be manufacturable.
  if (width < spec.min_width) {
    width = spec.min_width;
  }

  result.required_width = width;
  result.achieved_droop
      = result.irreducible_droop + a_coeff / width;

  if (width > spec.max_width) {
    result.feasible = false;
    result.limit_reason
        = "required strap width exceeds the maximum manufacturable width";
    return result;
  }

  result.feasible = true;
  return result;
}

// ANALYSIS-DRIVEN entry point: re-size a grid from a droop PSM actually
// measured, rather than from an assumed current density.
//
// The measured droop is decomposed as D_meas = D_irr + A/w_cur, which
// calibrates A = (D_meas - D_irr) * w_cur against the real extracted grid.
// Solving for the target then gives
//     w_req = w_cur * (D_meas - D_irr) / (D_target - D_irr).
// With D_irr = 0 this collapses to the naive w_cur * D_meas / D_target; the
// D_irr term is precisely what the naive law omits.
//
//   measured_droop     : worst droop reported by PSM for this net  [V]
//   current_width      : strap width the measurement was taken at  [m]
//   irreducible_droop  : package/bump droop floor                  [V]
//   target_droop       : budget to meet                            [V]
inline PdnSizingResult resizeFromMeasuredDroop(double measured_droop,
                                               double current_width,
                                               double irreducible_droop,
                                               double target_droop,
                                               double min_width = 0.0,
                                               double max_width
                                               = std::numeric_limits<
                                                   double>::max())
{
  PdnSizingResult result;
  result.irreducible_droop = irreducible_droop;

  const double headroom = target_droop - irreducible_droop;
  if (headroom <= 0.0) {
    result.feasible = false;
    result.required_width = std::numeric_limits<double>::infinity();
    result.achieved_droop = irreducible_droop;
    result.limit_reason
        = "target droop is at or below the irreducible package/bump droop; "
          "widening on-die straps cannot meet it";
    return result;
  }

  // Calibrated distributed coefficient of the real grid.
  const double a_coeff = (measured_droop - irreducible_droop) * current_width;
  if (a_coeff <= 0.0) {
    // Already at or below the package floor: nothing distributed to remove.
    result.feasible = true;
    result.required_width = std::max(current_width, min_width);
    result.achieved_droop = measured_droop;
    return result;
  }

  double width = a_coeff / headroom;
  if (width < min_width) {
    width = min_width;
  }

  result.required_width = width;
  result.achieved_droop = irreducible_droop + a_coeff / width;

  if (width > max_width) {
    result.feasible = false;
    result.limit_reason
        = "required strap width exceeds the maximum manufacturable width";
    return result;
  }

  result.feasible = true;
  return result;
}

}  // namespace psm
