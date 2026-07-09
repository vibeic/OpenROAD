# tapcell -bound_to_placement (vibeic fork): on a SPARSE die (large floorplan,
# std cells only in the bottom-left corner) stock tapcell floods the whole die
# with well-taps over empty silicon; -bound_to_placement ties only the placed-cell
# region (+ halo), so a sparse macro die is not carpeted with taps that would have
# no power-aware LVS anchor.
source "helpers.tcl"
read_lef Nangate45/Nangate45.lef
read_def sparse_tapcell.def

proc count_master {master} {
  set n 0
  foreach inst [[ord::get_db_block] getInsts] {
    if { [[$inst getMaster] getName] eq $master } { incr n }
  }
  return $n
}

# 1. Stock tapcell: well-taps across the whole die.
tapcell -distance 2 -tapcell_master TAPCELL_X1
set n_stock [count_master TAPCELL_X1]
tapcell_ripup

# 2. Bounded tapcell: only the placed-cell region + 3um latch-up halo.
tapcell -distance 2 -tapcell_master TAPCELL_X1 -bound_to_placement -placement_halo 3
set n_bounded [count_master TAPCELL_X1]

# 3. Every bounded tap center must sit inside the placed-cell bbox + halo.
set blk [ord::get_db_block]
set dbu [[ord::get_db_tech] getDbUnitsPerMicron]
set halo [expr { 3 * $dbu }]
set minx 1000000000000; set miny 1000000000000
set maxx -1000000000000; set maxy -1000000000000
foreach inst [$blk getInsts] {
  if { [[$inst getMaster] getName] eq "BUF_X1" } {
    set bb [$inst getBBox]
    if { [$bb xMin] < $minx } { set minx [$bb xMin] }
    if { [$bb yMin] < $miny } { set miny [$bb yMin] }
    if { [$bb xMax] > $maxx } { set maxx [$bb xMax] }
    if { [$bb yMax] > $maxy } { set maxy [$bb yMax] }
  }
}
set all_within 1
foreach inst [$blk getInsts] {
  if { [[$inst getMaster] getName] eq "TAPCELL_X1" } {
    set bb [$inst getBBox]
    set cx [expr { ([$bb xMin] + [$bb xMax]) / 2 }]
    set cy [expr { ([$bb yMin] + [$bb yMax]) / 2 }]
    if { $cx < ($minx - $halo) || $cx > ($maxx + $halo)
         || $cy < ($miny - $halo) || $cy > ($maxy + $halo) } {
      set all_within 0
    }
  }
}

puts "bounded_fewer_than_stock [expr { $n_bounded < $n_stock }]"
puts "bounded_nonzero [expr { $n_bounded > 0 }]"
puts "all_bounded_taps_within_region $all_within"
# PASSFAIL test: the harness (regression_test.sh) requires the LAST line to
# match ^(pass|OK). Print "pass" only when every invariant holds; otherwise a
# FAIL diagnostic (which will not match) so the test fails loudly.
if { $n_bounded < $n_stock && $n_bounded > 0 && $all_within } {
  puts "pass"
} else {
  puts "FAIL (stock=$n_stock bounded=$n_bounded within=$all_within)"
}
