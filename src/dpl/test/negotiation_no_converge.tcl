source "helpers.tcl"
# detailed_placement -use_negotiation must have the SAME contract as the
# default diamond path: if legalization leaves real violations behind, that
# is a failure (DPL-0701), not an informational warning. Otherwise the
# negotiation path could return success with genuine overlaps in the DEF.
# fragmented_row03.def cannot be legalized (rows too far from the instance).
read_lef Nangate45/Nangate45.lef
read_def fragmented_row03.def
if { [catch { detailed_placement -use_negotiation } msg] } {
  puts "NEGOTIATION_BLOCKED: $msg"
} else {
  puts "NEGOTIATION_REPORTED_SUCCESS"
}
