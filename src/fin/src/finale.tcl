# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2020-2025, The OpenROAD Authors

proc density_fill_debug { args } {
  fin::set_density_fill_debug_cmd
}

namespace eval fin {
# Parse an -area argument into an odb::Rect, defaulting to the core area.
proc parse_area { area_arg } {
  if { $area_arg eq "" } {
    return [ord::get_db_core]
  }
  if { [llength $area_arg] != 4 } {
    utl::error FIN 8 "The -area argument must be a list of 4 coordinates."
  }
  lassign $area_arg lx ly ux uy
  return [odb::Rect x [ord::microns_to_dbu $lx] [ord::microns_to_dbu $ly] \
    [ord::microns_to_dbu $ux] [ord::microns_to_dbu $uy]]
}

proc check_density_range { min_density max_density } {
  if { $min_density < 0.0 || $min_density > 1.0 } {
    utl::error FIN 21 "min_density must be between 0.0 and 1.0."
  }
  if { $max_density < 0.0 || $max_density > 1.0 } {
    utl::error FIN 22 "max_density must be between 0.0 and 1.0."
  }
  if { $min_density > $max_density } {
    utl::error FIN 23 "min_density must not exceed max_density."
  }
}
}

sta::define_cmd_args "density_fill" {[-rules rules_file]\
                                     [-area {lx ly ux uy}]\
                                     [-min_density density]\
                                     [-max_density density]\
                                     [-density_window window]\
                                     [-density_step step]\
                                     [-critical_nets nets]\
                                     [-critical_halo halo]}

proc density_fill { args } {
  sta::parse_key_args "density_fill" args \
    keys {-rules -area -min_density -max_density -density_window \
      -density_step -critical_nets -critical_halo} \
    flags {}

  if { [info exists keys(-rules)] } {
    set rules_file $keys(-rules)
  } else {
    utl::error FIN 7 "The -rules argument must be specified."
  }

  set area_arg ""
  if { [info exists keys(-area)] } {
    set area_arg $keys(-area)
  }
  set fill_area [fin::parse_area $area_arg]

  # Density targets are optional; with none given the historical
  # fill-everything-available behavior is preserved exactly.
  set density_target 0
  set min_density 0.0
  set max_density 1.0
  set window 0
  set step 0

  if { [info exists keys(-min_density)] } {
    set density_target 1
    set min_density $keys(-min_density)
  }
  if { [info exists keys(-max_density)] } {
    set density_target 1
    set max_density $keys(-max_density)
  }

  if { $density_target } {
    fin::check_density_range $min_density $max_density
    if { ![info exists keys(-density_window)] } {
      utl::error FIN 16 "-density_window must be specified with\
        -min_density/-max_density."
    }
    set window [ord::microns_to_dbu $keys(-density_window)]
    if { [info exists keys(-density_step)] } {
      set step [ord::microns_to_dbu $keys(-density_step)]
    } else {
      set step $window
    }
    if { $window <= 0 || $step <= 0 } {
      utl::error FIN 17 "-density_window and -density_step must be positive."
    }
  }

  # Coupling relief around timing-critical nets is optional.
  set critical_nets ""
  set critical_halo 0
  if { [info exists keys(-critical_nets)] } {
    set critical_nets [join $keys(-critical_nets) " "]
  }
  if { [info exists keys(-critical_halo)] } {
    set critical_halo [ord::microns_to_dbu $keys(-critical_halo)]
    if { $critical_halo <= 0 } {
      utl::error FIN 26 "-critical_halo must be positive."
    }
  }
  if { $critical_nets ne "" && $critical_halo == 0 } {
    utl::error FIN 27 "-critical_halo must be specified with -critical_nets."
  }

  fin::density_fill_cmd $rules_file $fill_area $density_target \
    $window $step $min_density $max_density $critical_nets $critical_halo
}

sta::define_cmd_args "check_metal_density" {[-area {lx ly ux uy}]\
                                            [-window window]\
                                            [-step step]\
                                            [-min_density density]\
                                            [-max_density density]\
                                            [-layer layer]}

proc check_metal_density { args } {
  sta::parse_key_args "check_metal_density" args \
    keys {-area -window -step -min_density -max_density -layer} flags {}

  if { ![info exists keys(-window)] } {
    utl::error FIN 18 "The -window argument must be specified."
  }
  set window [ord::microns_to_dbu $keys(-window)]
  if { [info exists keys(-step)] } {
    set step [ord::microns_to_dbu $keys(-step)]
  } else {
    set step $window
  }
  if { $window <= 0 || $step <= 0 } {
    utl::error FIN 19 "-window and -step must be positive."
  }

  set min_density 0.0
  set max_density 1.0
  if { [info exists keys(-min_density)] } {
    set min_density $keys(-min_density)
  }
  if { [info exists keys(-max_density)] } {
    set max_density $keys(-max_density)
  }
  fin::check_density_range $min_density $max_density

  # An empty layer name means every routing layer.
  set layer_name ""
  if { [info exists keys(-layer)] } {
    set layer_name $keys(-layer)
  }

  set area_arg ""
  if { [info exists keys(-area)] } {
    set area_arg $keys(-area)
  }
  set check_area [fin::parse_area $area_arg]

  return [fin::check_metal_density_cmd $check_area $window $step \
    $min_density $max_density $layer_name]
}
