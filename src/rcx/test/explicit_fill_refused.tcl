source helpers.tcl

read_lef Nangate45/Nangate45.lef
read_liberty Nangate45/Nangate45_typ.lib
read_def 45_gcd.def
source 45_via_resistance.tcl

define_process_corner -ext_model_index 0 X
set_extraction_rules_file 45_patterns.rules

# Negative control: the unchanged design remains extractable.
extract_parasitics -max_res 0 -coupling_threshold 0.1

density_fill -rules explicit_fill.json
set fill_count [llength [[ord::get_db_block] getFills]]
if { $fill_count == 0 } {
  error "The test did not create explicit dbFill geometry."
}

set status [catch {
  extract_parasitics -max_res 0 -coupling_threshold 0.1
} message]
if { !$status } {
  error "OpenRCX silently accepted $fill_count unsupported explicit fills."
}
if { $message ne "RCX-0540" } {
  error "Unexpected extraction error: $message"
}

puts "Explicit fill refusal verified for $fill_count shapes."
puts "pass"
