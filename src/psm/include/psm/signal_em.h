// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, The OpenROAD Authors
//
// Signal-net EM (EM4) -- a pure, header-only switching-current rule engine.
//
// EM3 (em_signoff.h) checks the POWER grid, where the current is unidirectional
// DC and a single J = I/A limit governs.  A SIGNAL net is different in kind: the
// current is AC and BIDIRECTIONAL -- each transition pushes charge C*V onto the
// wire and the next transition pulls it back off -- so the net DC current is
// ~zero and the DC check EM3 performs simply does not apply.  What governs a
// signal wire instead is
//
//   * the RMS current  (Joule self-heating of the wire), and
//   * the peak current (instantaneous current-density spike), and
//   * the average ABSOLUTE current (unidirectional mass transport per direction).
//
// This engine forms all three from the quantities a placed-and-routed design
// already knows -- load capacitance C, supply swing V, switching activity N
// [transitions/second] and transition time tr -- and checks each against its own
// per-layer limit table.
//
// The current model is the standard rectangular-pulse approximation, stated
// here in full so every number below is hand-checkable:
//
//   A transition moves charge  Q = C * V  in time tr, so while it is switching
//   the wire carries a constant current
//
//       i_peak = C * V / tr                                            (1)
//
//   The wire is switching for a fraction of wall-clock time
//
//       duty = N * tr            (clamped to 1: transitions cannot overlap)  (2)
//
//   and is carrying zero current the rest of the time.  Hence
//
//       i_avg_abs = i_peak * duty        = C * V * N                   (3)
//       i_rms     = i_peak * sqrt(duty)  = C * V * sqrt(N / tr)        (4)
//
//   Because the current alternates direction, only half of |i| flows in either
//   direction, so the unidirectional average that DC-style mass-transport limits
//   are written against is
//
//       i_avg_uni = i_avg_abs / 2                                      (5)
//
//   Note the exact identity  i_rms = sqrt(i_avg_abs * i_peak)  which follows
//   from (3) and (4) and is asserted by the correctness gate.
//
// The J-limits themselves are foundry data (they live in the PDK, Y*): this
// engine consumes them where a PDK or the user supplies them and can never
// manufacture a verdict without them.  The ENGINE is chip/PDK agnostic -- it
// contains no SKU, no rule id and no foundry constant; it only implements
// (1)-(5) and compares J = I/A against a limit.
//
// The file is header-only and depends on nothing but the C++ standard library
// and em_signoff.h (for the shared per-layer EMLimits table), so the correctness
// gate can drive the model with hand-computed C, V, N, tr and A without standing
// up an OpenDB / OpenSTA design -- the verdict is exactly reproducible.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "psm/em_signoff.h"

namespace psm {

// Which of the three limit families a violation was found against.
enum class SignalEMMode
{
  kNone,
  kAvg,
  kRms,
  kPeak
};

inline const char* signalEMModeName(SignalEMMode mode)
{
  switch (mode) {
    case SignalEMMode::kAvg:
      return "avg";
    case SignalEMMode::kRms:
      return "rms";
    case SignalEMMode::kPeak:
      return "peak";
    case SignalEMMode::kNone:
      break;
  }
  return "-";
}

// The drive conditions of one signal net.  A non-positive cap_f, supply_v or
// transition_s means the analysis inputs were not available for this net; such a
// net is skipped and can never become a violation (fail-safe).  A zero activity
// (a net that never toggles) is a perfectly valid input and yields zero current.
struct SignalEMDrive
{
  double cap_f = 0.0;         // total load capacitance C [F]
  double supply_v = 0.0;      // rail-to-rail swing V [V]
  double density_hz = 0.0;    // switching activity N [transitions/second]
  double transition_s = 0.0;  // transition time tr [s]

