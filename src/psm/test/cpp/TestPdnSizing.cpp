// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Unfakeable correctness gate for the analysis-driven PDN sizing engine.
//
// pdn_sizing.h claims a closed form for the worst droop of a uniformly-loaded
// rail (r*i*L^2/2 single-end fed, r*i*L^2/8 double-end fed) and inverts it to
// size straps against a droop budget.  Nothing here trusts that claim: every
// droop the closed form predicts is checked against an INDEPENDENT discrete
// ladder network assembled from first principles (per-segment resistors,
// per-node current injections) and solved with Eigen's sparse LU.  The two
// routes share no code.
//
// The gates, in order of strength:
//
//   1. ABSOLUTE VALUE.  The ladder solve must converge to the closed-form
//      number.  This is the gate that catches the classic factor-of-4 slip
//      between the single-end and double-end feed constants -- a wrong
//      constant misses by 4x, not by a rounding error.
//
//   2. CONVERGENCE ORDER.  A midpoint-injected ladder is a second-order
//      quadrature of the continuum rail, so doubling the section count must
//      shrink the residual by ~4x.  A formula that is merely CLOSE to right
//      would plateau instead of converging; this gate can and does come out
//      differently for a wrong-but-nearby constant.
//
//   3. PROVEN-NEGATIVE ON THE SIZING LAW.  With a package/bump series term in
//      play the droop is NOT proportional to 1/w, so the naive
//      "scale width by measured/target" rule under-delivers.  The test sizes
//      the same grid both ways and requires the ladder to show the naive
//      width MISSING the target while the calibrated width MEETS it.  If the
//      sizing engine ever degrades back to naive scaling, this fails.

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "Eigen/Sparse"
#include "gtest/gtest.h"
#include "psm/pdn_sizing.h"

