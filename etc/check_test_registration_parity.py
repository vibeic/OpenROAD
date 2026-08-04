#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, The OpenROAD Authors
"""Fail when a test is registered in CMake but not in bazel.

WHY THIS EXISTS
---------------
OpenROAD declares its regression tests in two systems that are meant to mirror
each other -- every `src/<mod>/test/BUILD` literally carries the comment
"# From CMakeLists.txt or_integration_tests(TESTS".  `etc/Build.sh` has
defaulted to bazel since the 2026-07-29 daily merge, so a test that exists only
in CMake never executes in the pipeline that produces the shipped binary.

Nothing compared the two sets, so the drift was invisible: the CMake run stayed
green, the register recorded a FAIL->PASS proof for each patch, and 28 of this
fork's own tests had stopped running in the build that ships.  Several BUILD
files even assert parity in a comment ("No tests are intentionally excluded")
while pointing at a `.kiro/specs/bazel-cmake-test-parity/` document that is in
neither this tree nor upstream -- a claim with no oracle behind it.

This is that oracle.  It is a program rather than prose for the same reason
`etc/find_dup_ids.sh` is: a rule a human has to remember to apply is a rule that
gets applied until the day it matters.

WHAT IT CHECKS
--------------
Both registration systems, enumerated at every location each one uses -- not the
one location a grep happened to guess:

  CMake integration  src/<mod>/test/CMakeLists.txt  or_integration_tests(...)
                     including names passed through a `set()`/`list(APPEND)` var
  CMake C++ unit     add_executable + add_test/gtest_discover_tests, in
                     src/<mod>/CMakeLists.txt (drt registers ONE LEVEL UP, in a
                     foreach), src/<mod>/test/CMakeLists.txt, and
                     src/<mod>/test/cpp/CMakeLists.txt
  bazel integration  regression_test(...) in src/<mod>/test/BUILD, including the
                     list comprehensions over COMPULSORY_TESTS / ALL_TESTS, plus
                     test_suite() wrappers
  bazel C++ unit     cc_test(...) anywhere under src/<mod>

C++ tests are matched on the target's PRINCIPAL SOURCE -- the one whose stem is
the target name.  All three of the obvious rules are wrong, each in its own
direction, and each was tried here first:

  by target NAME       bazel renames them (CMake `gcTest` is bazel
                       `gc_unittest`), so covered tests report as missing;
  by source INTERSECTION
                       drt's four unit tests all list `fixture.cpp`, so any
                       overlap makes the two genuinely-unwired ones look
                       covered -- a false green, the dangerous direction;
  by source SUBSET     drt's CMake `foreach` blanket-adds `fixture.cpp` to all
                       four targets even though `multiPinBTermTest.cpp` and
                       `routingViaConnectivityTest.cpp` never include it, so a
                       correctly-narrower bazel `srcs` reports as missing.

The principal source is the invariant: it is what "this test" MEANS, it survives
the rename, it is not shared with a sibling, and a bazel target that compiles it
is running that test whatever else it does or does not link.

Exit 0 when every CMake-registered test has a bazel target or appears in
KNOWN_CMAKE_ONLY below; exit 1 otherwise.

  python3 etc/check_test_registration_parity.py [tree_root]
"""

import ast
import os
import re
import sys
from pathlib import Path

# Tests that are CMake-only ON PURPOSE.  Every entry needs a reason, and the
# reason has to be about the TEST, not about how much work wiring it would be.
# This list is not a place to park a failure: adding a row here is a claim that
# the shipped binary does not need that test, and it is reviewed as one.
KNOWN_CMAKE_ONLY = {
    # `<mod>:cpp_tests` is a CMake-shaped harness: the .tcl shells out to
    # `build/src/<mod>/test/cpp` and runs whatever `Test*` binaries CMake put
    # there.  bazel has no such directory and does not need one -- under bazel
    # each gtest binary IS its own cc_test target and runs directly.  These are
    # superseded, not skipped, and the checker still requires the underlying
    # cc_test targets to exist.
    # (odb/rsz/utl/web declare a `cpp_tests` bazel target of their own, so they
    # do NOT belong here -- the stale-entry report below is what caught that.)
    "drt:cpp_tests": "CMake-shaped harness; bazel runs each gtest as its own cc_test",
    "psm:cpp_tests": "CMake-shaped harness; bazel runs each gtest as its own cc_test",
    # GPU tests are guarded in CMake behind a CUDA-enabled build.  They are
    # upstream's, and upstream has not wired a GPU configuration into bazel.
    # Not ours to wire, and not part of the shipped CPU image.
    "gpl:region01_gpu": "upstream CUDA-only; no GPU configuration exists in bazel",
    "gpl:region01_gpu_asym": "upstream CUDA-only; no GPU configuration exists in bazel",
    "gpl:fft_gpu_test": "upstream CUDA-only; no GPU configuration exists in bazel",
    "gpl:wl_gpu_test": "upstream CUDA-only; no GPU configuration exists in bazel",
}


