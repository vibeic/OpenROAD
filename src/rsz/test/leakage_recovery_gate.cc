// RS6 multi-Vt leakage-recovery correctness gate (standalone, dependency-free).
//
// Independent oracle: the test hand-sorts the candidates by leakage density and
// hand-accumulates the admittable prefix under the budget; leakageRecovery()
// reaches the same admitted set/recovery through the header path.  The budget
// bound and the "no phantom recovery" negative are checked directly.
//
// Checks (positive AND paired negative in one binary):
//   [P1] budget is NEVER exceeded (delay_used <= budget) across many budgets
//   [N1] zero and negative budget -> empty admit, 0 recovered, 0 delay used
//        (no phantom recovery); a positive budget on the SAME cells recovers > 0,
//        so this is correct-empty not broken-empty
//   [P2] MONOTONE in budget: as the budget grows the admitted set is a superset
//        and leakage_recovered is non-decreasing (the property first-fit greedy
//        would violate)
//   [P3] matches the independent density-sorted prefix (set + amount)
//   [P4] a free (zero-delay) positive swap is ALWAYS admitted, even at a tiny
//        budget; a zero-saving swap is never admitted (not worth its delay)
//   [P5] integral prefix recovery <= fractional LP bound, and == it when the
//        budget exactly covers a whole prefix

#include "rsz/LeakageRecovery.hh"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace rsz;

static int fails = 0;
static void ok(bool c, const char *m) {
  if (!c) { std::printf("  FAIL: %s\n", m); ++fails; }
}
static void near(double g, double e, const char *m, double tol) {
  if (std::fabs(g - e) > tol) { std::printf("  FAIL: %s: %.17g != %.17g\n", m, g, e); ++fails; }
}

// independent oracle: density-sorted prefix under a budget -> (recovered, delay, set)
static LeakageRecoveryResult oraclePrefix(const std::vector<VtSwapCandidate> &c, double budget) {
  LeakageRecoveryResult r;
  if (!(budget > 0.0)) return r;
  std::vector<int> ord;
  for (size_t i = 0; i < c.size(); ++i) if (c[i].leakage_saving > 0.0) ord.push_back((int)i);
  std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) {
    double da = (c[a].delay_penalty > 0.0) ? c[a].leakage_saving / c[a].delay_penalty : 1e300;
    double db = (c[b].delay_penalty > 0.0) ? c[b].leakage_saving / c[b].delay_penalty : 1e300;
    return da > db;
  });
  for (int i : ord) {
    if (r.delay_used + c[i].delay_penalty <= budget) {
      r.delay_used += c[i].delay_penalty;
      r.leakage_recovered += c[i].leakage_saving;
      r.admitted.push_back(i);
    } else break;
  }
  return r;
}

