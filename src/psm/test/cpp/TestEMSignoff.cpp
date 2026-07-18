// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, The OpenROAD Authors

// Unfakeable correctness gate for the EM current-density signoff rule engine
// (EM3) in em_signoff.h.
//
// The engine's job is a single, hand-computable rule: for every current-carrying
// segment, the DC current density is J = I / A, and the segment is a violation
// iff J exceeds the per-layer limit.  Every case below feeds a wire with a KNOWN
// current and a KNOWN cross-section area, computes J by hand, and asserts that
// the engine reports exactly that J and the correct PASS/FAIL verdict.  Because
// this is the same classifier IRSolver::checkCurrentDensity() drives with real
// solved currents + real LEF geometry, a pass here is a pass on the rule.

#include <cmath>
#include <vector>

#include "gtest/gtest.h"
#include "psm/em_signoff.h"

namespace psm {
namespace {

// A wire carrying I = 1.0 mA through a 0.10 um x 0.20 um cross-section has a
// hand-computed current density J = 1e-3 / (0.10 * 0.20) = 0.05 A/um^2.
constexpr double kI = 1.0e-3;     // A
constexpr double kW = 0.10;       // um
constexpr double kT = 0.20;       // um
constexpr double kArea = kW * kT;  // 0.02 um^2
constexpr double kJ = kI / kArea;  // 0.05 A/um^2

TEST(EMSignoff, JEqualsCurrentOverArea)
{
  std::vector<EMWireCurrent> wires;
  wires.push_back({"metal1", kI, kArea, false, 0, 0, 0, 0});

  EMLimits limits;
  limits.default_limit = 1.0;  // A/um^2, well above J -> must PASS

  const EMSignoffResult res = classifyCurrentDensity(wires, limits);

  ASSERT_EQ(res.wires.size(), 1u);
  // The load-bearing assertion: J == I/A to the bit.
  EXPECT_DOUBLE_EQ(res.wires[0].j, kJ);
  EXPECT_DOUBLE_EQ(res.wires[0].j, kI / kArea);
  EXPECT_EQ(res.checked, 1u);
  EXPECT_EQ(res.limited, 1u);
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
}

TEST(EMSignoff, FlagsOverLimitAndPassesUnderLimit)
{
  std::vector<EMWireCurrent> wires;
  wires.push_back({"metal1", kI, kArea, false, 0, 0, 0, 0});

  // Limit just BELOW the hand-computed J -> the wire must be flagged.
  {
    EMLimits limits;
    limits.default_limit = kJ * 0.5;  // 0.025 A/um^2 < 0.05 -> VIOLATED
    const EMSignoffResult res = classifyCurrentDensity(wires, limits);
    EXPECT_EQ(res.violations, 1u);
    EXPECT_FALSE(res.pass());
    ASSERT_EQ(res.wires.size(), 1u);
    EXPECT_TRUE(res.wires[0].violated);
    // Utilization ratio is exactly J/limit = 0.05 / 0.025 = 2.0.
    EXPECT_DOUBLE_EQ(res.wires[0].ratio, 2.0);
    EXPECT_DOUBLE_EQ(res.worst_ratio, 2.0);
    EXPECT_EQ(res.worst_layer, "metal1");
  }

  // Limit just ABOVE the hand-computed J -> the wire must pass.
  {
    EMLimits limits;
    limits.default_limit = kJ * 2.0;  // 0.10 A/um^2 > 0.05 -> OK
    const EMSignoffResult res = classifyCurrentDensity(wires, limits);
    EXPECT_EQ(res.violations, 0u);
    EXPECT_TRUE(res.pass());
    ASSERT_EQ(res.wires.size(), 1u);
    EXPECT_FALSE(res.wires[0].violated);
    EXPECT_DOUBLE_EQ(res.wires[0].ratio, 0.5);
  }
}

TEST(EMSignoff, PerLayerLimitsBeatDefault)
{
  // Two identical-J wires on different layers.  metal1 gets a tight per-layer
  // limit (violated); metal2 falls back to a loose default (ok).
  std::vector<EMWireCurrent> wires;
  wires.push_back({"metal1", kI, kArea, false, 0, 0, 0, 0});
  wires.push_back({"metal2", kI, kArea, false, 0, 0, 0, 0});

  EMLimits limits;
  limits.default_limit = kJ * 2.0;      // loose default -> metal2 ok
  limits.per_layer["metal1"] = kJ * 0.5;  // tight -> metal1 violated

  const EMSignoffResult res = classifyCurrentDensity(wires, limits);
  EXPECT_EQ(res.checked, 2u);
  EXPECT_EQ(res.limited, 2u);
  EXPECT_EQ(res.violations, 1u);
  EXPECT_EQ(res.worst_layer, "metal1");
}

TEST(EMSignoff, ZeroCurrentAndUnknownGeometryNeverViolate)
{
  std::vector<EMWireCurrent> wires;
  // Zero-current wire: J = 0 -> can never exceed any positive limit.
  wires.push_back({"metal1", 0.0, kArea, false, 0, 0, 0, 0});
  // Unknown geometry (area <= 0): skipped, not a violation, not "checked".
  wires.push_back({"metal2", kI, 0.0, false, 0, 0, 0, 0});
  wires.push_back({"metal3", kI, -1.0, false, 0, 0, 0, 0});

  EMLimits limits;
  limits.default_limit = 1e-9;  // absurdly tight, but J==0 still passes

  const EMSignoffResult res = classifyCurrentDensity(wires, limits);
  EXPECT_EQ(res.total, 3u);
  EXPECT_EQ(res.checked, 1u);          // only the zero-current wire had geometry
  EXPECT_EQ(res.skipped_no_area, 2u);  // the two area<=0 wires
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
}

TEST(EMSignoff, MissingLimitCannotFail)
{
  // No limits supplied at all: every checked segment is NO_LIMIT, so however
  // large J is, the signoff cannot manufacture a violation (fail-safe honesty).
  std::vector<EMWireCurrent> wires;
  wires.push_back({"metal1", 1.0e6, kArea, false, 0, 0, 0, 0});  // huge J

  EMLimits limits;  // default_limit == 0, no per-layer entries
  EXPECT_TRUE(limits.empty());

  const EMSignoffResult res = classifyCurrentDensity(wires, limits);
  EXPECT_EQ(res.checked, 1u);
  EXPECT_EQ(res.limited, 0u);
  EXPECT_EQ(res.skipped_no_limit, 1u);
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
}

}  // namespace
}  // namespace psm