  bool usable() const
  {
    return cap_f > 0.0 && supply_v > 0.0 && transition_s > 0.0
           && density_hz >= 0.0;
  }
};

// The three switching currents implied by a SignalEMDrive, per equations
// (1)-(5) above.
struct SignalEMCurrents
{
  double i_peak = 0.0;     // A
  double i_rms = 0.0;      // A
  double i_avg_abs = 0.0;  // A
  double i_avg_uni = 0.0;  // A
  double duty = 0.0;       // fraction of time the wire is switching [0,1]
  bool valid = false;
};

// Pure evaluation of (1)-(5).  No I/O, no globals -> exactly reproducible.
inline SignalEMCurrents computeSwitchingCurrents(const SignalEMDrive& d)
{
  SignalEMCurrents c;
  if (!d.usable()) {
    return c;  // valid == false, all currents zero
  }
  c.valid = true;
  if (!(d.density_hz > 0.0)) {
    return c;  // a net that never switches carries no current at all
  }
  // (1) charge C*V delivered in tr.
  c.i_peak = d.cap_f * d.supply_v / d.transition_s;
  // (2) transitions cannot overlap: a wire switching more than 100% of the time
  // is carrying i_peak continuously.
  c.duty = std::min(1.0, d.density_hz * d.transition_s);
  // (3) and (4).
  c.i_avg_abs = c.i_peak * c.duty;
  c.i_rms = c.i_peak * std::sqrt(c.duty);
  // (5) the current alternates direction.
  c.i_avg_uni = 0.5 * c.i_avg_abs;
  return c;
}

// One routed segment of a signal net presented to the engine.
//   area_um2        : current-carrying cross-section [um^2] (width * thickness
//                     for a wire, n_cuts * cut_area for a via).  <= 0 means the
//                     geometry was unavailable -> skipped, never a violation.
//   current_fraction: share of the net's switching current this segment carries.
//                     1.0 (the default) is the conservative screening choice --
//                     the driver-adjacent segment carries the whole net current.
struct SignalEMSegment
{
  std::string layer;
  double area_um2 = 0.0;
  bool is_via = false;
  double current_fraction = 1.0;
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
};

// A signal net: its drive conditions plus its routed segments.
struct SignalEMNet
{
  std::string name;
  SignalEMDrive drive;
  std::vector<SignalEMSegment> segments;
};

// Per-layer J-limits, one independent table per current family [A/um^2].  A
// family with no limit for a layer is reported as unlimited and can never
// become a violation.
struct SignalEMLimits
{
  EMLimits avg;
  EMLimits rms;
  EMLimits peak;

  bool empty() const { return avg.empty() && rms.empty() && peak.empty(); }
};

// One classified segment.
struct SignalEMSegmentResult
{
  std::string net;
  std::string layer;
  double area_um2 = 0.0;
  bool is_via = false;

  double i_avg = 0.0;  // unidirectional average current through this segment [A]
  double i_rms = 0.0;
  double i_peak = 0.0;

  double j_avg = 0.0;  // A/um^2
  double j_rms = 0.0;
  double j_peak = 0.0;

  double limit_avg = 0.0;  // <= 0 => no limit for this layer/family
  double limit_rms = 0.0;
  double limit_peak = 0.0;

  double ratio_avg = 0.0;  // j/limit (0 when no limit)
  double ratio_rms = 0.0;
  double ratio_peak = 0.0;

  bool violated_avg = false;
  bool violated_rms = false;
  bool violated_peak = false;

  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;

  bool violated() const
  {
    return violated_avg || violated_rms || violated_peak;
  }

  // Worst utilization across the three families, and which family it came from.
  double worstRatio() const
  {
    return std::max(ratio_avg, std::max(ratio_rms, ratio_peak));
  }

  SignalEMMode worstMode() const
  {
    const double worst = worstRatio();
    if (worst <= 0.0) {
      return SignalEMMode::kNone;
    }
    if (ratio_peak == worst) {
      return SignalEMMode::kPeak;
    }
    if (ratio_rms == worst) {
      return SignalEMMode::kRms;
    }
    return SignalEMMode::kAvg;
  }
};

// Aggregate signoff verdict.
struct SignalEMResult
{
  std::size_t nets = 0;              // nets presented
  std::size_t nets_driven = 0;       // nets with usable drive conditions
  std::size_t total = 0;             // segments presented
  std::size_t checked = 0;           // segments with usable geometry AND drive
  std::size_t limited = 0;           // checked segments with >= 1 positive limit
  std::size_t violations = 0;        // segments violating >= 1 family
  std::size_t violations_avg = 0;
  std::size_t violations_rms = 0;
  std::size_t violations_peak = 0;
  std::size_t skipped_no_area = 0;   // geometry unavailable
  std::size_t skipped_no_drive = 0;  // drive conditions unavailable
  std::size_t skipped_no_limit = 0;  // checked but no limit in any family

