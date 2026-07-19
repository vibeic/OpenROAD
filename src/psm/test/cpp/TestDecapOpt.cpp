// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Unfakeable correctness gate for droop-driven decap sizing.
//
// decap_opt.h inverts the first-order RC step response to answer "how much
// decap does this droop budget demand".  The gate never grades that inversion
// against itself: the capacitance it produces is fed to the INDEPENDENT
// backward-Euler transient MNA kernel in transient.h (already gated against
// the closed-form RC response by TestTransientRC.cpp), and the droop that
// kernel simulates must land on the requested budget.  Sizer and simulator
// share no code.
//
// The gates:
//
//   1. HAND-COMPUTABLE CAPACITANCE.  C = T / (R * ln(I*R / (I*R - D))) is
//      checked against a number worked out by hand in the comments below.
//
//   2. SIMULATED DROOP MEETS THE BUDGET.  Drive the transient kernel with the
//      sized capacitance; the simulated worst droop must equal the budget.
//      This is the cross-check that cannot be an echo -- a wrong inversion
//      lands the simulator somewhere other than the target.
//
//   3. PROVEN-NEGATIVE ON REMOVED DECAP.  Re-run the SAME simulator with the
//      capacitance halved and with it removed entirely; both must show a
//      STRICTLY WORSE droop that violates the budget.  Without this the
//      "meets budget" result could not distinguish a working sizer from a
//      harness where the droop never moves.
//
//   4. CHARGE-BOUND ORDERING.  The R -> infinity charge bound I*T/D must
//      strictly exceed the exact answer for every finite R, and must be
//      approached as R grows.
//
//   5. ALLOCATOR INVARIANTS, including a capacity-starved case that must
//      report the hand-computed shortfall instead of silently under-filling.

#include <cmath>
#include <iostream>
#include <vector>

#include "Eigen/Sparse"
#include "gtest/gtest.h"
#include "psm/decap_opt.h"
#include "transient.h"

