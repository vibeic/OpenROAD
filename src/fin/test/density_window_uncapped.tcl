# Proven-negative companion to density_window: on the same fixture, plain
# density_fill (no -max_density) drives both windows FAR above the 0.40 cap
# that the capped run holds them under.  Without this the cap in
# density_window.tcl could be passing vacuously.
source helpers.tcl

read_lef sky130hd/sky130hd.tlef
read_def density_window.def

density_fill -rules fill_met1.json

puts "--- uncapped fill: expect BOTH met1 windows above the 0.40 cap ---"
puts "over_cap [check_metal_density -window 100 -step 100 -max_density 0.40]"
check_metal_density -window 100 -step 100 -min_density 0.10 -max_density 0.40 \
  -report_file [make_result_file density_window_uncapped.rpt]
