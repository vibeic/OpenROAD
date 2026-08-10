#!/usr/bin/env python3
"""A golden must not assert a severity the code can no longer emit.

WHY THIS EXISTS (measured 2026-08-10)
-------------------------------------
`5549ae9a8a` promoted DPL-0701 and DPL-0033 from `logger_->warn` to
`logger_->error` on purpose -- a legality failure must never be a silent pass.
It regenerated the goldens of the tests IT ADDED and left upstream tests red:

    dpl.fragmented_row03.tcl   dpl.obstruction2.tcl (+ .py)
    dpl.report_failures.tcl    cts.array_no_blockages.tcl

Their `.ok` files still recorded `[WARNING DPL-0701]`. Nothing noticed, because
the fork's declared checks are static and cheap by design -- they must run in a
merge worktree in seconds -- while a golden mismatch only surfaces under a full
`ctest`, which the daily round does not run. So a fork patch could redden an
upstream test and ship.

The mismatch is visible in the source tree alone, so this closes that hole with
no build: if the code can only ever emit a message as an error, no golden may
claim it was a warning, and vice versa.

MATCHING ON THE TEXT, NOT JUST THE ID -- and why that is load-bearing
---------------------------------------------------------------------
A message id is NOT unique to one message. `ODB-220` is `logger->error(ODB, 220,
"Cannot map port {} to {} ...")` in dbPowerSwitch.cpp AND, in eighteen goldens,
the LEF parser's `[WARNING ODB-0220] ... NOWIREEXTENSIONATPIN ...`, which comes
from the table in src/odb/src/lef/clef/lefMsgTable.h and never passes through a
`logger_->` call this scan can see. Keying on (module, id) alone reported all
eighteen as defects on a tree that was correct. So a finding also requires the
golden's text to be the text of THAT call -- compared on the literal prefix
before the first `{}` placeholder.

WHAT IT DOES NOT CLAIM
----------------------
Deliberately narrow and fail-safe:

  * An id+text emitted at more than one severity is SKIPPED, not flagged. Both
    spellings are legitimately reachable and the source cannot settle it.
    (DPL-33 vs DPL-40 is the deliberate two-id version of that, unaffected.)
  * A message with no literal prefix to compare (format starts with `{}`) is
    skipped rather than guessed at.
  * It says nothing about the REST of a golden. A test can still be red for a
    reason this check cannot see; it only makes THIS failure mode unshippable.

Exit: 0 clean · 1 at least one golden asserts an unreachable severity
"""
from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

# `logger_->error(DPL, 701, "text {}")`, `->warn(utl::GRT,\n 12,\n "text")`.
# The id and the format string routinely sit on their own lines.
CALL = re.compile(
    r"->(?P<sev>error|warn)\s*\(\s*(?:utl::)?(?P<mod>[A-Z][A-Z0-9_]*)\s*,\s*"
    r"(?P<id>\d+)\s*,\s*(?P<fmt>(?:\s*\"(?:[^\"\\]|\\.)*\")+)",
    re.S,
)
# `[WARNING DPL-0701] text...` as the logger prints it.
GOLDEN = re.compile(
    r"\[(?P<sev>WARNING|ERROR)\s+(?P<mod>[A-Z][A-Z0-9_]*)-(?P<id>\d+)\]\s?(?P<text>[^\n]*)"
)

SEV_OF_GOLDEN = {"WARNING": "warn", "ERROR": "error"}
SRC_SUFFIXES = (".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp")
#: Below this many literal characters the prefix is too weak to identify a
#: message, so the pair is skipped instead of guessed at.
MIN_PREFIX = 12


def literal_prefix(fmt_tokens: str) -> str:
    """The fixed text a format string emits before its first substitution."""
    parts = re.findall(r"\"((?:[^\"\\]|\\.)*)\"", fmt_tokens)
    joined = "".join(parts)
    joined = joined.replace("\\n", "\n").replace('\\"', '"').replace("\\\\", "\\")
    return joined.split("{")[0].strip()


def emitted(root: Path) -> dict[tuple[str, int], dict[str, set[str]]]:
    """(module, id) -> literal prefix -> severities the SOURCE emits it at."""
    out: dict[tuple[str, int], dict[str, set[str]]] = defaultdict(
        lambda: defaultdict(set))
    for path in root.glob("src/**/*"):
        if not path.is_file() or path.suffix not in SRC_SUFFIXES:
            continue
        if "/test/" in path.as_posix():
            continue                      # a fixture's illustrative call is not
        try:                              # what the shipped code emits
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in CALL.finditer(text):
            prefix = literal_prefix(m.group("fmt"))
            if len(prefix) < MIN_PREFIX:
                continue
            out[(m.group("mod"), int(m.group("id")))][prefix].add(m.group("sev"))
    return out


def main(argv: list[str]) -> int:
    root = Path(argv[1] if len(argv) > 1 else ".").resolve()
    src = emitted(root)

    findings: set[str] = set()
    goldens = sorted(root.glob("src/*/test/**/*.ok"))
    for gold in goldens:
        try:
            text = gold.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in GOLDEN.finditer(text):
            key = (m.group("mod"), int(m.group("id")))
            by_prefix = src.get(key)
            if not by_prefix:
                continue                  # never emitted through a visible call
            want = SEV_OF_GOLDEN[m.group("sev")]
            golden_text = m.group("text").strip()
            for prefix, severities in by_prefix.items():
                if not golden_text.startswith(prefix):
                    continue              # a different message sharing the id
                if len(severities) != 1:
                    continue              # ambiguous by construction
                only = next(iter(severities))
                if only != want:
                    findings.add(
                        f"{gold.relative_to(root)}: asserts "
                        f"[{m.group('sev')} {m.group('mod')}-{int(m.group('id')):04d}] "
                        f"but the source only ever emits that message via "
                        f"logger_->{only}()")

    if findings:
        print("check_severity_golden_parity: "
              f"{len(findings)} golden assertion(s) name a severity the code "
              "cannot emit:")
        for f in sorted(findings):
            print(f"  {f}")
        print("  A patch that changes a message's severity must regenerate every "
              "golden that records it, or the test is red on a tree that is "
              "otherwise correct.")
        return 1

    print(f"check_severity_golden_parity: PASS — {len(goldens)} golden(s) "
          f"checked against {len(src)} emitted message id(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
