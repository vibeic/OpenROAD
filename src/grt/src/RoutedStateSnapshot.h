// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "odb/PtrSetMap.h"
#include "odb/db.h"
#include "odb/geom.h"

namespace utl {
class Logger;
}

namespace grt {

// A restorable copy of everything one `repair_antennas` pass plus the
// incremental `detailed_route` that realizes it can change:
//
//   * the SET of instances (a repair pass CREATES diode instances),
//   * where each surviving instance sits and its placement status
//     (the pass legalizes with the detailed placer),
//   * every net's detailed wire, verbatim, opcode for opcode,
//   * every net's route guides and its wire/jumper flags.
//
// It exists because the -reroute loop could previously only move FORWARD.
// A pass that made the antenna count WORSE was still the state the design
// shipped with, because there was nothing to go back to. Measured on a
// 15,690-instance design: the loop entered its second antenna stage with 2
// violating nets, saw 2 again on four of its six passes, and shipped 3.
//
// Restore is byte-level for the wires: the snapshot keeps the raw
// (opcode, data) arrays read back through dbWire's own public accessors and
// replays them through dbWire::addOneSeg, so a restored wire is identical to
// the wire that was snapped rather than a re-encoding of it.
class RoutedStateSnapshot
{
 public:
  RoutedStateSnapshot(odb::dbBlock* block, utl::Logger* logger);

  // Put the block back the way it was when this object was constructed.
  // Returns false and changes NOTHING if the block has drifted in a way this
  // snapshot cannot describe (an instance it recorded no longer exists) --
  // a refusal with a reason, never a partial restore.
  bool restore();

  int instCount() const { return static_cast<int>(insts_.size()); }
  int netCount() const { return static_cast<int>(nets_.size()); }

 private:
  struct InstState
  {
    odb::Point origin;
    odb::dbOrientType orient;
    odb::dbPlacementStatus status;
  };

  struct GuideState
  {
    odb::dbTechLayer* layer;
    odb::dbTechLayer* via_layer;
    odb::Rect box;
    bool is_congested;
    bool is_jumper;
    bool is_connected_to_term;
  };

  struct NetState
  {
    bool has_wire{false};
    std::vector<unsigned char> opcodes;
    std::vector<int> data;
    bool wire_ordered{false};
    bool has_jumpers{false};
    std::vector<GuideState> guides;
  };

  odb::dbBlock* block_;
  utl::Logger* logger_;
  // PtrMap, not std::map: odb deletes std::less for its pointer types so a
  // container cannot accidentally order by address. These are ordered by
  // object id, which is stable across runs.
  odb::PtrMap<odb::dbInst, InstState> insts_;
  odb::PtrMap<odb::dbNet, NetState> nets_;
};

}  // namespace grt
