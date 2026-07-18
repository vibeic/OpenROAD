# GP4 proven-negative: the same ECO without -freeze_placed.
#
# Plain -incremental locks the placed instances only for its first pass and
# then unlocks everything, so the design moves.  If this test ever reports no
# movement then eco_freeze.tcl is passing for free and proves nothing.
source helpers.tcl
read_lef ./nangate45.lef
read_def ./incremental01.def

set block [ord::get_db_block]

set eco {_276_ _277_ _278_}

foreach inst [$block getInsts] {
  set before([$inst getName]) [$inst getLocation]
}

foreach name $eco {
  [$block findInst $name] setPlacementStatus UNPLACED
}

global_placement -init_density_penalty 0.1 -incremental

set moved 0
set checked 0
foreach inst [$block getInsts] {
  set name [$inst getName]
  if { [lsearch -exact $eco $name] != -1 } {
    continue
  }
  incr checked
  if { [$inst getLocation] ne $before($name) } {
    incr moved
  }
}
if { $moved > 0 } {
  puts "PASS unfrozen run moved $moved of $checked instances, so the freeze is doing real work"
} else {
  puts "FAIL unfrozen run moved nothing; the freeze gate is vacuous"
}
