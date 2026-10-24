# extract_parasitics MUST NOT CHANGE THE ROUTING IT MEASURES (vibeic fork).
#
# Ext::extract() opens with odb::normalizeWireConnectivity(), which rewrites
# every non-supply net's dbWire through WireNormalizer so the RC tree can be
# walked in connectivity order. That rewrite is not geometry-preserving.
# MEASURED on a detail-routed gf180mcuD design, with nothing in the session but
# read_db /
# define_process_corner / extract_parasitics / write_db:
#
#     whole-design DRC before ...... 0 violation(s)
#     whole-design DRC after  ...... 599 violation(s)
#                                    (547 Short, 45 Metal Spacing, 7 NS Metal)
#     dbWire entry count ........... 32,634 -> 17,824
#
# and nothing said so. The flow extracted parasitics for post-route timing
# repair and then shipped the corrupted route.
#
# The measurement here is BYTES and MEMBERSHIP, not a count: which net names
# came back different, read through dbWire's own raw accessors. A count alone
# cannot tell "the same net twice" from "two different nets".
#
# NEGATIVE CONTROL: delete the `RoutingPreserver routing_preserver(...)` line
# from Ext::extract() in src/rcx/src/ext.cpp. Every routed net then reports
# changed. Measured on this testcase before the fix: 350 of 350.
source helpers.tcl

proc wire_signature { net } {
  set wire [$net getWire]
  if { $wire == "NULL" } {
    return "NOWIRE"
  }
  set n [$wire length]
  set sig "len=$n"
  for { set i 0 } { $i < $n } { incr i } {
    append sig " [$wire getOpcode $i]:[$wire getData $i]"
  }
  return $sig
}

proc all_signatures { block } {
  set sigs [dict create]
  foreach net [$block getNets] {
    if { [$net isSpecial] } {
      continue
    }
    dict set sigs [$net getName] [wire_signature $net]
  }
  return $sigs
}

read_lef Nangate45/Nangate45.lef
read_liberty Nangate45/Nangate45_typ.lib
read_def 45_gcd.def
source 45_via_resistance.tcl

set block [ord::get_db_block]
set before [all_signatures $block]
set routed 0
dict for { name sig } $before {
  if { $sig ne "NOWIRE" } {
    incr routed
  }
}
puts "EXTPRES_NETS: [dict size $before]"
puts "EXTPRES_ROUTED_BEFORE: $routed"

define_process_corner -ext_model_index 0 X
set_extraction_rules_file 45_patterns.rules
extract_parasitics -max_res 0 -coupling_threshold 0.1

set after [all_signatures $block]

set changed {}
set lost {}
foreach name [lsort [dict keys $before]] {
  if { ![dict exists $after $name] } {
    lappend lost $name
    continue
  }
  if { [dict get $before $name] ne [dict get $after $name] } {
    lappend changed $name
  }
}
# The load-bearing lines. Anything but 0 means extraction rewrote the caller's
# routing and the design it hands back is not the design it was given.
puts "EXTPRES_CHANGED: [llength $changed] -- [join [lrange $changed 0 7] {, }]"
puts "EXTPRES_LOST: [llength $lost] -- [join [lrange $lost 0 7] {, }]"

# ... and the parasitics are still the parasitics: same SPEF as the untouched
# extraction golden, so preserving the routing costs nothing measurable.
set spef_file [make_result_file extract_preserves_routing.spef]
write_spef $spef_file
diff_files 45_gcd.spefok $spef_file "^\\*(DATE|VERSION)"