# ---------------------------------------------------------------- CMake side

def _balanced_call_args(text, start):
    i = text.index("(", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                return text[i + 1: j]
    raise ValueError("unbalanced parens")


def _strip_comments(text):
    return "\n".join(re.sub(r"(?<!\\)#.*$", "", ln) for ln in text.splitlines())


def cmake_integration_tests(path):
    if not path.exists():
        return set()
    text = _strip_comments(path.read_text())

    variables = {}
    for m in re.finditer(r"\bset\s*\(", text):
        toks = _balanced_call_args(text, m.end() - 1).replace('"', " ").split()
        if toks:
            variables[toks[0]] = list(toks[1:])
    for m in re.finditer(r"\blist\s*\(\s*APPEND\b", text):
        toks = _balanced_call_args(text, m.end() - 1).replace('"', " ").split()
        if len(toks) >= 2:
            variables.setdefault(toks[1], []).extend(toks[2:])

    def expand(tok):
        m = re.fullmatch(r"\$\{(\w+)\}", tok)
        return variables.get(m.group(1), []) if m else [tok]

    tests = set()
    for m in re.finditer(r"\bor_integration_tests\b", text):
        toks = _balanced_call_args(text, m.end()).replace('"', " ").split()
        active = False
        for tok in toks[1:]:
            if tok in ("TESTS", "PASSFAIL_TESTS"):
                active = True
            elif active:
                tests.update(expand(tok))
    return tests


def cmake_cpp_tests(mod_dir):
    """-> {target: set(source basenames)}"""
    found = {}
    seen = set()
    for p in mod_dir.rglob("CMakeLists.txt"):
        if p in seen:
            continue
        seen.add(p)
        text = _strip_comments(p.read_text())
        for m in re.finditer(r"\bforeach\s*\(", text):
            toks = _balanced_call_args(text, m.end() - 1).replace('"', " ").split()
            if not toks:
                continue
            loopvar = toks[0]
            body = text[text.index(")", m.end() - 1):][:4000]
            ae = re.search(
                r"add_executable\s*\(\s*\$\{%s\}(?P<srcs>[^)]*)\)" % re.escape(loopvar),
                body)
            if not ae:
                continue
            for t in toks[1:]:
                if t.startswith("$") or t in ("IN", "LISTS", "ITEMS", "RANGE"):
                    continue
                found[t] = {
                    os.path.basename(s.replace("${%s}" % loopvar, t))
                    for s in ae.group("srcs").split()
                    if s.endswith((".cpp", ".cc", ".cxx"))}
        for m in re.finditer(r"\badd_executable\s*\(", text):
            toks = _balanced_call_args(text, m.end() - 1).replace('"', " ").split()
            if not toks or toks[0].startswith("$"):
                continue
            name = toks[0]
            if re.search(r"\b(add_test|gtest_discover_tests)\s*\([^)]*\b%s\b"
                         % re.escape(name), text):
                found[name] = {os.path.basename(s) for s in toks[1:]
                               if s.endswith((".cpp", ".cc", ".cxx"))}
    return found


# ---------------------------------------------------------------- bazel side

class BuildFile:
    """Evaluate a BUILD file's string-list vocabulary with Python's ast.

    The subset of Starlark these files use -- string lists, `+` concatenation,
    and single-variable list comprehensions -- is syntactically valid Python, so
    the parse is exact rather than a regex approximation.
    """

    def __init__(self, path):
        self.lists = {}
        self.regression = set()
        self.cc_tests = {}
        try:
            tree = ast.parse(path.read_text())
        except SyntaxError as exc:
            print(f"  !! unparsable BUILD {path}: {exc}", file=sys.stderr)
            return
        for node in tree.body:
            if (isinstance(node, ast.Assign) and len(node.targets) == 1
                    and isinstance(node.targets[0], ast.Name)):
                val = self._as_str_list(node.value)
                if val is not None:
                    self.lists[node.targets[0].id] = val
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
                self._call(node)
            elif isinstance(node, (ast.ListComp, ast.GeneratorExp)):
                self._comprehension(node)

    def _as_str_list(self, node):
        if isinstance(node, ast.List):
            out = []
            for e in node.elts:
                if isinstance(e, ast.Constant) and isinstance(e.value, str):
                    out.append(e.value)
                else:
                    return None
            return out
        if isinstance(node, ast.Name):
            return self.lists.get(node.id)
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            l = self._as_str_list(node.left)
            r = self._as_str_list(node.right)
            if l is not None and r is not None:
                return l + r
        return None

    @staticmethod
    def _kw(call, key):
        for kw in call.keywords:
            if kw.arg == key:
                return kw.value
        return None

    def _call(self, call):
        fn = call.func.id
        name_node = self._kw(call, "name")
        if name_node is None:
            return
        if not (isinstance(name_node, ast.Constant)
                and isinstance(name_node.value, str)):
            return
        if fn == "test_suite":
            self.regression.add(name_node.value)
            for t in (self._as_str_list(self._kw(call, "tests")) or []):
                self.regression.add(t.lstrip(":"))
        elif fn == "cc_test":
            srcs = self._as_str_list(self._kw(call, "srcs")) or []
            self.cc_tests[name_node.value] = {
                os.path.basename(s) for s in srcs
                if s.endswith((".cpp", ".cc", ".cxx"))}
        elif fn.endswith("_test"):
            self.regression.add(name_node.value)

    def _comprehension(self, comp):
        elt = comp.elt
        if not (isinstance(elt, ast.Call) and isinstance(elt.func, ast.Name)):
            return
        if not elt.func.id.endswith("_test") or len(comp.generators) != 1:
            return
        gen = comp.generators[0]
        if not isinstance(gen.target, ast.Name):
            return
        items = self._as_str_list(gen.iter)
        name_node = self._kw(elt, "name")
        if items is None or name_node is None:
            return
        for item in items:
            rendered = self._render(name_node, gen.target.id, item)
            if rendered is None:
                continue
            if elt.func.id == "cc_test":
                self.cc_tests.setdefault(rendered, set())
            else:
                self.regression.add(rendered)

    def _render(self, node, var, value):
        if isinstance(node, ast.Name) and node.id == var:
            return value
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return node.value
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            l = self._render(node.left, var, value)
            r = self._render(node.right, var, value)
            if l is not None and r is not None:
                return l + r
        return None


def principal_source(target, srcs):
    """The source that IS this test, as opposed to helpers compiled alongside.

    Matched on the stem so it survives case and separator differences between
    the two build systems.  Returns None when no source names the target, in
    which case the caller falls back to requiring the whole set.
    """
    want = target.lower().replace("_", "")
    for s in srcs:
        if os.path.splitext(s)[0].lower().replace("_", "") == want:
            return s
    return None


def covered_by_bazel(target, cmake_srcs, bazel_cc):
    if not cmake_srcs:
        return False
    principal = principal_source(target, cmake_srcs)
    if principal is not None:
        return any(principal in b for b in bazel_cc.values())
    # No source names the target (an unusual CMake shape): fall back to
    # demanding that some single bazel target compiles the whole set, which is
    # strict but never claims coverage that is not there.
    return any(cmake_srcs <= b for b in bazel_cc.values())


def bazel_tests(mod_dir):
    reg, cc = set(), {}
    for pattern in ("BUILD", "BUILD.bazel"):
        for p in mod_dir.rglob(pattern):
            bf = BuildFile(p)
            reg |= bf.regression
            cc.update(bf.cc_tests)
    return reg, cc


# ---------------------------------------------------------------- main

def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    src = root / "src"
    if not src.is_dir():
        print(f"ERROR: {src} is not a directory -- run from the OpenROAD tree root")
        return 2

    unwired = []
    checked = 0
    for mod_dir in sorted(p for p in src.iterdir()
                          if p.is_dir() and (p / "CMakeLists.txt").exists()):
        mod = mod_dir.name
        c_int = cmake_integration_tests(mod_dir / "test" / "CMakeLists.txt")
        c_cpp = cmake_cpp_tests(mod_dir)
        b_reg, b_cc = bazel_tests(mod_dir)
        checked += len(c_int) + len(c_cpp)

        for name in sorted(c_int - b_reg):
            unwired.append((f"{mod}:{name}", "integration"))
        for name, srcs in sorted(c_cpp.items()):
            if not covered_by_bazel(name, srcs, b_cc):
                unwired.append((f"{mod}:{name}", "cpp-unit"))

    unexpected = [(k, w) for k, w in unwired if k not in KNOWN_CMAKE_ONLY]
    stale = sorted(set(KNOWN_CMAKE_ONLY) - {k for k, _ in unwired})

    print(f"CMake-registered tests checked : {checked}")
    print(f"  with no bazel target         : {len(unwired)}")
    print(f"  of those, allowed by name    : {len(unwired) - len(unexpected)}")
    print(f"  UNEXPECTED                   : {len(unexpected)}")

    if stale:
        print("\nSTALE ALLOWLIST ENTRIES -- these are now wired, delete the rows:")
        for k in stale:
            print(f"  {k}")

    if unexpected:
        print("\nFAIL: registered in CMake, absent from the bazel build that "
              "produces the shipped binary:\n")
        for key, what in unexpected:
            print(f"  {what:12} {key}")
        print("\nWire each into its module's BUILD, or add it to KNOWN_CMAKE_ONLY")
        print("with a reason about the TEST. Declare every file the test reads:")
        print("bazel sandboxes, and an undeclared data file fails the test for a")
        print("reason that has nothing to do with the code under test.")
        return 1

    print("\nPASS: every CMake-registered test has a bazel target or a "
          "named, justified exemption.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
