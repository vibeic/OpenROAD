// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, The OpenROAD Authors

// Unfakeable correctness gate for the transient (dynamic IR-drop) MNA kernel.
//
// The backward-Euler time-stepper in transient.h is validated here against the
// CLOSED-FORM first-order RC step response.  A tiny 2-node network is built by
// hand -- node S is an (almost) ideal Vdd source tied through a resistor R to
// node B, and node B carries a capacitance C to ground.  With v_B(0) = 0 the
// analytic solution is
//     v_B(t) = Vdd * (1 - e^{-t / (R*C)}).
// The solver's per-step voltages must match this to < 1% of Vdd.  If they do
// not, the MNA assembly / time-stepping is wrong.  This is the exact same
// kernel IRSolver::solveTransient() drives on a real power grid, so a pass here
// is a pass on the physics.

#include <cmath>
#include <iostream>
#include <vector>

#include "Eigen/Sparse"
#include "gtest/gtest.h"
#include "transient.h"

namespace psm {
namespace {

// Builds the 2x2 conductance matrix for the RC test.  Node 0 = B (has cap),
// node 1 = S (ideal source pinned to Vdd through a stiff conductance g_src).
Eigen::SparseMatrix<double> BuildRcConductance(double g, double g_src)
{
  Eigen::SparseMatrix<double> mat(2, 2);
  std::vector<Eigen::Triplet<double>> t;
  // Node B KCL: g*v_B - g*v_S.
  t.emplace_back(0, 0, g);
  t.emplace_back(0, 1, -g);
  // Node S KCL: -g*v_B + (g + g_src)*v_S  (g_src ties S to the Vdd rail).
  t.emplace_back(1, 0, -g);
  t.emplace_back(1, 1, g + g_src);
  mat.setFromTriplets(t.begin(), t.end());
  mat.makeCompressed();
  return mat;
}

TEST(TransientRC, MatchesClosedFormStepResponse)
{
  const double vdd = 1.1;      // supply [V]
  const double r = 2.0;        // resistance [ohm]
  const double c = 5.0e-10;    // capacitance [F]
  const double tau = r * c;    // RC time constant = 1 ns
  const double g = 1.0 / r;    // conductance [S]
  const double g_src = 1.0e9 * g;  // stiff pin so v_S ~ Vdd

  // Small timestep so backward-Euler truncation error << 1% tolerance.
  const double dt = tau / 2000.0;
  const int nsteps = 8000;  // covers 4 * tau

  const Eigen::SparseMatrix<double> gmat = BuildRcConductance(g, g_src);

  Eigen::VectorXd cap_diag(2);
  cap_diag << c, 0.0;  // capacitance only on node B

  Eigen::VectorXd v0(2);
  v0 << 0.0, vdd;  // B discharged, S at the rail

  // Constant current injection: node B has no independent source; node S is
  // driven to Vdd through the stiff pin (Norton form g_src * Vdd).
  Eigen::VectorXd rhs_const(2);
  rhs_const << 0.0, g_src * vdd;
  auto rhs_fn = [&](int, double) -> Eigen::VectorXd { return rhs_const; };

  // Compare every step against the analytic RC step response.
  double max_rel_err = 0.0;
  auto observer = [&](int, double t, const Eigen::VectorXd& v) {
    const double analytic = vdd * (1.0 - std::exp(-t / tau));
    const double rel_err = std::abs(v[0] - analytic) / vdd;
    max_rel_err = std::max(max_rel_err, rel_err);
  };

  const TransientMNAResult result = solveTransientMNA(
      gmat, cap_diag, v0, dt, nsteps, rhs_fn, /*track_min=*/true, observer);

  std::cout << "[ANALYTIC GATE] max relative error vs closed-form RC = "
            << max_rel_err << " (" << max_rel_err * 100.0
            << "% of Vdd), tolerance = 1e-2" << std::endl;

  // Primary gate: solver tracks the closed-form RC curve to < 1% of Vdd.
  EXPECT_LT(max_rel_err, 1.0e-2)
      << "Transient MNA deviates from the analytic RC step response by "
      << max_rel_err * 100.0 << "% of Vdd (tol 1%).";

  // Sanity: node B relaxes toward Vdd (its minimum is the discharged start).
  EXPECT_NEAR(result.v_min[0], 0.0, 1.0e-3);
  EXPECT_GT(result.v_max[0], 0.98 * vdd);
}

// The triangular current pulse must conserve charge: its time-average over one
// clock period equals the average current (dimensionless multiplier averages to
// 1), and its peak equals 2/duty.
TEST(TransientRC, TriangularPulseConservesCharge)
{
  for (const double duty : {0.25, 0.5, 1.0}) {
    const double center = 0.5;
    const int samples = 200000;
    double integral = 0.0;
    double peak = 0.0;
    for (int i = 0; i < samples; ++i) {
      const double x = (i + 0.5) / samples;  // midpoint rule over [0,1)
      const double s = triangularPulseShape(x, center, duty);
      integral += s;
      peak = std::max(peak, s);
    }
    const double average = integral / samples;
    EXPECT_NEAR(average, 1.0, 1.0e-3)
        << "duty=" << duty << " average multiplier should be 1";
    EXPECT_NEAR(peak, 2.0 / duty, 2.0e-2)
        << "duty=" << duty << " peak multiplier should be 2/duty";
  }
}

// With zero capacitance the transient solve degenerates to a per-step quasi-
// static solve: at the current peak the droop is the peak-current droop, which
// must be strictly larger than the static (average-current) droop.  This is the
// core physical guarantee behind "dynamic >= static".
TEST(TransientRC, ZeroCapDynamicExceedsStatic)
{
  const double vdd = 1.0;
  const double r = 5.0;
  const double g = 1.0 / r;
  const double g_src = 1.0e9 * g;
  const double i_avg = 0.01;  // average current drawn at node B [A]

  const Eigen::SparseMatrix<double> gmat = BuildRcConductance(g, g_src);
  Eigen::VectorXd cap_diag(2);
  cap_diag << 0.0, 0.0;  // quasi-static

  // Static DC solution (constant i_avg at node B).
  Eigen::VectorXd j_static(2);
  j_static << -i_avg, g_src * vdd;  // supply-net sign convention: -I_avg
  Eigen::SparseLU<Eigen::SparseMatrix<double>> dc;
  dc.compute(gmat);
  const Eigen::VectorXd v_dc = dc.solve(j_static);
  const double static_droop = vdd - v_dc[0];

  // Transient with a full-period triangular pulse (duty=1 -> peak 2x average).
  const double period = 1.0e-9;
  const int steps = 400;
  const double dt = period / steps;
  auto rhs_fn = [&](int, double t) -> Eigen::VectorXd {
    const double phase = t / period;
    const double shape = triangularPulseShape(phase, 0.5, 1.0);
    Eigen::VectorXd rhs(2);
    rhs << -i_avg * shape, g_src * vdd;
    return rhs;
  };

  const TransientMNAResult result = solveTransientMNA(
      gmat, cap_diag, v_dc, dt, steps, rhs_fn, /*track_min=*/true);

  const double dynamic_droop = vdd - result.v_min[0];
  std::cout << "[ZERO-CAP] static droop = " << static_droop
            << " V, dynamic droop = " << dynamic_droop
            << " V, ratio = " << dynamic_droop / static_droop << std::endl;
  EXPECT_GT(dynamic_droop, static_droop)
      << "dynamic droop " << dynamic_droop << " must exceed static droop "
      << static_droop;
  // Peak current is 2x average, so quasi-static peak droop ~ 2x static.
  EXPECT_NEAR(dynamic_droop / static_droop, 2.0, 5.0e-2);
}

}  // namespace
}  // namespace psm
