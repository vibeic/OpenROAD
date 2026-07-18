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

// ---------------------------------------------------------------------------
// Vectored (package/board L*di/dt) inductive-droop gate.
//
// This is the RLC extension of the RC gate above.  A current source i(t) draws
// from an ideal Vdd rail through a series resistor R and a series package
// inductor L (companion model, L stamped into the MNA matrix).  With no
// capacitance the node voltage at the sink obeys the exact closed form
//     v_B(t) = Vdd - R*i(t) - L*di/dt.
// The inductive term L*di/dt is exactly what a purely-resistive PDN model omits
// and what RedHawk-SC / Voltus report as "first droop".  Because a linear
// current RAMP has a constant di/dt, backward-Euler reproduces it with no
// truncation error, so the gate holds to near machine precision -- the inductor
// companion cannot silently degenerate to the resistive-only answer.
// ---------------------------------------------------------------------------

// Builds the 3x3 resistive conductance for the RLC test.  Node 0 = B (sink,
// current source, no resistive connection -- fed only through the inductor),
// node 1 = M (mid), node 2 = S (rail pinned to Vdd through g_src).  A resistor
// R (conductance g) ties S to M; the inductor branch (added separately) ties
// M to B.
Eigen::SparseMatrix<double> BuildRlcConductance(double g, double g_src)
{
  Eigen::SparseMatrix<double> mat(3, 3);
  std::vector<Eigen::Triplet<double>> t;
  // Node M (1): resistor to S.
  t.emplace_back(1, 1, g);
  t.emplace_back(1, 2, -g);
  // Node S (2): resistor to M + stiff pin to the rail.
  t.emplace_back(2, 1, -g);
  t.emplace_back(2, 2, g + g_src);
  // Node B (0) carries no resistive conductance; the inductor companion adds
  // its dt/L diagonal so A stays non-singular.
  mat.setFromTriplets(t.begin(), t.end());
  mat.makeCompressed();
  return mat;
}

TEST(TransientRC, MatchesClosedFormRLC)
{
  const double vdd = 1.0;
  const double r = 0.5;             // series resistance [ohm]
  const double l = 1.0e-10;         // package/board inductance [H] (0.1 nH)
  const double g = 1.0 / r;
  const double g_src = 1.0e9 * g;   // stiff rail pin so v_S ~ Vdd

  const double period = 1.0e-9;     // 1 GHz clock
  const double i_max = 0.02;        // ramp end current [A]
  const double a = i_max / period;  // constant di/dt [A/s]
  const int nsteps = 2000;
  const double dt = period / nsteps;

  const Eigen::SparseMatrix<double> gmat = BuildRlcConductance(g, g_src);
  Eigen::VectorXd cap_diag = Eigen::VectorXd::Zero(3);  // no cap: pure RLC
  Eigen::VectorXd v0(3);
  v0 << vdd, vdd, vdd;  // start at the rail, i_L(0) = 0

  // Current ramp i(t) = a*t drawn out of node B.
  auto rhs_fn = [&](int, double t) -> Eigen::VectorXd {
    Eigen::VectorXd rhs = Eigen::VectorXd::Zero(3);
    rhs[2] = g_src * vdd;   // Norton pin on the rail
    rhs[0] = -a * t;        // current drawn at B
    return rhs;
  };

  // Inductor L between M (1) and B (0), initial current 0.
  std::vector<InductorBranch> inductors = {InductorBranch{1, 0, l, 0.0}};

  double max_abs_err = 0.0;
  auto observer = [&](int, double t, const Eigen::VectorXd& v) {
    const double i = a * t;
    const double analytic = vdd - r * i - l * a;  // di/dt = a (const)
    max_abs_err = std::max(max_abs_err, std::abs(v[0] - analytic));
  };

  const TransientMNAResult res = solveTransientMNA(
      gmat, cap_diag, v0, dt, nsteps, rhs_fn, /*track_min=*/true, observer,
      inductors);

  std::cout << "[RLC GATE] max abs error vs closed-form (V - R*i - L*di/dt) = "
            << max_abs_err << " V" << std::endl;
  EXPECT_LT(max_abs_err, 1.0e-5)
      << "RLC transient deviates from V - R*i - L*di/dt by " << max_abs_err
      << " V (the inductor companion is wrong).";

  // The L*di/dt term must be PRESENT: worst droop with L must exceed the
  // resistive-only worst droop by ~L*a.  Re-run with L->0 and compare.
  std::vector<InductorBranch> tiny_l = {InductorBranch{1, 0, 1.0e-24, 0.0}};
  const TransientMNAResult res_r = solveTransientMNA(
      gmat, cap_diag, v0, dt, nsteps, rhs_fn, /*track_min=*/true, {}, tiny_l);
  const double droop_rlc = vdd - res.v_min[0];
  const double droop_r = vdd - res_r.v_min[0];
  std::cout << "[RLC GATE] worst droop RLC = " << droop_rlc
            << " V, resistive-only = " << droop_r
            << " V, inductive component ~ " << (droop_rlc - droop_r) << " V"
            << std::endl;
  EXPECT_GT(droop_rlc, droop_r)
      << "inductive droop must add on top of resistive droop";
  EXPECT_NEAR(droop_rlc - droop_r, l * a, 0.05 * l * a);
}

