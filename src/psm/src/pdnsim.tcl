# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2022-2025, The OpenROAD Authors

sta::define_cmd_args "check_power_grid" {
  -net power_net
  [-error_file error_file]
  [-floorplanning]
  [-dont_require_terminals]
}

proc check_power_grid { args } {
  sta::parse_key_args "check_power_grid" args \
    keys {-net -error_file} \
    flags {-floorplanning -dont_require_terminals}

  if { ![info exists keys(-net)] } {
    utl::error PSM 57 "Argument -net not specified."
  }

  set error_file ""
  if { [info exists keys(-error_file)] } {
    set error_file $keys(-error_file)
  }

  set floorplanning [info exists flags(-floorplanning)]
  set dont_require_bterm [info exists flags(-dont_require_terminals)]

  psm::check_connectivity_cmd \
    [psm::find_net $keys(-net)] \
    $floorplanning \
    $error_file \
    $dont_require_bterm
}

sta::define_cmd_args "analyze_power_grid" {
  -net net_name
  [-corner corner]
  [-error_file error_file]
  [-voltage_file voltage_file]
  [-enable_em]
  [-em_outfile em_file]
  [-vsrc voltage_source_file]
  [-source_type FULL|BUMPS|STRAPS]
  [-allow_reuse]
  [-transient]
  [-period period]
  [-steps steps]
  [-num_periods num_periods]
  [-node_cap cap]
  [-total_cap cap]
  [-decap_cap cap]
  [-current_duty duty]
  [-spread_phases]
  [-current_profile file]
  [-vectored_profile file]
  [-package_r resistance]
  [-package_l inductance]
}

proc analyze_power_grid { args } {
  sta::parse_key_args "analyze_power_grid" args \
    keys {-net -corner -voltage_file -error_file -em_outfile -vsrc \
      -source_type -period -steps -num_periods -node_cap -total_cap \
      -decap_cap -current_duty -current_profile -vectored_profile \
      -package_r -package_l} \
    flags {-enable_em -allow_reuse -transient -spread_phases}
  if { ![info exists keys(-net)] } {
    utl::error PSM 58 "Argument -net not specified."
  }

  set error_file ""
  if { [info exists keys(-error_file)] } {
    set error_file $keys(-error_file)
  }

  set voltage_file ""
  if { [info exists keys(-voltage_file)] } {
    set voltage_file $keys(-voltage_file)
  }

  set voltage_source_file ""
  if { [info exists keys(-vsrc)] } {
    set voltage_source_file $keys(-vsrc)
  }

  set source_type "BUMPS"
  if { [info exists keys(-source_type)] } {
    set source_type $keys(-source_type)
  }

  # Transient (dynamic / di-dt) analysis path.  Fully backward compatible: the
  # command is byte-identical to the static path unless -transient is given.
  if { [info exists flags(-transient)] } {
    if { ![info exists keys(-period)] } {
      utl::error PSM 107 "Transient analysis requires -period."
    }
    set period [sta::time_ui_sta $keys(-period)]

    set steps 100
    if { [info exists keys(-steps)] } {
      set steps $keys(-steps)
    }
    set num_periods 1
    if { [info exists keys(-num_periods)] } {
      set num_periods $keys(-num_periods)
    }
    set node_cap 0.0
    if { [info exists keys(-node_cap)] } {
      set node_cap [sta::capacitance_ui_sta $keys(-node_cap)]
    }
    set total_cap 0.0
    if { [info exists keys(-total_cap)] } {
      set total_cap [sta::capacitance_ui_sta $keys(-total_cap)]
    }
    set decap_cap 0.0
    if { [info exists keys(-decap_cap)] } {
      set decap_cap [sta::capacitance_ui_sta $keys(-decap_cap)]
    }
    set current_duty 1.0
    if { [info exists keys(-current_duty)] } {
      set current_duty $keys(-current_duty)
    }
    set phase_spread [info exists flags(-spread_phases)]
    set current_profile ""
    if { [info exists keys(-current_profile)] } {
      set current_profile $keys(-current_profile)
    }
    set vectored_profile ""
    if { [info exists keys(-vectored_profile)] } {
      set vectored_profile $keys(-vectored_profile)
    }
    set package_r 0.0
    if { [info exists keys(-package_r)] } {
      set package_r $keys(-package_r)
    }
    set package_l 0.0
    if { [info exists keys(-package_l)] } {
      set package_l $keys(-package_l)
    }

    psm::analyze_power_grid_dynamic_cmd \
      [psm::find_net $keys(-net)] \
      [sta::parse_scene_or_default keys] \
      $source_type \
      $error_file \
      $voltage_file \
      $voltage_source_file \
      $period \
      $steps \
      $num_periods \
      $node_cap \
      $total_cap \
      $decap_cap \
      $current_duty \
      $phase_spread \
      $current_profile \
      $vectored_profile \
      $package_r \
      $package_l
    return
  }

  set enable_em [info exists flags(-enable_em)]
  set em_file ""
  if { [info exists keys(-em_outfile)] } {
    set em_file $keys(-em_outfile)
    if { !$enable_em } {
      utl::error PSM 55 "EM file cannot be specified without enabling em analysis."
    }
  }

  psm::analyze_power_grid_cmd \
    [psm::find_net $keys(-net)] \
    [sta::parse_scene_or_default keys] \
    $source_type \
    $error_file \
    [info exists flags(-allow_reuse)] \
    $enable_em \
    $em_file \
    $voltage_file \
    $voltage_source_file
}

