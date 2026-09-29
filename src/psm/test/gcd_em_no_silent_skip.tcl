# EM current-density signoff never drops a segment silently: every segment
# the power-grid solve carries appears in the report, and one that cannot be
# judged (no limit for its layer) is counted, listed by layer and makes the
# verdict NOT_MEASURED -- never a clean PASS.
source helpers.tcl
source em_signoff_helpers.tcl

read_lef Nangate45/Nangate45.lef
read_def Nangate45_data/gcd.def
read_liberty Nangate45/Nangate45_typ.lib
read_sdc Nangate45_data/gcd.sdc

set em_file [make_result_file gcd_em_no_silent_skip_segments.csv]
analyze_power_grid -net VDD -vsrc Vsrc_gcd_vdd.loc -enable_em \
  -em_outfile $em_file

# A limit for metal1 only.
set limits gcd_em_no_silent_skip.limits

set rpt [make_result_file gcd_em_no_silent_skip.csv]
check_current_density -net VDD -vsrc Vsrc_gcd_vdd.loc -em_limits_file $limits \
  -em_report $rpt -allow_reuse

set rows [em_report_rows $rpt]
set solved [em_outfile_count $em_file]
if { [llength $rows] != $solved } {
  error "report has [llength $rows] segments, the solve carried $solved"
}
set unlimited {}
foreach row $rows {
  lassign $row layer x0 y0 x1 y1 cur area j jl ratio status
  if { $status == "NO_LIMIT" } {
    dict incr unlimited $layer
  }
}
set cut_listed 0
foreach layer [em_layers_of_type CUT] {
  if { [dict exists $unlimited $layer] } {
    incr cut_listed
  }
}
if { ![dict exists $unlimited metal4] || $cut_listed == 0 } {
  error "segments without a limit were not all listed: $unlimited"
}
puts "EM no-silent-skip self-check: solved=$solved listed=[llength $rows] NO_LIMIT=$unlimited OK"
