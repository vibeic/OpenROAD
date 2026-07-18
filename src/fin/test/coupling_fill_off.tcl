# Proven-negative companion to coupling_fill: with no -critical_nets the fill
# comes right up to the rule file's space_to_non_fill (3um) and therefore sits
# well inside the 5um halo, so both gates in coupling_fill MUST fail here.
# Without this the halo checks could be passing vacuously.
source helpers.tcl
source coupling_helpers.tcl

read_lef sky130hd/sky130hd.tlef
read_def coupling_fill.def

density_fill -rules fill_met1.json
report_crit_fill "no-relief"

lassign [crit_fill_stats] count gap
if { $gap < 5000 } {
  puts "PASS unrelieved fill intrudes into the halo (gap $gap < 5000)"
} else {
  puts "FAIL unrelieved fill already respects the halo (test is vacuous)"
}

puts "--- halo band with NO relief: expect violations ---"
puts "violations [check_metal_density -layer met1 -window 12 -step 12 \
  -area {144 0 156 100} -min_density 0.1666 -max_density 0.1667]"
check_metal_density -layer met1 -window 12 -step 12 -area {144 0 156 100}
