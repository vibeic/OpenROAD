#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025, The OpenROAD Authors
#
# rcx_calibrate.sh — end-to-end OpenRCX field-solver rules calibration from a
# process file, then parasitic extraction of a routed design.
#
# This is a PARAMETERIZED (de-hardcoded) driver for the OpenRCX v2 FasterCap
# calibration flow whose reference scripts under rcx/test/rcx_v2/FasterCapModel
# carry absolute paths from the original author's machine.  Everything here is
# passed in; no path is baked.
#
#   process file  --(gen_solver_patterns)-->  field-solver patterns
#                 --(UniversalFormat2FasterCap + FasterCap)-->  per-pattern caps
#                 --(fasterCapParse)-->  per-pattern-type .caps tables
#                 --(init/read/write _rcx_model)-->  extraction rules model
#                 --(extract_parasitics -ext_model_file)-->  coupling SPEF
#
# Usage:
#   rcx_calibrate.sh -p PROCESS -m MET_CNT -o OUTDIR -s SCRIPTS_DIR \
#       [-w WIRE_CNT] [-j FC_JOBS] [-a FC_ERR] \
#       [-d DESIGN_DEF -l "LEF1 LEF2 ..." -n NET -e OUT_SPEF]
# where SCRIPTS_DIR holds UniversalFormat2FasterCap_923.py + fasterCapParse.py.
set -u -o noglob

WIRE_CNT=3; FC_JOBS=8; FC_ERR=0.01
PROCESS=""; MET_CNT=""; OUTDIR=""; SCRIPTS=""; DESIGN_DEF=""; LEFS=""
NET=""; OUT_SPEF=""
OPENROAD=${OPENROAD:-openroad}; FASTERCAP=${FASTERCAP:-FasterCap}

while getopts "p:m:o:s:w:j:a:d:l:n:e:" opt; do
  case $opt in
    p) PROCESS=$OPTARG ;; m) MET_CNT=$OPTARG ;; o) OUTDIR=$OPTARG ;;
    s) SCRIPTS=$OPTARG ;; w) WIRE_CNT=$OPTARG ;; j) FC_JOBS=$OPTARG ;;
    a) FC_ERR=$OPTARG ;; d) DESIGN_DEF=$OPTARG ;; l) LEFS=$OPTARG ;;
    n) NET=$OPTARG ;; e) OUT_SPEF=$OPTARG ;;
    *) echo "bad opt"; exit 2 ;;
  esac
done
[ -z "$PROCESS" ] || [ -z "$MET_CNT" ] || [ -z "$OUTDIR" ] || [ -z "$SCRIPTS" ] \
  && { echo "need -p PROCESS -m MET_CNT -o OUTDIR -s SCRIPTS_DIR"; exit 2; }

U2FC="$SCRIPTS/UniversalFormat2FasterCap_923.py"
# Prefer the dependency-free vendored parser next to this script; fall back to
# the reference one under the rcx_v2 test tree (which pulls unused heavy deps).
_SELFDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FCPARSE=${FCPARSE_PY:-"$_SELFDIR/fastercap_parse.py"}
[ -f "$FCPARSE" ] || FCPARSE="$SCRIPTS/fasterCapParse.py"
mkdir -p "$OUTDIR"; OUTDIR=$(cd "$OUTDIR" && pwd); PROCESS=$(readlink -f "$PROCESS")

echo "=== [1/5] gen_solver_patterns (wire_cnt=$WIRE_CNT) ==="
PATDIR="$OUTDIR/patgen"; rm -rf "$PATDIR"; mkdir -p "$PATDIR"; cd "$PATDIR"
printf 'gen_solver_patterns -process_file %s -process_name TYP -wire_cnt %s -version 2\nexit\n' \
  "$PROCESS" "$WIRE_CNT" | "$OPENROAD" -no_init > gen.log 2>&1
NPAT=$(find TYP -name wires 2>/dev/null | wc -l)
echo "patterns generated: $NPAT ; resistance table: $(ls resistance.TYP 2>/dev/null || echo MISSING)"
[ "$NPAT" -eq 0 ] && { echo "FAIL: no patterns"; tail -5 gen.log; exit 1; }

