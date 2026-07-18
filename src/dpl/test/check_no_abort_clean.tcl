source "helpers.tcl"
# Proven-negative for -no_abort: a LEGAL design must report zero violations,
# so the blocking contract cannot be satisfied by always returning non-zero.
read_lef Nangate45/Nangate45.lef
read_lef extra.lef
read_def check9.def
set violations [check_placement -verbose -no_abort]
puts "RETURNED_VIOLATIONS: $violations"
if { $violations > 0 } {
  puts "CALLER_SAW_NON_PASS"
} else {
  puts "CALLER_SAW_PASS"
}
