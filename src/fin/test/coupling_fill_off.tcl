# Proven-negative companion to coupling_fill: with no -critical_nets the fill
# comes right up to the rule file's space_to_non_fill (3um) and therefore sits
# inside the 5um halo band, so the same window check MUST report violations.
# Without this the halo check in coupling_fill.tcl could be passing vacuously.
source helpers.tcl

read_lef sky130hd/sky130hd.tlef
read_def coupling_fill.def

density_fill -rules fill_met1.json

puts "--- halo band with NO relief: expect violations (fill intrudes) ---"
puts "violations [check_metal_density -layer met1 -window 12 -step 12 \
  -area {144 0 156 100} -min_density 0.1666 -max_density 0.1667]"
check_metal_density -layer met1 -window 12 -step 12 -area {144 0 156 100}
