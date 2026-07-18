// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, The OpenROAD Authors

// Unfakeable correctness gate for the metal density rule engine (FL1) in
// fin/density_check.h.
//
// The engine's job is one hand-computable division: over each window,
// density = filled_area / window_area, flagged UNDER when it falls below the
// per-layer minimum and OVER when it rises above the maximum.  Every case below
// feeds a window with a KNOWN filled area and a KNOWN window area, states the
// quotient in the comment, and asserts the engine reports exactly that density
// and the correct verdict.  Because this is the same classifier
// DensityCheck::check() drives with real polygon-union areas measured off the
// design, a pass here is a pass on the rule.

#include <string>
#include <vector>

#include "fin/density_check.h"
#include "gtest/gtest.h"

namespace fin {
namespace {

// A 100 um x 100 um window is 10000 um^2.  With 2500 um^2 of metal in it the
// hand-computed density is 2500 / 10000 = 0.25 exactly.
constexpr double kWindowArea = 100.0 * 100.0;  // 10000 um^2
constexpr double kFilled = 2500.0;             // um^2
constexpr double kDensity = 0.25;

DensityWindow makeWindow(const std::string& layer = "metal1",
                         double filled = kFilled,
                         double window = kWindowArea)
{
  DensityWindow w;
  w.layer = layer;
  w.filled_area_um2 = filled;
  w.window_area_um2 = window;
  return w;
}

TEST(DensityCheck, DensityIsFilledOverWindow)
{
  DensityLimits limits;
  limits.default_min = 0.0;
  limits.default_max = 1.0;  // the full band -> nothing can violate

  const DensityCheckResult res = classifyDensity({makeWindow()}, limits);
  ASSERT_EQ(res.windows.size(), 1u);
  // The load-bearing assertion: density == filled/window to the bit.
  EXPECT_DOUBLE_EQ(res.windows[0].density, kDensity);
  EXPECT_DOUBLE_EQ(res.windows[0].density, kFilled / kWindowArea);
  EXPECT_EQ(res.checked, 1u);
  EXPECT_EQ(res.limited, 1u);
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
}

TEST(DensityCheck, FlagsUnderAndOverIndependently)
{
  // Minimum ABOVE the hand-computed 0.25 -> UNDER-density violation.
  {
    DensityLimits limits;
    limits.default_min = 0.30;
    const DensityCheckResult res = classifyDensity({makeWindow()}, limits);
    ASSERT_EQ(res.windows.size(), 1u);
    EXPECT_TRUE(res.windows[0].violated_min);
    EXPECT_FALSE(res.windows[0].violated_max);
    EXPECT_EQ(res.violations, 1u);
    EXPECT_EQ(res.violations_min, 1u);
    EXPECT_EQ(res.violations_max, 0u);
    EXPECT_FALSE(res.pass());
  }
  // Maximum BELOW 0.25 -> OVER-density violation.
  {
    DensityLimits limits;
    limits.default_max = 0.20;
    const DensityCheckResult res = classifyDensity({makeWindow()}, limits);
    ASSERT_EQ(res.windows.size(), 1u);
    EXPECT_FALSE(res.windows[0].violated_min);
    EXPECT_TRUE(res.windows[0].violated_max);
    EXPECT_EQ(res.violations_max, 1u);
    EXPECT_FALSE(res.pass());
  }
  // A band that straddles 0.25 -> clean.
  {
    DensityLimits limits;
    limits.default_min = 0.20;
    limits.default_max = 0.30;
    const DensityCheckResult res = classifyDensity({makeWindow()}, limits);
    EXPECT_EQ(res.violations, 0u);
    EXPECT_TRUE(res.pass());
  }
}

TEST(DensityCheck, BoundsAreInclusive)
{
  // density == min exactly is NOT under-density; density == max exactly is NOT
  // over-density.  A window sitting precisely on the foundry bound passes.
  DensityLimits limits;
  limits.default_min = kDensity;
  limits.default_max = kDensity;
  const DensityCheckResult res = classifyDensity({makeWindow()}, limits);
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
}

TEST(DensityCheck, PerLayerBandsBeatDefault)
{
  // Two identical-density windows on different layers.  metal1 gets a tight
  // per-layer minimum (violated); metal2 falls back to a permissive default.
  std::vector<DensityWindow> windows{makeWindow("metal1"),
                                     makeWindow("metal2")};
  DensityLimits limits;
  limits.default_min = 0.0;
  limits.default_max = 1.0;
  limits.per_layer["metal1"] = {0.50, 1.0};

  const DensityCheckResult res = classifyDensity(windows, limits);
  EXPECT_EQ(res.checked, 2u);
  EXPECT_EQ(res.limited, 2u);
  EXPECT_EQ(res.violations, 1u);
  EXPECT_EQ(res.violations_min, 1u);
  ASSERT_EQ(res.windows.size(), 2u);
  EXPECT_TRUE(res.windows[0].violated_min);
  EXPECT_FALSE(res.windows[1].violated());
}

TEST(DensityCheck, TracksExtremesAcrossWindows)
{
  // Densities 0.10, 0.25 and 0.80 over the same 10000 um^2 window.
  std::vector<DensityWindow> windows{
      makeWindow("metal1", 1000.0),
      makeWindow("metal2", 2500.0),
      makeWindow("metal3", 8000.0),
  };
  DensityLimits limits;
  limits.default_min = 0.0;
  limits.default_max = 1.0;

  const DensityCheckResult res = classifyDensity(windows, limits);
  ASSERT_TRUE(res.any_limited);
  EXPECT_DOUBLE_EQ(res.min_density, 0.10);
  EXPECT_EQ(res.min_density_layer, "metal1");
  EXPECT_DOUBLE_EQ(res.max_density, 0.80);
  EXPECT_EQ(res.max_density_layer, "metal3");
  EXPECT_EQ(res.violations, 0u);
}

TEST(DensityCheck, OneSidedBandsOnlyCheckTheirSide)
{
  // A min-only band cannot produce an over-density verdict however dense the
  // window is, and vice versa.
  {
    DensityLimits limits;
    limits.default_min = 0.10;  // max stays -1 => unbounded above
    const DensityCheckResult res
        = classifyDensity({makeWindow("metal1", 9999.0)}, limits);
    EXPECT_EQ(res.violations, 0u);
  }
  {
    DensityLimits limits;
    limits.default_max = 0.90;  // min stays -1 => unbounded below
    const DensityCheckResult res
        = classifyDensity({makeWindow("metal1", 1.0)}, limits);
    EXPECT_EQ(res.violations, 0u);
  }
}

TEST(DensityCheck, DegenerateWindowNeverViolates)
{
  DensityLimits limits;
  limits.default_min = 0.99;  // would flag anything real
  std::vector<DensityWindow> windows{makeWindow("metal1", 0.0, 0.0),
                                     makeWindow("metal2", 5.0, -1.0)};
  const DensityCheckResult res = classifyDensity(windows, limits);
  EXPECT_EQ(res.total, 2u);
  EXPECT_EQ(res.checked, 0u);
  EXPECT_EQ(res.skipped_no_area, 2u);
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
}

TEST(DensityCheck, MissingBandCannotFail)
{
  // No band supplied at all: an entirely empty window cannot be turned into a
  // violation out of absent foundry data.
  DensityLimits limits;
  EXPECT_TRUE(limits.empty());

  const DensityCheckResult res
      = classifyDensity({makeWindow("metal1", 0.0)}, limits);
  EXPECT_EQ(res.checked, 1u);
  EXPECT_EQ(res.limited, 0u);
  EXPECT_EQ(res.skipped_no_limit, 1u);
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
}

}  // namespace
}  // namespace fin
