# EM current-density signoff on a pad ring connected by abutment: the IO
# cells' PG pin shapes carry the ring's current where no SPECIALNET wire is
# drawn.  Every segment the solve carries is judged -- the ring's vias
# included -- and every segment on IO pin metal alone is judged on that drawn
# pin metal: none is left without a cross-section (NO_AREA).
source helpers.tcl
source em_signoff_helpers.tcl

read_lef ihp-sg13g2_data/sg13g2_tech.lef
read_lef ihp-sg13g2_data/sg13g2_stdcell.lef
read_lef ihp-sg13g2_data/IOLib.lef
read_def pad_connected_by_abutment.def
read_liberty ihp-sg13g2_data/sg13g2_stdcell_typ_1p20V_25C.lib
read_liberty ihp-sg13g2_data/IOLib_dummy.lib
source ihp-sg13g2_data/setRC.tcl

set em_file [make_result_file em_pad_ring_abutment_segments.csv]
analyze_power_grid -net VDD -enable_em -em_outfile $em_file

set rpt [make_result_file em_pad_ring_abutment.csv]
check_current_density -net VDD -em_limits_file em_pad_ring_abutment.limits \
  -em_report $rpt -allow_reuse

set solved [em_outfile_count $em_file]
set listed [llength [em_report_rows $rpt]]
if { $listed != $solved } {
  error "report has $listed segments, the solve carried $solved"
}

set block [ord::get_db_block]
set dbu [$block getDbUnitsPerMicron]

# The net's own SPECIALNET wiring, per layer.
set wires {}
foreach swire [[$block findNet VDD] getSWires] {
  foreach box [$swire getWires] {
    if { ![$box isVia] } {
      dict lappend wires [[$box getTechLayer] getName] \
        [list [$box xMin] [$box yMin] [$box xMax] [$box yMax]]
    }
  }
}
# The IO ring cells.
set io_cells {}
foreach inst [$block getInsts] {
  if { [string match PAD* [[$inst getMaster] getType]] } {
    set bbox [$inst getBBox]
    lappend io_cells [list [$bbox xMin] [$bbox yMin] [$bbox xMax] [$bbox yMax]]
  }
}
proc inside { x y rect } {
  lassign $rect xlo ylo xhi yhi
  return [expr { $x >= $xlo && $x <= $xhi && $y >= $ylo && $y <= $yhi }]
}

set routing [em_layers_of_type ROUTING]
set no_area 0
set pin_only 0
set pin_only_no_area 0
set pin_only_carrying 0
foreach row [em_report_rows $rpt] {
  lassign $row layer x0 y0 x1 y1 current area j jl ratio status cuts basis
  if { $status == "NO_AREA" } {
    incr no_area
  }
  if { [lsearch -exact $routing $layer] < 0 } {
    continue
  }
  set p0 [list [expr { round($x0 * $dbu) }] [expr { round($y0 * $dbu) }]]
  set p1 [list [expr { round($x1 * $dbu) }] [expr { round($y1 * $dbu) }]]
  set in_io 0
  foreach cell $io_cells {
    if { [inside {*}$p0 $cell] && [inside {*}$p1 $cell] } {
      set in_io 1
      break
    }
  }
  if { !$in_io } {
    continue
  }
  set on_wire 0
  if { [dict exists $wires $layer] } {
    foreach rect [dict get $wires $layer] {
      if { [inside {*}$p0 $rect] || [inside {*}$p1 $rect] } {
        set on_wire 1
        break
      }
    }
  }
  if { $on_wire } {
    continue
  }
  # A metal segment inside an IO cell, on no SPECIALNET wire: pin metal only.
  incr pin_only
  if { $status == "NO_AREA" } {
    incr pin_only_no_area
  }
  if { $current != 0 } {
    incr pin_only_carrying
  }
}
if { $pin_only == 0 || $pin_only_carrying == 0 } {
  error "no current-carrying segment on IO pin metal alone ($pin_only)"
}
if { $pin_only_no_area != 0 || $no_area != 0 } {
  error "segments without a cross-section: $no_area ($pin_only_no_area on IO pin metal)"
}
puts "EM pad-ring self-check: solved=$solved listed=$listed pin-metal segments=$pin_only carrying=$pin_only_carrying NO_AREA=$no_area OK"
