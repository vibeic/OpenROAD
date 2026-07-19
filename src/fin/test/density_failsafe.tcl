# Fail-safe: density_fill must never substitute a bound the caller did not give.
#
# Caught by FX-openroad4 reviewing the merged design: the check half already
# refuses to manufacture a verdict from absent foundry data, but the FILL half
# was defaulting the missing side of the band. Both directions were broken, in
# opposite ways:
#
#   -max_density alone  -> min defaulted to 0.0, nothing is ever "below" 0.0,
#                          so fill silently did NOTHING (measured: 0 fills).
#   -min_density alone  -> max defaulted to 1.0, so the overshoot budget was
#                          effectively removed and fill ran uncapped.
#
# A negative bound now means "not supplied" all the way down, matching the
# density engine's own convention.
#
# Fixture: 200um x 100um die, one met1 rect 50x50um => window (0,0)-(100,100)
# measures 0.2500 and window (100,0)-(200,100) measures 0.0000.
source helpers.tcl

read_lef sky130hd/sky130hd.tlef
read_def density_window.def

# -max_density ALONE: no min was supplied, so nothing is "short" and every
# fillable window stays in play. Fill must actually run, and must stop at 0.40.
puts "--- -max_density 0.40 alone: must FILL, capped at 0.40 ---"
density_fill -rules fill_met1.json -max_density 0.40 -density_window 100
set n [llength [[ord::get_db_block] getFills]]
puts "fills $n"
if { $n > 0 } {
  puts "PASS fill ran without an invented min (was 0 fills before the fix)"
} else {
  puts "FAIL fill did nothing -- the invented min_density 0.0 is back"
}
# Same caveat as density_window: on the fill's own grid this is 0 by
# construction and proves nothing. The falsifiable evidence that this test
# exists for is `fills > 0` above -- that number was 0 before the fix and is
# what the gate actually turns on.
puts "over_cap_selfcheck [check_metal_density -window 100 -step 100 \
  -max_density 0.40]"
check_metal_density -window 100 -step 50 -min_density 0.10 -max_density 0.40
