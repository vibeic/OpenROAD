# A pad strap shorter than it is wide must still be built.
#
# gf180mcu_fd_io__dvss faces the core with 1.0 um deep Metal2 pins, and
# LibreLane's gf180mcuD PDN puts the Metal4/Metal5 core ring (1.6 um wide,
# 1.7 um apart, 6 um off the core) close to the pad ring. From a WEST pad the
# strap to the VSS ring is then 8.82 um long and 9.5 um wide.
#
# Shape::cut read the strap's direction from its aspect ratio, so this strap
# counted as vertical: the pad's own obstruction at its west end was
# stretched across its whole length and every strap was cut away. With the
# only ground pad on that edge, the core's whole VSS grid had no source
# (vibe-ic subservient: PSM-0069 on VSS, 1,697,622 unconnected shapes).
#
# Here the ring is equally close on every edge, so the same cell on the SOUTH
# edge gets the same short strap (vertical, 9.5 um wide) and lost it too.
# Control: gf180mcu_fd_io__dvdd on the west edge has 4.655 um deep pins, its
# strap is longer than it is wide, and it connected before and after.
# Every pad must get its six straps.
source "helpers.tcl"

read_lef gf180/gf180mcu_6LM_1TM_9K_7t_tech.lef
read_lef gf180/gf180mcu_fd_io.lef

read_def pads_short_strap.def

set_voltage_domain -name {CORE} -power {VDD} -ground {VSS}

define_pdn_grid \
  -name stdcell_grid \
  -starts_with POWER \
  -voltage_domains {CORE}

add_pdn_ring \
  -grid stdcell_grid \
  -layers "Metal4 Metal5" \
  -widths "1.6 1.6" \
  -spacings "1.7 1.7" \
  -core_offsets "6 6" \
  -allow_out_of_die \
  -connect_to_pads

add_pdn_connect -grid stdcell_grid -layers "Metal4 Metal5"
add_pdn_connect -grid stdcell_grid -layers "Metal2 Metal5"
add_pdn_connect -grid stdcell_grid -layers "Metal2 Metal4"

pdngen

# Count the Metal2 straps that touch each pad.
set block [ord::get_db_block]
foreach {inst_name net_name} {u_vss_w VSS u_vss_s VSS u_vdd_w VDD} {
  set bbox [[$block findInst $inst_name] getBBox]
  set straps 0
  foreach swire [[$block findNet $net_name] getSWires] {
    foreach box [$swire getWires] {
      if { [$box isVia] || [[$box getTechLayer] getName] != "Metal2" } {
        continue
      }
      if { [$box xMax] >= [$bbox xMin] && [$box xMin] <= [$bbox xMax]
           && [$box yMax] >= [$bbox yMin] && [$box yMin] <= [$bbox yMax] } {
        incr straps
      }
    }
  }
  puts "$inst_name $net_name straps: $straps"
  if { $straps != 6 } {
    error "$inst_name $net_name requires all six pad straps, got $straps"
  }
}

set def_file [make_result_file pads_short_strap.def]
write_def $def_file
diff_files pads_short_strap.defok $def_file
