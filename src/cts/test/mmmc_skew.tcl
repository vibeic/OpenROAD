# CT3: clock skew reported per timing scene rather than at one corner only.
#
# Two independent clock trees, both one buffer deep:
#   clk1 -> buf1 -> n_c1 -> ff1/CK, ff2/CK   ff1 and ff2 are co-located, so
#                                            the two sinks are the same point
#                                            and the skew is exactly zero.
#   clk2 -> buf2 -> n_c2 -> ff3/CK, ff4/CK   ff3 is 2.5 um from buf2 and ff4
#                                            is 45 um away, so the skew is
#                                            real and corner-dependent.
source "helpers.tcl"

define_corners fast typical slow
read_liberty -corner fast Nangate45/Nangate45_fast.lib
read_liberty -corner typical Nangate45/Nangate45_typ.lib
read_liberty -corner slow Nangate45/Nangate45_slow.lib
read_lef Nangate45/Nangate45.lef
read_def mmmc_skew.def

create_clock -name c1 -period 5 clk1
create_clock -name c2 -period 5 clk2
set_propagated_clock [all_clocks]

source Nangate45/Nangate45.rc
set_wire_rc -clock -layer metal5
set_wire_rc -signal -layer metal3
estimate_parasitics -placement

report_cts_skew -verbose

# The zero-skew claim is only meaningful if the fixture really is symmetric,
# so prove the precondition rather than assume it.
set p1 [[[ord::get_db_block] findITerm "ff1/CK"] getAvgXY]
set p2 [[[ord::get_db_block] findITerm "ff2/CK"] getAvgXY]
if { [lrange $p1 1 2] == [lrange $p2 1 2] } {
  puts "PASS fixture precondition: ff1/CK and ff2/CK are co-located"
} else {
  puts "FAIL fixture precondition: $p1 vs $p2"
}

# POSITIVE GATE: a tree whose two sinks are the same point has zero skew, and
# must report zero at every scene -- not just at the one scene a single-corner
# implementation happens to look at.
foreach scene {fast typical slow} {
  set skew [cts_clock_skew clk1 -scene $scene]
  if { abs($skew) < 1e-15 } {
    puts "PASS symmetric tree skew is 0 at scene $scene"
  } else {
    puts [format "FAIL symmetric tree skew %.6f ns at scene %s" \
      [expr { $skew * 1e9 }] $scene]
  }
}

# PROVEN NEGATIVE: the unbalanceable tree must report its real residual, not
# zero.  Reporting zero here is what "we balanced it" would look like if the
# balancing were faked.
set residual {}
foreach scene {fast typical slow} {
  set skew [cts_clock_skew clk2 -scene $scene]
  lappend residual $skew
  if { $skew > 0.0 } {
    puts [format "PASS asymmetric tree reports real residual %.6f ns at scene %s" \
      [expr { $skew * 1e9 }] $scene]
  } else {
    puts [format "FAIL asymmetric tree reported %.6f ns at scene %s" \
      [expr { $skew * 1e9 }] $scene]
  }
}

# PROVEN NEGATIVE: the three scenes must not all return the same number.  An
# implementation that evaluates one corner and prints it three times passes
# every check above and fails this one.
lassign $residual r_fast r_typ r_slow
if { $r_fast != $r_typ && $r_typ != $r_slow } {
  puts [format "PASS scenes differ: fast %.6f typ %.6f slow %.6f ns" \
    [expr { $r_fast * 1e9 }] [expr { $r_typ * 1e9 }] [expr { $r_slow * 1e9 }]]
} else {
  puts [format "FAIL scenes identical: fast %.6f typ %.6f slow %.6f ns" \
    [expr { $r_fast * 1e9 }] [expr { $r_typ * 1e9 }] [expr { $r_slow * 1e9 }]]
}

# Insertion delay, unlike skew, is dominated by cell delay and so must grow
# strictly from the fast scene to the slow one.  Skew deliberately is not
# checked for that ordering: the skew here comes from the wire RC difference
# between a 2.5 um and a 45 um branch, and set_layer_rc is the same at every
# scene, so skew barely moves across corners while insertion delay moves 4x.
# Asserting a corner ordering on skew would be asserting a law that does not
# hold. This is the check that would catch the scene lookup being wired up
# backwards.
set ins {}
foreach scene {fast typical slow} {
  lappend ins [cts_clock_insertion_delay clk2 -scene $scene]
}
lassign $ins i_fast i_typ i_slow
if { $i_fast < $i_typ && $i_typ < $i_slow } {
  puts [format "PASS insertion delay grows fast %.4f < typ %.4f < slow %.4f ns" \
    [expr { $i_fast * 1e9 }] [expr { $i_typ * 1e9 }] [expr { $i_slow * 1e9 }]]
} else {
  puts [format "FAIL insertion delay not monotonic: fast %.4f typ %.4f slow %.4f ns" \
    [expr { $i_fast * 1e9 }] [expr { $i_typ * 1e9 }] [expr { $i_slow * 1e9 }]]
}
