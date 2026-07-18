#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025, The OpenROAD Authors
"""fastercap_gate.py — end-to-end analytic correctness gate for the
lef_stack_to_rcx_process converter, using OpenRCX pattern generation + the
FasterCap field solver.

SYNTHETIC parallel-plate stack (fabricated numbers, NO PDK): a wide upper metal
(width W, length L) over a lower ground plane, separated by an inter-level
dielectric of thickness d and relative permittivity er.  The converter emits
the OpenRCX process file; OpenRCX `gen_solver_patterns` turns the stack into a
field-solver geometry; FasterCap computes the capacitance.  The gate asserts:

  (1) PHYSICAL BAND: the field-solved plate capacitance is within [0.5x, 2x] of
      the closed-form parallel-plate  C = e0 * er * (W*L) / d  (a wide plate is
      parallel-plate-dominated; the band absorbs fringe + mesh error).
  (2) er-SCALING: C(er=2*e) > C(er=e) monotonically — the dielectric constant
      the converter wrote must propagate through OpenRCX into the solver.  This
      is robust to absolute fringe/tiling error and isolates the physics law.

Requires openroad + FasterCap on PATH (present in the runtime image); run where
those exist.  This is the committed public artifact — nothing proprietary.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

E0 = 8.854e-12  # F/m

HERE = Path(__file__).resolve().parent
CONVERTER = HERE / "lef_stack_to_rcx_process.py"
# UniversalFormat2FasterCap ships with the rcx v2 FasterCap regression scripts.
FC_CONV = (HERE / ".." / "test" / "rcx_v2" / "FasterCapModel" / "scripts"
           / "UniversalFormat2FasterCap_923.py").resolve()

# Synthetic wide-plate stack (fabricated; um).
MIN_WIDTH = 3.0
THICK = 0.30
BASE_GAP = 0.30
RPERSQ = 0.10


def _synth_lef(path: Path) -> None:
    path.write_text(
        "LAYER M1\n  TYPE ROUTING ;\n  WIDTH %g ;\n  SPACING %g ;\n"
        "  THICKNESS %g ;\n  RESISTANCE RPERSQ %g ;\nEND M1\n"
        "LAYER v1\n  TYPE CUT ;\nEND v1\n"
        "LAYER M2\n  TYPE ROUTING ;\n  WIDTH %g ;\n  SPACING %g ;\n"
        "  THICKNESS %g ;\n  RESISTANCE RPERSQ %g ;\nEND M2\n"
        % (MIN_WIDTH, MIN_WIDTH, THICK, RPERSQ,
           MIN_WIDTH, MIN_WIDTH, THICK, RPERSQ))


def _run(cmd: str, cwd: Path) -> str:
    p = subprocess.run(["bash", "-lc", cmd], cwd=str(cwd),
                       capture_output=True, text=True, timeout=600)
    return (p.stdout or "") + "\n" + (p.stderr or "")


def solve_one(work: Path, er: float) -> tuple[float, float, float]:
    """Run converter -> gen_solver_patterns -> FasterCap for a given er.
    Returns (C_fastercap_F, C_analytic_F, gap_d_um)."""
    lef = work / "synth.lef"
    _synth_lef(lef)
    proc = work / "process"
    rc = subprocess.run(
        [sys.executable, str(CONVERTER), "--lef", str(lef),
         "--ild-er", str(er), "--base-gap", str(BASE_GAP), "--out", str(proc)],
        capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError(f"converter failed: {rc.stderr}")

    pat = work / "pat"
    pat.mkdir(exist_ok=True)
    _run("printf 'gen_solver_patterns -process_file ../process "
         "-process_name TYP -wire_cnt 1 -version 2\\nexit\\n' | "
         "openroad -no_init > gen.log 2>&1", pat)

    # Locate the over-ground pattern (upper metal over lower ground plane).
    wires = None
    for w in (pat / "TYP").rglob("wires"):
        if "Over1/M2oM1" in str(w):
            wires = w
            break
    if wires is None:
        raise RuntimeError("no Over1/M2oM1 pattern generated")

    # Parse gap d = (upper wire bottom height) - (ground-plane top height).
    txt = wires.read_text()
    gp = re.search(r"GROUND_PLANE\s+\d+\s+\S+\s+HEIGHT\s+([-0-9.]+)\s+"
                   r"([-0-9.]+)", txt)
    wr = re.search(r"WIRE\s+\d+\s+\S+\s+LL\s+[-0-9.]+\s+([-0-9.]+)", txt)
    gp_top = float(gp.group(2))
    wire_bot = float(wr.group(1))
    d_um = wire_bot - gp_top
    # Wide-plate length from the pattern.
    lm = re.search(r"LENGTH\s+([0-9.]+)", txt)
    length_um = float(lm.group(1))

    c_analytic = E0 * er * (MIN_WIDTH * 1e-6 * length_um * 1e-6) / (d_um * 1e-6)

    # Convert the pattern to FasterCap + run.
    fc_out = pat / "fc_out"
    if fc_out.exists():
        _run("rm -rf fc_out", pat)
    fc_out.mkdir()
    _run(f"python3 {FC_CONV} ../process TYP/Over1/M2oM1 fc_out standard "
         "> conv.log 2>&1", pat)
    lst = next(iter(sorted(fc_out.rglob("*.lst"))), None)
    if lst is None:
        raise RuntimeError("FasterCap conversion produced no .lst")
    # The .lst references Wires/ six levels up from its dir; symlink so it
    # resolves regardless of nesting depth.
    depth_root = lst.parent
    for _ in range(6):
        depth_root = depth_root.parent
    src_wires = next(iter(pat.rglob("Wires")), None)
    link = depth_root / "Wires"
    if src_wires and not link.exists():
        try:
            link.symlink_to(src_wires)
        except OSError:
            pass
    out = _run(f"cd {lst.parent} && FasterCap -b {lst.name} -g -a0.01 "
               "2>&1 | tail -40", pat)

    # Last (converged) capacitance matrix; take g2 (upper metal) self term.
    mats = re.findall(
        r"g1_\S+\s+([-0-9.eE]+)\s+([-0-9.eE]+)\s*\n\s*g2_\S+\s+"
        r"([-0-9.eE]+)\s+([-0-9.eE]+)", out)
    if not mats:
        raise RuntimeError(f"no capacitance matrix parsed from FasterCap:\n"
                           f"{out[-800:]}")
    c_self = abs(float(mats[-1][3]))  # g2 diagonal = upper-metal self cap
    return c_self, c_analytic, d_um


def main() -> int:
    with tempfile.TemporaryDirectory() as d:
        base = Path(d)
        results = {}
        for er in (2.0, 4.0):
            w = base / f"er{er}"
            w.mkdir()
            c_fc, c_an, gap = solve_one(w, er)
            ratio = c_fc / c_an if c_an else 0.0
            results[er] = (c_fc, c_an, ratio, gap)
            print(f"er={er}: FasterCap C={c_fc*1e15:.3f} fF, "
                  f"analytic pp C={c_an*1e15:.3f} fF (gap {gap:.2f} um), "
                  f"ratio={ratio:.3f}")

        fails = []
        # (1) physical band on both corners
        for er, (c_fc, c_an, ratio, _g) in results.items():
            if not (0.5 <= ratio <= 2.0):
                fails.append(f"er={er} band: ratio {ratio:.3f} outside [0.5,2]")
        # (2) er-scaling monotonic (2x er -> more cap)
        if results[4.0][0] <= results[2.0][0]:
            fails.append("er-scaling: C(er=4) not > C(er=2)")

        if fails:
            for f in fails:
                print("FAIL:", f)
            return 1
        print("PASS: FasterCap analytic gate — plate C within [0.5x,2x] of "
              "e0*er*A/d at er in {2,4}, and C scales with er (converter er "
              "propagates through OpenRCX into the field solver).")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
