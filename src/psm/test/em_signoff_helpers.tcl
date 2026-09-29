# Helpers shared by the EM current-density signoff regressions.

# Rows of a check_current_density -em_report CSV, as lists, header dropped.
proc em_report_rows { file } {
  set f [open $file]
  gets $f header
  set rows {}
  while { [gets $f line] >= 0 } {
    lappend rows [split $line ","]
  }
  close $f
  return $rows
}

# Number of data rows of an analyze_power_grid -em_outfile CSV.
proc em_outfile_count { file } {
  set f [open $file]
  gets $f header
  set n 0
  while { [gets $f line] >= 0 } {
    incr n
  }
  close $f
  return $n
}

# Tech layer names of one type (ROUTING / CUT).
proc em_layers_of_type { type } {
  set names {}
  foreach layer [[ord::get_db_tech] getLayers] {
    if { [$layer getType] == $type } {
      lappend names [$layer getName]
    }
  }
  return $names
}
