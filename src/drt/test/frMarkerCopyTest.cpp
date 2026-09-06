// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Regression gate: copying an frMarker must not throw away the two rectangles
// that say what the violation is between.
//
// frMarker's hand-written copy constructor used to copy constraint_, bbox_,
// layerNum_, srcs_, vioHasDir_ and vioIsH_ and silently drop victims_ and
// aggressors_, while its copy-ASSIGNMENT was `= default` and kept them. Nothing
// in the router copies a marker by assignment on the way to the design, and
// everything copies one by construction:
//
//   TritonRoute::getDRCMarkers      std::make_unique<frMarker>(*marker)
//   FlexDRWorker::endAddMarkers     std::make_unique<frMarker>(m)
//   FlexDRWorker::setMarkers        markers_.push_back(marker)
//
// so every marker in the block, in the DRC report and in the GUI marker browser
// arrived with no victim and no aggressor, and
// FlexDRWorker::route_queue_update_from_marker's movableAggressorNets loop
// iterated an always-empty container. `std::vector::operator=` also picks
// between element assignment and element construction depending on capacity, so
// the two disagreeing copies could give bestMarkers_ different contents for the
// same input.
//
// This pins both copies to the same answer.

#include "db/obj/frMarker.h"

#include <utility>

#include "gtest/gtest.h"
#include "odb/geom.h"

namespace drt {
namespace {

// A marker of the shape the NS-Metal check produces: one same-net junction
// between a fixed cell-pin rectangle and a non-fixed routed rectangle.
frMarker makeMarker()
{
  frMarker m;
  m.setBBox(odb::Rect(100, 200, 180, 310));
  m.setLayerNum(2);
  auto* owner = reinterpret_cast<frBlockObject*>(0x1);
  m.addSrc(owner);
  m.addVictim(owner,
              std::make_tuple(2, odb::Rect(100, 200, 560, 1160), true));
  m.addAggressor(owner,
                 std::make_tuple(2, odb::Rect(-580, 200, 180, 720), false));
  return m;
}

void expectRectsKept(frMarker& copy)
{
  ASSERT_EQ(copy.getVictims().size(), 1u);
  ASSERT_EQ(copy.getAggressors().size(), 1u);

  const auto& v = copy.getVictims().front().second;
  EXPECT_EQ(std::get<0>(v), 2);
  EXPECT_EQ(std::get<1>(v), odb::Rect(100, 200, 560, 1160));
  EXPECT_TRUE(std::get<2>(v));

  const auto& a = copy.getAggressors().front().second;
  EXPECT_EQ(std::get<0>(a), 2);
  EXPECT_EQ(std::get<1>(a), odb::Rect(-580, 200, 180, 720));
  EXPECT_FALSE(std::get<2>(a));
}

TEST(FrMarkerCopy, CopyConstructionKeepsVictimAndAggressor)
{
  frMarker original = makeMarker();
  frMarker copy(original);
  expectRectsKept(copy);
}

TEST(FrMarkerCopy, CopyAssignmentKeepsVictimAndAggressor)
{
  frMarker original = makeMarker();
  frMarker copy;
  copy = original;
  expectRectsKept(copy);
}

// The two copies must agree. They did not, and which one a std::vector used
// depended on its capacity.
TEST(FrMarkerCopy, ConstructionAndAssignmentAgree)
{
  frMarker original = makeMarker();
  frMarker constructed(original);
  frMarker assigned;
  assigned = original;
  EXPECT_EQ(constructed.getVictims().size(), assigned.getVictims().size());
  EXPECT_EQ(constructed.getAggressors().size(),
            assigned.getAggressors().size());
  EXPECT_EQ(constructed.getBBox(), assigned.getBBox());
  EXPECT_EQ(constructed.getLayerNum(), assigned.getLayerNum());
  EXPECT_EQ(constructed.getSrcs().size(), assigned.getSrcs().size());
}

}  // namespace
}  // namespace drt
