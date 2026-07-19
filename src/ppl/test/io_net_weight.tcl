# FP2 gate: per-net weight (odb dbNet weight) biases IO pin placement toward
# the weighted net, so a timing/congestion-critical net wins the slot nearest
# its sinks. u_a and u_b are stacked at the SAME x near the bottom edge, so both
# top-level input pins (in_a, in_b) contend for the same best bottom-edge slot;
# the pin whose net is weighted must end up strictly closer to that sink.
#
# Gate (all on ONE binary):
#   - determinism: the equal-weight run reproduces itself exactly.
#   - legality invariant: every pin is placed (getFirstPinLocation found==1).
#   - proven-negative: the whole ppl regression suite is unchanged (weight<=1 ->
#     exact stock HPWL); here the equal-weight assignment is the neutral ref.
#   - directional FLIP (load-bearing, non-echoing): weighting in_a makes in_a
#     the strictly-closer pin; weighting in_b flips it. A do-nothing / constant
#     implementation cannot produce BOTH.
source "helpers.tcl"
read_lef Nangate45/Nangate45.lef
read_liberty Nangate45/Nangate45_typ.lib
read_verilog io_net_weight.v
link_design io_net_weight

initialize_floorplan -die_area {0 0 20 20} -core_area {2 2 18 18} \
  -site FreePDK45_38x28_10R_NP_162NW_34O
make_tracks

set block [ord::get_db_block]
# u_a and u_b share the SAME x (their A pins co-locate at x); u_c is far away so
# `out` does not compete for the bottom-center slots the inputs want.
[$block findInst u_a] setOrigin 12000 5600
[$block findInst u_a] setPlacementStatus FIRM
[$block findInst u_b] setOrigin 12000 9000
[$block findInst u_b] setPlacementStatus FIRM
[$block findInst u_c] setOrigin 2500 15000
[$block findInst u_c] setPlacementStatus FIRM

# X-distance from a placed input BTerm to its single sink (buffer A pin). Every
# bottom-edge slot shares the same y, so the y-part of the pin->sink HPWL is a
# per-net constant independent of the slot chosen; the ONLY axis the assignment
# (and hence the weight) can affect is x. Measuring x isolates that axis and
# keeps the two nets comparable even though their sinks sit at different y.
proc pin_to_sink {bt_name inst_name} {
  set block [ord::get_db_block]
  set bt [$block findBTerm $bt_name]
  lassign [$bt getFirstPinLocation] found x y
  if { !$found } { return -1 }
  set it [[$block findInst $inst_name] findITerm A]
  lassign [$it getAvgXY] ok sx sy
  return [expr { abs($x - $sx) }]
}

proc bt_loc {bt_name} {
  set block [ord::get_db_block]
  set bt [$block findBTerm $bt_name]
  lassign [$bt getFirstPinLocation] found x y
  return "$x $y"
}

proc reset_pins {} {
  # place_pins re-places every non-FIXED BTerm each call (initNetlist skips only
  # isFixed() pins, and PLACED is not fixed), so only the weights need resetting.
  set block [ord::get_db_block]
  foreach net [$block getNets] { $net setWeight 1 }
}

proc run {heavy w} {
  set block [ord::get_db_block]
  reset_pins
  if {$heavy ne ""} { [$block findNet $heavy] setWeight $w }
  place_pins -hor_layers metal3 -ver_layers metal2 \
    -corner_avoidance 0 -min_distance 0.12
}

# --- equal weight (neutral reference) ---
run "" 1
set base_a [pin_to_sink in_a u_a]
set base_b [pin_to_sink in_b u_b]
puts "EQUAL  : in_a=[bt_loc in_a] d_a=$base_a | in_b=[bt_loc in_b] d_b=$base_b"

# determinism: identical inputs -> identical output
run "" 1
if { [pin_to_sink in_a u_a] != $base_a || [pin_to_sink in_b u_b] != $base_b } {
  error "FAIL determinism: equal-weight run not reproducible"
}

# --- weight in_a ---
run in_a 20
set wa_a [pin_to_sink in_a u_a]
set wa_b [pin_to_sink in_b u_b]
puts "WEIGT_A: in_a=[bt_loc in_a] d_a=$wa_a | in_b=[bt_loc in_b] d_b=$wa_b"

# --- weight in_b ---
run in_b 20
set wb_a [pin_to_sink in_a u_a]
set wb_b [pin_to_sink in_b u_b]
puts "WEIGT_B: in_a=[bt_loc in_a] d_a=$wb_a | in_b=[bt_loc in_b] d_b=$wb_b"

# legality: every input pin was actually placed.
foreach v [list $base_a $base_b $wa_a $wa_b $wb_a $wb_b] {
  if { $v < 0 } { error "FAIL legality: an input pin was not placed" }
}

# directional FLIP (load-bearing, non-echoing): the weighted pin is the closer
# one, and the ownership of the best slot flips with which net is weighted.
if { !($wa_a < $wa_b) } {
  error "FAIL: weighting in_a did not make in_a the closer pin ($wa_a !< $wa_b)"
}
if { !($wb_b < $wb_a) } {
  error "FAIL: weighting in_b did not make in_b the closer pin ($wb_b !< $wb_a)"
}
# and each weighted pin is no worse than the neutral reference for itself.
if { !($wa_a <= $base_a && $wb_b <= $base_b) } {
  error "FAIL: weighted pin not improved vs equal-weight reference"
}
puts "pass: weighted net wins the nearer slot; assignment flips with the weight"
