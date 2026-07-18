# CT5: common-path pessimism credit computed from the clock tree topology.
#
# The design has a hand-built tree so the answer is known by inspection:
#
#   clk -> root_buf -> n_root -+-> buf_a -> n_a -> ff1/CK, ff2/CK
#                              +-> buf_b -> n_b -> ff3/CK, ff4/CK
#
# ff1 and ff2 share everything up to buf_a's output, so their credit is the
# arrival at buf_a/Z.  ff1 and ff3 only share up to root_buf's output, so
# their credit is the (smaller) arrival at root_buf/Z.
source "helpers.tcl"

read_liberty Nangate45/Nangate45_typ.lib
read_lef Nangate45/Nangate45.lef
read_def crpr_credit.def

create_clock -name core -period 5 clk
set_propagated_clock [all_clocks]

source Nangate45/Nangate45.rc
set_wire_rc -clock -layer metal5
set_wire_rc -signal -layer metal3
estimate_parasitics -placement

report_cts_crpr -verbose

# The oracle is STA's own arrival at the shared driver pin, reached through
# get_property -- a different code path from the CTS-time tree walk.  Both
# are printed in ns; 1e-4 ns is the resolution get_property reports at.
proc check_credit { p1 p2 shared } {
  set mine [expr { [cts_crpr_credit $p1 $p2] * 1e9 }]
  set oracle [get_property [get_pin $shared] arrival_max_rise]
  if { abs($mine - $oracle) < 1e-4 } {
    puts [format "PASS %s/%s credit %.4f ns == arrival at %s %.4f ns" \
      $p1 $p2 $mine $shared $oracle]
  } else {
    puts [format "FAIL %s/%s credit %.4f ns != arrival at %s %.4f ns" \
      $p1 $p2 $mine $shared $oracle]
  }
}

check_credit ff1/CK ff2/CK buf_a/Z
check_credit ff3/CK ff4/CK buf_b/Z
check_credit ff1/CK ff3/CK root_buf/Z
check_credit ff2/CK ff4/CK root_buf/Z

# PROVEN NEGATIVE: a pair that branches at the root must earn strictly less
# credit than a pair that branches one level deeper.  An implementation that
# returned a single tree-wide number -- or that credited the whole insertion
# delay to every pair -- passes the checks above and fails here.
set deep [cts_crpr_credit ff1/CK ff2/CK]
set shallow [cts_crpr_credit ff1/CK ff3/CK]
if { $shallow < $deep } {
  puts [format "PASS cross-branch credit %.4f ns < same-branch credit %.4f ns" \
    [expr { $shallow * 1e9 }] [expr { $deep * 1e9 }]]
} else {
  puts [format "FAIL cross-branch credit %.4f ns not less than %.4f ns" \
    [expr { $shallow * 1e9 }] [expr { $deep * 1e9 }]]
}

# PROVEN NEGATIVE: a pin that is not a sink of this tree must be rejected, not
# silently credited 0.
if { [catch { cts_crpr_credit ff1/CK root_buf/A } msg] } {
  puts "PASS non-sink pin rejected"
} else {
  puts "FAIL non-sink pin silently credited $msg"
}
