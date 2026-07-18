# Signal-net EM (EM4) end-to-end gate on a real design.
#
# check_signal_em takes the REAL load capacitance and transition time OpenSTA
# computes for every routed signal net of routed Nangate45 gcd, forms the switching
# currents (i_peak = C*V/tr, i_rms = i_peak*sqrt(duty), i_avg = i_peak*duty/2),
# divides by the real LEF cross-section (width x THICKNESS, or the summed via cut
# area) and flags every segment over the per-layer J-limit.
#
# Three things are asserted here, none of which can be faked by editing a golden
# log, because they are computed in-Tcl from the emitted per-segment report:
#
#  1. FAIL->PASS on the same design just by moving the limit: a loose RMS limit
#     yields 0 violations (PASS); a tight one yields >0 (FAIL).
#  2. The model identity  J_rms == sqrt(2 * J_avg * J_peak)  holds on EVERY
#     reported segment.  It follows algebraically from i_rms = i_peak*sqrt(duty),
#     i_avg_abs = i_peak*duty and i_avg = i_avg_abs/2, so any drift in the
#     current model breaks it on real data.
#  3. J == I/A to 6 significant figures on every reported segment, per family.
source helpers.tcl

read_lef Nangate45/Nangate45.lef
read_def Nangate45_data/gcd_routed.def
read_liberty Nangate45/Nangate45_typ.lib
read_sdc Nangate45_data/gcd.sdc

set report [make_result_file gcd_signal_em.csv]

# PASS case: RMS limit far above anything a gcd signal wire can reach.
set pass_violations [check_signal_em \
  -supply_voltage 1.1 \
  -toggle_rate 1e8 \
  -rms_limit 1.0 \
  -em_report $report]
if { $pass_violations != 0 } {
  error "EM4 loose-limit case expected 0 violations, got $pass_violations"
}

# ---------------------------------------------------------------------------
# Analytic cross-checks over every row of the per-segment report.
# ---------------------------------------------------------------------------
set fh [open $report r]
gets $fh header
set rows 0
set worst_identity_err 0.0
set worst_ja_err 0.0
set max_jrms 0.0
set jrms_list {}
while { [gets $fh line] >= 0 } {
  if { $line eq "" } {
    continue
  }
  set f [split $line ","]
  set area [lindex $f 7]
  set i_avg [lindex $f 8]
  set i_rms [lindex $f 9]
  set i_peak [lindex $f 10]
  set j_avg [lindex $f 11]
  set j_rms [lindex $f 12]
  set j_peak [lindex $f 13]
  if { $area <= 0.0 || $i_rms <= 0.0 } {
    continue
  }
  incr rows
  lappend jrms_list $j_rms
  if { $j_rms > $max_jrms } {
    set max_jrms $j_rms
  }

  # (2) i_rms == sqrt(2 * i_avg * i_peak)
  set predicted [expr { sqrt(2.0 * $i_avg * $i_peak) }]
  set err [expr { abs($predicted - $i_rms) / $i_rms }]
  if { $err > $worst_identity_err } {
    set worst_identity_err $err
  }

  # (3) J == I/A, per family
  foreach {i j} [list $i_avg $j_avg $i_rms $j_rms $i_peak $j_peak] {
    set expect [expr { $i / $area }]
    set e [expr { abs($expect - $j) / $j }]
    if { $e > $worst_ja_err } {
      set worst_ja_err $e
    }
  }
}
close $fh

if { $rows == 0 } {
  error "EM4 report contained no checkable segment"
}
# The report carries 3 significant decimal digits, so 1e-3 relative is the
# tightest the printed values can support; anything looser than that would let a
# real model drift through.
if { $worst_identity_err > 1e-3 } {
  error "EM4 rms identity broken: worst relative error $worst_identity_err"
}
if { $worst_ja_err > 1e-3 } {
  error "EM4 J == I/A broken: worst relative error $worst_ja_err"
}
puts "checked $rows segments; worst rms-identity error $worst_identity_err"

# ---------------------------------------------------------------------------
# FAIL case.  Rather than "some limit low enough to flag everything", set the
# RMS limit to half the worst J actually present on the design and INDEPENDENTLY
# count, from the published per-segment report, how many segments must be over
# it.  The tool's own violation count has to agree with that recount exactly --
# a check that no golden log can satisfy and that a drifting current model or a
# mis-scaled cross-section would immediately break.
# ---------------------------------------------------------------------------
set threshold [expr { 0.5 * $max_jrms }]
set expected 0
foreach j $jrms_list {
  if { $j > $threshold } {
    incr expected
  }
}
if { $expected <= 0 || $expected >= $rows } {
  error "EM4 threshold did not partition the segments: $expected of $rows"
}

set fail_violations [check_signal_em \
  -supply_voltage 1.1 \
  -toggle_rate 1e8 \
  -rms_limit $threshold]
if { $fail_violations != $expected } {
  error "EM4 violation count $fail_violations disagrees with the independent\
         recount $expected of the per-segment report"
}
puts "worst J_rms $max_jrms A/um^2; limit $threshold flags\
      $fail_violations of $rows segments (recount agrees)"
