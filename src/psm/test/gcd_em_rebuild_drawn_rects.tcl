# A network built for floorplanning (check_power_grid -floorplanning skips the
# pins of instances that are not FIXED) is rebuilt from scratch when the grid
# is first solved.  The rebuilt network must hold exactly the drawn rectangles
# of a fresh build -- the geometry EM current density judges a wire on -- and
# none carried over from the floorplanning build.
source helpers.tcl

read_lef Nangate45/Nangate45.lef
read_def Nangate45_data/gcd.def
read_liberty Nangate45/Nangate45_typ.lib
read_sdc Nangate45_data/gcd.sdc

# The drawn-rectangle total of the one network construction in a log.
proc drawn_rects { log } {
  set counts [regexp -all -inline {Total drawn rects: (\d+)} $log]
  if { [llength $counts] != 2 } {
    error "expected one network construction, found [expr { [llength $counts] / 2 }]"
  }
  return [lindex $counts 1]
}

set_debug_level PSM construct 1

# A fresh build for analysis.
with_output_to_variable log {
  analyze_power_grid -net VDD -vsrc Vsrc_gcd_vdd.loc
}
set fresh [drawn_rects $log]

# A floorplanning build, then the rebuild made by the first solve.
psm::clear_solvers
with_output_to_variable log { check_power_grid -net VDD -floorplanning }
set floorplan [drawn_rects $log]
with_output_to_variable log {
  analyze_power_grid -net VDD -vsrc Vsrc_gcd_vdd.loc
}
set rebuilt [drawn_rects $log]

set_debug_level PSM construct 0

if { $floorplan >= $fresh } {
  error "floorplanning build has $floorplan drawn rectangles, not fewer than\
    the $fresh of a fresh build"
}
if { $rebuilt != $fresh } {
  error "rebuilt network has $rebuilt drawn rectangles, a fresh build $fresh"
}
puts "drawn rectangles: rebuild == fresh build, floorplanning build fewer: OK"
