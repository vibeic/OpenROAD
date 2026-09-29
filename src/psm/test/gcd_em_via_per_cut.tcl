# EM current-density signoff: a via is judged per cut against its OWN cut
# layer.  Every via row must be labeled with its cut layer (not the metal
# below it), carry its cut count, and be judged on current / cuts against a
# per-cut limit given as "<cut layer> per_cut <A per cut>".
source helpers.tcl
source em_signoff_helpers.tcl

read_lef Nangate45/Nangate45.lef
read_def Nangate45_data/gcd.def
read_liberty Nangate45/Nangate45_typ.lib
read_sdc Nangate45_data/gcd.sdc

set cut_layers [em_layers_of_type CUT]

# Loose per-cut limit: every via judged per cut, none over.
set loose gcd_em_via_per_cut.loose_limits
set rpt [make_result_file gcd_em_via_per_cut_loose.csv]
set v [check_current_density -net VDD -vsrc Vsrc_gcd_vdd.loc \
  -em_limits_file $loose -em_report $rpt]
set vias 0
foreach row [em_report_rows $rpt] {
  lassign $row layer x0 y0 x1 y1 cur area j jl ratio status cuts basis
  if { [lsearch -exact $cut_layers $layer] >= 0 } {
    incr vias
    if { $basis != "PER_CUT" || $cuts < 1 } {
      error "via row on $layer not judged per cut: $row"
    }
    if { abs($j - $cur / $cuts) > 1e-12 + 1e-3 * abs($j) } {
      error "via row on $layer: per-cut current $j != $cur / $cuts"
    }
  }
  if { $status == "NO_AREA" || $status == "NO_LIMIT" } {
    error "unjudged segment with a complete limits file: $row"
  }
}
if { $vias == 0 } {
  error "no via judged: every via was dropped"
}
if { $v != 0 } {
  error "loose per-cut limit expected 0 violations, got $v"
}

# Tight per-cut limit: violations, and every one is a via on its cut layer.
set tight gcd_em_via_per_cut.tight_limits
set rpt [make_result_file gcd_em_via_per_cut_tight.csv]
set v [check_current_density -net VDD -vsrc Vsrc_gcd_vdd.loc \
  -em_limits_file $tight -em_report $rpt -allow_reuse]
if { $v <= 0 } {
  error "tight per-cut limit expected via violations, got $v"
}
foreach row [em_report_rows $rpt] {
  lassign $row layer x0 y0 x1 y1 cur area j jl ratio status cuts basis
  if { $status == "VIOLATED" && [lsearch -exact $cut_layers $layer] < 0 } {
    error "a routing segment violated a via-only tight limit: $row"
  }
}
puts "EM via per-cut self-check: vias=$vias loose=0 tight=$v OK"
