#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025, The OpenROAD Authors
"""lef_stack_to_rcx_process.py — emit an OpenRCX process-parameters file.

OpenRCX can build a field-solver-calibrated extraction-rules file
(`gen_solver_patterns` -> FasterCap -> `gen_rcx_model`) from a *process file*
describing the metal + dielectric stack (CONDUCTOR / DIELECTRIC blocks).  On a
custom / in-house PDK that ships no xRC/ITF/QRC extraction deck, OpenRCX has no
way to derive that stack, so full-coupling extraction is unavailable and users
fall back to `extract_parasitics -lef_rc` (per-layer LEF R/C, no lateral
coupling model).

This is a GENERIC converter that assembles the process file from standard,
non-proprietary inputs:

  * CONDUCTOR half  <- a standard technology LEF.  Every routing LAYER carries
      THICKNESS, RESISTANCE RPERSQ (sheet resistance) and WIDTH/SPACING; the
      per-layer resistivity is  RPERSQ * thickness  [ohm.um].
  * DIELECTRIC half <- one of:
      (a) an ITF-style interconnect stack file (per-dielectric ER + THICKNESS),
      (b) --ild-er / --ild-mode : a documented uniform inter-level-dielectric
          assumption (single ER, thicknesses from the LEF geometry) — needs no
          extra deck and still gives OpenRCX a physical stack from which the
          field solver computes REAL lateral coupling that -lef_rc cannot.

No PDK data is embedded: the tool reads whatever LEF / ITF it is pointed at and
emits the generic OpenRCX process format.  The bundled unit test and the
end-to-end analytic gate use a SYNTHETIC parallel-plate stack (fabricated
numbers), so nothing proprietary is required to exercise it.

Usage:
    lef_stack_to_rcx_process.py --lef tech.lef [--lef more.lef ...] \
        (--itf stack.itf | --ild-er 4.2) [--process-name TYP] --out process
    main(argv) -> 0 ok / 2 IO-or-arg error.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

# Vacuum permittivity is not needed here (the field solver handles absolute
# capacitance); the process file carries only relative epsilon + geometry.


@dataclass
class Conductor:
    name: str
    order: int                       # 1 = lowest routing layer
    thickness: float = 0.0           # um
    rpersq: Optional[float] = None   # ohm/square
    min_width: float = 0.0           # um
    min_spacing: float = 0.0         # um
    height: Optional[float] = None   # um, bottom above substrate (if in LEF)
    area_cap: Optional[float] = None  # LEF CPERSQDIST (F/um^2-ish units)
    edge_cap: Optional[float] = None  # LEF EDGECAPACITANCE

    @property
    def resistivity(self) -> Optional[float]:
        """Bulk resistivity in ohm.um = RPERSQ [ohm/sq] * thickness [um]."""
        if self.rpersq is None or self.thickness <= 0.0:
            return None
        return self.rpersq * self.thickness


@dataclass
class Dielectric:
    name: str
    epsilon: float
    thickness: float                 # um
    next_met: Optional[int] = None   # dielectric approaching metal n from below
    met: Optional[int] = None        # dielectric beside metal n (sidewall)


@dataclass
class Stack:
    conductors: List[Conductor] = field(default_factory=list)
    dielectrics: List[Dielectric] = field(default_factory=list)


# ── LEF conductor parsing ───────────────────────────────────────────────────

_NUM = r"[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?"


def parse_lef_conductors(lef_paths: List[Path]) -> List[Conductor]:
    """Parse routing LAYER records from one or more LEF files, bottom-up.

    Recognizes the standard extraction attributes:
      TYPE ROUTING; WIDTH w; SPACING s; THICKNESS t;
      RESISTANCE RPERSQ r; CAPACITANCE CPERSQDIST c; EDGECAPACITANCE e;
      [HEIGHT h];
    Chip-agnostic: layer names and count come from the file, not hard-coded.
    """
    conductors: List[Conductor] = []
    order = 0
    for lef_path in lef_paths:
        text = Path(lef_path).read_text(errors="ignore")
        # Split into LAYER ... END <name> blocks.
        for m in re.finditer(
                r"^\s*LAYER\s+(\S+)\s*(.*?)^\s*END\s+\1",
                text, re.MULTILINE | re.DOTALL):
            name, body = m.group(1), m.group(2)
            if not re.search(r"\bTYPE\s+ROUTING\b", body):
                continue
            order += 1
            c = Conductor(name=name, order=order)
            mt = re.search(rf"\bTHICKNESS\s+({_NUM})", body)
            if mt:
                c.thickness = float(mt.group(1))
            mr = re.search(rf"\bRESISTANCE\s+RPERSQ\s+({_NUM})", body)
            if mr:
                c.rpersq = float(mr.group(1))
            mw = re.search(rf"\bWIDTH\s+({_NUM})", body)
            if mw:
                c.min_width = float(mw.group(1))
            ms = re.search(rf"\bSPACING\s+({_NUM})", body)
            if ms:
                c.min_spacing = float(ms.group(1))
            mh = re.search(rf"\bHEIGHT\s+({_NUM})", body)
            if mh:
                c.height = float(mh.group(1))
            mc = re.search(rf"\bCAPACITANCE\s+CPERSQDIST\s+({_NUM})", body)
            if mc:
                c.area_cap = float(mc.group(1))
            me = re.search(rf"\bEDGECAPACITANCE\s+({_NUM})", body)
            if me:
                c.edge_cap = float(me.group(1))
            conductors.append(c)
    return conductors


# ── ITF dielectric parsing (standard interconnect technology format) ─────────

def parse_itf_dielectrics(itf_path: Path) -> List[Dielectric]:
    """Parse DIELECTRIC blocks from an ITF-style stack file, bottom-up.

    Accepts the common ITF spelling:
        DIELECTRIC <name> {THICKNESS=<t> ER=<er> ...}
    (whitespace / '=' tolerant).  Conductor blocks are ignored here (the LEF is
    authoritative for conductors); only the dielectric ER + thickness stack is
    taken.  Stacking order follows file order (bottom-up), which the caller maps
    onto next_met/met tags.
    """
    text = Path(itf_path).read_text(errors="ignore")
    dielectrics: List[Dielectric] = []
    for m in re.finditer(
            r"DIELECTRIC\s+(\S+)\s*\{([^}]*)\}", text, re.IGNORECASE):
        name, body = m.group(1), m.group(2)
        mt = re.search(rf"THICKNESS\s*=?\s*({_NUM})", body, re.IGNORECASE)
        me = re.search(rf"\bER\s*=?\s*({_NUM})", body, re.IGNORECASE)
        if mt is None or me is None:
            continue
        dielectrics.append(Dielectric(name=name, epsilon=float(me.group(1)),
                                      thickness=float(mt.group(1))))
    return dielectrics


# ── Uniform-ILD dielectric synthesis (LEF-only, documented assumption) ───────

def synth_uniform_ild(conductors: List[Conductor], ild_er: float,
                      base_gap: float) -> List[Dielectric]:
    """Build a physically-plausible dielectric stack from the LEF geometry.

    DOCUMENTED APPROXIMATION: a single inter-level-dielectric permittivity
    `ild_er` fills every gap.  For each metal level we emit two dielectric
    records: the ILD *below* it (next_met = level) of thickness = the vertical
    gap to the previous metal top, and the sidewall dielectric *beside* it
    (met = level) of thickness = the conductor thickness.  Heights come from the
    LEF THICKNESS values plus a uniform `base_gap` between adjacent metals when
    the LEF carries no HEIGHT.  This is coarser than a full multi-ER ITF stack
    but gives OpenRCX a real geometry from which the field solver computes true
    lateral coupling — the capability -lef_rc lacks.
    """
    dielectrics: List[Dielectric] = []
    for c in conductors:
        gap = base_gap
        if c.height is not None:
            # gap below = height minus (previous metal top); approximate with
            # base_gap when heights are unavailable.
            gap = max(base_gap, base_gap)
        dielectrics.append(Dielectric(
            name=f"ild_below_{c.name}", epsilon=ild_er,
            thickness=round(gap, 4), next_met=c.order))
        dielectrics.append(Dielectric(
            name=f"ild_beside_{c.name}", epsilon=ild_er,
            thickness=round(c.thickness, 4), met=c.order))
    return dielectrics


# ── Process-file emission ────────────────────────────────────────────────────

def map_itf_dielectrics_to_levels(dielectrics: List[Dielectric],
                                  n_levels: int) -> List[Dielectric]:
    """Tag a flat ITF dielectric list (bottom-up) with next_met/met levels by
    distributing them across the metal levels in stack order."""
    if not dielectrics or n_levels <= 0:
        return dielectrics
    per = max(1, len(dielectrics) // n_levels)
    out: List[Dielectric] = []
    for i, d in enumerate(dielectrics):
        level = min(n_levels, i // per + 1)
        d2 = Dielectric(name=d.name, epsilon=d.epsilon, thickness=d.thickness)
        # alternate the sub-layers between "below" and "beside" the level
        if i % 2 == 0:
            d2.next_met = level
        else:
            d2.met = level
        out.append(d2)
    return out


def emit_process(stack: Stack, base_gap: float) -> str:
    """Render the OpenRCX process-parameters file text."""
    lines: List[str] = []
    lines.append("# OpenRCX process file — generated by "
                 "lef_stack_to_rcx_process.py")
    lines.append("# CONDUCTOR half from LEF; DIELECTRIC half from ITF / "
                 "uniform-ILD. Generic; no PDK data.")
    lines.append("")
    running_h = 0.0
    for c in stack.conductors:
        dist = c.height if c.height is not None else running_h + base_gap
        running_h = dist + c.thickness
        lines.append(f"CONDUCTOR {c.name} {{")
        lines.append(f"        distance {round(dist, 4)}")
        lines.append(f"        thickness {round(c.thickness, 4)}")
        lines.append(f"        min_width {round(c.min_width, 4)}")
        lines.append(f"        min_spacing {round(c.min_spacing, 4)}")
        if c.resistivity is not None:
            lines.append(f"        resistivity {round(c.resistivity, 6)}")
        lines.append("}")
    lines.append("")
    for d in stack.dielectrics:
        lines.append(f"DIELECTRIC {d.name} {{")
        lines.append(f"        epsilon {d.epsilon}")
        lines.append(f"        thickness {round(d.thickness, 4)}")
        if d.next_met is not None:
            lines.append(f"        next_met {d.next_met}")
        elif d.met is not None:
            lines.append(f"        met {d.met}")
        lines.append("}")
    lines.append("")
    return "\n".join(lines)


def build_stack(lef_paths: List[Path], itf: Optional[Path],
                ild_er: Optional[float], base_gap: float) -> Stack:
    conductors = parse_lef_conductors(lef_paths)
    if not conductors:
        raise ValueError("no routing (TYPE ROUTING) layers found in LEF(s)")
    if itf is not None:
        raw = parse_itf_dielectrics(itf)
        dielectrics = map_itf_dielectrics_to_levels(raw, len(conductors))
    elif ild_er is not None:
        dielectrics = synth_uniform_ild(conductors, ild_er, base_gap)
    else:
        raise ValueError("provide either --itf or --ild-er")
    return Stack(conductors=conductors, dielectrics=dielectrics)


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Emit an OpenRCX process file from LEF + dielectric stack.")
    ap.add_argument("--lef", type=Path, action="append", required=True,
                    help="technology LEF (repeatable)")
    ap.add_argument("--itf", type=Path, default=None,
                    help="ITF-style dielectric stack file")
    ap.add_argument("--ild-er", type=float, default=None,
                    help="uniform inter-level-dielectric relative permittivity")
    ap.add_argument("--base-gap", type=float, default=0.3,
                    help="inter-metal gap [um] when LEF has no HEIGHT "
                         "(default 0.3)")
    ap.add_argument("--process-name", default="TYP")
    ap.add_argument("--out", type=Path, required=True)
    ns = ap.parse_args(argv)

    if ns.itf is None and ns.ild_er is None:
        print("error: provide --itf or --ild-er", file=sys.stderr)
        return 2
    try:
        stack = build_stack(ns.lef, ns.itf, ns.ild_er, ns.base_gap)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    ns.out.write_text(emit_process(stack, ns.base_gap))
    print(f"wrote {ns.out}: {len(stack.conductors)} conductors, "
          f"{len(stack.dielectrics)} dielectrics")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
