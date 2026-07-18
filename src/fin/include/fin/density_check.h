// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, The OpenROAD Authors
//
// Metal density check (FL1) -- a pure, header-only rule engine.
//
// OpenROAD can INSERT dummy metal fill (density_fill), but it had no way to
// MEASURE the result: no per-window, per-layer metal density and no comparison
// against the foundry's min/max density window rule.  That measurement is what
// decides whether fill is needed at all, whether the fill that ran was enough,
// and whether it overshot -- it is the signoff half of the fill flow.
//
// The rule itself is one division:
//
//     density = filled_area / window_area                              (1)
//
// evaluated over a sliding window of the size and step the PDK specifies, and
// compared against a per-layer [min, max] band.  A window below min is an
// under-density (CMP dishing) violation; above max is an over-density
// (planarity / etch loading) violation.
//
// The density band is foundry data (it lives in the PDK, Y*): this engine
// consumes it where a PDK or the user supplies it and can never manufacture a
// verdict without it -- a layer with no band is reported NO_LIMIT and cannot
// fail.  The ENGINE is chip/PDK agnostic: it contains no SKU, no rule id and no
// foundry constant; it only implements (1) and compares against a band.
//
// The file is header-only and depends on nothing but the C++ standard library,
// so the correctness gate can drive the rule with hand-computed filled and
// window areas without standing up an OpenDB design.

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fin {

// One measured window.  Areas are in um^2.  A window_area_um2 <= 0 means the
// window degenerated (fully clipped away) and is skipped, never a violation.
struct DensityWindow
{
  std::string layer;
  double filled_area_um2 = 0.0;
  double window_area_um2 = 0.0;
  double x0 = 0.0;  // window bounds [um], carried through for reporting
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
};

// Per-layer density band as a fraction in [0,1].  A negative bound means "no
// bound of this kind for this layer".  A layer with no explicit entry falls back
// to the default band.
struct DensityLimits
{
  double default_min = -1.0;
  double default_max = -1.0;
  std::map<std::string, std::pair<double, double>> per_layer;

  std::pair<double, double> bandFor(const std::string& layer) const
  {
    const auto it = per_layer.find(layer);
    if (it != per_layer.end()) {
      return it->second;
    }
    return {default_min, default_max};
  }

  bool empty() const
  {
    return default_min < 0.0 && default_max < 0.0 && per_layer.empty();
  }
};

// One classified window.
struct DensityWindowResult
{
  std::string layer;
  double filled_area_um2 = 0.0;
  double window_area_um2 = 0.0;
  double density = 0.0;      // == filled_area_um2 / window_area_um2
  double min_limit = -1.0;   // < 0 => no bound
  double max_limit = -1.0;   // < 0 => no bound
  bool violated_min = false;
  bool violated_max = false;
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;

  bool violated() const { return violated_min || violated_max; }
};

// Aggregate verdict.
struct DensityCheckResult
{
  std::size_t total = 0;             // windows presented
  std::size_t checked = 0;           // windows with usable area
  std::size_t limited = 0;           // checked windows with >= 1 bound
  std::size_t violations = 0;        // windows violating either bound
  std::size_t violations_min = 0;
  std::size_t violations_max = 0;
  std::size_t skipped_no_area = 0;   // degenerate windows
  std::size_t skipped_no_limit = 0;  // checked but no band for the layer

  double min_density = 0.0;  // lowest density seen over LIMITED windows
  double max_density = 0.0;  // highest density seen over LIMITED windows
  std::string min_density_layer;
  std::string max_density_layer;
  bool any_limited = false;

  std::vector<DensityWindowResult> windows;

  bool pass() const { return violations == 0; }
};

// Classify a batch of measured windows against the per-layer bands.  Pure: no
// I/O, no globals, order-preserving -> exactly reproducible and hand-checkable.
inline DensityCheckResult classifyDensity(
    const std::vector<DensityWindow>& windows,
    const DensityLimits& limits,
    bool keep_detail = true)
{
  DensityCheckResult res;
  res.total = windows.size();

  for (const auto& w : windows) {
    if (!(w.window_area_um2 > 0.0)) {
      res.skipped_no_area++;
      continue;
    }
    res.checked++;

    DensityWindowResult r;
    r.layer = w.layer;
    r.filled_area_um2 = w.filled_area_um2;
    r.window_area_um2 = w.window_area_um2;
    r.x0 = w.x0;
    r.y0 = w.y0;
    r.x1 = w.x1;
    r.y1 = w.y1;
    // The load-bearing quantity: metal density over this window.
    r.density = w.filled_area_um2 / w.window_area_um2;

    const auto band = limits.bandFor(w.layer);
    r.min_limit = band.first;
    r.max_limit = band.second;

    if (r.min_limit >= 0.0 || r.max_limit >= 0.0) {
      res.limited++;
      if (r.min_limit >= 0.0 && r.density < r.min_limit) {
        r.violated_min = true;
        res.violations_min++;
      }
      if (r.max_limit >= 0.0 && r.density > r.max_limit) {
        r.violated_max = true;
        res.violations_max++;
      }
      if (r.violated()) {
        res.violations++;
      }
      if (!res.any_limited) {
        res.any_limited = true;
        res.min_density = r.density;
        res.max_density = r.density;
        res.min_density_layer = r.layer;
        res.max_density_layer = r.layer;
      } else {
        if (r.density < res.min_density) {
          res.min_density = r.density;
          res.min_density_layer = r.layer;
        }
        if (r.density > res.max_density) {
          res.max_density = r.density;
          res.max_density_layer = r.layer;
        }
      }
    } else {
      res.skipped_no_limit++;
    }

    if (keep_detail) {
      res.windows.push_back(std::move(r));
    }
  }
  return res;
}

}  // namespace fin