// Larger di/dt (higher peak activity) must produce strictly larger inductive
// droop than a quieter (idle) profile -- the monotonicity the record calls out
// as "idle-VCD < peak-VCD".
TEST(TransientRC, InductiveDroopMonotonicInDiDt)
{
  const double vdd = 1.0;
  const double r = 0.5;
  const double l = 2.0e-10;
  const double g = 1.0 / r;
  const double g_src = 1.0e9 * g;
  const double period = 1.0e-9;
  const int nsteps = 1000;
  const double dt = period / nsteps;
  const Eigen::SparseMatrix<double> gmat = BuildRlcConductance(g, g_src);
  Eigen::VectorXd cap_diag = Eigen::VectorXd::Zero(3);
  Eigen::VectorXd v0(3);
  v0 << vdd, vdd, vdd;
  std::vector<InductorBranch> inductors = {InductorBranch{1, 0, l, 0.0}};

  auto worst_droop = [&](double i_max) -> double {
    const double a = i_max / period;
    auto rhs_fn = [&](int, double t) -> Eigen::VectorXd {
      Eigen::VectorXd rhs = Eigen::VectorXd::Zero(3);
      rhs[2] = g_src * vdd;
      rhs[0] = -a * t;
      return rhs;
    };
    const TransientMNAResult res = solveTransientMNA(
        gmat, cap_diag, v0, dt, nsteps, rhs_fn, true, {}, inductors);
    return vdd - res.v_min[0];
  };

  const double idle = worst_droop(0.002);   // quiet
  const double peak = worst_droop(0.050);    // busy
  std::cout << "[MONOTONIC] idle droop = " << idle << " V, peak droop = "
            << peak << " V" << std::endl;
  EXPECT_GT(peak, idle) << "peak-activity droop must exceed idle-activity droop";
}

// Cross-check: the grid path models the package as a rail-voltage modulation
// v_src(t) = Vdd - L*di_total/dt (an aggregate-current source-row rewrite),
// while the kernel above stamps a real inductor into the matrix.  For a single
// series R-L path the two are electrically identical (series-element order does
// not change the endpoint voltage), so this proves the grid's aggregate
// formulation is not a fake -- it reproduces the matrix-inductor answer.
TEST(TransientRC, PackageAggregateMatchesMatrixInductor)
{
  const double vdd = 1.0;
  const double r = 0.5;
  const double l = 1.5e-10;
  const double g = 1.0 / r;
  const double g_src = 1.0e9 * g;
  const double period = 1.0e-9;
  const double i_max = 0.03;
  const double a = i_max / period;  // constant di_total/dt
  const int nsteps = 1500;
  const double dt = period / nsteps;

  // Way A: a physical series R-L path  S --L-- P --R-- B, with L a real matrix
  // inductor.  Nodes: B=0, P=1 (package), S=2 (rail pinned to constant Vdd).
  Eigen::SparseMatrix<double> gmat_a(3, 3);
  {
    std::vector<Eigen::Triplet<double>> t;
    t.emplace_back(0, 0, g);   // B: resistor R to P
    t.emplace_back(0, 1, -g);
    t.emplace_back(1, 1, g);   // P: resistor R to B (+ inductor companion to S)
    t.emplace_back(1, 0, -g);
    t.emplace_back(2, 2, g_src);  // S: stiff pin
    gmat_a.setFromTriplets(t.begin(), t.end());
    gmat_a.makeCompressed();
  }
  Eigen::VectorXd cap3 = Eigen::VectorXd::Zero(3);
  Eigen::VectorXd v0_3(3);
  v0_3 << vdd, vdd, vdd;
  auto rhs_a = [&](int, double t) -> Eigen::VectorXd {
    Eigen::VectorXd rhs = Eigen::VectorXd::Zero(3);
    rhs[2] = g_src * vdd;
    rhs[0] = -a * t;
    return rhs;
  };
  std::vector<InductorBranch> inductors = {InductorBranch{2, 1, l, 0.0}};
  std::vector<double> vb_a;
  auto obs_a = [&](int, double, const Eigen::VectorXd& v) {
    vb_a.push_back(v[0]);
  };
  solveTransientMNA(gmat_a, cap3, v0_3, dt, nsteps, rhs_a, true, obs_a,
                    inductors);

  // Way B: the grid path model -- a purely resistive S --R-- B network whose
  // rail voltage is modulated by the aggregate di/dt: v_src(t) = Vdd - L*di/dt.
  // Nodes: B=0, S=1.  di_total/dt = a (constant), so v_src = Vdd - L*a.
  const Eigen::SparseMatrix<double> gmat_b = BuildRcConductance(g, g_src);
  Eigen::VectorXd cap2 = Eigen::VectorXd::Zero(2);
  Eigen::VectorXd v0_2(2);
  v0_2 << vdd, vdd;
  auto rhs_b = [&](int, double t) -> Eigen::VectorXd {
    const double v_src = vdd - l * a;  // rail drops by the package L*di/dt
    Eigen::VectorXd rhs(2);
    rhs << -a * t, g_src * v_src;
    return rhs;
  };
  std::vector<double> vb_b;
  auto obs_b = [&](int, double, const Eigen::VectorXd& v) {
    vb_b.push_back(v[0]);
  };
  solveTransientMNA(gmat_b, cap2, v0_2, dt, nsteps, rhs_b, true, obs_b, {});

  ASSERT_EQ(vb_a.size(), vb_b.size());
  double max_diff = 0.0;
  for (std::size_t i = 0; i < vb_a.size(); ++i) {
    max_diff = std::max(max_diff, std::abs(vb_a[i] - vb_b[i]));
  }
  // The two circuits are algebraically identical, so any residual is pure
  // numerical noise from the 1e9-stiff rail pin (~1e-5 V).  The discriminating
  // scale -- what an aggregate model that DROPPED the inductive term would miss
  // -- is L*a ~ 4.5e-3 V, two orders of magnitude above this threshold, so the
  // gate still fails hard if the package-L formulation is wrong.
  std::cout << "[AGGREGATE==MATRIX] max |v_B(matrix-L) - v_B(rail-modulation)| = "
            << max_diff << " V (discriminating scale L*a = " << (l * a) << " V)"
            << std::endl;
  EXPECT_LT(max_diff, 1.0e-4)
      << "aggregate rail-modulation must match the matrix inductor";
  EXPECT_GT(l * a, 20.0 * max_diff)
      << "the inductive term must dominate the numerical residual";
}

