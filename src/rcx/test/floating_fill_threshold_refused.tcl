source helpers.tcl

read_lef Nangate45/Nangate45.lef
read_liberty Nangate45/Nangate45_typ.lib
read_def floating_fill_single.def
source 45_via_resistance.tcl

define_process_corner -ext_model_index 0 X
set_extraction_rules_file 45_patterns.rules
set status [catch {
  extract_parasitics -max_res 0 -coupling_threshold 0.1
} message]
if {!$status || $message ne "RCX-0540"} {
  error "Nonzero-threshold fill extraction did not refuse with RCX-0540."
}

puts "Unsupported nonzero-threshold fill profile refusal verified."
puts "pass"
