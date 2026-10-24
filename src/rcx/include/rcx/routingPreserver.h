// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2026, The OpenROAD Authors

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace odb {
class dbBlock;
class dbNet;
}  // namespace odb

namespace utl {
class Logger;
}

namespace rcx {

// vibeic fork: EXTRACTION MUST NOT CHANGE THE ROUTING IT MEASURES.
//
// Parasitic extraction begins by calling odb::normalizeWireConnectivity(),
// which walks every non-supply net through WireNormalizer and REWRITES its
// dbWire into a normalized, chained path. The RC tree walk needs that
// connectivity. The caller's routing does not survive it.
//
// MEASURED on a detail-routed gf180mcuD design (655 signal nets), with nothing
// else in the session -- read_db, define_process_corner, extract_parasitics,
// write_db:
//
//     whole-design DRC before extract_parasitics ...... 0 violation(s)
//     whole-design DRC after  extract_parasitics ...... 599 violation(s)
//                                                       (547 Short,
//                                                        45 Metal Spacing,
//                                                        7 NS Metal)
//     dbWire entry count ............................. 32,634 -> 17,824
//
// The same 599 are reported by two independent builds of the checker, survive a
// write_db/read_db round trip, and are absent from the database one step
// earlier -- real geometry, created by the re-encode. Downstream it is silent:
// the flow that extracted parasitics to do post-route timing repair went on to
// ship the corrupted route.
//
// So the wire stream of every net is saved before the ordering and replayed
// afterwards through dbWire::addOneSeg -- odb's own documented way to "save and
// restore routing without re-encoding it". The parasitics are untouched:
// write_spef produces a byte-identical SPEF either side of the restore (only
// its *DATE line differs). The wire's normalized-connectivity flag is restored
// too, so a later consumer that needs normalized wires still normalizes them
// for itself.
//
// This does NOT fix WireNormalizer's re-encode, which is the underlying defect
// and is not this class's to repair. It stops that defect from reaching the
// caller's database.
//
// ONE instance covers as many blocks as it is given, because the multi-chip
// path orders EVERY die's block and only then extracts across them -- restoring
// per-die inside that loop would put the routing back before the inter-chip
// pass has read it. Snapshot each block as it is about to be ordered; the
// destructor restores them all, so extract()'s early returns and an exception
// are covered too.
class RoutingPreserver
{
 public:
  explicit RoutingPreserver(utl::Logger* logger);
  ~RoutingPreserver();

  RoutingPreserver(const RoutingPreserver&) = delete;
  RoutingPreserver& operator=(const RoutingPreserver&) = delete;

  // Record every net in `block` whose wire the ordering may rewrite. Safe to
  // call for several blocks and safe to call with nullptr.
  void snapshot(odb::dbBlock* block);

  // What the destructor would restore right now. Exposed so a test can assert
  // the class is able to fail -- a preserver that restores nothing is
  // indistinguishable from one that works, on a run where nothing moved.
  int changedCount() const;

 private:
  struct Saved
  {
    odb::dbNet* net{nullptr};
    bool normalized_connectivity{false};
    std::vector<std::pair<unsigned char, int>> stream;
  };

  static bool sameStream(odb::dbNet* net,
                         const std::vector<std::pair<unsigned char, int>>& s);

  utl::Logger* logger_{nullptr};
  std::vector<Saved> saved_;
};

}  // namespace rcx
