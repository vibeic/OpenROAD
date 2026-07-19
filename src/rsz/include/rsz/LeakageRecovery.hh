// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026-2026, The OpenROAD Authors
//
// Multi-Vt leakage-recovery admission kernel (RS6).
//
// Post-timing-closure a design has POSITIVE slack on most paths; a leakage-
// recovery pass spends that slack by swapping low-Vt (fast, leaky) cells to
// high-Vt (slower, low-leakage) equivalents wherever the added delay still fits
// the path's slack margin.  Commercial resizers (the leakage-recovery phase of
// Tempus/PrimeTime-driven ECO, and the swap loop inside a sizer) do this greedily
// by leakage-saved-per-delay.  OpenROAD's own Lagrangian global sizer already
// carries Vt-aware presize modes (GlobalSizingConfig.hh); what was missing is the
// budget-constrained ADMISSION kernel that decides, given a path slack budget,
// which swaps to take -- provably without ever burning more delay than the budget.
//
// Model.  Each swap candidate i saves leakage L_i (> 0) at a path-delay cost
// D_i (>= 0).  Given a slack budget S (the path's positive timing margin), admit
// candidates in DECREASING leakage density L_i / D_i (a free swap, D_i == 0, has
// infinite density and is always taken), extending the admitted PREFIX while the
// cumulative delay stays <= S, and stopping at the first candidate that would
// exceed it.  Admitting a prefix of a fixed density order -- rather than the
// skip-ahead first-fit that a naive greedy uses -- is what makes the recovery
// MONOTONE in the budget: a larger S can only extend the prefix, never drop an
// already-admitted swap, so more budget never recovers less leakage (first-fit
// can invert this by dropping a big-delay/big-leakage swap to squeeze a small
// one).  The prefix value is exactly the integral part of the fractional-knapsack
// optimum, so it is bounded above by that LP optimum and is the largest fully-
// admittable prefix.
//
// Header-only and dependency-free so the correctness gate can hand-sort the
// densities, hand-accumulate the prefix, prove the budget is never exceeded,
// prove zero/negative budget recovers exactly zero (no phantom recovery), prove
// monotonicity in the budget, and check the integral prefix against the fractional
// LP bound.

#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace rsz {

// One low-Vt -> high-Vt swap candidate.
struct VtSwapCandidate
{
  double leakage_saving = 0.0;  // leakage saved by the swap [W or a.u.] (> 0 to matter)
  double delay_penalty = 0.0;   // extra path delay the slower hi-Vt cell adds [s] (>= 0)
};

// Result of a budget-constrained leakage-recovery admission.
struct LeakageRecoveryResult
{
  double leakage_recovered = 0.0;      // total leakage saved by admitted swaps
  double delay_used = 0.0;             // total delay spent (<= budget, always)
  std::vector<int> admitted;           // admitted candidate indices, density order
};

// Leakage density L/D; a zero-penalty (free) swap has infinite density.
inline double vtSwapDensity(const VtSwapCandidate &c)
{
  if (!(c.delay_penalty > 0.0)) {
    return std::numeric_limits<double>::infinity();
  }
  return c.leakage_saving / c.delay_penalty;
}

// Admit swaps in decreasing leakage density, extending the prefix while the
// cumulative delay stays within slack_budget; stop at the first swap that would
// exceed it.  Only positive-saving candidates are considered (a swap that saves
// nothing is never worth its delay).  A non-positive budget admits nothing.
inline LeakageRecoveryResult leakageRecovery(const std::vector<VtSwapCandidate> &cands,
                                             double slack_budget)
{
  LeakageRecoveryResult r;
  if (!(slack_budget > 0.0)) {
    return r;  // no positive margin -> no recovery, nothing spent
  }
  // Rank positive-saving candidates by density (desc), tie-break by index (stable).
  std::vector<int> order;
  order.reserve(cands.size());
  for (std::size_t i = 0; i < cands.size(); ++i) {
    if (cands[i].leakage_saving > 0.0) {
      order.push_back(static_cast<int>(i));
    }
  }
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
    return vtSwapDensity(cands[a]) > vtSwapDensity(cands[b]);
  });
  // Extend the admitted prefix while it fits the budget; stop at the first miss.
  for (int idx : order) {
    const double d = cands[idx].delay_penalty;
    if (r.delay_used + d <= slack_budget) {
      r.delay_used += d;
      r.leakage_recovered += cands[idx].leakage_saving;
      r.admitted.push_back(idx);
    } else {
      break;  // prefix admission: do not skip ahead (keeps recovery monotone)
    }
  }
  return r;
}

// Fractional-knapsack (LP-relaxation) leakage recovery: the integral prefix plus
// the admittable FRACTION of the next candidate.  This is the upper bound the
// integral prefix can never exceed; the gate checks integral <= fractional.
inline double leakageRecoveryFractionalBound(const std::vector<VtSwapCandidate> &cands,
                                             double slack_budget)
{
  if (!(slack_budget > 0.0)) {
    return 0.0;
  }
  std::vector<int> order;
  for (std::size_t i = 0; i < cands.size(); ++i) {
    if (cands[i].leakage_saving > 0.0) {
      order.push_back(static_cast<int>(i));
    }
  }
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
    return vtSwapDensity(cands[a]) > vtSwapDensity(cands[b]);
  });
  double budget = slack_budget, leak = 0.0;
  for (int idx : order) {
    const double d = cands[idx].delay_penalty;
    if (d <= budget) {
      budget -= d;
      leak += cands[idx].leakage_saving;
    } else {
      if (d > 0.0) {
        leak += cands[idx].leakage_saving * (budget / d);  // fractional take
      }
      break;
    }
  }
  return leak;
}

}  // namespace rsz
