# repair_antennas -reroute (vibeic fork): the detailed-route-aware
# repair -> reroute -> repair loop, driven internally by ONE command.
#
# A real detailed-routed design (gcd, standard sky130hs tech) still has
# process-antenna violations after routing. Stock `repair_antennas
# -iterations 1` inserts diodes but cannot itself re-run detailed_route, so
# it refreshes only the global route guides and leaves the diode-dirty nets'
# detailed wire removed -- a false-clean (check_antennas then reports 0 only
# because those nets have no wire left to measure). `-reroute` instead
# re-realizes the dirty nets with an incremental detailed_route between
# repair passes and converges the antenna check to a genuine 0 with the
# routing intact.
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

# A real detailed route of gcd on sky130hs has antenna violations.
set before [check_antennas]
puts "antenna_violations_present [expr { $before > 0 }]"

# One command drives the whole repair->incremental-reroute->repair loop.
set after [repair_antennas sky130_fd_sc_hs__diode_2 -reroute -iterations 10]
puts "antenna_violations_after $after"

if { $before > 0 && $after == 0 } {
  puts "repair_antennas_reroute: PASS"
} else {
  puts "repair_antennas_reroute: FAIL (before=$before after=$after)"
}
