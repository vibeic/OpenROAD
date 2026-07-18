source "helpers.tcl"
# -no_abort: the caller regains control AND the violation count is
# machine-visible. check2.def has a real overlap; the command must NOT throw,
# must return a non-zero count the caller can act on, and the flow must
# continue past it. A warning nobody can consult would be a silent pass.
read_lef Nangate45/Nangate45.lef
read_def check2.def
set violations [check_placement -verbose -no_abort]
puts "RETURNED_VIOLATIONS: $violations"
if { $violations > 0 } {
  puts "CALLER_SAW_NON_PASS"
} else {
  puts "CALLER_SAW_PASS"
}
puts "FLOW_CONTINUED"
