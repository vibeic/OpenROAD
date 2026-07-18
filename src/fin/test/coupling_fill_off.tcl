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

# Without relief met1's 9 band windows fill to 0.388875, well above the band,
# so they flip from clean to over-density and ALL 54 windows violate.  The
# delta against coupling_fill is exactly 9 -- met1's windows -- which is the
# unfakeable part: 54 vs 45 is the halo doing its job, nothing else moved.
puts "--- halo band with NO relief: expect 54 = every window, met1 included ---"
puts "band_viol [check_metal_density -window 12 -step 12 \
  -area {144 0 156 100} -min_density 0.1666 -max_density 0.1667]"
check_metal_density -window 12 -step 12 -area {144 0 156 100} \
  -min_density 0.1666 -max_density 0.1667 \
  -report_file [make_result_file coupling_fill_off_band.rpt]
