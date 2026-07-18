# GP4: ECO global placement that leaves the untouched instances alone.
#
# The scenario is the one an ECO actually produces: a design that is already
# placed, plus a handful of instances that are not.  With -freeze_placed the
# placed ones must come back at exactly the coordinates they went in with,
# not merely close to them.
source helpers.tcl
read_lef ./nangate45.lef
read_def ./incremental01.def

set block [ord::get_db_block]

# The instances standing in for newly added ECO cells.
set eco {_276_ _277_ _278_}

foreach inst [$block getInsts] {
  set before([$inst getName]) [$inst getLocation]
}

foreach name $eco {
  [$block findInst $name] setPlacementStatus UNPLACED
}

global_placement -init_density_penalty 0.1 -incremental -freeze_placed

# POSITIVE GATE: byte-identical coordinates on everything that was frozen.
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
    if { $moved <= 3 } {
      puts "  moved $name: $before($name) -> [$inst getLocation]"
    }
  }
}
if { $moved == 0 } {
  puts "PASS $checked frozen instances kept byte-identical coordinates"
} else {
  puts "FAIL $moved of $checked frozen instances moved"
}

# The freeze must not be achieved by refusing to do the work: the ECO cells
# still have to land somewhere legal inside the core.
set core [$block getCoreArea]
set core_lx [$core xMin]
set core_ly [$core yMin]
set core_ux [$core xMax]
set core_uy [$core yMax]
set placed 0
foreach name $eco {
  set inst [$block findInst $name]
  lassign [$inst getLocation] x y
  # odb renders the unplaced status as NONE.
  if { [$inst getPlacementStatus] == "NONE" } {
    puts "FAIL ECO cell $name left unplaced"
  } elseif { $x >= $core_lx && $x <= $core_ux && $y >= $core_ly && $y <= $core_uy } {
    incr placed
  } else {
    puts "FAIL ECO cell $name placed outside core at $x $y"
  }
}
if { $placed == [llength $eco] } {
  puts "PASS all [llength $eco] ECO cells placed inside the core"
}
