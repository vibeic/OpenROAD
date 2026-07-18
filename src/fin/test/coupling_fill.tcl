# Coupling-aware fill: fill must be held an extra -critical_halo away from the
# nets named by -critical_nets, on top of the rule file's space_to_non_fill.
#
# Fixture: 200um x 100um die.  Net "crit" is a met1 shape spanning
# x = 149.0 .. 151.0 um for the full height of the die.  With
# -critical_halo 5 the keep-out is exactly x = 144.0 .. 156.0, so a 12um
# window band centred on the net may contain the net itself and nothing else:
#
#   metal  = 2um  x 12um = 24 um^2      (the net crossing the window)
#   window = 12um x 12um = 144 um^2
#   density = 24 / 144 = 0.166666...    <- hand computed
#
# So every window over -area {144 0 156 100} must measure in
# [0.1666, 0.1667].  Any fill intruding into the halo raises it.
source helpers.tcl

read_lef sky130hd/sky130hd.tlef
read_def coupling_fill.def

density_fill -rules fill_met1.json -critical_nets {crit} -critical_halo 5

puts "--- halo band: expect 0 violations (net only, no fill) ---"
puts "violations [check_metal_density -layer met1 -window 12 -step 12 \
  -area {144 0 156 100} -min_density 0.1666 -max_density 0.1667]"
check_metal_density -layer met1 -window 12 -step 12 -area {144 0 156 100}

# Fill outside the halo is untouched, so the design is still filled.
puts "--- outside the halo: fill is still present ---"
check_metal_density -layer met1 -window 12 -step 12 -area {100 0 112 100}