sta::define_cmd_args "check_current_density" {
  -net net_name
  [-corner corner]
  [-em_limit limit]
  [-em_limits_file file]
  [-em_report report_file]
  [-vsrc voltage_source_file]
  [-source_type FULL|BUMPS|STRAPS]
  [-allow_reuse]
}

proc check_current_density { args } {
  sta::parse_key_args "check_current_density" args \
    keys {-net -corner -em_limit -em_limits_file -em_report -vsrc -source_type} \
    flags {-allow_reuse}

  if { ![info exists keys(-net)] } {
    utl::error PSM 117 "Argument -net not specified."
  }

  set default_limit 0.0
  if { [info exists keys(-em_limit)] } {
    set default_limit $keys(-em_limit)
    sta::check_positive_float "-em_limit" $default_limit
  }

  set limits_file ""
  if { [info exists keys(-em_limits_file)] } {
    set limits_file $keys(-em_limits_file)
  }

  set report_file ""
  if { [info exists keys(-em_report)] } {
    set report_file $keys(-em_report)
  }

  set voltage_source_file ""
  if { [info exists keys(-vsrc)] } {
    set voltage_source_file $keys(-vsrc)
  }

  set source_type "BUMPS"
  if { [info exists keys(-source_type)] } {
    set source_type $keys(-source_type)
  }

  return [psm::check_current_density_cmd \
    [psm::find_net $keys(-net)] \
    [sta::parse_scene_or_default keys] \
    $source_type \
    $voltage_source_file \
    [info exists flags(-allow_reuse)] \
    $default_limit \
    $limits_file \
    $report_file]
}

sta::define_cmd_args "check_signal_em" {
  [-corner corner]
  [-supply_voltage volts]
  [-toggle_rate transitions_per_second]
  [-activity_file file]
  [-avg_limit limit]
  [-rms_limit limit]
  [-peak_limit limit]
  [-em_limits_file file]
  [-em_report report_file]
}

