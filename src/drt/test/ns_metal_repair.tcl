# The post-route NS-Metal repair CLEARS a real neck.
#
# Two same-net met5 rectangles, each 2.5 x 2.5 um (well over met5's MINWIDTH of
# 1.6 um), overlapping ONLY at a 0.1 x 0.1 um corner. That corner is the sole
# path between the two bulk regions, so the metal is connected but through a
# neck 16x below what the layer can print -- the same shape vibeic-eda#153
# measured on gf180mcuD (80 x 110 dbu against MINWIDTH 460) reduced to the
# smallest DEF that reproduces it.
#
# `repair_ns_metal` runs the production patchNonSufficientMetalViolations()
# against the loaded design and reports before/patched/after as DRT-0704.
# `after` is measured by the CHECKER, not by the repair, so a pass that claims
# success while clearing nothing -- or one that manufactures new markers where
# it lands -- fails this file rather than passing it.
#
# The repair must extend a FULL MINWIDTH beyond the junction on every side.
# A minWidth x minWidth patch CENTRED on the neck (the v2 attempt recorded in
# vibeic/OpenROAD#13) overlaps each neighbour by only 0.85 um, which fails the
# same-net bridging test x^2 + y^2 >= minWidth^2 and leaves the marker standing
# while adding two more. This test goes red for that regression.
source "helpers.tcl"
read_lef sky130hd/sky130hd.tlef
read_def ns_metal_repair.def
set drc_file [make_result_file ns_metal_repair.drc]
drt::repair_ns_metal -output_file $drc_file
diff_files $drc_file ns_metal_repair.drcok
