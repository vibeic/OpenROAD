# LEF + dielectric stack -> OpenRCX process file

`lef_stack_to_rcx_process.py` assembles the OpenRCX *process-parameters file*
(CONDUCTOR / DIELECTRIC blocks) that `gen_solver_patterns` + FasterCap +
`gen_rcx_model` consume to build an extraction-rules file.  It lets OpenRCX do
field-solver-calibrated (coupling-aware) RC extraction on a PDK that ships no
xRC/ITF/QRC extraction deck.

Inputs (standard, non-proprietary):
- `--lef tech.lef` (repeatable): CONDUCTOR half.  Each routing LAYER's
  THICKNESS, RESISTANCE RPERSQ, WIDTH, SPACING are read; resistivity is
  `RPERSQ * thickness` [ohm.um].
- `--itf stack.itf`: DIELECTRIC half from an ITF-style stack (per-dielectric
  `ER` + `THICKNESS`), OR
- `--ild-er E`: a documented uniform inter-level-dielectric permittivity (LEF
  geometry + single ER) — needs no extra deck and still gives the field solver
  a real geometry for lateral coupling.

Example:
```
lef_stack_to_rcx_process.py --lef tech.lef --ild-er 4.2 --out process
openroad -no_init <<'EOF'
gen_solver_patterns -process_file process -process_name TYP -wire_cnt 3 -version 2
EOF
# -> FasterCap on the patterns -> gen_rcx_model -> extract_parasitics
```

Tiers:
- **Tier 0** (no converter): `extract_parasitics -lef_rc` with any base rules
  file gives a first-order SPEF straight from the LEF R/C (no lateral-coupling
  model).
- **Tier 1** (this tool): the converter's process file -> FasterCap-calibrated
  rules -> full-coupling `extract_parasitics`.

Tests (synthetic, no PDK data):
- `test_lef_stack_to_rcx_process.py` — deterministic unit test: asserts
  resistivity = RPERSQ*thickness, geometry passthrough, ITF + uniform-ILD.
- `fastercap_gate.py` — end-to-end analytic gate (needs openroad + FasterCap):
  a synthetic wide parallel-plate stack, converter -> gen_solver_patterns ->
  FasterCap, asserts the field-solved plate cap is within [0.5x, 2x] of the
  closed-form `e0*er*A/d` and scales with er.
