// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#include "Netlist.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "odb/geom.h"
#include "ppl/IOPlacer.h"

namespace ppl {

Netlist::Netlist()
{
  net_pointer_.push_back(0);
}

void Netlist::addIONet(const IOPin& io_pin,
                       const std::vector<InstancePin>& inst_pins,
                       int net_weight)
{
  db_pin_idx_map_[io_pin.getBTerm()] = io_pins_.size();
  io_pins_.push_back(io_pin);
  net_weights_.push_back(net_weight);
  inst_pins_.insert(inst_pins_.end(), inst_pins.begin(), inst_pins.end());
  net_pointer_.push_back(inst_pins_.size());
}

int Netlist::createIOGroup(const std::vector<odb::dbBTerm*>& pin_list,
                           bool order,
                           const int group_idx)
{
  int pin_cnt = 0;
  std::vector<int> pin_indices;
  for (odb::dbBTerm* bterm : pin_list) {
    int pin_idx = db_pin_idx_map_[bterm];
    if (pin_idx < 0) {
      return pin_cnt;
    }
    io_pins_[pin_idx].setInGroup();
    io_pins_[pin_idx].setGroupIdx(group_idx);
    pin_indices.push_back(pin_idx);
    pin_cnt++;
  }

  io_groups_.push_back({pin_indices, order});
  return pin_indices.size();
}

void Netlist::addIOGroup(const std::vector<int>& pin_group, bool order)
{
  io_groups_.push_back({pin_group, order});
}

void Netlist::getSinksOfIO(int idx, std::vector<InstancePin>& sinks)
{
  int net_start = net_pointer_[idx];
  int net_end = net_pointer_[idx + 1];
  for (int sink_idx = net_start; sink_idx < net_end; ++sink_idx) {
    sinks.push_back(inst_pins_[sink_idx]);
  }
}

int Netlist::numSinksOfIO(int idx)
{
  int net_start = net_pointer_[idx];
  int net_end = net_pointer_[idx + 1];
  return net_end - net_start;
}

int Netlist::numIOPins()
{
  return io_pins_.size();
}

odb::Rect Netlist::getBB(int idx, const odb::Point& slot_pos)
{
  int net_start = net_pointer_[idx];
  int net_end = net_pointer_[idx + 1];

  int min_x = slot_pos.x();
  int min_y = slot_pos.y();
  int max_x = slot_pos.x();
  int max_y = slot_pos.y();

  for (int idx = net_start; idx < net_end; ++idx) {
    odb::Point pos = inst_pins_[idx].getPos();
    min_x = std::min(min_x, pos.x());
    max_x = std::max(max_x, pos.x());
    min_y = std::min(min_y, pos.y());
    max_y = std::max(max_y, pos.y());
  }

  odb::Point upper_bounds = odb::Point(max_x, max_y);
  odb::Point lower_bounds = odb::Point(min_x, min_y);

  odb::Rect net_b_box(lower_bounds, upper_bounds);
  return net_b_box;
}

int Netlist::computeIONetHPWL(int idx, const odb::Point& slot_pos)
{
  int net_start = net_pointer_[idx];
  int net_end = net_pointer_[idx + 1];

  int min_x = slot_pos.x();
  int min_y = slot_pos.y();
  int max_x = slot_pos.x();
  int max_y = slot_pos.y();

  for (int idx = net_start; idx < net_end; ++idx) {
    odb::Point pos = inst_pins_[idx].getPos();
    min_x = std::min(min_x, pos.x());
    max_x = std::max(max_x, pos.x());
    min_y = std::min(min_y, pos.y());
    max_y = std::max(max_y, pos.y());
  }

  int x = max_x - min_x;
  int y = max_y - min_y;

  return (x + y);
}

// Assignment-objective cost: the geometric IO-net HPWL scaled by the net's
// odb weight. A default-weight net (weight <= 1, and odb defaults every net to
// 1) returns the unweighted HPWL byte-for-byte, so a design with no explicit
// net weights assigns pins identically to stock; only a net explicitly
// weighted >= 2 (e.g. a timing- or congestion-critical net) is biased toward a
// slot with smaller HPWL. Kept separate from computeIONetHPWL(), which remains
// the pure geometric HPWL used for reporting.
int Netlist::computeIONetCost(int idx, const odb::Point& slot_pos)
{
  const int hpwl = computeIONetHPWL(idx, slot_pos);
  const int weight = net_weights_[idx];
  if (weight <= 1 || hpwl == std::numeric_limits<int>::max()) {
    return hpwl;
  }
  const int64_t cost = static_cast<int64_t>(hpwl) * weight;
  // Keep the cost inside int and clear of the Hungarian tie-break packing:
  // values >= 2^24 skip the rank byte and INT_MAX is the fail sentinel.
  constexpr int64_t kCostCap = std::numeric_limits<int>::max() / 2;
  return static_cast<int>(std::min(cost, kCostCap));
}

int Netlist::computeDstIOtoPins(int idx, const odb::Point& slot_pos)
{
  int net_start = net_pointer_[idx];
  int net_end = net_pointer_[idx + 1];

  int total_distance = 0;

  for (int idx = net_start; idx < net_end; ++idx) {
    odb::Point pin_pos = inst_pins_[idx].getPos();
    total_distance += std::abs(pin_pos.x() - slot_pos.x())
                      + std::abs(pin_pos.y() - slot_pos.y());
  }

  return total_distance;
}

void Netlist::sortPinsFromGroup(int group_idx, Edge edge)
{
  PinGroupByIndex& group = io_groups_[group_idx];
  std::vector<int>& pin_indices = group.pin_indices;
  if (group.order && (edge == Edge::top || edge == Edge::left)) {
    std::ranges::reverse(pin_indices);
  }
}

void Netlist::reset()
{
  inst_pins_.clear();
  net_pointer_.clear();
  io_pins_.clear();
  net_weights_.clear();
  io_groups_.clear();
  db_pin_idx_map_.clear();
  net_pointer_.push_back(0);
}

int IOPin::getArea() const
{
  int area = std::abs((upper_bound_.getX() - lower_bound_.getX())
                      * (upper_bound_.getY() - lower_bound_.getY()));
  return area;
}

}  // namespace ppl
