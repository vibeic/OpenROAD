// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, The OpenROAD Authors

// Unfakeable correctness gate for the signal-net EM rule engine (EM4) in
// signal_em.h.
//
// The engine's job is a closed-form, hand-computable model.  For a net with load
// capacitance C driven rail-to-rail over swing V, switching N times per second
// with transition time tr:
//
//     i_peak    = C * V / tr
//     duty      = min(1, N * tr)
//     i_avg_abs = i_peak * duty       == C * V * N
//     i_rms     = i_peak * sqrt(duty) == C * V * sqrt(N / tr)
//     i_avg_uni = i_avg_abs / 2       (the current alternates direction)
//
// and then, per routed segment of cross-section A, J = I / A per family.
//
// Every case below feeds KNOWN C, V, N, tr and A, states the arithmetic result
// in the comment, and asserts the engine reproduces exactly that number and the
// correct PASS/FAIL verdict.  Because this is the same classifier
// PDNSim::checkSignalEM() drives with real OpenSTA load caps / activities /
// slews and real LEF geometry, a pass here is a pass on the rule.

#include <cmath>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "psm/signal_em.h"

namespace psm {
namespace {

// The worked example used throughout, chosen so every intermediate is exact:
//
//   C  = 1.0e-14 F   (10 fF)
//   V  = 1.0 V
//   tr = 1.0e-10 s   (100 ps)
//   N  = 2.5e9 transitions/s
//
//   i_peak    = 1.0e-14 * 1.0 / 1.0e-10 = 1.0e-4 A   (100 uA)
//   duty      = 2.5e9 * 1.0e-10         = 0.25
//   i_avg_abs = 1.0e-4 * 0.25           = 2.5e-5 A   (== C*V*N = 2.5e-5)
//   i_rms     = 1.0e-4 * sqrt(0.25)     = 5.0e-5 A
//   i_avg_uni = 2.5e-5 / 2              = 1.25e-5 A
constexpr double kC = 1.0e-14;   // F
constexpr double kV = 1.0;       // V
constexpr double kTr = 1.0e-10;  // s
constexpr double kN = 2.5e9;     // transitions/s

constexpr double kIPeak = 1.0e-4;   // A
constexpr double kDuty = 0.25;      // -
constexpr double kIAvgAbs = 2.5e-5; // A
constexpr double kIRms = 5.0e-5;    // A
constexpr double kIAvgUni = 1.25e-5;  // A

// A 0.10 um wide wire on a 0.20 um thick layer: A = 0.02 um^2.  Hence
//   J_peak = 1.0e-4  / 0.02 = 5.00e-3 A/um^2
//   J_rms  = 5.0e-5  / 0.02 = 2.50e-3 A/um^2
//   J_avg  = 1.25e-5 / 0.02 = 6.25e-4 A/um^2
constexpr double kArea = 0.10 * 0.20;  // 0.02 um^2
constexpr double kJPeak = 5.00e-3;
constexpr double kJRms = 2.50e-3;
constexpr double kJAvg = 6.25e-4;

constexpr double kRelTol = 1e-12;

SignalEMDrive nominalDrive()
{
  SignalEMDrive d;
  d.cap_f = kC;
  d.supply_v = kV;
  d.density_hz = kN;
  d.transition_s = kTr;
  return d;
}

SignalEMNet nominalNet(const std::string& layer = "metal1",
                       double area_um2 = kArea)
{
  SignalEMNet net;
  net.name = "n1";
  net.drive = nominalDrive();
  SignalEMSegment seg;
  seg.layer = layer;
  seg.area_um2 = area_um2;
  net.segments.push_back(seg);
  return net;
}

TEST(SignalEM, SwitchingCurrentsMatchHandComputedModel)
{
  const SignalEMCurrents c = computeSwitchingCurrents(nominalDrive());
  ASSERT_TRUE(c.valid);

  EXPECT_NEAR(c.i_peak, kIPeak, kIPeak * kRelTol);
  EXPECT_NEAR(c.duty, kDuty, kDuty * kRelTol);
  EXPECT_NEAR(c.i_avg_abs, kIAvgAbs, kIAvgAbs * kRelTol);
  EXPECT_NEAR(c.i_rms, kIRms, kIRms * kRelTol);
  EXPECT_NEAR(c.i_avg_uni, kIAvgUni, kIAvgUni * kRelTol);

  // The same numbers by the alternate closed forms: i_avg_abs == C*V*N and
  // i_rms == C*V*sqrt(N/tr).  Agreement of two independent expressions is what
  // makes the model, not the code path, the thing under test.
  EXPECT_NEAR(c.i_avg_abs, kC * kV * kN, kIAvgAbs * kRelTol);
  EXPECT_NEAR(c.i_rms, kC * kV * std::sqrt(kN / kTr), kIRms * kRelTol);
}

TEST(SignalEM, RmsIsGeometricMeanOfAverageAndPeak)
{
  // i_rms = i_peak*sqrt(duty) and i_avg_abs = i_peak*duty, so identically
  // i_rms == sqrt(i_avg_abs * i_peak).  This holds for ANY drive, so sweep.
  for (const double density : {1.0e6, 1.0e8, 2.5e9, 4.0e9}) {
    for (const double tr : {1.0e-11, 5.0e-11, 1.0e-10}) {
      SignalEMDrive d = nominalDrive();
      d.density_hz = density;
      d.transition_s = tr;
      const SignalEMCurrents c = computeSwitchingCurrents(d);
      ASSERT_TRUE(c.valid);
      const double geo = std::sqrt(c.i_avg_abs * c.i_peak);
      EXPECT_NEAR(c.i_rms, geo, std::max(geo, 1e-30) * 1e-9)
          << "density=" << density << " tr=" << tr;
      // and RMS is always bracketed by the average and the peak
      EXPECT_LE(c.i_avg_abs, c.i_rms + 1e-30);
      EXPECT_LE(c.i_rms, c.i_peak + 1e-30);
    }
  }
}

TEST(SignalEM, BidirectionalAverageIsHalfTheAbsoluteAverage)
{
  // This is the whole reason a signal net is not a power net: the charge that
  // goes up comes back down, so the mass-transport-relevant current in EITHER
  // direction is half the mean |i|.
  const SignalEMCurrents c = computeSwitchingCurrents(nominalDrive());
  EXPECT_NEAR(c.i_avg_uni, 0.5 * c.i_avg_abs, kIAvgUni * kRelTol);
}

TEST(SignalEM, JEqualsCurrentOverAreaPerFamily)
{
  std::vector<SignalEMNet> nets{nominalNet()};
  SignalEMLimits limits;
  // Loose limits in all three families -> must PASS, but J is still reported.
  limits.avg.default_limit = 1.0;
  limits.rms.default_limit = 1.0;
  limits.peak.default_limit = 1.0;

  const SignalEMResult res = classifySignalEM(nets, limits);
  ASSERT_EQ(res.segments.size(), 1u);
  const SignalEMSegmentResult& s = res.segments[0];

  EXPECT_NEAR(s.j_peak, kJPeak, kJPeak * kRelTol);
  EXPECT_NEAR(s.j_rms, kJRms, kJRms * kRelTol);
  EXPECT_NEAR(s.j_avg, kJAvg, kJAvg * kRelTol);
  // J == I/A to the bit, per family.
  EXPECT_DOUBLE_EQ(s.j_peak, s.i_peak / kArea);
  EXPECT_DOUBLE_EQ(s.j_rms, s.i_rms / kArea);
  EXPECT_DOUBLE_EQ(s.j_avg, s.i_avg / kArea);

  EXPECT_EQ(res.nets, 1u);
  EXPECT_EQ(res.nets_driven, 1u);
  EXPECT_EQ(res.checked, 1u);
  EXPECT_EQ(res.limited, 1u);
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
}

TEST(SignalEM, EachFamilyFlagsIndependently)
{
  std::vector<SignalEMNet> nets{nominalNet()};

  // RMS limit set to exactly half the hand-computed J_rms -> RMS violated with
  // utilization exactly 2.0; avg and peak left loose -> not violated.
  {
    SignalEMLimits limits;
    limits.avg.default_limit = 1.0;
    limits.peak.default_limit = 1.0;
    limits.rms.default_limit = kJRms * 0.5;  // 1.25e-3

    const SignalEMResult res = classifySignalEM(nets, limits);
    ASSERT_EQ(res.segments.size(), 1u);
    EXPECT_TRUE(res.segments[0].violated_rms);
    EXPECT_FALSE(res.segments[0].violated_avg);
    EXPECT_FALSE(res.segments[0].violated_peak);
    EXPECT_NEAR(res.segments[0].ratio_rms, 2.0, 1e-12);
    EXPECT_EQ(res.violations, 1u);
    EXPECT_EQ(res.violations_rms, 1u);
    EXPECT_EQ(res.violations_avg, 0u);
    EXPECT_EQ(res.violations_peak, 0u);
    EXPECT_FALSE(res.pass());
    EXPECT_EQ(res.worst_mode, SignalEMMode::kRms);
    EXPECT_EQ(res.worst_layer, "metal1");
    EXPECT_EQ(res.worst_net, "n1");
    EXPECT_NEAR(res.worst_ratio, 2.0, 1e-12);
  }

  // Peak limit at a quarter of J_peak -> peak violated, utilization 4.0, and it
  // dominates the worst-mode reporting.
  {
    SignalEMLimits limits;
    limits.avg.default_limit = 1.0;
    limits.rms.default_limit = 1.0;
    limits.peak.default_limit = kJPeak * 0.25;  // 1.25e-3

    const SignalEMResult res = classifySignalEM(nets, limits);
    ASSERT_EQ(res.segments.size(), 1u);
    EXPECT_TRUE(res.segments[0].violated_peak);
    EXPECT_NEAR(res.segments[0].ratio_peak, 4.0, 1e-12);
    EXPECT_EQ(res.violations, 1u);
    EXPECT_EQ(res.violations_peak, 1u);
    EXPECT_EQ(res.worst_mode, SignalEMMode::kPeak);
  }

  // Avg limit at a fifth of J_avg -> avg violated, utilization 5.0.
  {
    SignalEMLimits limits;
    limits.rms.default_limit = 1.0;
    limits.peak.default_limit = 1.0;
    limits.avg.default_limit = kJAvg * 0.2;  // 1.25e-4

    const SignalEMResult res = classifySignalEM(nets, limits);
    ASSERT_EQ(res.segments.size(), 1u);
    EXPECT_TRUE(res.segments[0].violated_avg);
    EXPECT_NEAR(res.segments[0].ratio_avg, 5.0, 1e-12);
    EXPECT_EQ(res.violations_avg, 1u);
    EXPECT_EQ(res.worst_mode, SignalEMMode::kAvg);
  }
}

TEST(SignalEM, DutyIsClampedAtUnity)
{
  // N*tr = 1e11 * 1e-10 = 10 > 1: transitions cannot overlap, so the wire is
  // carrying i_peak continuously and all three currents collapse onto i_peak.
  SignalEMDrive d = nominalDrive();
  d.density_hz = 1.0e11;
  const SignalEMCurrents c = computeSwitchingCurrents(d);
  ASSERT_TRUE(c.valid);
  EXPECT_DOUBLE_EQ(c.duty, 1.0);
  EXPECT_DOUBLE_EQ(c.i_avg_abs, c.i_peak);
  EXPECT_DOUBLE_EQ(c.i_rms, c.i_peak);
  EXPECT_NEAR(c.i_peak, kIPeak, kIPeak * kRelTol);
  // Without the clamp the unphysical model would report i_avg_abs = C*V*N =
  // 1.0e-3 A, ten times the peak the driver can actually deliver.
  EXPECT_LT(c.i_avg_abs, kC * kV * d.density_hz);
}

TEST(SignalEM, NonSwitchingNetCarriesNoCurrent)
{
  SignalEMDrive d = nominalDrive();
  d.density_hz = 0.0;  // a tied-off / constant net
  const SignalEMCurrents c = computeSwitchingCurrents(d);
  EXPECT_TRUE(c.valid);  // inputs were fine; the answer is simply zero
  EXPECT_DOUBLE_EQ(c.i_peak, 0.0);
  EXPECT_DOUBLE_EQ(c.i_rms, 0.0);
  EXPECT_DOUBLE_EQ(c.i_avg_abs, 0.0);

  SignalEMNet net = nominalNet();
  net.drive = d;
  SignalEMLimits limits;
  limits.rms.default_limit = 1e-12;  // absurdly tight
  const SignalEMResult res = classifySignalEM({net}, limits);
  EXPECT_EQ(res.checked, 1u);
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
}

TEST(SignalEM, MissingDriveOrGeometryNeverViolates)
{
  SignalEMLimits limits;
  limits.rms.default_limit = 1e-12;  // absurdly tight

  // No load capacitance extracted for this net -> skipped, not a violation.
  {
    SignalEMNet net = nominalNet();
    net.drive.cap_f = 0.0;
    const SignalEMResult res = classifySignalEM({net}, limits);
    EXPECT_EQ(res.total, 1u);
    EXPECT_EQ(res.checked, 0u);
    EXPECT_EQ(res.skipped_no_drive, 1u);
    EXPECT_EQ(res.nets_driven, 0u);
    EXPECT_TRUE(res.pass());
  }
  // No transition time (unannotated slew) -> skipped.
  {
    SignalEMNet net = nominalNet();
    net.drive.transition_s = 0.0;
    const SignalEMResult res = classifySignalEM({net}, limits);
    EXPECT_EQ(res.skipped_no_drive, 1u);
    EXPECT_TRUE(res.pass());
  }
  // Routed segment with no usable cross-section -> skipped.
  {
    SignalEMNet net = nominalNet("metal1", 0.0);
    const SignalEMResult res = classifySignalEM({net}, limits);
    EXPECT_EQ(res.checked, 0u);
    EXPECT_EQ(res.skipped_no_area, 1u);
    EXPECT_TRUE(res.pass());
  }
}

TEST(SignalEM, MissingLimitCannotFail)
{
  // No limits supplied at all: however large J is, the signoff cannot
  // manufacture a violation out of absent foundry data.
  SignalEMNet net = nominalNet("metal1", 1e-9);  // vanishing area -> huge J
  SignalEMLimits limits;
  EXPECT_TRUE(limits.empty());

  const SignalEMResult res = classifySignalEM({net}, limits);
  EXPECT_EQ(res.checked, 1u);
  EXPECT_EQ(res.limited, 0u);
  EXPECT_EQ(res.skipped_no_limit, 1u);
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
}

TEST(SignalEM, PerLayerLimitsBeatDefault)
{
  // Two identical segments on different layers; metal1 gets a tight per-layer
  // RMS limit, metal2 falls back to a loose default.
  SignalEMNet net = nominalNet();
  SignalEMSegment m2;
  m2.layer = "metal2";
  m2.area_um2 = kArea;
  net.segments.push_back(m2);

  SignalEMLimits limits;
  limits.rms.default_limit = kJRms * 2.0;
  limits.rms.per_layer["metal1"] = kJRms * 0.5;

  const SignalEMResult res = classifySignalEM({net}, limits);
  EXPECT_EQ(res.checked, 2u);
  EXPECT_EQ(res.limited, 2u);
  EXPECT_EQ(res.violations, 1u);
  EXPECT_EQ(res.worst_layer, "metal1");
  EXPECT_NEAR(res.worst_ratio, 2.0, 1e-12);
}

TEST(SignalEM, CurrentFractionScalesEverySegment)
{
  // A segment carrying half the net current sees exactly half the J in every
  // family -- the hook the per-segment downstream-capacitance model uses.
  SignalEMNet net = nominalNet();
  net.segments[0].current_fraction = 0.5;

  SignalEMLimits limits;
  limits.avg.default_limit = 1.0;
  limits.rms.default_limit = 1.0;
  limits.peak.default_limit = 1.0;

  const SignalEMResult res = classifySignalEM({net}, limits);
  ASSERT_EQ(res.segments.size(), 1u);
  EXPECT_NEAR(res.segments[0].j_peak, 0.5 * kJPeak, kJPeak * kRelTol);
  EXPECT_NEAR(res.segments[0].j_rms, 0.5 * kJRms, kJRms * kRelTol);
  EXPECT_NEAR(res.segments[0].j_avg, 0.5 * kJAvg, kJAvg * kRelTol);
}

}  // namespace
}  // namespace psm