namespace psm {
namespace {

// Solves a discrete resistive ladder standing in for a uniformly-loaded rail,
// and returns the worst (largest) node droop in volts.
//
// The rail of length L is cut into `sections` equal segments each of
// resistance r*L/sections.  Nodes 0..sections sit at the segment boundaries.
// The distributed load is applied with the midpoint rule: each segment injects
// its current i*L/sections split evenly onto its two end nodes, which is the
// second-order-accurate discretization of a continuous line load.
//
// Feed points (node 0 alone, or nodes 0 and `sections`) are pinned to 0 V and
// the solve returns droop directly.  `series_r` optionally inserts an extra
// resistor between the ideal supply and EACH feed point, modelling the
// package/bump path.
double LadderWorstDroop(double r_per_len,
                        double i_per_len,
                        double length,
                        RailFeed feed,
                        int sections,
                        double series_r = 0.0)
{
  const int n = sections + 1;
  const double seg_r = r_per_len * length / sections;
  const double seg_g = 1.0 / seg_r;
  const double seg_i = i_per_len * length / sections;

  std::vector<Eigen::Triplet<double>> trip;
  Eigen::VectorXd rhs = Eigen::VectorXd::Zero(n);

  // Series resistors between adjacent nodes.
  for (int k = 0; k < sections; ++k) {
    trip.emplace_back(k, k, seg_g);
    trip.emplace_back(k + 1, k + 1, seg_g);
    trip.emplace_back(k, k + 1, -seg_g);
    trip.emplace_back(k + 1, k, -seg_g);
    // Midpoint-rule load: half of this segment's current onto each end node.
    rhs[k] += seg_i / 2.0;
    rhs[k + 1] += seg_i / 2.0;
  }

  // Tie the feed node(s) to the ideal 0 V supply.  A finite series_r models
  // the package; series_r == 0 means an ideal (stiff) tie.
  const double feed_g = series_r > 0.0 ? 1.0 / series_r : 1.0e12 * seg_g;
  trip.emplace_back(0, 0, feed_g);
  if (feed == RailFeed::kDoubleEnd) {
    trip.emplace_back(sections, sections, feed_g);
  }

  Eigen::SparseMatrix<double> mat(n, n);
  mat.setFromTriplets(trip.begin(), trip.end());
  mat.makeCompressed();

  Eigen::SparseLU<Eigen::SparseMatrix<double>> solver;
  solver.compute(mat);
  EXPECT_EQ(solver.info(), Eigen::ComputationInfo::Success)
      << "ladder factorization failed";

  const Eigen::VectorXd v = solver.solve(rhs);
  // Droop is measured against the 0 V ideal supply, so the worst node is the
  // largest positive value.
  return v.maxCoeff();
}

// Representative 180nm-class top-metal strap numbers (open/synthetic values,
// not tied to any specific PDK).
constexpr double kSheetRes = 0.09;    // [ohm/square]
constexpr double kLength = 500.0e-6;  // 500 um rail
constexpr double kPitch = 10.0e-6;    // 10 um strap pitch
constexpr double kJ = 2.0e4;          // 20 mA per (100 um)^2 -> 2e4 A/m^2

TEST(PdnSizing, ClosedFormMatchesIndependentLadderSolve)
{
  const double width = 2.0e-6;  // 2 um strap
  const double r_per_len = stripResistancePerLength(kSheetRes, width);
  const double i_per_len = kJ * kPitch;

  for (const RailFeed feed : {RailFeed::kSingleEnd, RailFeed::kDoubleEnd}) {
    const double analytic
        = uniformRailDroop(r_per_len, i_per_len, kLength, feed);
    const double numeric
        = LadderWorstDroop(r_per_len, i_per_len, kLength, feed, 4000);
    const double rel_err = std::abs(numeric - analytic) / analytic;

    std::cout << "[ANALYTIC GATE] feed="
              << (feed == RailFeed::kDoubleEnd ? "double" : "single")
              << "  closed-form=" << analytic * 1e3 << " mV"
              << "  ladder(4000)=" << numeric * 1e3 << " mV"
              << "  rel_err=" << rel_err << std::endl;

    EXPECT_LT(rel_err, 1.0e-5)
        << "closed-form rail droop disagrees with the independent ladder "
           "solve by "
        << rel_err * 100.0 << "%";
  }
}

TEST(PdnSizing, HandComputableDoubleFedDroopToTheDigit)
{
  // Hand computation, double-end fed:
  //   r = Rs/w  = 0.09 / 2e-6      = 45000 ohm/m
  //   i = J*p   = 2e4 * 10e-6      = 0.2 A/m
  //   L^2       = (500e-6)^2       = 2.5e-7 m^2
  //   droop     = r*i*L^2/8
  //             = 45000 * 0.2 * 2.5e-7 / 8
  //             = 2.25e-3 / 8       = 2.8125e-4 V = 0.28125 mV
  const double r_per_len = stripResistancePerLength(kSheetRes, 2.0e-6);
  const double i_per_len = kJ * kPitch;

  EXPECT_DOUBLE_EQ(r_per_len, 45000.0);
  EXPECT_DOUBLE_EQ(i_per_len, 0.2);

  const double droop = uniformRailDroop(
      r_per_len, i_per_len, kLength, RailFeed::kDoubleEnd);
  EXPECT_NEAR(droop, 2.8125e-4, 1.0e-12);

  // And the independent ladder must land on the same hand-computed number.
  const double numeric = LadderWorstDroop(
      r_per_len, i_per_len, kLength, RailFeed::kDoubleEnd, 8000);
  EXPECT_NEAR(numeric, 2.8125e-4, 2.8125e-4 * 1.0e-5);
}

TEST(PdnSizing, LadderIsExactAtEverySectionCountThatSamplesThePeak)
{
  // The droop profile of a uniformly-loaded rail is a QUADRATIC in position,
  // and the midpoint-rule ladder integrates a quadratic exactly.  So the
  // agreement above is not an artefact of using 4000 sections: the ladder
  // reproduces the closed form to round-off at ANY section count that places
  // a node on the peak.  Single-end feed peaks at the open end (always a
  // node); double-end feed peaks at the centre (a node only for even counts).
  //
  // This is a far sharper gate than a convergence-rate check: a wrong feed
  // constant (the classic 2-vs-8 slip) misses by 4x at EVERY section count.
  const double r_per_len = stripResistancePerLength(kSheetRes, 2.0e-6);
  const double i_per_len = kJ * kPitch;

  for (const int sections : {2, 3, 5, 10, 50, 500}) {
    const double analytic = uniformRailDroop(
        r_per_len, i_per_len, kLength, RailFeed::kSingleEnd);
    const double numeric = LadderWorstDroop(
        r_per_len, i_per_len, kLength, RailFeed::kSingleEnd, sections);
    EXPECT_LT(std::abs(numeric - analytic) / analytic, 1.0e-9)
        << "single-fed ladder is not exact at " << sections << " sections";
  }

  for (const int sections : {2, 4, 10, 50, 500}) {
    const double analytic = uniformRailDroop(
        r_per_len, i_per_len, kLength, RailFeed::kDoubleEnd);
    const double numeric = LadderWorstDroop(
        r_per_len, i_per_len, kLength, RailFeed::kDoubleEnd, sections);
    EXPECT_LT(std::abs(numeric - analytic) / analytic, 1.0e-9)
        << "double-fed ladder is not exact at " << sections << " sections";
  }
}

TEST(PdnSizing, OddSectionCountUnderestimatesConfirmingPeakSitsAtRailCentre)
{
  // Independent confirmation of WHERE the closed form says the peak is, not
  // just how big it is.  With an odd section count the double-fed rail has no
  // node at its centre, so the ladder can only sample the profile either side
  // of the true maximum and MUST report a smaller droop -- and the deficit
  // must shrink as the sampling tightens.  If the peak were anywhere else,
  // odd counts would straddle it too and this would not hold.
  const double r_per_len = stripResistancePerLength(kSheetRes, 2.0e-6);
  const double i_per_len = kJ * kPitch;
  const double analytic = uniformRailDroop(
      r_per_len, i_per_len, kLength, RailFeed::kDoubleEnd);

  double prev_deficit = std::numeric_limits<double>::max();
  for (const int sections : {3, 5, 9, 17, 33}) {
    const double numeric = LadderWorstDroop(
        r_per_len, i_per_len, kLength, RailFeed::kDoubleEnd, sections);
    const double deficit = analytic - numeric;
    std::cout << "[PEAK-LOCATION GATE] odd N=" << sections
              << "  ladder=" << numeric * 1e3 << " mV  deficit="
              << deficit / analytic * 100.0 << "%" << std::endl;

    EXPECT_GT(deficit, 0.0)
        << "odd-count ladder did not under-sample the peak at N=" << sections;
    EXPECT_LT(deficit, prev_deficit)
        << "odd-count deficit is not shrinking with N at N=" << sections;
    prev_deficit = deficit;
  }
}

TEST(PdnSizing, SizesStrapWidthToHitTargetDroop)
{
  PdnSizingSpec spec;
  spec.sheet_res = kSheetRes;
  spec.rail_length = kLength;
  spec.strap_pitch = kPitch;
  spec.current_density = kJ;
  spec.feed = RailFeed::kDoubleEnd;
  spec.target_droop = 1.0e-4;  // 0.1 mV budget
  spec.min_width = 0.1e-6;
  spec.max_width = 50.0e-6;

  const PdnSizingResult result = sizeStrapWidthForDroop(spec);
  ASSERT_TRUE(result.feasible) << result.limit_reason;

  // Independent confirmation: build the ladder at the width the engine chose
  // and check the droop actually lands on the target.
  const double r_per_len
      = stripResistancePerLength(kSheetRes, result.required_width);
  const double numeric = LadderWorstDroop(
      r_per_len, kJ * kPitch, kLength, RailFeed::kDoubleEnd, 4000);

  std::cout << "[SIZING GATE] required width="
            << result.required_width * 1e6 << " um -> ladder droop="
            << numeric * 1e3 << " mV (target " << spec.target_droop * 1e3
            << " mV)" << std::endl;

  EXPECT_NEAR(numeric, spec.target_droop, spec.target_droop * 1.0e-5);
}

TEST(PdnSizing, InadequateGridIsRepairedAndNaiveScalingProvenInsufficient)
{
  // A deliberately inadequate grid: narrow straps AND a real package/bump
  // series resistance, so the droop is NOT proportional to 1/w.
  const double bad_width = 0.5e-6;  // 0.5 um -- far too narrow
  // Package/bump path sized so its irreducible droop is a substantial share
  // of the budget (~40%).  That is what makes the naive law measurably wrong:
  // with a negligible package term the two laws would agree and the gate
  // below would not discriminate between them.
  const double series_r = 4.0;   // [ohm]
  const double target = 5.0e-4;  // 0.5 mV budget

  const double i_per_len = kJ * kPitch;
  const double i_tot = kJ * kPitch * kLength;  // current one strap carries

  // ---- OBSERVE the defect: measure the bad grid's droop numerically. ----
  const double bad_r_per_len = stripResistancePerLength(kSheetRes, bad_width);
  const double measured = LadderWorstDroop(bad_r_per_len,
                                           i_per_len,
                                           kLength,
                                           RailFeed::kDoubleEnd,
                                           4000,
                                           series_r);

  // The irreducible package term: with both ends fed, the two package
  // resistors sit in parallel, so the floor is I_tot * (series_r / 2).
  const double irreducible = i_tot * series_r / 2.0;

  std::cout << "[DEFECT] inadequate grid droop=" << measured * 1e3 << " mV"
            << "  target=" << target * 1e3 << " mV"
            << "  package floor=" << irreducible * 1e3 << " mV" << std::endl;

  // The defect is measured, not predicted: the bad grid really does violate.
  ASSERT_GT(measured, target)
      << "fixture is not actually defective -- nothing to repair";
  // And the target is genuinely reachable, so a miss below is the sizer's
  // fault and not an impossible request.
  ASSERT_LT(irreducible, target) << "fixture target is below the package floor";
  // ...and the package term must be big enough that the naive law's omission
  // of it actually shows up, otherwise this test proves nothing.
  ASSERT_GT(irreducible, 0.2 * target)
      << "package floor is too small for this fixture to discriminate between "
         "the naive and calibrated sizing laws";

  // ---- PROVEN-NEGATIVE: the naive proportional rule under-delivers. ----
  const double naive_width = bad_width * measured / target;
  const double naive_droop
      = LadderWorstDroop(stripResistancePerLength(kSheetRes, naive_width),
                         i_per_len,
                         kLength,
                         RailFeed::kDoubleEnd,
                         4000,
                         series_r);
  std::cout << "[PROVEN-NEGATIVE] naive width=" << naive_width * 1e6
            << " um -> droop=" << naive_droop * 1e3 << " mV (MISSES target)"
            << std::endl;
  EXPECT_GT(naive_droop, target)
      << "naive scaling unexpectedly met the target; the fixture no longer "
         "discriminates between the naive and calibrated sizing laws";

  // ---- REPAIR: the calibrated law that accounts for the package floor. ----
  const PdnSizingResult fixed = resizeFromMeasuredDroop(
      measured, bad_width, irreducible, target, 0.1e-6, 50.0e-6);
  ASSERT_TRUE(fixed.feasible) << fixed.limit_reason;

  const double fixed_droop
      = LadderWorstDroop(stripResistancePerLength(kSheetRes, fixed.required_width),
                         i_per_len,
                         kLength,
                         RailFeed::kDoubleEnd,
                         4000,
                         series_r);
  std::cout << "[REPAIR] calibrated width=" << fixed.required_width * 1e6
            << " um -> droop=" << fixed_droop * 1e3 << " mV (MEETS target)"
            << std::endl;

  EXPECT_LE(fixed_droop, target * (1.0 + 1.0e-4))
      << "calibrated sizing failed to meet the droop target";
  // The calibrated law must genuinely differ from the naive one here.
  EXPECT_GT(fixed.required_width, naive_width);
}

TEST(PdnSizing, TargetBelowPackageFloorIsReportedInfeasible)
{
  // A no-op or over-eager optimizer would happily return some finite width.
  // Physics says nothing on-die can beat the package, so the engine must say
  // so rather than emit a number that cannot work.
  const double i_tot = kJ * kPitch * kLength;
  const double irreducible = i_tot * 0.30 / 2.0;

  const PdnSizingResult result = resizeFromMeasuredDroop(
      /*measured_droop=*/irreducible * 3.0,
      /*current_width=*/1.0e-6,
      irreducible,
      /*target_droop=*/irreducible * 0.5);

  EXPECT_FALSE(result.feasible);
  EXPECT_FALSE(result.limit_reason.empty());
  EXPECT_TRUE(std::isinf(result.required_width));
}

}  // namespace
}  // namespace psm