namespace psm {
namespace {

// Conductance matrix for the sizing model: node 0 = the die node carrying the
// decap, node 1 = the supply, pinned stiffly to Vdd.
Eigen::SparseMatrix<double> BuildNodeConductance(double g, double g_src)
{
  Eigen::SparseMatrix<double> mat(2, 2);
  std::vector<Eigen::Triplet<double>> t;
  t.emplace_back(0, 0, g);
  t.emplace_back(0, 1, -g);
  t.emplace_back(1, 0, -g);
  t.emplace_back(1, 1, g + g_src);
  mat.setFromTriplets(t.begin(), t.end());
  mat.makeCompressed();
  return mat;
}

// Simulates the droop event with the independent transient MNA kernel and
// returns the worst droop [V] seen on the die node.
//
//   i / r / c : event current [A], supply resistance [ohm], decap [F]
//   t_event   : event duration [s]
double SimulatedWorstDroop(double i, double r, double c, double t_event)
{
  const double vdd = 1.8;
  const double g = 1.0 / r;
  const double g_src = 1.0e9 * g;  // stiff supply pin

  const int nsteps = 20000;
  const double dt = t_event / nsteps;

  const Eigen::SparseMatrix<double> gmat = BuildNodeConductance(g, g_src);

  Eigen::VectorXd cap_diag(2);
  cap_diag << c, 0.0;

  // Start at the undisturbed DC operating point.
  Eigen::VectorXd v0(2);
  v0 << vdd, vdd;

  // A current step of amplitude i drawn out of the die node for the whole
  // window, plus the Norton source holding node 1 at Vdd.
  Eigen::VectorXd rhs(2);
  rhs << -i, g_src * vdd;
  auto rhs_fn = [&](int, double) -> Eigen::VectorXd { return rhs; };

  const TransientMNAResult result = solveTransientMNA(
      gmat, cap_diag, v0, dt, nsteps, rhs_fn, /*track_min=*/true);

  return vdd - result.v_min[0];
}

// Representative event: 40 mA drawn for 200 ps through a 2 ohm supply path.
constexpr double kI = 40.0e-3;    // [A]
constexpr double kT = 200.0e-12;  // [s]
constexpr double kR = 2.0;        // [ohm]

TEST(DecapOpt, RequiredCapMatchesHandComputation)
{
  // Hand computation:
  //   I*R      = 0.04 * 2            = 0.08 V   (droop with no decap)
  //   D        = 0.04 V              (budget: half the DC droop)
  //   I*R - D  = 0.04 V
  //   ratio    = 0.08 / 0.04         = 2
  //   ln(2)    = 0.693147180559945...
  //   C        = T / (R * ln 2)
  //            = 200e-12 / (2 * 0.6931471805599453)
  //            = 200e-12 / 1.3862943611198906
  //            = 1.4426950408889634e-10 F = 144.27 pF
  DecapSizingSpec spec;
  spec.peak_current = kI;
  spec.event_duration = kT;
  spec.effective_resistance = kR;
  spec.allowed_droop = 0.04;

  const DecapSizingResult result = sizeDecapForDroop(spec);
  ASSERT_TRUE(result.feasible) << result.limit_reason;

  EXPECT_DOUBLE_EQ(result.dc_droop, 0.08);
  EXPECT_NEAR(result.required_cap, 1.4426950408889634e-10, 1.0e-22);

  std::cout << "[ANALYTIC GATE] required decap = "
            << result.required_cap * 1e12 << " pF (hand-computed 144.2695 pF)"
            << std::endl;
}

TEST(DecapOpt, SizedCapMeetsBudgetInIndependentTransientSolve)
{
  const double budget = 0.04;

  DecapSizingSpec spec;
  spec.peak_current = kI;
  spec.event_duration = kT;
  spec.effective_resistance = kR;
  spec.allowed_droop = budget;

  const DecapSizingResult result = sizeDecapForDroop(spec);
  ASSERT_TRUE(result.feasible) << result.limit_reason;

  const double simulated
      = SimulatedWorstDroop(kI, kR, result.required_cap, kT);

  std::cout << "[CROSS-CHECK] sized C=" << result.required_cap * 1e12
            << " pF -> MNA-simulated droop=" << simulated * 1e3 << " mV"
            << "  (budget " << budget * 1e3 << " mV)" << std::endl;

  // Backward-Euler over 20k steps leaves well under 0.1% truncation error.
  EXPECT_NEAR(simulated, budget, budget * 1.0e-3)
      << "the independent transient solve disagrees with the sizing inversion";
}

TEST(DecapOpt, ProvenNegativeRemovedAndHalvedDecapViolateBudget)
{
  const double budget = 0.04;

  DecapSizingSpec spec;
  spec.peak_current = kI;
  spec.event_duration = kT;
  spec.effective_resistance = kR;
  spec.allowed_droop = budget;

  const double c_req = sizeDecapForDroop(spec).required_cap;
  ASSERT_GT(c_req, 0.0);

  const double droop_sized = SimulatedWorstDroop(kI, kR, c_req, kT);
  const double droop_half = SimulatedWorstDroop(kI, kR, c_req / 2.0, kT);
  const double droop_none = SimulatedWorstDroop(kI, kR, 0.0, kT);

  std::cout << "[PROVEN-NEGATIVE] droop: sized=" << droop_sized * 1e3
            << " mV  half-decap=" << droop_half * 1e3
            << " mV  no-decap=" << droop_none * 1e3 << " mV" << std::endl;

  // Removing decap must make things strictly worse, and must break the
  // budget.  This is what proves the "meets budget" result above is a real
  // measurement and not a harness that reports the target no matter what.
  EXPECT_GT(droop_half, droop_sized);
  EXPECT_GT(droop_half, budget)
      << "halving the decap did not violate the budget -- the harness is not "
         "sensitive to capacitance and the positive result is meaningless";
  EXPECT_GT(droop_none, droop_half);
  // With no decap at all the droop is the full DC value I*R.
  EXPECT_NEAR(droop_none, kI * kR, kI * kR * 1.0e-3);
}

TEST(DecapOpt, ChargeBoundStrictlyDominatesAndIsApproachedAsRGrows)
{
  const double budget = 0.02;
  const double charge_bound = chargeBoundDecap(kI, kT, budget);

  // Hand check: I*T/D = 0.04 * 200e-12 / 0.02 = 4.0e-10 F = 400 pF.
  EXPECT_NEAR(charge_bound, 4.0e-10, 1.0e-22);

  double prev_gap = std::numeric_limits<double>::max();
  for (const double r : {1.0, 10.0, 100.0, 1000.0}) {
    DecapSizingSpec spec;
    spec.peak_current = kI;
    spec.event_duration = kT;
    spec.effective_resistance = r;
    spec.allowed_droop = budget;

    const DecapSizingResult result = sizeDecapForDroop(spec);
    ASSERT_TRUE(result.feasible) << result.limit_reason;

    // A finite supply resistance always lets the grid deliver part of the
    // charge, so the exact answer must sit strictly below the bound.
    EXPECT_LT(result.required_cap, charge_bound)
        << "exact RC sizing exceeded the R->infinity charge bound at R=" << r;

    const double gap = charge_bound - result.required_cap;
    EXPECT_LT(gap, prev_gap)
        << "the exact answer is not approaching the charge bound as R grows";
    prev_gap = gap;
  }

  std::cout << "[LIMIT GATE] charge bound " << charge_bound * 1e12
            << " pF approached monotonically as R grows" << std::endl;
}

TEST(DecapOpt, NoDecapNeededWhenDcDroopAlreadyMeetsBudget)
{
  DecapSizingSpec spec;
  spec.peak_current = kI;
  spec.event_duration = kT;
  spec.effective_resistance = kR;
  spec.allowed_droop = 0.5;  // budget far above I*R = 0.08 V

  const DecapSizingResult result = sizeDecapForDroop(spec);
  EXPECT_TRUE(result.feasible);
  EXPECT_DOUBLE_EQ(result.required_cap, 0.0);

  // And the simulator agrees: with no decap the droop is I*R, inside budget.
  EXPECT_LT(SimulatedWorstDroop(kI, kR, 0.0, kT), spec.allowed_droop);
}

TEST(DecapOpt, AllocatorSkipsCleanRegionsAndMeetsDemandWhenCapacityAllows)
{
  const double target = 0.03;

  std::vector<DecapRegion> regions = {
      {"clean", 0.02, 10.0e-3, 1.0e-9, 0.0, 0.0},   // droop under target
      {"hot", 0.06, 20.0e-3, 1.0e-9, 0.0, 0.0},     // needs decap
      {"warm", 0.045, 15.0e-3, 1.0e-9, 0.0, 0.0},   // needs decap
  };

  const DecapAllocationResult result
      = allocateDecap(regions, target, kT);

  EXPECT_TRUE(result.feasible);
  EXPECT_EQ(result.regions_touched, 2);
  // A region already inside budget must not consume any decap.
  EXPECT_DOUBLE_EQ(regions[0].allocated, 0.0);
  EXPECT_GT(regions[1].allocated, 0.0);
  EXPECT_GT(regions[2].allocated, 0.0);
  // Ample capacity -> nothing unmet, and the books balance.
  EXPECT_DOUBLE_EQ(result.total_shortfall, 0.0);
  EXPECT_DOUBLE_EQ(result.total_allocated, result.total_required);

  // Each allocation must actually fix its region in the independent solve.
  for (std::size_t idx = 1; idx < regions.size(); ++idx) {
    const DecapRegion& region = regions[idx];
    const double r_eff = region.droop / region.peak_current;
    const double simulated = SimulatedWorstDroop(
        region.peak_current, r_eff, region.allocated, kT);
    std::cout << "[ALLOCATOR] region " << region.name << ": "
              << region.droop * 1e3 << " mV -> " << simulated * 1e3
              << " mV with " << region.allocated * 1e12 << " pF" << std::endl;
    EXPECT_NEAR(simulated, target, target * 1.0e-3);
  }
}

TEST(DecapOpt, AllocatorReportsHandComputedShortfallWhenCapacityStarved)
{
  const double target = 0.03;

  // Size the demand first so the capacity can be starved by a known amount.
  DecapSizingSpec spec;
  spec.peak_current = 20.0e-3;
  spec.event_duration = kT;
  spec.effective_resistance = 0.06 / 20.0e-3;  // droop / current
  spec.allowed_droop = target;
  const double demand = sizeDecapForDroop(spec).required_cap;
  ASSERT_GT(demand, 0.0);

  // Give the region only 40% of what it needs.
  const double capacity = demand * 0.4;
  std::vector<DecapRegion> regions
      = {{"starved", 0.06, 20.0e-3, capacity, 0.0, 0.0}};

  const DecapAllocationResult result = allocateDecap(regions, target, kT);

  std::cout << "[STARVED GATE] demand=" << demand * 1e12
            << " pF  capacity=" << capacity * 1e12
            << " pF  shortfall=" << result.total_shortfall * 1e12 << " pF"
            << std::endl;

  EXPECT_FALSE(result.feasible)
      << "a capacity-starved region was reported as satisfied";
  EXPECT_DOUBLE_EQ(regions[0].allocated, capacity);
  // The shortfall is exactly the 60% that did not fit -- reported, not hidden.
  EXPECT_NEAR(result.total_shortfall, demand * 0.6, demand * 1.0e-9);

  // And the starved region genuinely still violates the budget.
  const double simulated = SimulatedWorstDroop(
      20.0e-3, 0.06 / 20.0e-3, capacity, kT);
  EXPECT_GT(simulated, target)
      << "the starved region met the budget anyway, so the shortfall report "
         "is not measuring anything";
}

}  // namespace
}  // namespace psm
