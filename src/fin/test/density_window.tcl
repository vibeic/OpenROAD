# Density-driven fill (FL1 actuator half), gated through the SHARED measurement
# core -- the same check_metal_density the signoff half uses.
#
# Fixture: 200um x 100um die, one met1 special-wire rect covering
# (0, 25) - (50, 75) um  =>  50um x 50um = 2500 um^2 of metal.
# With -window 100 -step 100 the die holds two 100um x 100um windows:
#   window (0,0)-(100,100):   2500 / 10000 = 0.2500  <- hand computed
#   window (100,0)-(200,100):    0 / 10000 = 0.0000
#
# met1 is the only layer carrying metal; the other routing layers measure
# 0.0000, so the violation counts below are per-layer multiples.
source helpers.tcl

read_lef sky130hd/sky130hd.tlef
read_def density_window.def

# Hand-computed density, reproduced by the measurement core.
puts "--- measure: expect met1 windows at 0.2500 and 0.0000 ---"
check_metal_density -window 100 -step 100 -min_density 0.10 -max_density 0.90 \
  -report_file [make_result_file density_window_measure.rpt]

# Boundary sharpness, both directions.  0.2500 is inclusive on each bound, so
# the count must step by exactly one met1 window as the bound crosses it.
puts "--- min 0.2500 (inclusive, met1 full window must NOT violate) ---"
puts "min_at   [check_metal_density -window 100 -step 100 -min_density 0.2500]"
puts "--- min 0.2501 (just outside, met1 full window MUST violate) ---"
puts "min_over [check_metal_density -window 100 -step 100 -min_density 0.2501]"
puts "--- max 0.2500 (inclusive, met1 full window must NOT violate) ---"
puts "max_at   [check_metal_density -window 100 -step 100 -max_density 0.2500]"
puts "--- max 0.2499 (just outside, met1 full window MUST violate) ---"
puts "max_over [check_metal_density -window 100 -step 100 -max_density 0.2499]"

# Density-driven fill: both met1 windows are below 0.30 so both are topped up,
# and the per-window budget must stop before any window passes 0.40.
puts "--- density-driven fill, cap 0.40 ---"
density_fill -rules fill_met1.json -min_density 0.30 -max_density 0.40 \
  -density_window 100 -density_step 100

puts "--- post-fill: expect 0 windows above the 0.40 cap ---"
puts "over_cap [check_metal_density -window 100 -step 100 -max_density 0.40]"
check_metal_density -window 100 -step 100 -min_density 0.10 -max_density 0.40 \
  -report_file [make_result_file density_window_postfill.rpt]
