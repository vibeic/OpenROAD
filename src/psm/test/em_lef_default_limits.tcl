# EM current-density signoff with no limit given reads the tech LEF's own
# DCCURRENTDENSITY AVERAGE limits, spec-literally (LEF reference units): a
# ROUTING layer's value is mA per um of width -> value * 1e-3 / THICKNESS
# A/um^2; a CUT layer's value is mA per um^2 of cut area -> value * 1e-3 A/um^2.
source helpers.tcl
source em_signoff_helpers.tcl

read_liberty sky130hd/sky130_fd_sc_hd__ss_n40C_1v40.lib
read_lef "sky130hd/sky130hd.tlef"
read_lef "sky130hd/sky130hd_std_cell.lef"
read_def "sky130hd_data/insert_decap_gcd.def"

source sky130hd/sky130hd.rc

set rpt [make_result_file em_lef_default_limits.csv]
check_current_density -net VDD -em_report $rpt

# Expected limits from the tech LEF this test reads.
set expected [dict create]
set f [open sky130hd/sky130hd.tlef]
set layer ""
set type ""
set thick 0
while { [gets $f line] >= 0 } {
  if { [regexp {^LAYER\s+(\S+)} $line -> name] } {
    set layer $name
    set type ""
    set thick 0
  } elseif { [regexp {^\s*TYPE\s+(\S+)\s*;} $line -> t] } {
    set type $t
  } elseif { [regexp {^\s*THICKNESS\s+([0-9.]+)} $line -> t] } {
    set thick $t
  } elseif { [regexp {^\s*DCCURRENTDENSITY\s+AVERAGE\s+([0-9.]+)\s*;} $line -> v] } {
    if { $type == "ROUTING" && $thick > 0 } {
      dict set expected $layer [expr { $v * 1e-3 / $thick }]
    } elseif { $type == "CUT" } {
      dict set expected $layer [expr { $v * 1e-3 }]
    }
  }
}
close $f

set seen [dict create]
foreach row [em_report_rows $rpt] {
  lassign $row layer x0 y0 x1 y1 cur area j jl ratio status cuts basis
  if { ![dict exists $expected $layer] || $status == "NO_AREA" } {
    continue
  }
  set want [dict get $expected $layer]
  if { abs($jl - $want) > 1e-3 * $want } {
    error "$layer limit $jl, the LEF gives $want"
  }
  dict incr seen $layer
}
set cuts_seen 0
foreach layer [em_layers_of_type CUT] {
  if { [dict exists $seen $layer] } {
    incr cuts_seen
  }
}
if { ![dict exists $seen met1] || $cuts_seen == 0 } {
  error "LEF-native limits not applied to routing and cut layers: $seen"
}
puts "EM LEF-default self-check: judged per layer=$seen OK"
