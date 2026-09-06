// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "RoutedStateSnapshot.h"

#include <vector>

#include "utl/Logger.h"

namespace grt {

RoutedStateSnapshot::RoutedStateSnapshot(odb::dbBlock* block,
                                         utl::Logger* logger)
    : block_(block), logger_(logger)
{
  for (odb::dbInst* inst : block_->getInsts()) {
    insts_[inst] = InstState{
        inst->getOrigin(), inst->getOrient(), inst->getPlacementStatus()};
  }

  for (odb::dbNet* net : block_->getNets()) {
    if (net->isSpecial()) {
      // Special (power/ground) nets are not touched by antenna repair or by
      // the incremental reroute; snapping them would only cost memory.
      continue;
    }
    NetState state;
    state.wire_ordered = net->isWireOrdered();
    state.has_jumpers = net->hasJumpers();
    odb::dbWire* wire = net->getWire();
    if (wire != nullptr) {
      state.has_wire = true;
      const int length = static_cast<int>(wire->length());
      state.opcodes.reserve(length);
      state.data.reserve(length);
      for (int i = 0; i < length; i++) {
        state.opcodes.push_back(wire->getOpcode(i));
        state.data.push_back(wire->getData(i));
      }
    }
    for (odb::dbGuide* guide : net->getGuides()) {
      state.guides.push_back(GuideState{guide->getLayer(),
                                        guide->getViaLayer(),
                                        guide->getBox(),
                                        guide->isCongested(),
                                        guide->isJumper(),
                                        guide->isConnectedToTerm()});
    }
    nets_[net] = std::move(state);
  }
}

bool RoutedStateSnapshot::restore()
{
  // Refuse before touching anything if the block drifted in a way this
  // snapshot cannot describe. Antenna repair only ADDS instances and only
  // rewrites wires, so either of these means something else edited the
  // design and a "restore" would be a guess.
  std::vector<odb::dbInst*> to_destroy;
  int live_snapped_insts = 0;
  for (odb::dbInst* inst : block_->getInsts()) {
    if (insts_.find(inst) == insts_.end()) {
      to_destroy.push_back(inst);
    } else {
      live_snapped_insts++;
    }
  }
  if (live_snapped_insts != static_cast<int>(insts_.size())) {
    logger_->warn(utl::GRT,
                  314,
                  "Cannot restore the routed-state snapshot: it recorded {} "
                  "instances and only {} of them still exist. Nothing was "
                  "changed.",
                  insts_.size(),
                  live_snapped_insts);
    return false;
  }
  for (const auto& [net, state] : nets_) {
    if (block_->findNet(net->getConstName()) != net) {
      logger_->warn(utl::GRT,
                    315,
                    "Cannot restore the routed-state snapshot: net {} no "
                    "longer exists. Nothing was changed.",
                    net->getConstName());
      return false;
    }
  }

  // Wires first: they carry ITerm references to the instances that are about
  // to be destroyed, so dropping them before the instances leaves no window
  // in which a wire names an object that is already gone.
  for (const auto& [net, state] : nets_) {
    odb::dbWire* wire = net->getWire();
    if (wire != nullptr) {
      odb::dbWire::destroy(wire);
    }
    net->clearGuides();
  }

  for (odb::dbInst* inst : to_destroy) {
    odb::dbInst::destroy(inst);
  }

  // Only instances that actually moved are touched. setOrigin() refuses to
  // move a FIRM/LOCKED instance, so a mover has to clear the status first --
  // doing that to every instance would fire a placement callback for each of
  // the thousands that never moved.
  int moved = 0;
  for (const auto& [inst, state] : insts_) {
    const bool same = inst->getOrigin() == state.origin
                      && inst->getOrient() == state.orient
                      && inst->getPlacementStatus() == state.status;
    if (same) {
      continue;
    }
    inst->setPlacementStatus(odb::dbPlacementStatus::NONE);
    inst->setOrient(state.orient);
    inst->setOrigin(state.origin.x(), state.origin.y());
    inst->setPlacementStatus(state.status);
    moved++;
  }

  for (const auto& [net, state] : nets_) {
    if (state.has_wire) {
      odb::dbWire* new_wire = odb::dbWire::create(net);
      const int length = static_cast<int>(state.opcodes.size());
      for (int i = 0; i < length; i++) {
        new_wire->addOneSeg(state.opcodes[i], state.data[i]);
      }
      net->setWireOrdered(state.wire_ordered);
    }
    net->setJumpers(state.has_jumpers);
    for (const GuideState& guide : state.guides) {
      odb::dbGuide* new_guide = odb::dbGuide::create(
          net, guide.layer, guide.via_layer, guide.box, guide.is_congested);
      new_guide->setIsJumper(guide.is_jumper);
      new_guide->setIsConnectedToTerm(guide.is_connected_to_term);
    }
  }

  logger_->info(utl::GRT,
                316,
                "Restored the routed state: destroyed {} instance(s) created "
                "since the snapshot, moved {} back, rewrote {} net(s).",
                to_destroy.size(),
                moved,
                nets_.size());
  return true;
}

}  // namespace grt
