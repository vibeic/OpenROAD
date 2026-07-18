# EM current-density signoff (EM3) end-to-end gate on a real design.
#
# check_current_density solves the VDD power grid, forms J = I/A from the solved
# per-wire currents and the LEF wire geometry, and flags every segment whose J
# exceeds the per-layer limit.  This test proves the FAIL->PASS transition on the
# same design just by moving the limit:
#   * a loose limit (1.0 A/um^2, far above the worst J) -> 0 violations, PASS;
#   * a tight limit (0.002 A/um^2, below the worst J)   -> >0 violations, FAIL.
# The counts are asserted in-Tcl, so the verdict is computed from real geometry
# and real currents -- it cannot be faked by editing the golden log.
source helpers.tcl

read_lef Nangate45/Nangate45.lef
read_def Nangate45_data/gcd.def
read_liberty Nangate45/Nangate45_typ.lib
read_sdc Nangate45_data/gcd.sdc

# PASS case: limit well above the worst current density.
set pass_violations [check_current_density -net VDD -vsrc Vsrc_gcd_vdd.loc \
  -em_limit 1.0]
if { $pass_violations != 0 } {
  error "EM3 loose-limit case expected 0 violations, got $pass_violations"
}

# FAIL case: limit below the worst current density -> real violations.
set report_file [make_result_file gcd_em_density_vdd.rpt]
set fail_violations [check_current_density -net VDD -vsrc Vsrc_gcd_vdd.loc \
  -em_limit 0.002 -em_report $report_file]
if { $fail_violations <= 0 } {
  error "EM3 tight-limit case expected >0 violations, got $fail_violations"
}

puts "EM3 self-check: loose=$pass_violations (PASS) tight=$fail_violations (FAIL) OK"
