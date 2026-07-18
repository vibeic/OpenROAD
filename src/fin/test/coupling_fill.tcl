# Coupling-aware fill: fill must be held an extra -critical_halo away from the
# nets named by -critical_nets, on top of the rule file's space_to_non_fill.
#
# Fixture: 200um x 100um die.  Net "crit" is a met1 shape spanning
# x = 149.0 .. 151.0 um for the full height of the die.  fill.json gives met1
# space_to_non_fill = 3um, so plain fill comes exactly 3000 DBU from the net
# (see coupling_fill_off).  With -critical_halo 5 nothing may sit closer than
# 5000 DBU, i.e. the keep-out is exactly x = 144.0 .. 156.0 um.
#
# That band is also checked by density, which is hand-computable: over a
# 12um x 12um window covering x = 144..156 the only metal is the net itself,
#   metal  = 2um  x 12um =  24 um^2
#   window = 12um x 12um = 144 um^2
#   density = 24 / 144 = 0.166666...
source helpers.tcl
source coupling_helpers.tcl

read_lef sky130hd/sky130hd.tlef
read_def coupling_fill.def

density_fill -rules fill_met1.json -critical_nets {crit} -critical_halo 5
report_crit_fill "relief"

# Direct geometric gate: no fill closer than the 5000 DBU halo.
lassign [crit_fill_stats] count gap
if { $count == 0 } {
  puts "FAIL relief produced no met1 fill at all"
} elseif { $gap >= 5000 } {
  puts "PASS min gap $gap >= 5000"
} else {
  puts "FAIL min gap $gap < 5000"
}

# Density gate over the keep-out band: the net and nothing else.
puts "--- halo band: expect 0 violations ---"
puts "violations [check_metal_density -layer met1 -window 12 -step 12 \
  -area {144 0 156 100} -min_density 0.1666 -max_density 0.1667]"
check_metal_density -layer met1 -window 12 -step 12 -area {144 0 156 100}

# Fill away from the critical net is untouched.
puts "--- outside the halo: fill is still present ---"
check_metal_density -layer met1 -window 12 -step 12 -area {100 0 112 100}
