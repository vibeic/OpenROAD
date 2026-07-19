# Metal density check (FL1) digit gate on a fixture whose density is known by
# hand before the tool runs.
#
# density_fixture.def is a 100 um x 100 um die carrying exactly ONE piece of
# metal: a met1 stripe of width 50 um running from (0, 25) to (50, 25), i.e. the
# rectangle x in [0, 50], y in [0, 50].  So by construction
#
#   metal area                       = 50 * 50   = 2500 um^2
#   one 100x100 window               = 100 * 100 = 10000 um^2
#   density over that window         = 2500 / 10000 = 0.25   exactly
#
#   with 50x50 windows the die splits into four, and the metal fills exactly one
#   of them:
#     window (0,0)-(50,50)     -> 2500 / 2500 = 1.0   exactly
#     the other three          -> 0    / 2500 = 0.0   exactly
#
# The tool has to reproduce those numbers to the digit off the real ODB polygon
# union, and then flag exactly the windows a hand count says are outside a band.
# None of this can be satisfied by editing a golden log: the densities are
# re-derived from the emitted per-window report and compared in Tcl.
source helpers.tcl

read_lef sky130hd/sky130hd.tlef
read_def density_fixture.def

proc met1_densities { report } {
  set fh [open $report r]
  gets $fh header
  set out {}
  while { [gets $fh line] >= 0 } {
    if { $line eq "" } {
      continue
    }
    set f [split $line ","]
    if { [lindex $f 0] ne "met1" } {
      continue
    }
    lappend out [list [lindex $f 1] [lindex $f 2] [lindex $f 7]]
  }
  close $fh
  return $out
}

# Total windows the check emits, over every routing layer in the tech.
proc window_count { report } {
  set fh [open $report r]
  gets $fh header
  set n 0
  while { [gets $fh line] >= 0 } {
    if { $line ne "" } {
      incr n
    }
  }
  close $fh
  return $n
}

# ---------------------------------------------------------------------------
# One 100x100 window over the whole die -> density must be exactly 0.25.
# ---------------------------------------------------------------------------
set report [make_result_file density_one_window.csv]
set v [check_metal_density -window 100 -min_density 0.0 -max_density 1.0 \
  -area {0 0 100 100} -report_file $report]
if { $v != 0 } {
  error "FL1 full-band case expected 0 violations, got $v"
}
set d [met1_densities $report]
if { [llength $d] != 1 } {
  error "FL1 expected 1 met1 window, got [llength $d]"
}
set density [lindex [lindex $d 0] 2]
if { abs($density - 0.25) > 1e-9 } {
  error "FL1 expected density 0.25, tool reported $density"
}
puts "one 100x100 window: density $density"

# ---------------------------------------------------------------------------
# Four 50x50 windows -> exactly one at 1.0 and three at 0.0.
# ---------------------------------------------------------------------------
set report4 [make_result_file density_four_windows.csv]
check_metal_density -window 50 -min_density 0.0 -max_density 1.0 \
  -area {0 0 100 100} -report_file $report4
set d4 [met1_densities $report4]
if { [llength $d4] != 4 } {
  error "FL1 expected 4 met1 windows, got [llength $d4]"
}
set full 0
set empty 0
foreach w $d4 {
  set density [lindex $w 2]
  if { abs($density - 1.0) < 1e-9 } {
    incr full
    if { [lindex $w 0] != 0.0 || [lindex $w 1] != 0.0 } {
      error "FL1 the full window should be the one at (0,0), got\
             ([lindex $w 0],[lindex $w 1])"
    }
  } elseif { abs($density) < 1e-9 } {
    incr empty
  } else {
    error "FL1 unexpected density $density; every 50x50 window must be 0 or 1"
  }
}
if { $full != 1 || $empty != 3 } {
  error "FL1 expected 1 full + 3 empty windows, got $full + $empty"
}
puts "four 50x50 windows: 1 at density 1.0, 3 at density 0.0"

# ---------------------------------------------------------------------------
# Proven-negative.  The check spans every ROUTING layer, so with 50x50 windows
# it emits 4 windows per routing layer -- and by construction EXACTLY ONE of
# them (met1 at the origin) holds any metal at all, at density 1.0; every other
# window on every other layer is empty at density 0.0.
#
# So a minimum of 0.5 must flag every window but that one, and a maximum of 0.5
# must flag that one and nothing else.  The two counts are exactly complementary
# and must sum to the total -- a partition no golden log can fake.
# ---------------------------------------------------------------------------
set total [window_count $report4]
set under [check_metal_density -window 50 -min_density 0.5 -area {0 0 100 100}]
if { $under != $total - 1 } {
  error "FL1 min-density 0.5 expected [expr { $total - 1 }] under-density\
         windows, got $under"
}
set over [check_metal_density -window 50 -max_density 0.5 -area {0 0 100 100}]
if { $over != 1 } {
  error "FL1 max-density 0.5 expected 1 over-density window, got $over"
}
if { $under + $over != $total } {
  error "FL1 under ($under) + over ($over) must partition the $total windows"
}
puts "of $total windows, min 0.5 flags $under and max 0.5 flags $over\
      (complementary)"

# ---------------------------------------------------------------------------
# Fail-safe: with no band at all the check cannot manufacture a violation, even
# though three of the four windows are completely empty.
# ---------------------------------------------------------------------------
set none [check_metal_density -window 50 -area {0 0 100 100}]
if { $none != 0 } {
  error "FL1 no-band case must report 0 violations, got $none"
}
puts "no band: $none violations"
