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
  // Unknown geometry never violates, but a segment that was not judged is
  // never a clean result either: the verdict is NOT_MEASURED, not PASS.
  EXPECT_FALSE(res.pass());
  EXPECT_STREQ(res.verdict(), "NOT_MEASURED");
  EXPECT_EQ(res.no_area_by_layer.at("metal2"), 1u);
  EXPECT_EQ(res.no_area_by_layer.at("metal3"), 1u);
  EXPECT_EQ(res.wires.size(), 3u);  // the skipped ones are listed, not dropped
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
  // A missing limit cannot manufacture a FAIL -- nor a PASS: the segment was
  // not judged, so the verdict is NOT_MEASURED.
  EXPECT_FALSE(res.pass());
  EXPECT_STREQ(res.verdict(), "NOT_MEASURED");
  EXPECT_EQ(res.no_limit_by_layer.at("metal1"), 1u);
}

TEST(EMSignoff, ViaIsJudgedPerCutAgainstItsCutLayer)
{
  // A 4-cut via carrying 4e-4 A: 1e-4 A per cut.
  EMWireCurrent via{"via1", 4.0e-4, 0.0, true, 0, 0, 0, 0};
  via.cuts = 4;
  EMLimits limits;
  limits.per_cut["via1"] = 1.5e-4;  // A per cut
  EMSignoffResult res = classifyCurrentDensity({via}, limits);
  EXPECT_EQ(res.checked, 1u);
  EXPECT_EQ(res.per_cut, 1u);
  EXPECT_EQ(res.violations, 0u);
  EXPECT_TRUE(res.pass());
  EXPECT_EQ(res.wires.at(0).basis, "PER_CUT");
  EXPECT_DOUBLE_EQ(res.wires.at(0).j, 1.0e-4);

  limits.per_cut["via1"] = 0.9e-4;  // below the per-cut current
  res = classifyCurrentDensity({via}, limits);
  EXPECT_EQ(res.violations, 1u);
  EXPECT_STREQ(res.verdict(), "FAIL");
}

TEST(EMSignoff, ViaWithAreaIsJudgedOnItsCutArea)
{
  // No per-cut limit: an areal limit applies to J = I / (cuts x cut area).
  EMWireCurrent via{"via1", 4.0e-4, 4 * 0.0676, true, 0, 0, 0, 0};
  via.cuts = 4;
  EMLimits limits;
  limits.per_layer["via1"] = 1.0e-3;  // A/um^2
  const EMSignoffResult res = classifyCurrentDensity({via}, limits);
  EXPECT_EQ(res.wires.at(0).basis, "AREAL");
  EXPECT_NEAR(res.wires.at(0).j, 4.0e-4 / (4 * 0.0676), 1e-12);
  EXPECT_EQ(res.violations, 1u);
}

// Drawn width (DBU) of the metal carrying a segment: the narrowest position
// along the segment, each position as wide as its widest covering shape.

TEST(EMDrawnWidth, ChainOfAbuttingShapesIsJudgedOnItsNarrowestLink)
{
  // Three abutting pin rectangles along x, the middle one narrow.  Neither
  // end nor the midpoint of the segment lies in the narrow link, and no
  // single rectangle contains both ends.
  const std::vector<EMDrawnRect> chain{{0, 0, 10000, 1000},
                                       {10000, 0, 11000, 300},
                                       {11000, 0, 30000, 1000}};
  EXPECT_EQ(emDrawnWidthAcross(chain, 1000, 150, 29000, 150), 300);
  // Same chain along y.
  const std::vector<EMDrawnRect> column{{0, 0, 1000, 10000},
                                        {0, 10000, 300, 11000},
                                        {0, 11000, 1000, 30000}};
  EXPECT_EQ(emDrawnWidthAcross(column, 150, 1000, 150, 29000), 300);
}

TEST(EMDrawnWidth, TwoLinkChainIsJudgedOnTheNarrowLinkNotTheMidpoint)
{
  // The midpoint (x=8000) lies in the wide shape; the segment still crosses
  // into the narrow one.
  const std::vector<EMDrawnRect> chain{{0, 0, 12000, 1000},
                                       {12000, 0, 20000, 200}};
  EXPECT_EQ(emDrawnWidthAcross(chain, 2000, 100, 14000, 100), 200);
}

TEST(EMDrawnWidth, WireUnderANarrowerEnclosureIsJudgedOnTheWire)
{
  // A via enclosure sits on a 600-wide wire; it is narrower across the wire
  // but the metal there is the wire.
  const std::vector<EMDrawnRect> shapes{{0, 0, 20000, 600},
                                        {5000, 50, 6000, 550}};
  EXPECT_EQ(emDrawnWidthAcross(shapes, 5200, 300, 5800, 300), 600);
  EXPECT_EQ(emDrawnWidthAcross(shapes, 1000, 300, 9000, 300), 600);
}

TEST(EMDrawnWidth, UncoveredStretchHasNoArea)
{
  const std::vector<EMDrawnRect> gap{{0, 0, 1000, 500}, {2000, 0, 3000, 500}};
  EXPECT_EQ(emDrawnWidthAcross(gap, 500, 250, 2500, 250), 0);
  EXPECT_EQ(emDrawnWidthAcross({}, 0, 0, 100, 0), 0);
}

TEST(EMDrawnWidth, SingleShapeAndPointSegment)
{
  const std::vector<EMDrawnRect> wire{{0, 0, 10000, 400}};
  EXPECT_EQ(emDrawnWidthAcross(wire, 100, 200, 9900, 200), 400);
  EXPECT_EQ(emDrawnWidthAcross(wire, 5000, 200, 5000, 200), 400);
}

}  // namespace
}  // namespace psm
