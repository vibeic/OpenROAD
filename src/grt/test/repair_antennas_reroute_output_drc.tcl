# repair_antennas -reroute -output_drc (vibeic fork): the SCOPED route's own
# residual must be nameable.
#
# TritonRoute::setParams() re-reads DRC_RPT_FILE from every detailed_route
# invocation's own -output_drc and resets it to "" when the option is absent.
# The detailed_route that `-reroute` runs internally was never given one, so
# whatever that route left behind carried no layer, no net and no location --
# anywhere. Measured on a production antenna repair: the scoped route finished
# with 599 whole-design violations (547 Short, 45 Metal Spacing, 7 NS Metal)
# and the only -output_drc in the whole run had been handed to the BASE route,
# which had nothing to report. The residual was a bare integer for the rest of
# the flow.
#
# NEGATIVE CONTROL for this test: delete `-output_drc $rpt` from the
# repair_antennas call below. The file is then never created and
# OUTPUT_DRC_WRITTEN goes to 0 -- the same one-argument edit that is the fix.
source "helpers.tcl"
read_liberty "sky130hs/sky130hs_tt.lib"
read_lef "sky130hs/sky130hs.tlef"
read_lef "sky130hs/sky130hs_std_cell.lef"
read_def "gcd_sky130.def"

set_thread_count 1
set_placement_padding -global -left 2 -right 2
set_global_routing_layer_adjustment met2-met5 0.15
set_routing_layers -signal met1-met5
global_route

detailed_route -verbose 0

set rpt [make_result_file repair_antennas_reroute_output_drc.rpt]
file delete -force $rpt

set before [check_antennas]
puts "antenna_violations_present [expr { $before > 0 }]"

set after [repair_antennas sky130_fd_sc_hs__diode_2 -reroute -iterations 10 \
  -output_drc $rpt]
puts "antenna_violations_after $after"

# The claim is that the scoped route's verification now REACHES a report file.
# Existence is the load-bearing half: a run that leaves no file cannot name
# anything, whatever its violation count turns out to be.
puts "OUTPUT_DRC_WRITTEN: [expr { [file exists $rpt] ? 1 : 0 }]"
if { [file exists $rpt] } {
  set fh [open $rpt r]
  set body [read $fh]
  close $fh
  puts "OUTPUT_DRC_VIOLATIONS: [regexp -all {violation type:} $body]"
}
