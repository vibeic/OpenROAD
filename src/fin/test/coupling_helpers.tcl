# Shared measurements for the coupling-aware fill tests.
#
# The "crit" net in coupling_fill.def is a met1 shape spanning
# x = 149000 .. 151000 DBU for the full height of the die, so the horizontal
# gap from any fill to that net is exact integer arithmetic.

proc crit_fill_stats {} {
  set block [ord::get_db_block]
  set lo 149000
  set hi 151000
  set count 0
  set best -1
  foreach fill [$block getFills] {
    if { [[$fill getTechLayer] getName] ne "met1" } {
      continue
    }
    incr count
    set r [$fill getRect]
    set x1 [$r xMin]
    set x2 [$r xMax]
    if { $x2 <= $lo } {
      set gap [expr { $lo - $x2 }]
    } elseif { $x1 >= $hi } {
      set gap [expr { $x1 - $hi }]
    } else {
      set gap 0
    }
    if { $best < 0 || $gap < $best } {
      set best $gap
    }
  }
  return [list $count $best]
}

proc report_crit_fill { label } {
  lassign [crit_fill_stats] count gap
  puts "$label met1 fills $count min gap to crit $gap"
}
