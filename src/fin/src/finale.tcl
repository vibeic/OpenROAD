# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2020-2025, The OpenROAD Authors

proc density_fill_debug { args } {
  fin::set_density_fill_debug_cmd
}

sta::define_cmd_args "density_fill" {[-rules rules_file]\
                                     [-area {lx ly ux uy}]}

proc density_fill { args } {
  sta::parse_key_args "density_fill" args \
    keys {-rules -area} flags {}

  if { [info exists keys(-rules)] } {
    set rules_file $keys(-rules)
  } else {
    utl::error FIN 7 "The -rules argument must be specified."
  }

  if { [info exists keys(-area)] } {
    set area $keys(-area)
    if { [llength $area] != 4 } {
      utl::error FIN 8 "The -area argument must be a list of 4 coordinates."
    }
    lassign $area lx ly ux uy
    set lx [ord::microns_to_dbu $lx]
    set ly [ord::microns_to_dbu $ly]
    set ux [ord::microns_to_dbu $ux]
    set uy [ord::microns_to_dbu $uy]
    set fill_area [odb::Rect x $lx $ly $ux $uy]
  } else {
    set fill_area [ord::get_db_core]
  }

  fin::density_fill_cmd $rules_file $fill_area
}

sta::define_cmd_args "check_metal_density" {[-window window]\
                                            [-step step]\
                                            [-min_density density]\
                                            [-max_density density]\
                                            [-limits_file file]\
                                            [-report_file file]\
                                            [-area {lx ly ux uy}]}

proc check_metal_density { args } {
  sta::parse_key_args "check_metal_density" args \
    keys {-window -step -min_density -max_density -limits_file -report_file \
          -area} \
    flags {}

  if { ![info exists keys(-window)] } {
    utl::error FIN 26 "The -window argument must be specified."
  }
  set window [ord::microns_to_dbu $keys(-window)]

  set step 0
  if { [info exists keys(-step)] } {
    set step [ord::microns_to_dbu $keys(-step)]
  }

  set min_density -1.0
  if { [info exists keys(-min_density)] } {
    set min_density $keys(-min_density)
  }

  set max_density -1.0
  if { [info exists keys(-max_density)] } {
    set max_density $keys(-max_density)
  }

  set limits_file ""
  if { [info exists keys(-limits_file)] } {
    set limits_file $keys(-limits_file)
  }

  set report_file ""
  if { [info exists keys(-report_file)] } {
    set report_file $keys(-report_file)
  }

  if { [info exists keys(-area)] } {
    set area $keys(-area)
    if { [llength $area] != 4 } {
      utl::error FIN 27 "The -area argument must be a list of 4 coordinates."
    }
    lassign $area lx ly ux uy
    set check_area [odb::Rect x [ord::microns_to_dbu $lx] \
      [ord::microns_to_dbu $ly] [ord::microns_to_dbu $ux] \
      [ord::microns_to_dbu $uy]]
  } else {
    set check_area [ord::get_db_core]
  }

  return [fin::check_metal_density_cmd $check_area $window $step \
    $min_density $max_density $limits_file $report_file]
}
