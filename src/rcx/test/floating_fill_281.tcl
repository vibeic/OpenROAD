source helpers.tcl

read_lef Nangate45/Nangate45.lef
read_liberty Nangate45/Nangate45_typ.lib
read_def floating_fill_281.def
source 45_via_resistance.tcl

set block [ord::get_db_block]
if {[llength [$block getFills]] != 281} {
  error "The selected-scale test requires 281 explicit dbFill rectangles."
}

define_process_corner -ext_model_index 0 X
set_extraction_rules_file 45_patterns.rules
extract_parasitics -max_res 0 -coupling_threshold 0

if {[llength [$block getFills]] != 281} {
  error "Extraction did not preserve the 281 source dbFill rectangles."
}
foreach index {0 140 280} {
  if {[$block findNet "__rcx_floating_fill__${index}"] != "NULL"} {
    error "A temporary floating-fill net escaped extraction."
  }
  if {[$block findNonDefaultRule "__rcx_floating_fill__${index}_ndr"] != "NULL"} {
    error "A temporary floating-fill width rule escaped extraction."
  }
}

set reduced 0.0
foreach cc [$block getCCSegs] {
  set source [[$cc getSourceNet] getName]
  set target [[$cc getTargetNet] getName]
  if {($source == "A" && $target == "B")
      || ($source == "B" && $target == "A")} {
    set reduced [expr {$reduced + [$cc getCapacitance 0]}]
  }
}
if {!($reduced > 0.0)} {
  error "The 281-fill profile produced no effective A-B coupling."
}

puts "Selected-scale floating fill verified: 281 rectangles, A-B=${reduced}fF."
puts "pass"