proc check_signal_em { args } {
  sta::parse_key_args "check_signal_em" args \
    keys {-corner -supply_voltage -toggle_rate -activity_file -avg_limit \
          -rms_limit -peak_limit -em_limits_file -em_report} \
    flags {}

  set supply_voltage 0.0
  if { [info exists keys(-supply_voltage)] } {
    set supply_voltage $keys(-supply_voltage)
    sta::check_positive_float "-supply_voltage" $supply_voltage
  }

  set toggle_rate 0.0
  if { [info exists keys(-toggle_rate)] } {
    set toggle_rate $keys(-toggle_rate)
    sta::check_positive_float "-toggle_rate" $toggle_rate
  }

  set activity_file ""
  if { [info exists keys(-activity_file)] } {
    set activity_file $keys(-activity_file)
  }

  set avg_limit 0.0
  if { [info exists keys(-avg_limit)] } {
    set avg_limit $keys(-avg_limit)
    sta::check_positive_float "-avg_limit" $avg_limit
  }

  set rms_limit 0.0
  if { [info exists keys(-rms_limit)] } {
    set rms_limit $keys(-rms_limit)
    sta::check_positive_float "-rms_limit" $rms_limit
  }

  set peak_limit 0.0
  if { [info exists keys(-peak_limit)] } {
    set peak_limit $keys(-peak_limit)
    sta::check_positive_float "-peak_limit" $peak_limit
  }

  set limits_file ""
  if { [info exists keys(-em_limits_file)] } {
    set limits_file $keys(-em_limits_file)
  }

  set report_file ""
  if { [info exists keys(-em_report)] } {
    set report_file $keys(-em_report)
  }

  return [psm::check_signal_em_cmd \
    [sta::parse_scene_or_default keys] \
    $supply_voltage \
    $toggle_rate \
    $activity_file \
    $avg_limit \
    $rms_limit \
    $peak_limit \
    $limits_file \
    $report_file]
}

sta::define_cmd_args "insert_decap" { -target_cap target_cap\
                                      -cells cell_info\
                                      [-net net_name]\
                                    }

proc insert_decap { args } {
  sta::parse_key_args "insert_decap" args \
    keys {-target_cap -cells -net} flags {}

  set target_cap 0.0
  if { [info exists keys(-target_cap)] } {
    set target_cap $keys(-target_cap)
    sta::check_positive_float "-target_cap" $target_cap
    # F/m
    set target_cap [expr [sta::capacitance_ui_sta $target_cap] / [sta::distance_ui_sta 1.0]]
  }

  if { ![info exists keys(-cells)] } {
    utl::error PSM 182 "Missing mandatory argument -cells."
  }

  set cells_and_decap $keys(-cells)

  # Check even size
  if { [llength $cells_and_decap] % 2 != 0 } {
    utl::error PSM 181 "-cells must be a list of cell and decap pairs"
  }

  # Add decap cells
  set db [ord::get_db]
  foreach {cell_name decap} $cells_and_decap {
    set decap_value $decap
    sta::check_positive_float "-cells" $decap_value
    # F/m
    set decap_value [expr [sta::capacitance_ui_sta $decap_value] / [sta::distance_ui_sta 1.0]]
    # Find master with cell_name
    set matched 0
    foreach lib [$db getLibs] {
      foreach master [$lib getMasters] {
        set master_name [$master getConstName]
        if { [string match $cell_name $master_name] } {
          psm::add_decap_master $master $decap_value
          set matched 1
        }
      }
    }
    if { !$matched } {
      utl::warn "PSM" 280 "$cell_name did not match any masters."
    }
  }
  # Get net name
  set net_name ""
  if { [info exists keys(-net)] } {
    set net_name $keys(-net)
  }

  # Insert decap cells
  psm::insert_decap_cmd $target_cap $net_name
}

sta::define_cmd_args "write_pg_spice" {
  -net net_name
  [-vsrc vsrc_file]
  [-corner corner]
  [-source_type FULL|BUMPS|STRAPS]
  spice_file
  }

proc write_pg_spice { args } {
  sta::parse_key_args "write_pg_spice" args \
    keys {-vsrc -net -corner -source_type} flags {}
  sta::check_argc_eq1 "write_pg_spice" $args

  if { ![info exists keys(-net)] } {
    utl::error PSM 59 "Argument -net not specified."
  }

  set voltage_source_file ""
  if { [info exists keys(-vsrc)] } {
    set voltage_source_file $keys(-vsrc)
  }

  set source_type "BUMPS"
  if { [info exists keys(-source_type)] } {
    set source_type $keys(-source_type)
  }

  psm::write_spice_file_cmd \
    [psm::find_net $keys(-net)] \
    [sta::parse_scene_or_default keys] \
    $source_type \
    [lindex $args 0] \
    $voltage_source_file
}

sta::define_cmd_args "set_pdnsim_net_voltage" {
  -net net_name
  -voltage volt
  [-corner corner]}

