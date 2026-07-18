// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Regression gate for the routing-layer via-connectivity pre-check.
//
// A custom / incomplete PDK tech LEF can define routing layers while omitting
// the cut-layer via (or VIARULE) definitions that connect them.  Stock
// TritonRoute's initDefaultVias only errors (DRT-233/234) for a CUT layer that
// exists but lacks a via; when the cut layer is missing entirely -- a genuine
// gap between two routing layers -- nothing fires and detailed_route silently
// emits a DEF with SPECIALNETS but no +ROUTED signal geometry.
//
// firstUnconnectedRoutingPair() closes that gap: it reports the first adjacent
// routing-layer pair with no bridging cut-via so the router can raise a clear
// rule-file-requirement error (DRT-353) instead of routing nothing.

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "io/io.h"

namespace drt {
namespace {

io::RoutingLayerView routing(const char* name)
{
  io::RoutingLayerView v;
  v.name = name;
  v.is_routing = true;
  return v;
}

io::RoutingLayerView cut(const char* name, bool has_via)
{
  io::RoutingLayerView v;
  v.name = name;
  v.is_cut = true;
  v.has_default_via = has_via;
  return v;
}

TEST(RoutingViaConnectivity, FullyConnectedStackPasses)
{
  const std::vector<io::RoutingLayerView> layers = {routing("M1"),
                                                    cut("via1", true),
                                                    routing("M2"),
                                                    cut("via2", true),
                                                    routing("M3")};
  const auto gap = io::firstUnconnectedRoutingPair(layers);
  EXPECT_TRUE(gap.first.empty());
  EXPECT_TRUE(gap.second.empty());
}

TEST(RoutingViaConnectivity, SingleRoutingLayerNeedsNoVia)
{
  const std::vector<io::RoutingLayerView> layers = {routing("M1")};
  EXPECT_TRUE(io::firstUnconnectedRoutingPair(layers).first.empty());
}

// A missing cut layer between two routing layers (the silent stock case).
TEST(RoutingViaConnectivity, MissingCutLayerReported)
{
  const std::vector<io::RoutingLayerView> layers = {routing("M1"),
                                                    routing("M2")};
  const auto gap = io::firstUnconnectedRoutingPair(layers);
  EXPECT_EQ(gap.first, "M1");
  EXPECT_EQ(gap.second, "M2");
}

// A cut layer that exists but has no default via is equally disconnected.
TEST(RoutingViaConnectivity, CutWithoutViaReported)
{
  const std::vector<io::RoutingLayerView> layers = {
      routing("M1"), cut("via1", false), routing("M2")};
  const auto gap = io::firstUnconnectedRoutingPair(layers);
  EXPECT_EQ(gap.first, "M1");
  EXPECT_EQ(gap.second, "M2");
}

// The first gap higher in an otherwise-connected stack is the one reported.
TEST(RoutingViaConnectivity, ReportsFirstGapInStack)
{
  const std::vector<io::RoutingLayerView> layers = {routing("M1"),
                                                    cut("via1", true),
                                                    routing("M2"),
                                                    cut("via2", false),
                                                    routing("M3")};
  const auto gap = io::firstUnconnectedRoutingPair(layers);
  EXPECT_EQ(gap.first, "M2");
  EXPECT_EQ(gap.second, "M3");
}

}  // namespace
}  // namespace drt
