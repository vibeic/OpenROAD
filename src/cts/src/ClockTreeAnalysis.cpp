// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "ClockTreeAnalysis.h"

#include <algorithm>
#include <limits>
#include <stack>
#include <string>
#include <vector>

#include "sta/Clock.hh"
#include "sta/Graph.hh"
#include "sta/Liberty.hh"
#include "sta/Mode.hh"
#include "sta/PathExpanded.hh"
#include "sta/Sdc.hh"
#include "sta/Search.hh"

namespace cts {

using utl::CTS;

ClockTreeAnalysis::ClockTreeAnalysis(utl::Logger* logger,
                                     odb::dbDatabase* db,
                                     sta::dbNetwork* network,
                                     sta::dbSta* sta)
    : logger_(logger), db_(db), network_(network), openSta_(sta)
{
  scenes_ = openSta_->scenes();
}

void ClockTreeAnalysis::initSta()
{
  openSta_->ensureGraph();
  for (sta::Mode* mode : openSta_->modes()) {
    openSta_->ensureClkNetwork(mode);
  }
  openSta_->updateTiming(false);
}

bool ClockTreeAnalysis::isSink(odb::dbITerm* iterm) const
{
  odb::dbInst* inst = iterm->getInst();
  sta::LibertyCell* libertyCell
      = network_->libertyCell(network_->dbToSta(inst->getMaster()));
  if (!libertyCell) {
    return true;
  }
  if (inst->isBlock()) {
    return true;
  }
  sta::LibertyPort* inputPort
      = libertyCell->findLibertyPort(iterm->getMTerm()->getConstName());
  if (inputPort) {
    return inputPort->isRegClk();
  }
  return false;
}

bool ClockTreeAnalysis::propagateClock(odb::dbITerm* iterm) const
{
  odb::dbInst* inst = iterm->getInst();
  sta::LibertyCell* libertyCell
      = network_->libertyCell(network_->dbToSta(inst->getMaster()));
  if (!libertyCell) {
    return false;
  }
  if (libertyCell->isInverter() || libertyCell->isBuffer()) {
    return true;
  }
  if (!libertyCell->isSequential()) {
    return true;
  }
  sta::LibertyPort* inputPort
      = libertyCell->findLibertyPort(iterm->getMTerm()->getConstName());
  if (inputPort) {
    return inputPort->isClockGateClock() || inputPort->isLatchData();
  }
  return false;
}

std::vector<odb::dbNet*> ClockTreeAnalysis::clockRootNets() const
{
  std::vector<odb::dbNet*> roots;
  for (sta::Mode* mode : openSta_->modes()) {
    for (sta::Clock* clk : mode->sdc()->clocks()) {
      if (clk->isVirtual()) {
        continue;
      }
      for (const sta::Pin* pin : clk->leafPins()) {
        odb::dbITerm* instTerm;
        odb::dbBTerm* port;
        odb::dbModITerm* modIterm;
        network_->staToDb(pin, instTerm, port, modIterm);
        odb::dbNet* net = instTerm ? instTerm->getNet()
                                   : (port ? port->getNet() : nullptr);
        if (net && std::ranges::find(roots, net) == roots.end()) {
          roots.push_back(net);
        }
      }
    }
  }
  return roots;
}

bool ClockTreeAnalysis::buildTree(odb::dbNet* clkNet)
{
  nodes_.clear();
  sinks_.clear();

  ClockTreeNode root;
  root.id = 0;
  root.name = clkNet->getName();
  nodes_.push_back(root);

  std::stack<int> toVisit;
  toVisit.push(0);

  while (!toVisit.empty()) {
    const int driverId = toVisit.top();
    toVisit.pop();

    odb::dbNet* driverNet;
    if (nodes_[driverId].inputTerm != nullptr) {
      odb::dbITerm* out = nodes_[driverId].inputTerm->getInst()->getFirstOutput();
      if (!out || !out->getNet()) {
        continue;
      }
      driverNet = out->getNet();
    } else {
      driverNet = clkNet;
    }

    for (odb::dbITerm* iterm : driverNet->getITerms()) {
      if (iterm->getIoType() != odb::dbIoType::INPUT) {
        continue;
      }
      const bool sink = isSink(iterm);
      if (!sink && !propagateClock(iterm)) {
        continue;
      }
      const int id = nodes_.size();
      ClockTreeNode node;
      node.id = id;
      node.parent = driverId;
      node.name = iterm->getInst()->getName();
      node.inputTerm = iterm;
      node.isSink = sink;
      nodes_.push_back(node);
      nodes_[driverId].children.push_back(id);
      if (sink) {
        sinks_.push_back(id);
      } else {
        toVisit.push(id);
      }
    }
  }

  return !sinks_.empty();
}

float ClockTreeAnalysis::pinArrival(odb::dbITerm* iterm,
                                    odb::dbNet* rootNet,
                                    const sta::Scene* scene) const
{
  sta::Pin* pin = network_->dbToSta(iterm);
  if (!pin) {
    return 0.0;
  }
  sta::Vertex* vertex = openSta_->graph()->pinDrvrVertex(pin);
  if (!vertex) {
    return 0.0;
  }

  sta::VertexPathIterator pathIter(vertex, openSta_);
  while (pathIter.hasNext()) {
    sta::Path* path = pathIter.next();
    if (path->clkEdge(openSta_) == nullptr) {
      continue;
    }
    // One scene at a time: with several scenes loaded the vertex carries a
    // path per scene, and taking whichever comes first is what makes a
    // single-corner tool silently report one corner's number as if it were
    // every corner's.
    if (path->scene(openSta_) != scene) {
      continue;
    }
    if (path->clkEdge(openSta_)->transition() != sta::RiseFall::rise()) {
      continue;
    }
    if (path->minMax(openSta_) != sta::MinMax::max()) {
      continue;
    }
    if (path->clock(openSta_) == nullptr) {
      continue;
    }

    sta::PathExpanded expand(path, openSta_);
    const sta::Path* start = expand.startPath();
    odb::dbITerm* startTerm;
    odb::dbBTerm* startPort;
    odb::dbModITerm* startModIterm;
    network_->staToDb(start->pin(openSta_), startTerm, startPort, startModIterm);
    odb::dbNet* startNet = startTerm ? startTerm->getNet()
                                     : (startPort ? startPort->getNet() : nullptr);
    if (startNet == rootNet) {
      return path->arrival();
    }
  }
  return 0.0;
}

void ClockTreeAnalysis::annotateArrivals(odb::dbNet* rootNet)
{
  for (ClockTreeNode& node : nodes_) {
    if (node.inputTerm == nullptr) {
      // The root is the reference point; its arrival is 0 in every scene.
      node.arrival.assign(scenes_.size(), 0.0);
      node.arrivalOut.assign(scenes_.size(), 0.0);
      continue;
    }
    node.arrival.reserve(scenes_.size());
    for (const sta::Scene* scene : scenes_) {
      node.arrival.push_back(pinArrival(node.inputTerm, rootNet, scene));
    }
    odb::dbITerm* out = node.inputTerm->getInst()->getFirstOutput();
    if (node.isSink || out == nullptr) {
      // A sink has no clock output to continue from; it can never be the
      // shared node of a pair anyway, since it has no children.
      node.arrivalOut.assign(scenes_.size(), 0.0);
      continue;
    }
    node.arrivalOut.reserve(scenes_.size());
    for (const sta::Scene* scene : scenes_) {
      node.arrivalOut.push_back(pinArrival(out, rootNet, scene));
    }
  }
}

int ClockTreeAnalysis::depth(int id) const
{
  int d = 0;
  while (nodes_[id].parent != -1) {
    id = nodes_[id].parent;
    ++d;
  }
  return d;
}

int ClockTreeAnalysis::lowestCommonAncestor(int a, int b) const
{
  int da = depth(a);
  int db = depth(b);
  while (da > db) {
    a = nodes_[a].parent;
    --da;
  }
  while (db > da) {
    b = nodes_[b].parent;
    --db;
  }
  while (a != b) {
    a = nodes_[a].parent;
    b = nodes_[b].parent;
  }
  return a;
}

int ClockTreeAnalysis::sceneIndex(const char* sceneName) const
{
  if (sceneName == nullptr || sceneName[0] == '\0') {
    const sta::Scene* cmdScene = openSta_->cmdScene();
    for (size_t i = 0; i < scenes_.size(); ++i) {
      if (scenes_[i] == cmdScene) {
        return i;
      }
    }
    return 0;
  }
  for (size_t i = 0; i < scenes_.size(); ++i) {
    if (scenes_[i]->name() == sceneName) {
      return i;
    }
  }
  logger_->error(CTS, 248, "No timing scene named {}.", sceneName);
  return 0;
}

int ClockTreeAnalysis::findSink(const char* pinName) const
{
  for (const int sinkId : sinks_) {
    if (nodes_[sinkId].inputTerm->getName() == pinName) {
      return sinkId;
    }
  }
  return -1;
}

double ClockTreeAnalysis::crprCreditBetween(const char* sinkPin1,
                                            const char* sinkPin2,
                                            const char* sceneName)
{
  if (scenes_.empty()) {
    logger_->error(CTS, 249, "No timing scenes defined.");
  }
  initSta();
  const int scene = sceneIndex(sceneName);

  for (odb::dbNet* rootNet : clockRootNets()) {
    if (!buildTree(rootNet)) {
      continue;
    }
    const int a = findSink(sinkPin1);
    const int b = findSink(sinkPin2);
    if (a == -1 || b == -1) {
      continue;
    }
    annotateArrivals(rootNet);
    return nodes_[lowestCommonAncestor(a, b)].arrivalOut[scene];
  }

  logger_->error(CTS,
                 250,
                 "{} and {} are not both sinks of one clock tree.",
                 sinkPin1,
                 sinkPin2);
  return 0.0;
}

double ClockTreeAnalysis::skewAtScene(const char* clockNetName,
                                      const char* sceneName)
{
  if (scenes_.empty()) {
    logger_->error(CTS, 251, "No timing scenes defined.");
  }
  initSta();
  const int scene = sceneIndex(sceneName);

  for (odb::dbNet* rootNet : clockRootNets()) {
    if (rootNet->getName() != clockNetName) {
      continue;
    }
    if (!buildTree(rootNet)) {
      logger_->error(
          CTS, 252, "Clock net {} has no sinks; skew is undefined.",
          clockNetName);
    }
    annotateArrivals(rootNet);
    float minArr = std::numeric_limits<float>::max();
    float maxArr = std::numeric_limits<float>::lowest();
    for (const int sinkId : sinks_) {
      const float a = nodes_[sinkId].arrival[scene];
      minArr = std::min(minArr, a);
      maxArr = std::max(maxArr, a);
    }
    return maxArr - minArr;
  }

  logger_->error(CTS, 253, "{} is not a clock root net.", clockNetName);
  return 0.0;
}

double ClockTreeAnalysis::insertionDelayAtScene(const char* clockNetName,
                                                const char* sceneName)
{
  if (scenes_.empty()) {
    logger_->error(CTS, 254, "No timing scenes defined.");
  }
  initSta();
  const int scene = sceneIndex(sceneName);

  for (odb::dbNet* rootNet : clockRootNets()) {
    if (rootNet->getName() != clockNetName) {
      continue;
    }
    if (!buildTree(rootNet)) {
      logger_->error(CTS,
                     255,
                     "Clock net {} has no sinks; insertion delay is "
                     "undefined.",
                     clockNetName);
    }
    annotateArrivals(rootNet);
    float maxArr = std::numeric_limits<float>::lowest();
    for (const int sinkId : sinks_) {
      maxArr = std::max(maxArr, nodes_[sinkId].arrival[scene]);
    }
    return maxArr;
  }

  logger_->error(CTS, 256, "{} is not a clock root net.", clockNetName);
  return 0.0;
}

void ClockTreeAnalysis::reportSkew(bool verbose)
{
  const std::vector<odb::dbNet*> roots = clockRootNets();
  if (roots.empty()) {
    logger_->warn(CTS, 240, "No clock roots found; nothing to report.");
    return;
  }
  if (scenes_.empty()) {
    logger_->warn(CTS, 241, "No timing scenes defined; nothing to report.");
    return;
  }
  initSta();

  for (odb::dbNet* rootNet : roots) {
    if (!buildTree(rootNet)) {
      logger_->warn(CTS,
                    242,
                    "Clock net {} has no sinks; skew is undefined.",
                    rootNet->getName());
      continue;
    }
    annotateArrivals(rootNet);

    logger_->report("Clock net {} ({} sinks)", rootNet->getName(),
                    sinks_.size());
    for (size_t s = 0; s < scenes_.size(); ++s) {
      float minArr = std::numeric_limits<float>::max();
      float maxArr = std::numeric_limits<float>::lowest();
      for (const int sinkId : sinks_) {
        const float a = nodes_[sinkId].arrival[s];
        minArr = std::min(minArr, a);
        maxArr = std::max(maxArr, a);
      }
      logger_->report(
          "  scene {}: min insertion delay {:.4f} ns, max {:.4f} ns, skew "
          "{:.4f} ns",
          scenes_[s]->name(),
          minArr * 1e9,
          maxArr * 1e9,
          (maxArr - minArr) * 1e9);
      if (verbose) {
        for (const int sinkId : sinks_) {
          logger_->report("    sink {} arrival {:.4f} ns",
                          nodes_[sinkId].inputTerm->getName(),
                          nodes_[sinkId].arrival[s] * 1e9);
        }
      }
    }
  }
}

void ClockTreeAnalysis::reportCrprCredit(bool verbose)
{
  const std::vector<odb::dbNet*> roots = clockRootNets();
  if (roots.empty()) {
    logger_->warn(CTS, 243, "No clock roots found; nothing to report.");
    return;
  }
  if (scenes_.empty()) {
    logger_->warn(CTS, 244, "No timing scenes defined; nothing to report.");
    return;
  }
  initSta();

  for (odb::dbNet* rootNet : roots) {
    if (!buildTree(rootNet)) {
      logger_->warn(CTS,
                    245,
                    "Clock net {} has no sinks; no sink pairs to credit.",
                    rootNet->getName());
      continue;
    }
    if (sinks_.size() < 2) {
      // One sink is not a pair.  Reporting a 0 credit here would read as "no
      // pessimism to remove" when the truth is "no pair exists to remove it
      // between", so say which one it is.
      logger_->report("Clock net {}: 1 sink, no sink pairs.",
                      rootNet->getName());
      continue;
    }
    annotateArrivals(rootNet);

    logger_->report("Clock net {} ({} sinks)", rootNet->getName(),
                    sinks_.size());
    for (size_t s = 0; s < scenes_.size(); ++s) {
      if (verbose) {
        logger_->report("  scene {} sink pairs:", scenes_[s]->name());
      }
      float minCredit = std::numeric_limits<float>::max();
      float maxCredit = std::numeric_limits<float>::lowest();
      double sumCredit = 0.0;
      size_t pairs = 0;
      for (size_t i = 0; i < sinks_.size(); ++i) {
        for (size_t j = i + 1; j < sinks_.size(); ++j) {
          const int lca = lowestCommonAncestor(sinks_[i], sinks_[j]);
          // The credit is the arrival where the two paths finally split --
          // the shared driver's output pin.  Everything up to there is one
          // physical path travelled twice, so it cannot separate the sinks
          // and must not be charged as skew.
          const float credit = nodes_[lca].arrivalOut[s];
          minCredit = std::min(minCredit, credit);
          maxCredit = std::max(maxCredit, credit);
          sumCredit += credit;
          ++pairs;
          if (verbose) {
            logger_->report("    {} / {} share {} credit {:.4f} ns",
                            nodes_[sinks_[i]].inputTerm->getName(),
                            nodes_[sinks_[j]].inputTerm->getName(),
                            nodes_[lca].name,
                            credit * 1e9);
          }
        }
      }
      logger_->report(
          "  scene {}: {} sink pairs, common-path credit min {:.4f} ns, max "
          "{:.4f} ns, avg {:.4f} ns",
          scenes_[s]->name(),
          pairs,
          minCredit * 1e9,
          maxCredit * 1e9,
          (sumCredit / pairs) * 1e9);
    }
  }
}

}  // namespace cts