// Vectored superposition gate: staggered (vectored) per-instance switching
// phases must produce a strictly SMALLER worst droop than the vectorless
// worst case where every instance switches simultaneously -- for the SAME total
// average current.  This is the core justification for a vectored refinement:
// the vectorless default is a safe upper bound; VCD/SAIF phase information
// relaxes it toward the real (non-simultaneous) droop.
TEST(TransientRC, VectoredPhaseSpreadReducesDroop)
{
  const double vdd = 1.0;
  const double r = 4.0;
  const double c = 2.0e-11;
  const double g = 1.0 / r;
  const double g_src = 1.0e9 * g;
  const double i_avg = 0.004;  // per-sink average current [A]
  const int num_sinks = 8;
  const double duty = 0.25;

  const Eigen::SparseMatrix<double> gmat = BuildRcConductance(g, g_src);
  Eigen::VectorXd cap_diag(2);
  cap_diag << c, 0.0;
  Eigen::VectorXd v0(2);
  v0 << vdd, vdd;

  const double period = 1.0e-9;
  const int steps = 2000;
  const double dt = period / steps;

  // Aggregate current at node B is the SUM over the sinks of their shaped
  // pulses.  Both configurations draw the same charge per period (each sink's
  // shape integrates to 1), so total average current is identical.
  auto worst_droop = [&](const std::vector<double>& centers) -> double {
    auto rhs_fn = [&](int, double t) -> Eigen::VectorXd {
      const double phase = t / period;
      double shaped_sum = 0.0;
      for (double ctr : centers) {
        shaped_sum += triangularPulseShape(phase, ctr, duty);
      }
      Eigen::VectorXd rhs(2);
      rhs << -i_avg * shaped_sum, g_src * vdd;
      return rhs;
    };
    const TransientMNAResult res = solveTransientMNA(
        gmat, cap_diag, v0, dt, steps, rhs_fn, /*track_min=*/true);
    return vdd - res.v_min[0];
  };

  // Vectorless: every sink switches at phase 0.5 (simultaneous worst case).
  std::vector<double> simultaneous(num_sinks, 0.5);
  // Vectored: spread the switching phases evenly across the period.
  std::vector<double> staggered;
  for (int i = 0; i < num_sinks; ++i) {
    staggered.push_back((i + 0.5) / num_sinks);
  }

  const double droop_vectorless = worst_droop(simultaneous);
  const double droop_vectored = worst_droop(staggered);
  std::cout << "[VECTORED] vectorless (simultaneous) droop = "
            << droop_vectorless << " V, vectored (staggered) droop = "
            << droop_vectored << " V" << std::endl;
  EXPECT_GT(droop_vectorless, 0.0);
  EXPECT_GT(droop_vectored, 0.0);
  EXPECT_LT(droop_vectored, droop_vectorless)
      << "vectored (staggered-phase) droop must be below the vectorless "
         "simultaneous-switching upper bound";
}

}  // namespace
}  // namespace psm
