source "helpers.tcl"
# A negotiation legalizer that leaves real violations behind must SAY SO in a
# way a caller can consult. That is the property this test pins.
#
# It used to pin something stronger and wrong: that `detailed_placement` THROWS
# on non-convergence, matching the diamond path's DPL-0036. The symmetry
# argument looked right and was not, because the two paths are not peers --
# diamond is opt-in behind `-use_diamond_legalizer`, negotiation is what a bare
# `detailed_placement` runs since upstream made it the default. Imposing the
# opt-in path's abort contract on the default one changed the command for every
# caller in the tool, and it cost real coverage (measured 2026-08-10):
#
#   * src/cts/test/array_no_blockages.tcl -- whose whole point is a
#     deliberately congested array -- aborted before its report_clock_skew,
#     so the test stopped testing what it exists for;
#   * dpl.{fragmented_row03,obstruction2,report_failures} went red against
#     upstream-pristine goldens;
#   * and the throw sat ABOVE updateDbInstLocations(), so a caller that caught
#     it -- the documented way to continue -- then wrote a DEF holding
#     PRE-legalization positions. The legalization was computed and discarded.
#
# Nothing we rely on needed the throw. Our own PnR flow catches
# `detailed_placement` and gates on `check_placement`, whose DPL-0033 is fatal
# by default and which returns a count under `-no_abort`; its own comment says
# "a `detailed_placement` that merely does not throw is not evidence the
# placement is legal". So the guarantee lives in check_placement, and this test
# guards the half detailed_placement owns: the failure is reported, with a
# count, and is therefore consultable.
#
# fragmented_row03.def cannot be legalized (rows too far from the instance).
read_lef Nangate45/Nangate45.lef
read_def fragmented_row03.def

detailed_placement

# The violation must reach the caller as a NUMBER it can act on. This is the
# same mechanism check_no_abort.tcl pins and the same one our PnR flow calls,
# so the guarantee is tested where it actually lives.
set violations [check_placement -no_abort]
puts "RETURNED_VIOLATIONS: $violations"
if { $violations > 0 } {
  puts "CALLER_SAW_NON_PASS"
} else {
  puts "NEGOTIATION_HID_ITS_VIOLATIONS"
}
puts "FLOW_CONTINUED"
