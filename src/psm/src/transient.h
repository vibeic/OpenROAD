// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, The OpenROAD Authors

#pragma once

// Time-domain (transient / dynamic) power-grid analysis kernel.
//
// PSM's baseline solve() is a single static DC operating point: it factorizes
// the conductance matrix G once and solves G*v = j for the average current
// vector.  This header adds the missing time-domain piece needed for dynamic
// (di/dt) voltage-droop analysis: a backward-Euler Modified-Nodal-Analysis
// (MNA) time-stepper that also accounts for the per-node capacitance to
// ground.
//
// The continuous system is
//     C dv/dt + G v = i(t)
// with G the conductance matrix (identical to the static path, including the
// ideal-source stamping) and C a diagonal matrix of lumped node capacitance to
// ground.  Discretizing with the A-stable backward-Euler rule v' ~ (v_k -
// v_{k-1})/dt gives
//     (G + C/dt) v_k = i(t_k) + (C/dt) v_{k-1}.
// For a fixed timestep dt the left-hand matrix A = G + C/dt is constant across
// all steps, so it is factorized exactly once and reused via back-substitution
// for every step -- the same one-factorization/many-solves structure the
// static path already relies on.
//
// This kernel is deliberately free of any OpenDB / OpenSTA dependency: it
// operates purely on Eigen matrices.  That lets the exact same code be driven
// (a) by IRSolver on a real extracted power grid and (b) by a self-contained
// unit test that checks it against the closed-form first-order RC step
// response.  If the analytic gate passes, the time-stepper is correct.

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

#include "Eigen/Sparse"

