// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2026, The OpenROAD Authors

#include "rcx/routingPreserver.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "odb/db.h"
#include "utl/Logger.h"

namespace rcx {

namespace {

// The raw (opcode, data) stream odb itself produces. length()/getOpcode()/
// getData() are the accessors dbWire::addOneSeg() replays verbatim, so this is
// the wire's bytes and not a re-rendering of them.
std::vector<std::pair<unsigned char, int>> readStream(odb::dbWire* wire)
{
  std::vector<std::pair<unsigned char, int>> stream;
  if (wire == nullptr) {
    return stream;
  }
  const uint32_t n = wire->length();
  stream.reserve(n);
  for (uint32_t i = 0; i < n; i++) {
    stream.emplace_back(wire->getOpcode(i), wire->getData(i));
  }
  return stream;
}

}  // namespace

RoutingPreserver::RoutingPreserver(utl::Logger* logger) : logger_(logger)
{
}

void RoutingPreserver::snapshot(odb::dbBlock* block)
{
  if (block == nullptr) {
    return;
  }
  for (odb::dbNet* net : block->getNets()) {
    // Exactly the nets odb::normalizeWireConnectivity() will touch. Anything
    // it skips has nothing to preserve, and saving it would only cost memory.
    if (net->getSigType().isSupply()) {
      continue;
    }
    odb::dbWire* wire = net->getWire();
    if (wire == nullptr) {
      continue;
    }
    Saved saved;
    saved.net = net;
    saved.normalized_connectivity = wire->hasNormalizedConnectivity();
    saved.stream = readStream(wire);
    saved_.push_back(std::move(saved));
  }
}

bool RoutingPreserver::sameStream(
    odb::dbNet* net,
    const std::vector<std::pair<unsigned char, int>>& s)
{
  odb::dbWire* wire = net->getWire();
  if (wire == nullptr) {
    return s.empty();
  }
  if (wire->length() != s.size()) {
    return false;
  }
  for (uint32_t i = 0; i < wire->length(); i++) {
    if (wire->getOpcode(i) != s[i].first || wire->getData(i) != s[i].second) {
      return false;
    }
  }
  return true;
}

int RoutingPreserver::changedCount() const
{
  int changed = 0;
  for (const Saved& saved : saved_) {
    if (!sameStream(saved.net, saved.stream)) {
      changed++;
    }
  }
  return changed;
}

RoutingPreserver::~RoutingPreserver()
{
  int restored = 0;
  for (const Saved& saved : saved_) {
    if (sameStream(saved.net, saved.stream)) {
      continue;  // the ordering left this one alone; do not touch it
    }
    odb::dbWire* wire = saved.net->getWire();
    if (wire != nullptr) {
      odb::dbWire::destroy(wire);
    }
    wire = odb::dbWire::create(saved.net);
    if (wire == nullptr) {
      continue;
    }
    for (const auto& [op, value] : saved.stream) {
      wire->addOneSeg(op, value);
    }
    wire->setNormalizedConnectivity(saved.normalized_connectivity);
    restored++;
  }
  if (restored > 0 && logger_ != nullptr) {
    // Reported, not silent: the number IS the size of the defect this works
    // around, and a run where it drops to 0 means WireNormalizer was fixed and
    // this can go.
    logger_->info(utl::RCX,
                  700,
                  "Restored the routing of {} of {} net(s) that wire ordering "
                  "rewrote. Extraction does not change the route it measures; "
                  "the parasitics are unaffected.",
                  restored,
                  saved_.size());
  }
}

}  // namespace rcx
