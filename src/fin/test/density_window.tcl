# Metal density windows: measurement must reproduce a hand-computed value
# exactly, the min/max comparison must be sharp at the boundary, and
# density-driven fill must respect the max_density cap.
#
# Fixture: 200um x 100um die, one met1 special-wire rect covering
# (0, 25) - (50, 75) um  =>  50um x 50um = 2500 um^2 of metal.
# With -window 100 -step 100 there are two 100um x 100um windows:
#   window (0,0)-(100,100):   2500 / 10000 = 0.2500  <- hand computed
#   window (100,0)-(200,100):    0 / 10000 = 0.0000
source helpers.tcl

read_lef sky130hd/sky130hd.tlef
read_def density_window.def

# The FIN-0014 summary line below must report the range 0.2500 to 0.0000.
puts "--- measure ---"
check_metal_density -layer met1 -window 100 -step 100

# min_density exactly at the measured value: window 0 is NOT a violation, so
# only the empty window counts.
puts "--- min 0.2500 (boundary) -> expect 1 ---"
puts "violations [check_metal_density -layer met1 -window 100 -step 100 \
  -min_density 0.2500]"

# One step outside the boundary: window 0 MUST now fail too.
puts "--- min 0.2501 (just outside) -> expect 2 ---"
puts "violations [check_metal_density -layer met1 -window 100 -step 100 \
  -min_density 0.2501]"

# Same sharpness on the upper bound.
puts "--- max 0.2500 (boundary) -> expect 0 ---"
puts "violations [check_metal_density -layer met1 -window 100 -step 100 \
  -max_density 0.2500]"
puts "--- max 0.2499 (just outside) -> expect 1 ---"
puts "violations [check_metal_density -layer met1 -window 100 -step 100 \
  -max_density 0.2499]"

# Density-driven fill: both windows are below 0.30 so both are filled, and no
# window may end up above 0.40.
puts "--- density-driven fill, cap 0.40 ---"
density_fill -rules fill_met1.json -min_density 0.30 -max_density 0.40 \
  -density_window 100 -density_step 100

puts "--- post-fill: expect 0 windows above the 0.40 cap ---"
puts "violations [check_metal_density -layer met1 -window 100 -step 100 \
  -max_density 0.40]"
