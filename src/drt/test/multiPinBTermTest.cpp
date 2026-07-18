// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Regression gate for DRT-0302 ("Unsupported multiple pins on bterm").
//
// TritonRoute aborts the whole route when a bterm carries more than one pin,
// UNLESS those pins can be merged into a single router terminal.  Power/ground
// bterms already merge (upstream).  When a hierarchical macro is FLATTENED
// before routing, a supply/special NET's landing pads can surface as a
// (possibly signal-typed) bterm with several pins; commercial routers merge
// those same-PG-net bterms instead of aborting.  This test pins down
// io::multiPinBTermSupported(): the flatten PG case is now merged (stock
// aborted), while a genuine multi-pin SIGNAL bterm on an ordinary net still
// aborts (the honest limitation -- its pins may be must-connect).

#include <string>

#include "gtest/gtest.h"
#include "io/io.h"
#include "odb/db.h"
#include "odb/dbTypes.h"
#include "utl/Logger.h"

namespace drt {
namespace {

using odb::dbBlock;
using odb::dbBox;
using odb::dbBPin;
using odb::dbBTerm;
using odb::dbChip;
using odb::dbDatabase;
using odb::dbNet;
using odb::dbSigType;
using odb::dbTech;
using odb::dbTechLayer;
using odb::dbTechLayerType;

class MultiPinBTerm : public ::testing::Test
{
 protected:
  MultiPinBTerm()
  {
    db_ = dbDatabase::create();
    db_->setLogger(&logger_);
    dbTech* tech = dbTech::create(db_, "tech");
    layer_ = dbTechLayer::create(tech, "M1", dbTechLayerType::ROUTING);
    dbChip* chip = dbChip::create(db_, tech);
    block_ = dbBlock::create(chip, "top");
  }
  ~MultiPinBTerm() override { dbDatabase::destroy(db_); }

  // Creates a bterm on a fresh net with the given net/term sig types, special
  // flag, and pin count.
  dbBTerm* make(const char* name,
                dbSigType net_sig,
                dbSigType term_sig,
                bool special,
                int npins)
  {
    dbNet* net = dbNet::create(block_, (std::string(name) + "_net").c_str());
    net->setSigType(net_sig);
    if (special) {
      net->setSpecial();
    }
    dbBTerm* bterm = dbBTerm::create(net, name);
    bterm->setSigType(term_sig);
    for (int i = 0; i < npins; i++) {
      dbBPin* bpin = dbBPin::create(bterm);
      dbBox::create(bpin, layer_, i * 200, 0, i * 200 + 100, 100);
    }
    return bterm;
  }

  utl::Logger logger_;
  dbDatabase* db_;
  dbBlock* block_;
  dbTechLayer* layer_;
};

// The STOCK rule keyed only off the bterm sig type: any non-supply bterm with
// >1 pins aborted.  Reproduced here so the stock-fails/fork-passes contrast is
// explicit and unfakeable.
bool stockRule(dbBTerm* term)
{
  if (term->getBPins().size() <= 1) {
    return true;
  }
  return term->getSigType().isSupply();
}

TEST_F(MultiPinBTerm, SinglePinAlwaysSupported)
{
  EXPECT_TRUE(io::multiPinBTermSupported(
      make("s1", dbSigType::SIGNAL, dbSigType::SIGNAL, false, 1)));
}

TEST_F(MultiPinBTerm, PowerMultiPinSupported)
{
  auto* t = make("p", dbSigType::POWER, dbSigType::POWER, false, 3);
  EXPECT_TRUE(io::multiPinBTermSupported(t));
  EXPECT_TRUE(stockRule(t));  // already allowed upstream
}

// The reproduced negative: a genuine multi-pin SIGNAL bterm on an ordinary
// signal net still aborts.  Both stock and fork reject it -- the honest
// boundary of the fix.
TEST_F(MultiPinBTerm, GenuineSignalMultiPinStillAborts)
{
  auto* t = make("sig", dbSigType::SIGNAL, dbSigType::SIGNAL, false, 2);
  EXPECT_FALSE(io::multiPinBTermSupported(t));
  EXPECT_FALSE(stockRule(t));
}

// THE FIX: a flatten-exposed supply-net landing pad -- bterm typed SIGNAL but
// its NET is a supply net -- is now merged.  Stock aborted; fork passes.
TEST_F(MultiPinBTerm, FlattenedSupplyNetMultiPinMerges)
{
  auto* t = make("vpwr", dbSigType::POWER, dbSigType::SIGNAL, false, 2);
  EXPECT_FALSE(stockRule(t)) << "stock DRT-0302 aborted this flatten PG case";
  EXPECT_TRUE(io::multiPinBTermSupported(t))
      << "fork merges the same-PG-net bterm";
}

// THE FIX (special-net form): a special (PDN) net's multi-pin bterm merges even
// when it is not sig-typed supply.
TEST_F(MultiPinBTerm, SpecialNetMultiPinMerges)
{
  auto* t = make("vdd", dbSigType::SIGNAL, dbSigType::SIGNAL, true, 2);
  EXPECT_FALSE(stockRule(t));
  EXPECT_TRUE(io::multiPinBTermSupported(t));
}

}  // namespace
}  // namespace drt
