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
# The shared check covers EVERY routing layer, so the band is judged on all 6
# (li1, met1..met5) x 9 windows = 54.  Only met1 carries metal, so met1's 9
# windows sit in the band at 0.166667 and the other 45 are empty and flag
# under-density.  45 is therefore the "relief worked" signal: met1 clean.
puts "--- halo band: expect 45 = 54 - met1's 9 clean windows ---"
puts "band_viol [check_metal_density -window 12 -step 12 \
  -area {144 0 156 100} -min_density 0.1666 -max_density 0.1667]"
check_metal_density -window 12 -step 12 -area {144 0 156 100} \
  -min_density 0.1666 -max_density 0.1667 \
  -report_file [make_result_file coupling_fill_band.rpt]

# Fill away from the critical net is untouched.
puts "--- outside the halo: fill is still present ---"
check_metal_density -window 12 -step 12 -area {100 0 112 100} \
  -min_density 0.10 -max_density 0.90