proc set_pdnsim_net_voltage { args } {
  sta::parse_key_args "set_pdnsim_net_voltage" args \
    keys {-net -corner -voltage} flags {}
  if { [info exists keys(-net)] && [info exists keys(-voltage)] } {
    set net [psm::find_net $keys(-net)]
    set voltage $keys(-voltage)
    set corner [sta::parse_scene_or_default keys]
    psm::set_net_voltage_cmd $net $corner $voltage
  } else {
    utl::error PSM 62 "Argument -net or -voltage not specified.\
      Please specify both -net and -voltage arguments."
  }
}

sta::define_cmd_args "set_pdnsim_inst_power" {
  -inst instance
  -power power
  [-corner corner]}

proc set_pdnsim_inst_power { args } {
  sta::parse_key_args "set_pdnsim_inst_power" args \
    keys {-inst -corner -power} flags {}
  if { [info exists keys(-inst)] && [info exists keys(-power)] } {
    set inst [psm::find_inst $keys(-inst)]
    set power $keys(-power)
    set corner [sta::parse_scene_or_default keys]
    psm::set_inst_power $inst $corner $power
  } else {
    utl::error PSM 63 "Argument -inst or -power not specified.\
      Please specify both -inst and -power arguments."
  }
}

sta::define_cmd_args "set_pdnsim_source_settings" {
  [-bump_dx pitch]
  [-bump_dy pitch]
  [-bump_size size]
  [-bump_interval interval]
  [-strap_track_pitch pitch]
  [-external_resistance resistance]
}

proc set_pdnsim_source_settings { args } {
  sta::parse_key_args "set_pdnsim_source_settings" args \
    keys {-bump_dx -bump_dy -bump_size -bump_interval -strap_track_pitch -external_resistance} \
    flags {}

  set dx 0
  if { [info exists keys(-bump_dx)] } {
    set dx $keys(-bump_dx)
  }
  set dy 0
  if { [info exists keys(-bump_dy)] } {
    set dy $keys(-bump_dy)
  }
  set size 0
  if { [info exists keys(-bump_size)] } {
    set size $keys(-bump_size)
  }
  set interval 0
  if { [info exists keys(-bump_interval)] } {
    set interval $keys(-bump_interval)
  }

  set track_pitch 0
  if { [info exists keys(-strap_track_pitch)] } {
    set track_pitch $keys(-strap_track_pitch)
  }

  set resistance 0
  if { [info exists keys(-external_resistance)] } {
    set resistance $keys(-external_resistance)
  }

  psm::set_source_settings $dx $dy $size $interval $track_pitch $resistance
}

sta::define_cmd_args "size_pdn_for_droop" {
  -net net_name
  -target_droop droop
  -current_width width
  [-irreducible_droop droop]
  [-min_width width]
  [-max_width width]
}

