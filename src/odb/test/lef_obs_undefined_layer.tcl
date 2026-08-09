source "helpers.tcl"

# A macro OBS section may reference something the loaded technology cannot
# resolve.  The most common case is a layer of TYPE OVERLAP: LEF defines it as
# a layer type, but a technology LEF is not required to declare a layer of that
# type and many do not.
#
# Such an entry must be skipped on its own.  It must not truncate the OBS
# section, and the geometry that is dropped must be reported.
#
# Each macro below declares 63 rectangles on metalA plus one unresolvable
# entry, placed at a different position in the section:
#
#   blockA  unresolvable LAYER first
#   blockB  unresolvable LAYER in the middle
#   blockC  unresolvable LAYER last
#   blockD  unresolvable VIA in the middle
#
# All four must keep all 63 metalA rectangles.

read_lef "data/obs_undefined_layer_tech.lef"
read_lef "data/obs_undefined_layer_macro.lef"

proc find_master { name } {
  foreach lib [[ord::get_db] getLibs] {
    foreach master [$lib getMasters] {
      if { [$master getName] == $name } {
        return $master
      }
    }
  }
  return "NULL"
}

proc obstruction_count { name } {
  set master [find_master $name]
  if { $master == "NULL" } {
    error "master $name not found"
  }
  return [llength [$master getObstructions]]
}

proc obstruction_layers { name } {
  set master [find_master $name]
  set layers []
  foreach box [$master getObstructions] {
    lappend layers [[$box getTechLayer] getName]
  }
  return [lsort -unique $layers]
}

check "blockA obstruction count" {obstruction_count blockA} 63
check "blockB obstruction count" {obstruction_count blockB} 63
check "blockC obstruction count" {obstruction_count blockC} 63
check "blockD obstruction count" {obstruction_count blockD} 63

check "blockA obstruction layers" {obstruction_layers blockA} metalA
check "blockB obstruction layers" {obstruction_layers blockB} metalA
check "blockC obstruction layers" {obstruction_layers blockC} metalA
check "blockD obstruction layers" {obstruction_layers blockD} metalA

# The pin geometry is on a layer that does resolve and must be untouched.
check "blockA pin geometry" {
  llength [[lindex [[lindex [[find_master blockA] getMTerms] 0] getMPins] 0] getGeometry]
} 1

exit_summary
