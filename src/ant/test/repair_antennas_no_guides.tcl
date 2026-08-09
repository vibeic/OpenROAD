# repair_antennas on a design restored from a DETAIL-ROUTED DEF.
#
# No global_route ran in this session, so no net carries guides.  grt's
# RepairAntennas::checkAntennaViolations calls makeNetWiresFromGuides()
# unconditionally, and WireBuilder::dbNetIsLocal() read
# *(net->getGuides().begin()) without first asking whether the set was empty
# -- a null dereference (Signal 11) on the first multi-terminal signal net.
# Note dbNetIsLocal() is evaluated BEFORE the is_detailed_routed term of the
# same condition, so a fully detail-routed net reaches it too.
#
# ant's own check_antennas never hit this: it gates the same call on
# use_grt_routes (guides present AND no detailed routes).  repair_antennas has
# no such gate, and reading a routed DEF back for a post-route repair is an
# ordinary resume/checkpoint flow -- so the crash is reachable from valid
# input, not from a malformed design.
#
# guides_present pins the precondition this test exists for: if the fixture
# ever starts carrying guides the test would still pass while covering
# nothing, so the count is asserted rather than assumed.
source "helpers.tcl"
read_lef merged_spacing.lef
read_def sw130_random.def

set_thread_count 1

set guide_count 0
foreach net [[ord::get_db_block] getNets] {
  incr guide_count [llength [$net getGuides]]
}
puts "guides_present $guide_count"

set diodes [repair_antennas sky130_fd_sc_ms__diode_2 -iterations 1]
puts "repair_antennas_returned $diodes"

if { $guide_count == 0 } {
  puts "repair_antennas_no_guides: PASS"
} else {
  puts "repair_antennas_no_guides: FAIL (fixture is no longer guide-less)"
}
