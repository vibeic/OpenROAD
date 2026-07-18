# Window geometry is caller-supplied on BOTH density_fill and
# check_metal_density, so a caller can drive fill over one window and then
# measure over another. Fill would consider the design done while signoff
# judges different geometry -- a silent disagreement between the two halves.
#
# density_fill records the window/step it used; check_metal_density warns when
# it is invoked over different geometry. This gate proves the warning fires on
# a mismatch and stays quiet on a match.
#
# It also captures WHY the warning earns its keep. The 100/50 case below is not
# merely a bookkeeping difference: the same design that peaks at 0.399998 and
# PASSES its 0.40 cap on the 100/100 grid it was filled over peaks at 0.400200
# and FAILS at 100/50, because a window straddling two separately-budgeted
# regions was never in the budget. A per-window cap can only promise the
# windows it was given. Drive fill at a step no coarser than the step you
# intend to sign off at.
source helpers.tcl

read_lef sky130hd/sky130hd.tlef
read_def density_window.def

# Drive fill over a 100um window.
density_fill -rules fill_met1.json -min_density 0.30 -max_density 0.40 \
  -density_window 100 -density_step 100

# Same geometry: no warning expected.
puts "--- check at the SAME 100/100 geometry: expect NO FIN-0052 ---"
check_metal_density -window 100 -step 100 -max_density 0.40

# Different window: FIN-0052 must fire.
puts "--- check at a DIFFERENT 50um window: expect FIN-0052 ---"
check_metal_density -window 50 -step 50 -max_density 0.40

# Different step at the same window: FIN-0052 must fire too.
puts "--- check at 100um window but 50um step: expect FIN-0052 ---"
check_metal_density -window 100 -step 50 -max_density 0.40
