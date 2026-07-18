#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025, The OpenROAD Authors
"""Deterministic unit test for lef_stack_to_rcx_process.py.

Pure-Python (no OpenROAD / FasterCap): fabricates a SYNTHETIC 2-metal LEF + a
synthetic ITF dielectric stack with hand-chosen numbers, runs the converter,
and asserts the emitted OpenRCX process file carries the correct conductor
resistivity (= RPERSQ * thickness), geometry, and dielectric epsilon.  No PDK
data.  Exit 0 on pass, 1 on failure.
"""
from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lef_stack_to_rcx_process as conv  # noqa: E402

# ── Synthetic fixtures (fabricated numbers, no PDK) ──────────────────────────

SYNTH_LEF = """
LAYER M1
    TYPE ROUTING ;
    WIDTH 0.20 ;
    SPACING 0.20 ;
    THICKNESS 0.50 ;
    RESISTANCE RPERSQ 0.100 ;
    CAPACITANCE CPERSQDIST 0.00010 ;
    EDGECAPACITANCE 0.00005 ;
END M1

LAYER via1
    TYPE CUT ;
END via1

LAYER M2
    TYPE ROUTING ;
    WIDTH 0.40 ;
    SPACING 0.40 ;
    THICKNESS 0.80 ;
    RESISTANCE RPERSQ 0.050 ;
    CAPACITANCE CPERSQDIST 0.00008 ;
    EDGECAPACITANCE 0.00004 ;
END M2
"""

SYNTH_ITF = """
DIELECTRIC ild_a { THICKNESS=0.30 ER=4.20 }
DIELECTRIC ild_b { THICKNESS=0.50 ER=3.90 }
"""


def _num(process_text: str, cond: str, key: str) -> float:
    m = re.search(rf"CONDUCTOR {cond} \{{(.*?)\}}", process_text, re.DOTALL)
    assert m, f"conductor {cond} missing"
    mk = re.search(rf"{key}\s+([-+0-9.eE]+)", m.group(1))
    assert mk, f"{key} missing in {cond}"
    return float(mk.group(1))


def run() -> int:
    fails = []
    with tempfile.TemporaryDirectory() as d:
        dp = Path(d)
        lef = dp / "synth.lef"
        lef.write_text(SYNTH_LEF)
        itf = dp / "synth.itf"
        itf.write_text(SYNTH_ITF)
        out = dp / "process"

        rc = conv.main(["--lef", str(lef), "--itf", str(itf),
                        "--out", str(out)])
        if rc != 0:
            print(f"FAIL: converter rc={rc}")
            return 1
        text = out.read_text()

        # 1. Two conductors, in bottom-up order (via1 skipped).
        conductors = re.findall(r"CONDUCTOR (\S+) \{", text)
        if conductors != ["M1", "M2"]:
            fails.append(f"conductor order/set wrong: {conductors}")

        # 2. resistivity = RPERSQ * thickness.
        #    M1: 0.100 * 0.50 = 0.05 ;  M2: 0.050 * 0.80 = 0.04
        for cond, want in (("M1", 0.100 * 0.50), ("M2", 0.050 * 0.80)):
            got = _num(text, cond, "resistivity")
            if abs(got - want) > 1e-9:
                fails.append(f"{cond} resistivity {got} != {want}")

        # 3. geometry passthrough.
        if abs(_num(text, "M1", "thickness") - 0.50) > 1e-9:
            fails.append("M1 thickness wrong")
        if abs(_num(text, "M2", "min_width") - 0.40) > 1e-9:
            fails.append("M2 min_width wrong")
        if abs(_num(text, "M1", "min_spacing") - 0.20) > 1e-9:
            fails.append("M1 min_spacing wrong")

        # 4. dielectric epsilons present from the ITF.
        eps = set(re.findall(r"epsilon\s+([0-9.]+)", text))
        for want in ("4.2", "3.9"):
            if want not in eps:
                fails.append(f"dielectric epsilon {want} missing (got {eps})")

        # 5. uniform-ILD mode also works (LEF-only, no ITF).
        out2 = dp / "process_ild"
        rc2 = conv.main(["--lef", str(lef), "--ild-er", "4.2",
                         "--out", str(out2)])
        if rc2 != 0:
            fails.append("uniform-ILD mode rc != 0")
        else:
            t2 = out2.read_text()
            if "epsilon 4.2" not in t2:
                fails.append("uniform-ILD epsilon missing")
            if abs(_num(t2, "M2", "resistivity") - 0.04) > 1e-9:
                fails.append("uniform-ILD M2 resistivity wrong")

    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("PASS: lef_stack_to_rcx_process converter unit test "
          "(resistivity=RPERSQ*thickness, geometry, ITF+uniform-ILD)")
    return 0


if __name__ == "__main__":
    raise SystemExit(run())
