# The routed-state snapshot that `repair_antennas -reroute` uses to end on its
# BEST state, tested on its own.
#
# The loop is only allowed to say "I put the design back" if the design really
# is back. A count is not evidence of that: two states can both report 9
# violating nets and be different designs. So this checks three things after a
# restore -- the violating-net SET, and the whole DEF, byte for byte, against
# a DEF written before the pass -- and it first proves that the pass it undid
# actually changed something, so a restore that did nothing cannot pass.
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

set before_nets [grt::antenna_violating_nets]
puts "before_violating_nets [llength $before_nets]"
set before_def [make_result_file repair_antennas_best_state_before.def]
write_def $before_def

grt::routed_state_take

# One repair pass plus the incremental reroute that realizes it. This creates
# diode instances, rewrites net wires and replaces guides -- everything the
# snapshot claims to be able to undo.
repair_antennas sky130_fd_sc_hs__diode_2 -iterations 1
detailed_route -verbose 0

set mid_def [make_result_file repair_antennas_best_state_mid.def]
write_def $mid_def
puts "pass_changed_the_design [diff_files $before_def $mid_def]"

if { ![grt::routed_state_restore] } {
  puts "restore_refused 1"
}
grt::routed_state_discard

set after_nets [grt::antenna_violating_nets]
set after_def [make_result_file repair_antennas_best_state_after.def]
write_def $after_def

puts "after_violating_nets [llength $after_nets]"
puts "membership_restored [expr { $after_nets eq $before_nets }]"
puts "def_restored_byte_for_byte [expr { [diff_files $before_def $after_def] == 0 }]"