  double worst_ratio = 0.0;
  double worst_j = 0.0;
  double worst_limit = 0.0;
  std::string worst_layer;
  std::string worst_net;
  SignalEMMode worst_mode = SignalEMMode::kNone;

  std::vector<SignalEMSegmentResult> segments;

  bool pass() const { return violations == 0; }
};

// Classify a batch of signal nets against the per-layer J-limits.  Pure: no I/O,
// no globals, order-preserving -> exactly reproducible and hand-checkable.
inline SignalEMResult classifySignalEM(const std::vector<SignalEMNet>& nets,
                                       const SignalEMLimits& limits,
                                       bool keep_detail = true)
{
  SignalEMResult res;
  res.nets = nets.size();

  for (const auto& net : nets) {
    const SignalEMCurrents currents = computeSwitchingCurrents(net.drive);
    if (currents.valid) {
      res.nets_driven++;
    }

    for (const auto& seg : net.segments) {
      res.total++;
      if (!currents.valid) {
        res.skipped_no_drive++;
        continue;
      }
      if (!(seg.area_um2 > 0.0)) {
        res.skipped_no_area++;
        continue;
      }
      res.checked++;

      const double frac = seg.current_fraction;
      SignalEMSegmentResult r;
      r.net = net.name;
      r.layer = seg.layer;
      r.area_um2 = seg.area_um2;
      r.is_via = seg.is_via;
      r.x0 = seg.x0;
      r.y0 = seg.y0;
      r.x1 = seg.x1;
      r.y1 = seg.y1;

      r.i_avg = currents.i_avg_uni * frac;
      r.i_rms = currents.i_rms * frac;
      r.i_peak = currents.i_peak * frac;

      // The load-bearing quantity, once per family: J = I / A.
      r.j_avg = r.i_avg / seg.area_um2;
      r.j_rms = r.i_rms / seg.area_um2;
      r.j_peak = r.i_peak / seg.area_um2;

      r.limit_avg = limits.avg.limitFor(seg.layer);
      r.limit_rms = limits.rms.limitFor(seg.layer);
      r.limit_peak = limits.peak.limitFor(seg.layer);

      bool any_limit = false;
      if (r.limit_avg > 0.0) {
        any_limit = true;
        r.ratio_avg = r.j_avg / r.limit_avg;
        r.violated_avg = r.j_avg > r.limit_avg;
      }
      if (r.limit_rms > 0.0) {
        any_limit = true;
        r.ratio_rms = r.j_rms / r.limit_rms;
        r.violated_rms = r.j_rms > r.limit_rms;
      }
      if (r.limit_peak > 0.0) {
        any_limit = true;
        r.ratio_peak = r.j_peak / r.limit_peak;
        r.violated_peak = r.j_peak > r.limit_peak;
      }

      if (any_limit) {
        res.limited++;
        res.violations_avg += r.violated_avg ? 1 : 0;
        res.violations_rms += r.violated_rms ? 1 : 0;
        res.violations_peak += r.violated_peak ? 1 : 0;
        if (r.violated()) {
          res.violations++;
        }
        const double ratio = r.worstRatio();
        if (ratio > res.worst_ratio) {
          res.worst_ratio = ratio;
          res.worst_layer = seg.layer;
          res.worst_net = net.name;
          res.worst_mode = r.worstMode();
          switch (res.worst_mode) {
            case SignalEMMode::kPeak:
              res.worst_j = r.j_peak;
              res.worst_limit = r.limit_peak;
              break;
            case SignalEMMode::kRms:
              res.worst_j = r.j_rms;
              res.worst_limit = r.limit_rms;
              break;
            default:
              res.worst_j = r.j_avg;
              res.worst_limit = r.limit_avg;
              break;
          }
        }
      } else {
        res.skipped_no_limit++;
      }

      if (keep_detail) {
        res.segments.push_back(std::move(r));
      }
    }
  }
  return res;
}

}  // namespace psm