# Analysis-driven strap sizing.  Reads the worst static IR drop the most recent
# analyze_power_grid measured on -net, and reports the strap width that would
# hold the droop to -target_droop given the irreducible package/bump floor.
# This is ADVISORY: it computes the required geometry from the real measured
# droop, it does not regenerate the grid.
proc size_pdn_for_droop { args } {
  sta::parse_key_args "size_pdn_for_droop" args \
    keys {-net -target_droop -current_width -irreducible_droop \
      -min_width -max_width} flags {}

  if { ![info exists keys(-net)] } {
    utl::error PSM 190 "Argument -net not specified."
  }
  if { ![info exists keys(-target_droop)] } {
    utl::error PSM 191 "Argument -target_droop not specified."
  }
  if { ![info exists keys(-current_width)] } {
    utl::error PSM 192 "Argument -current_width not specified."
  }

  set net [psm::find_net $keys(-net)]
  set target [sta::voltage_ui_sta $keys(-target_droop)]
  set cur_w [sta::distance_ui_sta $keys(-current_width)]

  set irr 0.0
  if { [info exists keys(-irreducible_droop)] } {
    set irr [sta::voltage_ui_sta $keys(-irreducible_droop)]
  }
  set min_w 0.0
  if { [info exists keys(-min_width)] } {
    set min_w [sta::distance_ui_sta $keys(-min_width)]
  }
  set max_w 1.0e6
  if { [info exists keys(-max_width)] } {
    set max_w [sta::distance_ui_sta $keys(-max_width)]
  }

  set measured [psm::get_worst_ir_drop_cmd $net]
  if { $measured <= 0.0 } {
    utl::error PSM 193 "No IR drop data for net [$net getName]. Run\
      analyze_power_grid before size_pdn_for_droop."
  }

  set req_w [psm::size_pdn_required_width_cmd \
    $measured $cur_w $irr $target $min_w $max_w]
  set ach [psm::size_pdn_achieved_droop_cmd \
    $measured $cur_w $irr $target $min_w $max_w]

  utl::report [format "Measured worst droop  : %.3e V" $measured]
  utl::report [format "Target droop          : %.3e V" $target]
  utl::report [format "Irreducible floor     : %.3e V" $irr]
  utl::report [format "Current strap width   : %.3e m" $cur_w]

  if { $req_w > $max_w } {
    # req_w is +Inf (below the package floor) or exceeds the width window.
    utl::report "Sizing verdict        : INFEASIBLE"
    if { $target <= $irr } {
      utl::report "Reason                : target at or below the irreducible\
        package/bump floor; on-die widening cannot meet it"
    } else {
      utl::report [format "Reason                : required width %.3e m\
        exceeds max width %.3e m" $req_w $max_w]
    }
    return
  }

  utl::report [format "Required strap width  : %.3e m" $req_w]
  utl::report [format "Predicted droop       : %.3e V" $ach]
  utl::report "Sizing verdict        : FEASIBLE"
}

sta::define_cmd_args "size_decap_for_droop" {
  -peak_current current
  -event_duration time
  -resistance res
  -target_droop droop
}
# NOTE: all four values are in the active OpenROAD UI units (current, time,
# resistance, voltage), the same convention every other PSM/STA command uses.

# Droop-driven decap sizing.  Reports the decoupling capacitance that holds a
# switching event's droop to -target_droop, together with the conservative
# charge bound.  Advisory: reports the required capacitance, does not place it.
proc size_decap_for_droop { args } {
  sta::parse_key_args "size_decap_for_droop" args \
    keys {-peak_current -event_duration -resistance -target_droop} flags {}

  foreach k {-peak_current -event_duration -resistance -target_droop} {
    if { ![info exists keys($k)] } {
      utl::error PSM 194 "Argument $k not specified."
    }
  }

  set i [sta::current_ui_sta $keys(-peak_current)]
  set t [sta::time_ui_sta $keys(-event_duration)]
  set r [sta::resistance_ui_sta $keys(-resistance)]
  set d [sta::voltage_ui_sta $keys(-target_droop)]

  set cap [psm::size_decap_required_cap_cmd $i $t $r $d]
  set bound [psm::decap_charge_bound_cmd $i $t $d]
  set dc [expr { $i * $r }]

  utl::report [format "Event current         : %.3e A" $i]
  utl::report [format "Event duration        : %.3e s" $t]
  utl::report [format "Effective resistance  : %.3e ohm" $r]
  utl::report [format "DC droop (no decap)   : %.3e V" $dc]
  utl::report [format "Target droop          : %.3e V" $d]

  if { $d <= 0.0 } {
    utl::report "Decap verdict         : INFEASIBLE (non-positive budget)"
    return
  }
  if { $d >= $dc } {
    utl::report "Required decap        : 0 F (DC droop already meets budget)"
    utl::report "Decap verdict         : FEASIBLE"
    return
  }

  utl::report [format "Required decap        : %.3e F" $cap]
  utl::report [format "Charge bound (I*T/D)  : %.3e F" $bound]
  utl::report "Decap verdict         : FEASIBLE"
}

namespace eval psm {
proc find_net { net_name } {
  set net [[ord::get_db_block] findNet $net_name]
  if { $net == "NULL" } {
    utl::error PSM 28 "Cannot find net $net_name in the design."
  }
  return $net
}

proc find_inst { inst_name } {
  set inst [[ord::get_db_block] findInst $inst_name]
  if { $inst == "NULL" } {
    utl::error PSM 29 "Cannot find instance $inst_name in the design."
  }
  return $inst
}
}
