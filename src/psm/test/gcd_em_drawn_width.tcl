# EM current-density signoff judges a wire on the drawn metal that carries
# it.  The solver slices the union of a layer's drawn shapes (straps, via
# enclosures, pins) into rectangles; a node pair can land in a thin slice of
# that decomposition.  No metal segment may be judged on a cross-section
# narrower than its layer's minimum drawn width (LEF WIDTH x THICKNESS).
source helpers.tcl
source em_signoff_helpers.tcl

read_lef Nangate45/Nangate45.lef
read_def Nangate45_data/gcd.def
read_liberty Nangate45/Nangate45_typ.lib
read_sdc Nangate45_data/gcd.sdc

# THICKNESS per routing layer, from the LEF this test reads.
set thickness [dict create]
set f [open Nangate45/Nangate45.lef]
set layer ""
while { [gets $f line] >= 0 } {
  if { [regexp {^LAYER\s+(\S+)} $line -> name] } {
    set layer $name
  } elseif { [regexp {^\s*THICKNESS\s+([0-9.]+)} $line -> t] && $layer != "" } {
    dict set thickness $layer $t
  }
}
close $f

set dbu [[ord::get_db_tech] getDbUnitsPerMicron]
set rpt [make_result_file gcd_em_drawn_width.csv]
check_current_density -net VDD -vsrc Vsrc_gcd_vdd.loc -em_limit 1.0 \
  -em_report $rpt

set narrow 0
set judged 0
foreach row [em_report_rows $rpt] {
  lassign $row layer x0 y0 x1 y1 cur area j jl ratio status cuts basis
  if { ![dict exists $thickness $layer] || $basis == "NO_AREA" } {
    continue
  }
  incr judged
  set min_width [expr { [[[ord::get_db_tech] findLayer $layer] getWidth] / double($dbu) }]
  set width [expr { $area / [dict get $thickness $layer] }]
  if { $width < $min_width * (1.0 - 1e-6) } {
    incr narrow
  }
}
if { $judged == 0 } {
  error "no metal segment judged"
}
if { $narrow != 0 } {
  error "$narrow metal segment(s) judged on a width below the layer minimum"
}
puts "EM drawn-width self-check: metal segments judged=$judged below-minimum=0 OK"