echo "=== [2/5] FasterCap over all patterns (jobs=$FC_JOBS, err=$FC_ERR) ==="
# U2FC needs a RELATIVE output folder (an absolute one makes it mkdir at /),
# and it writes the shared geometry into <cwd>/Wires.  Convert with cwd=PATDIR
# and out folder "fc"; the geometry lands at PATDIR/Wires.
FCROOT="$PATDIR/fc"; rm -rf "$FCROOT"; mkdir -p "$FCROOT"
run_one_fc() {   # $1 = a converted wires.lst
  local lst=$1 dir; dir=$(dirname "$lst")
  # the .lst references Wires/ and Dielectrics/ six levels up from its own dir;
  # symlink the real geometry dirs (written by U2FC at PATDIR) there so
  # FasterCap resolves them regardless of nesting depth
  local up6; up6=$(cd "$dir/../../../../../.." 2>/dev/null && pwd)
  if [ -n "$up6" ]; then
    [ -e "$up6/Wires" ] || ln -sfn "$PATDIR/Wires" "$up6/Wires" 2>/dev/null
    [ -e "$up6/Dielectrics" ] || \
      ln -sfn "$PATDIR/Dielectrics" "$up6/Dielectrics" 2>/dev/null
  fi
  ( cd "$dir" && "$FASTERCAP" -b "$(basename "$lst")" -g -a"$FC_ERR" \
      > wires.log 2>&1 )
}
export -f run_one_fc; export FASTERCAP FC_ERR PATDIR
for ptype in $(ls "$PATDIR/TYP"); do
  ( cd "$PATDIR" && python3 "$U2FC" "$PROCESS" "TYP/$ptype" "fc" standard \
      >> "$OUTDIR/u2fc.log" 2>&1 ) || true
done
mapfile -t LSTS < <(find "$FCROOT" -name '*.lst' | sort)
echo "FasterCap runs to do: ${#LSTS[@]}"
printf '%s\n' "${LSTS[@]}" | xargs -P "$FC_JOBS" -I{} bash -c 'run_one_fc "$@"' _ {}
NLOG=$(find "$FCROOT" -name wires.log | wc -l)
echo "wires.log produced: $NLOG"

echo "=== [3/5] parse FasterCap output per pattern-type ==="
PARSED="$OUTDIR/parsed"; rm -rf "$PARSED"; mkdir -p "$PARSED"; cd "$PARSED"
CAPS_ARGS=""
for ptype in $(ls "$FCROOT" 2>/dev/null | grep -vi wires); do
  [ -d "$FCROOT/$ptype" ] || continue
  find "$FCROOT/$ptype" -name wires.log | sort > "$ptype.list"
  [ -s "$ptype.list" ] || continue
  # wire arg: coupling patterns (OverUnder/UnderDiag) use 2, else 1
  wa=1; case "$ptype" in *Under*|*Diag*) wa=2 ;; esac
  python3 "$FCPARSE" -in_list_file "$ptype.list" -wire "$wa" \
    -out_file "$ptype.caps" > "$ptype.parse.log" 2>&1 || \
    echo "  parse warn: $ptype"
  [ -s "$ptype.caps" ] && CAPS_ARGS="$CAPS_ARGS $PARSED/$ptype.caps"
done
echo "caps tables: $CAPS_ARGS"

echo "=== [4/5] build rcx model (init/read/write) ==="
MODEL="$OUTDIR/calibrated.rcx.model"
{ echo "init_rcx_model -corner_names \"TYP\" -met_cnt $MET_CNT"
  for c in $CAPS_ARGS; do echo "read_rcx_tables -corner TYP -file $c -wire 3"; done
  echo "read_rcx_tables -corner TYP -file $PATDIR/resistance.TYP -wire 3"
  echo "write_rcx_model -file $MODEL"
  echo "exit"; } > "$OUTDIR/mkmodel.tcl"
( cd "$OUTDIR" && "$OPENROAD" -no_init < mkmodel.tcl > mkmodel.log 2>&1 )
echo "model: $(ls -la $MODEL 2>/dev/null || echo MISSING)"
[ -f "$MODEL" ] || { echo "FAIL: model not written"; tail -15 "$OUTDIR/mkmodel.log"; exit 1; }

if [ -n "$DESIGN_DEF" ] && [ -n "$OUT_SPEF" ]; then
  echo "=== [5/5] extract_parasitics with calibrated model ==="
  { for l in $LEFS; do echo "read_lef $l"; done
    echo "read_def $DESIGN_DEF"
    echo "define_process_corner -ext_model_index 0 X"
    echo "set_extraction_rules_file $MODEL"
    echo "extract_parasitics -coupling_threshold 0.1 -version 2.0 -max_res 0"
    echo "write_spef $OUT_SPEF"; echo "exit"; } > "$OUTDIR/extract.tcl"
  ( cd "$OUTDIR" && "$OPENROAD" -no_init < extract.tcl > extract.log 2>&1 )
  echo "SPEF: $(ls -la $OUT_SPEF 2>/dev/null || echo MISSING)"
  grep -E "RCX-050|nets finished|Using LEF" "$OUTDIR/extract.log" | tail -3
fi
echo "=== DONE ==="