int main() {
  // Candidates: {leakage_saving, delay_penalty}.  Densities:
  //  0: 6.0 / 3.0 = 2.0     1: 4.0 / 1.0 = 4.0     2: 9.0 / 3.0 = 3.0
  //  3: 2.0 / 0.0 = inf(free)  4: 5.0 / 5.0 = 1.0   5: 0.0 / 2.0 = 0-saving (skip)
  std::vector<VtSwapCandidate> c = {
    {6.0, 3.0}, {4.0, 1.0}, {9.0, 3.0}, {2.0, 0.0}, {5.0, 5.0}, {0.0, 2.0},
  };
  // density order (desc): 3(inf), 1(4), 2(3), 0(2), 4(1)   [5 excluded, 0-saving]

  // [P1] budget never exceeded, over a sweep.
  for (double b = 0.0; b <= 15.0; b += 0.5) {
    LeakageRecoveryResult r = leakageRecovery(c, b);
    ok(r.delay_used <= b + 1e-12, "P1 delay_used <= budget");
  }

  // [N1] zero / negative budget -> nothing; positive on same cells -> something.
  LeakageRecoveryResult z = leakageRecovery(c, 0.0);
  ok(z.admitted.empty() && z.leakage_recovered == 0.0 && z.delay_used == 0.0, "N1 zero budget empty");
  LeakageRecoveryResult neg = leakageRecovery(c, -5.0);
  ok(neg.admitted.empty() && neg.leakage_recovered == 0.0, "N1 negative budget empty");
  ok(leakageRecovery(c, 1.0).leakage_recovered > 0.0, "N1 positive budget recovers > 0 (not broken-empty)");

  // [P2] monotone in budget: superset admitted set + non-decreasing recovery.
  double prev_leak = -1.0;
  std::vector<int> prev_set;
  for (double b = 0.0; b <= 15.0; b += 0.5) {
    LeakageRecoveryResult r = leakageRecovery(c, b);
    ok(r.leakage_recovered + 1e-12 >= prev_leak, "P2 recovery non-decreasing in budget");
    // admitted set is a superset of the previous budget's set
    for (int idx : prev_set) {
      ok(std::find(r.admitted.begin(), r.admitted.end(), idx) != r.admitted.end(),
         "P2 admitted set is a superset as budget grows");
    }
    prev_leak = r.leakage_recovered;
    prev_set = r.admitted;
  }

  // [P3] matches the independent density-sorted prefix (set + amount).
  for (double b : {1.0, 2.0, 4.0, 5.0, 7.0, 10.0, 12.0}) {
    LeakageRecoveryResult got = leakageRecovery(c, b);
    LeakageRecoveryResult exp = oraclePrefix(c, b);
    near(got.leakage_recovered, exp.leakage_recovered, "P3 recovery == oracle", 1e-12);
    near(got.delay_used, exp.delay_used, "P3 delay == oracle", 1e-12);
    ok(got.admitted == exp.admitted, "P3 admitted set == oracle");
  }

  // At budget 4.0 the density-prefix is {3(free,0),1(1),2(3)} -> delay 4, leak 2+4+9=15.
  LeakageRecoveryResult r4 = leakageRecovery(c, 4.0);
  near(r4.delay_used, 4.0, "P3 b=4 delay==4", 1e-12);
  near(r4.leakage_recovered, 15.0, "P3 b=4 leak==15", 1e-12);
  ok((r4.admitted == std::vector<int>{3, 1, 2}), "P3 b=4 admitted == {3,1,2}");

  // [P4] free positive swap always admitted at a tiny budget; 0-saving never.
  LeakageRecoveryResult tiny = leakageRecovery(c, 1e-9);
  ok(std::find(tiny.admitted.begin(), tiny.admitted.end(), 3) != tiny.admitted.end(),
     "P4 free swap admitted at tiny budget");
  LeakageRecoveryResult big = leakageRecovery(c, 100.0);  // budget covers everything
  ok(std::find(big.admitted.begin(), big.admitted.end(), 5) == big.admitted.end(),
     "P4 zero-saving swap never admitted");

  // [P5] integral prefix <= fractional LP bound; equal when budget covers a whole prefix.
  for (double b : {1.5, 2.5, 4.0, 6.0, 8.5, 11.0}) {
    double integral = leakageRecovery(c, b).leakage_recovered;
    double frac = leakageRecoveryFractionalBound(c, b);
    ok(integral <= frac + 1e-12, "P5 integral <= fractional bound");
  }
  // budget 4.0 exactly covers prefix {3,1,2} -> integral == fractional there.
  near(leakageRecovery(c, 4.0).leakage_recovered, leakageRecoveryFractionalBound(c, 4.0),
       "P5 integral == fractional at exact-prefix budget", 1e-12);

  if (fails == 0) {
    std::printf("leakage recovery gate PASS: b=4 -> leak=%.1f delay=%.1f admitted={3,1,2} monotone+budget-safe\n",
                r4.leakage_recovered, r4.delay_used);
    return 0;
  }
  std::printf("leakage recovery gate FAIL: %d checks\n", fails);
  return 1;
}
