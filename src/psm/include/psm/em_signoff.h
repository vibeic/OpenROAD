// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, The OpenROAD Authors
//
// EM current-density signoff (EM3) -- a pure, header-only rule engine.
//
// It consumes per-wire currents (I) and current-carrying cross-section areas
// (A) -- the quantities PSM's static/EM power-grid solve already produces -- and
// flags every metal/via segment whose DC current density
//
//     J = I / A
//
// exceeds a per-layer limit.  The J-limits themselves are foundry data (they
// live in the PDK, Y*): this engine consumes them where a PDK or the user
// supplies them and otherwise falls back to an explicit, documented placeholder.
// The ENGINE in this file is chip/PDK agnostic -- it contains no SKU, no rule
// id, and no foundry constant; it only implements J = I/A vs a limit.
//
// The file is header-only and depends on nothing but the C++ standard library
// (mirroring transient.h) so the correctness gate can drive the classification
// math with hand-computed I, A and limits without standing up an OpenDB /
// OpenSTA design -- the verdict is exactly reproducible and hand-checkable.

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace psm {

// One current-carrying metal/via segment presented to the EM rule engine.
//   current_a : magnitude of the DC current through the segment [A].
//   area_um2  : current-carrying cross-section [um^2].  For a wire this is
//               width * thickness; for a via it is n_cuts * cut_area.  A value
//               <= 0 means the geometry needed to form a cross-section was not
//               available, and the segment is skipped (never a violation).
//   layer     : routing/cut layer name, used to look up the per-layer J-limit.
//   x0..y1    : segment endpoints [um], carried through for reporting only.
struct EMWireCurrent
{
  std::string layer;
  double current_a = 0.0;
  double area_um2 = 0.0;
  bool is_via = false;
  int cuts = 0;  // a via's effective cut count (each carries current / cuts)
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
};

// Per-layer current-density limits [A/um^2].  A layer with no explicit entry
// falls back to default_limit.  A non-positive *effective* limit means "no
// limit is defined for this layer" -- such a segment is counted as
// checked-but-unlimited and can never become a violation, so a missing foundry
// table can neither manufacture a false PASS nor a false FAIL.
//
// A cut layer may instead carry a per-cut current limit [A per cut] (e.g. a
// foundry "maximum current per via" rule): a via on that layer is judged on
// its current per cut, current / cuts, against that limit.
struct EMLimits
{
  double default_limit = 0.0;                // A/um^2  (<= 0 => none)
  std::map<std::string, double> per_layer;   // layer name -> A/um^2
  std::map<std::string, double> per_cut;     // cut layer name -> A per cut

  double limitFor(const std::string& layer) const
  {
    const auto it = per_layer.find(layer);
    if (it != per_layer.end()) {
      return it->second;
    }
    return default_limit;
  }

  double perCutLimitFor(const std::string& layer) const
  {
    const auto it = per_cut.find(layer);
    return it != per_cut.end() ? it->second : 0.0;
  }

  bool empty() const
  {
    return default_limit <= 0.0 && per_layer.empty() && per_cut.empty();
  }
};

// One classified segment.
struct EMWireResult
{
  std::string layer;
  double current_a = 0.0;
  double area_um2 = 0.0;
  double j = 0.0;          // A/um^2 == current_a / area_um2
  double limit = 0.0;      // A/um^2 applied (<= 0 => no limit for this layer)
  double ratio = 0.0;      // j / limit (0 when no limit)
  bool violated = false;   // limit > 0 && j > limit
  bool is_via = false;
  int cuts = 0;
  // How the segment was judged: "AREAL" (J = I/A vs A/um^2), "PER_CUT"
  // (I/cuts vs A per cut: j and limit are then in A per cut), or why it was
  // not: "NO_AREA" (no cross-section), "NO_LIMIT" (no limit for the layer).
  std::string basis;
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
};

// Aggregate signoff verdict over all classified segments.
struct EMSignoffResult
{
  std::size_t total = 0;             // segments presented
  std::size_t checked = 0;           // segments with usable geometry (area > 0)
  std::size_t limited = 0;           // checked segments that had a positive limit
  std::size_t violations = 0;        // limited segments with j > limit
  std::size_t skipped_no_area = 0;   // not judged: missing geometry
  std::size_t skipped_no_limit = 0;  // not judged: no J-limit for the layer
  std::size_t per_cut = 0;           // vias judged per cut
  std::map<std::string, std::size_t> no_area_by_layer;
  std::map<std::string, std::size_t> no_limit_by_layer;
  double worst_j = 0.0;              // A/um^2 at the worst utilization
  double worst_limit = 0.0;          // A/um^2 at the worst utilization
  double worst_ratio = 0.0;          // max (j / limit) over limited segments
  std::string worst_layer;
  std::vector<EMWireResult> wires;   // per-segment detail (for the report)

  // Every segment judged and none over its limit.  A segment that was not
  // judged (no geometry or no limit) is never a clean result.
  bool pass() const
  {
    return violations == 0 && skipped_no_area == 0 && skipped_no_limit == 0;
  }
  const char* verdict() const
  {
    if (violations > 0) {
      return "FAIL";
    }
    return pass() ? "PASS" : "NOT_MEASURED";
  }
};

// Classify a batch of per-wire currents against per-layer J-limits.  Pure: no
// I/O, no globals, order-preserving -> exactly reproducible and hand-checkable.
inline EMSignoffResult classifyCurrentDensity(
    const std::vector<EMWireCurrent>& wires,
    const EMLimits& limits,
    bool keep_detail = true)
{
  EMSignoffResult res;
  res.total = wires.size();
  for (const auto& w : wires) {
    EMWireResult r;
    r.layer = w.layer;
    r.current_a = w.current_a;
    r.area_um2 = w.area_um2;
    r.is_via = w.is_via;
    r.cuts = w.cuts;
    r.x0 = w.x0;
    r.y0 = w.y0;
    r.x1 = w.x1;
    r.y1 = w.y1;

    const double per_cut_limit
        = w.is_via && w.cuts > 0 ? limits.perCutLimitFor(w.layer) : 0.0;
    if (per_cut_limit <= 0.0 && !(w.area_um2 > 0.0)) {
      // Never silently: the segment is counted, listed and kept in the detail.
      res.skipped_no_area++;
      res.no_area_by_layer[w.layer]++;
      r.basis = "NO_AREA";
      if (keep_detail) {
        res.wires.push_back(std::move(r));
      }
      continue;
    }
    res.checked++;

    if (per_cut_limit > 0.0) {
      // Each of the via's cuts carries current / cuts.
      res.per_cut++;
      r.basis = "PER_CUT";
      r.j = w.current_a / w.cuts;
      r.limit = per_cut_limit;
    } else {
      // The load-bearing quantity: current density = current / cross-section.
      r.basis = "AREAL";
      r.j = w.current_a / w.area_um2;
      r.limit = limits.limitFor(w.layer);
    }

    if (r.limit > 0.0) {
      res.limited++;
      r.ratio = r.j / r.limit;
      r.violated = r.j > r.limit;
      if (r.violated) {
        res.violations++;
      }
      if (r.ratio > res.worst_ratio) {
        res.worst_ratio = r.ratio;
        res.worst_j = r.j;
        res.worst_limit = r.limit;
        res.worst_layer = r.layer;
      }
    } else {
      res.skipped_no_limit++;
      res.no_limit_by_layer[r.layer]++;
      r.basis = "NO_LIMIT";
    }

    if (keep_detail) {
      res.wires.push_back(std::move(r));
    }
  }
  return res;
}

}  // namespace psm
