// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#pragma once

#include <string>
#include <vector>

#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "odb/db.h"
#include "sta/Scene.hh"
#include "utl/Logger.h"

namespace cts {

// One node of the post-CTS clock distribution tree.  The root is the clock
// source; interior nodes are the clock buffers/inverters/gaters the tree was
// built from; leaves are the sinks (register clock pins and macro clock pins).
struct ClockTreeNode
{
  int id = -1;
  int parent = -1;
  std::string name;
  // Input pin of this node's instance.  Null at the root, where the clock
  // enters the block through a bterm rather than through a cell.
  odb::dbITerm* inputTerm = nullptr;
  std::vector<int> children;
  bool isSink = false;
  // Rise arrival at inputTerm, one entry per scene, in the scene order
  // reported by scenes().  On the root this is 0 by definition -- it is the
  // point every path is measured from.
  std::vector<float> arrival;
  // Rise arrival at this instance's output pin.  Two sinks under the same
  // node share the driver's cell arc as well as everything above it, so this
  // -- not the input arrival -- is where their common path actually ends.
  std::vector<float> arrivalOut;
};

// Post-CTS analysis of the clock distribution tree that TritonCTS just built.
//
// Both analyses here read the tree back out of the db rather than out of the
// in-memory TreeBuilder graph, because the builders are torn down at the end
// of runTritonCts().  Reading the db also means these commands work on a tree
// that was read in from DEF, not just one built in this session.
class ClockTreeAnalysis
{
 public:
  ClockTreeAnalysis(utl::Logger* logger,
                    odb::dbDatabase* db,
                    sta::dbNetwork* network,
                    sta::dbSta* sta);

  // CT3: per-scene insertion delay and skew.  A tree that is balanced at the
  // typical scene is not necessarily balanced at the fast or slow one, so the
  // numbers are reported per scene rather than collapsed into one.  Verbose
  // adds the arrival at every individual sink.
  void reportSkew(bool verbose);

  // CT5: common-path pessimism credit earned by the tree's own topology.  For
  // a sink pair the credit is the arrival at their lowest common ancestor --
  // the part of the clock path both sinks physically share, which therefore
  // cannot contribute real skew between them.  Verbose adds every sink pair
  // with the shared node it was credited against.
  void reportCrprCredit(bool verbose);

  // The CT5 credit for one named sink pair, in seconds, so a caller can
  // assert on it instead of scraping the report.  sceneName may be empty for
  // the command scene.  Errors out if either pin is not a sink of the same
  // clock tree -- returning 0 for "not found" would be indistinguishable from
  // a genuine zero-length common path.
  double crprCreditBetween(const char* sinkPin1,
                           const char* sinkPin2,
                           const char* sceneName);

  // The CT3 skew of one clock tree at one scene, in seconds.
  double skewAtScene(const char* clockNetName, const char* sceneName);

  // The worst sink insertion delay of one clock tree at one scene, in
  // seconds.  Unlike skew, this is dominated by cell delay and so scales with
  // the corner, which is what makes it the useful per-scene number.
  double insertionDelayAtScene(const char* clockNetName,
                               const char* sceneName);

 private:
  void initSta();
  int sceneIndex(const char* sceneName) const;
  // Index into nodes_ of the sink whose input pin is named pinName, or -1.
  int findSink(const char* pinName) const;

  // Walks the clock fanout of clkNet into nodes_, rooted at the clock source.
  // Returns false if the net drives nothing the walk recognizes as a sink.
  bool buildTree(odb::dbNet* clkNet);
  void annotateArrivals(odb::dbNet* rootNet);
  float pinArrival(odb::dbITerm* iterm,
                   odb::dbNet* rootNet,
                   const sta::Scene* scene) const;
  int lowestCommonAncestor(int a, int b) const;
  int depth(int id) const;

  bool isSink(odb::dbITerm* iterm) const;
  bool propagateClock(odb::dbITerm* iterm) const;

  // Clock roots TritonCTS would have built trees for, i.e. the nets the sdc
  // clocks are defined on.
  std::vector<odb::dbNet*> clockRootNets() const;

  utl::Logger* logger_ = nullptr;
  odb::dbDatabase* db_ = nullptr;
  sta::dbNetwork* network_ = nullptr;
  sta::dbSta* openSta_ = nullptr;

  std::vector<ClockTreeNode> nodes_;
  std::vector<int> sinks_;
  sta::SceneSeq scenes_;
};

}  // namespace cts
