source "helpers.tcl"
# The negotiation legalizer must have the SAME contract as the diamond path
# (-use_diamond_legalizer / DPL-0036): if legalization leaves real violations
# behind, that is a failure (DPL-0701), not an informational warning.
# Otherwise the negotiation path could return success with genuine overlaps in
# the DEF.
# Upstream made negotiation the DEFAULT detailed-placement algorithm and
# removed the -use_negotiation flag, so a bare detailed_placement now
# exercises the path this test guards.
# fragmented_row03.def cannot be legalized (rows too far from the instance).
read_lef Nangate45/Nangate45.lef
read_def fragmented_row03.def
if { [catch { detailed_placement } msg] } {
  puts "NEGOTIATION_BLOCKED: $msg"
} else {
  puts "NEGOTIATION_REPORTED_SUCCESS"
}