namespace psm {

// Result of a transient MNA solve.  All vectors are indexed by the same node
// ordering as the input matrices.
struct TransientMNAResult
{
  // Per-node minimum voltage seen over the whole simulated window.
  Eigen::VectorXd v_min;
  // Per-node maximum voltage seen over the whole simulated window.
  Eigen::VectorXd v_max;
  // Full voltage snapshot at the worst (most-extreme) timestep.
  Eigen::VectorXd v_at_worst;
  // The most-extreme single-node voltage over the window (global min if
  // track_min, else global max).
  double worst_value = 0.0;
  // Step index (1-based) and time [s] at which worst_value occurred.
  int worst_step = -1;
  double worst_time = 0.0;
};

// Vectorless per-clock triangular current-pulse shape.
//
// Returns a dimensionless multiplier applied to an instance's average current
// I_avg so that the shaped waveform (a) is a triangular pulse of duty-cycle
// `duty` centered at normalized phase `center` within each clock period and (b)
// conserves charge: its integral over one full period equals I_avg * T (i.e.
// the time-average of the multiplier is 1).  A triangle of base width duty*T
// and peak height h has area 0.5*duty*T*h; setting that equal to T gives the
// peak multiplier h = 2/duty.  So instantaneous peak current = (2/duty)*I_avg.
//
//   phase  : normalized position within the clock period, wrapped to [0,1).
//   center : normalized pulse center within the period, in [0,1).
//   duty   : pulse base width as a fraction of the period, in (0,1].
//
// The distance is measured circularly so the pulse wraps cleanly across the
// period boundary.
inline double triangularPulseShape(double phase, double center, double duty)
{
  if (duty <= 0.0) {
    return 0.0;
  }
  // Wrap phase into [0,1).
  double frac = phase - std::floor(phase);
  double dist = std::abs(frac - center);
  dist = std::min(dist, 1.0 - dist);  // circular distance on the unit period
  const double half_width = duty / 2.0;
  if (dist >= half_width) {
    return 0.0;
  }
  const double peak = 2.0 / duty;
  return peak * (1.0 - dist / half_width);
}

// Backward-Euler MNA transient solve.
//
//   g         : NxN conductance matrix (with ideal-source stamping baked in,
//               exactly as assembled for the static solve -- must be
//               non-singular).
//   cap_diag  : length-N per-node capacitance-to-ground [F].  Entries may be 0
//               (ideal source nodes carry no capacitance); an all-zero
//               cap_diag degenerates to a per-step quasi-static solve.
//   v0        : length-N initial condition (typically the static DC solution).
//   dt        : timestep [s] (> 0, constant).
//   nsteps    : number of timesteps to advance (>= 1).
//   rhs_fn    : rhs_fn(k, t_k) returns the length-N current-injection vector at
//               step k (1-based) and time t_k = k*dt.
//   track_min : if true the "worst" node is the global minimum voltage (supply
//               droop); if false it is the global maximum (ground bounce).
//   observer  : optional per-step callback observer(k, t_k, v_k); used by unit
//               tests to compare against an analytic reference.
//
// Returns per-node min/max envelopes plus the worst-step snapshot.  Throws
// std::runtime_error if the constant system matrix cannot be factorized.
inline TransientMNAResult solveTransientMNA(
    const Eigen::SparseMatrix<double>& g,
    const Eigen::VectorXd& cap_diag,
    const Eigen::VectorXd& v0,
    double dt,
    int nsteps,
    const std::function<Eigen::VectorXd(int, double)>& rhs_fn,
    bool track_min,
    const std::function<void(int, double, const Eigen::VectorXd&)>& observer
    = {})
{
  const int n = static_cast<int>(g.rows());

  // c_over_dt[i] = C_i / dt, the per-node capacitive admittance contribution.
  const Eigen::VectorXd c_over_dt = cap_diag / dt;

  // Build the constant left-hand-side A = G + C/dt.  Only diagonal entries are
  // added, so this preserves G's sparsity pattern.
  Eigen::SparseMatrix<double> a_matrix = g;
  for (int i = 0; i < n; ++i) {
    if (c_over_dt[i] != 0.0) {
      a_matrix.coeffRef(i, i) += c_over_dt[i];
    }
  }
  a_matrix.makeCompressed();

  // Factorize once, reuse for every step.
  Eigen::SparseLU<Eigen::SparseMatrix<double>> solver;
  solver.compute(a_matrix);
  if (solver.info() != Eigen::ComputationInfo::Success) {
    throw std::runtime_error(
        "Transient MNA: factorization of (G + C/dt) failed");
  }

  TransientMNAResult result;
  result.v_min = v0;
  result.v_max = v0;
  result.v_at_worst = v0;
  result.worst_value
      = track_min ? std::numeric_limits<double>::max()
                  : std::numeric_limits<double>::lowest();

  Eigen::VectorXd v_prev = v0;
  for (int k = 1; k <= nsteps; ++k) {
    const double t_k = k * dt;

    // RHS = i(t_k) + (C/dt) * v_{k-1}.
    Eigen::VectorXd rhs = rhs_fn(k, t_k);
    rhs += c_over_dt.cwiseProduct(v_prev);

    const Eigen::VectorXd v_k = solver.solve(rhs);
    if (solver.info() != Eigen::ComputationInfo::Success) {
      throw std::runtime_error("Transient MNA: back-substitution failed");
    }

    result.v_min = result.v_min.cwiseMin(v_k);
    result.v_max = result.v_max.cwiseMax(v_k);

    // Track the most-extreme single-node voltage and the step it occurred on.
    int extreme_idx = 0;
    const double step_extreme
        = track_min ? v_k.minCoeff(&extreme_idx) : v_k.maxCoeff(&extreme_idx);
    const bool is_worse = track_min ? (step_extreme < result.worst_value)
                                    : (step_extreme > result.worst_value);
    if (is_worse) {
      result.worst_value = step_extreme;
      result.worst_step = k;
      result.worst_time = t_k;
      result.v_at_worst = v_k;
    }

    if (observer) {
      observer(k, t_k, v_k);
    }

    v_prev = v_k;
  }

  return result;
}

}  // namespace psm
