// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2026, The OpenROAD Authors

// rcx::RoutingPreserver -- the mechanism BOTH extraction paths depend on to
// leave the caller's routing alone across odb::normalizeWireConnectivity().
//
// The repo has NO multi-chip (HIER) testcase, so the multi-chip call site
// cannot be exercised end to end here. What is exercised is the exact shape
// that site uses: ONE preserver given SEVERAL blocks, restoring all of them
// when it goes out of scope. The single-chip path's end-to-end proof is the
// tcl test rcx/test/extract_preserves_routing, which runs a real
// extract_parasitics and compares every net's raw wire stream by membership.
//
// The negative control is a COMMITTED TEST CASE, not an edit to make by hand:
// MutationStandsWithoutAPreserver performs the same mutation with no preserver
// in scope and asserts it SURVIVES. If the mutation ever stopped changing the
// wire, that test would fail -- so PreserverRestoresTheWire cannot pass
// vacuously.

#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "odb/db.h"
#include "odb/dbWireCodec.h"
#include "rcx/routingPreserver.h"
#include "utl/Logger.h"

namespace rcx {

namespace {

struct Fixture
{
  utl::Logger logger;
  odb::dbDatabase* db{nullptr};
  odb::dbTechLayer* met1{nullptr};

  Fixture()
  {
    db = odb::dbDatabase::create();
    db->setLogger(&logger);
    odb::dbTech* tech = odb::dbTech::create(db, "tech");
    met1
        = odb::dbTechLayer::create(tech, "met1", odb::dbTechLayerType::ROUTING);
    met1->setWidth(100);
    met1->setPitch(200);
  }

  ~Fixture() { odb::dbDatabase::destroy(db); }

  odb::dbBlock* makeBlock(const char* chip_name, const char* block_name)
  {
    odb::dbChip* chip = odb::dbChip::create(db, db->getTech(), chip_name);
    odb::dbBlock* block = odb::dbBlock::create(chip, block_name);
    block->setDieArea(odb::Rect(0, 0, 100000, 100000));
    return block;
  }

  // A net carrying a two-segment wire, which is what a re-encode rewrites.
  odb::dbNet* makeRoutedNet(odb::dbBlock* block, const char* name)
  {
    odb::dbNet* net = odb::dbNet::create(block, name);
    odb::dbWire* wire = odb::dbWire::create(net);
    odb::dbWireEncoder encoder;
    encoder.begin(wire);
    encoder.newPath(met1, odb::dbWireType::ROUTED);
    encoder.addPoint(1000, 1000);
    encoder.addPoint(9000, 1000);
    encoder.newPath(met1, odb::dbWireType::ROUTED);
    encoder.addPoint(9000, 1000);
    encoder.addPoint(9000, 7000);
    encoder.end();
    return net;
  }
};

std::vector<std::pair<unsigned char, int>> stream(odb::dbNet* net)
{
  std::vector<std::pair<unsigned char, int>> out;
  odb::dbWire* wire = net->getWire();
  if (wire == nullptr) {
    return out;
  }
  for (uint32_t i = 0; i < wire->length(); i++) {
    out.emplace_back(wire->getOpcode(i), wire->getData(i));
  }
  return out;
}

// Stands in for what WireNormalizer does to a wire: destroy it and lay the same
// connectivity down as a DIFFERENT encoding. The test asserts the bytes moved,
// so this can never silently become a no-op.
void reencode(odb::dbNet* net, odb::dbTechLayer* layer)
{
  odb::dbWire* wire = net->getWire();
  ASSERT_NE(wire, nullptr);
  odb::dbWire::destroy(wire);
  wire = odb::dbWire::create(net);
  odb::dbWireEncoder encoder;
  encoder.begin(wire);
  encoder.newPath(layer, odb::dbWireType::ROUTED);
  encoder.addPoint(1000, 1000);
  encoder.addPoint(9000, 1000);
  encoder.addPoint(9000, 7000);
  encoder.end();
  wire->setNormalizedConnectivity(true);
}

}  // namespace

TEST(TestRoutingPreserver, MutationStandsWithoutAPreserver)
{
  Fixture f;
  odb::dbBlock* block = f.makeBlock("chip", "block");
  odb::dbNet* net = f.makeRoutedNet(block, "n1");
  const auto before = stream(net);
  ASSERT_FALSE(before.empty());

  reencode(net, f.met1);

  // THE CONTROL. Without a preserver the rewrite survives -- which is what the
  // shipped tool did, and why the next test is worth anything.
  EXPECT_NE(stream(net), before);
  EXPECT_TRUE(net->getWire()->hasNormalizedConnectivity());
}

TEST(TestRoutingPreserver, PreserverRestoresTheWire)
{
  for (const bool normalized_connectivity : {false, true}) {
    SCOPED_TRACE(normalized_connectivity);
    Fixture f;
    odb::dbBlock* block = f.makeBlock("chip", "block");
    odb::dbNet* net = f.makeRoutedNet(block, "n1");
    net->getWire()->setNormalizedConnectivity(normalized_connectivity);
    const auto before = stream(net);
    ASSERT_FALSE(before.empty());

    {
      RoutingPreserver preserver(&f.logger);
      preserver.snapshot(block);
      reencode(net, f.met1);
      net->getWire()->setNormalizedConnectivity(!normalized_connectivity);
      // It really did move while the preserver was alive -- otherwise the
      // restore below would be proving nothing.
      EXPECT_EQ(preserver.changedCount(), 1);
    }

    EXPECT_EQ(stream(net), before);
    EXPECT_EQ(net->getWire()->hasNormalizedConnectivity(),
              normalized_connectivity);
  }
}

// The multi-chip shape: ONE preserver, SEVERAL blocks, every block restored.
// MultiChipExtractor::run() snapshots each DIE as it orders it and lets the
// single preserver restore them all after extractInterChipParasitics(), which
// reads the ordered wires of all of them.
TEST(TestRoutingPreserver, OnePreserverCoversEveryBlockItIsGiven)
{
  Fixture f;
  odb::dbBlock* die0 = f.makeBlock("chip0", "die0");
  odb::dbBlock* die1 = f.makeBlock("chip1", "die1");
  odb::dbNet* n0 = f.makeRoutedNet(die0, "n0");
  odb::dbNet* n1 = f.makeRoutedNet(die1, "n1");
  const auto before0 = stream(n0);
  const auto before1 = stream(n1);

  {
    RoutingPreserver preserver(&f.logger);
    preserver.snapshot(die0);
    reencode(n0, f.met1);
    preserver.snapshot(die1);
    reencode(n1, f.met1);
    EXPECT_EQ(preserver.changedCount(), 2);
  }

  EXPECT_EQ(stream(n0), before0);
  EXPECT_EQ(stream(n1), before1);
}

// A net the ordering does not touch must not be rewritten either: restoring a
// wire that never moved would churn the database and defeat the whole point of
// a byte-preserving pass.
TEST(TestRoutingPreserver, LeavesUntouchedWiresAlone)
{
  Fixture f;
  odb::dbBlock* block = f.makeBlock("chip", "block");
  odb::dbNet* net = f.makeRoutedNet(block, "n1");
  const auto before = stream(net);

  {
    RoutingPreserver preserver(&f.logger);
    preserver.snapshot(block);
    EXPECT_EQ(preserver.changedCount(), 0);
  }

  EXPECT_EQ(stream(net), before);
}

}  // namespace rcx
