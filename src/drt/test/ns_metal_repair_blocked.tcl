# The post-route NS-Metal repair REFUSES a neck it cannot widen, and says so.
#
# Same 0.1 x 0.1 um same-net corner neck as ns_metal_repair, but placed 1.55 um
# from the die corner. The repair needs a full MINWIDTH (1.6 um) of room on
# every side; on the two low sides only 1.55 um exists. Growing anyway would
# push metal outside the die, so the pass leaves the junction REPORTED and
# counts it unresolved in DRT-0703.
#
# This is the direction that matters most. A repair pass that always reports
# success is the defect one level up, and the marker this file keeps is a REAL
# geometric neck -- the honest outcome is to report it, never to loosen the
# checker until the count reaches zero. DRT-0704 records before=1 patched=0
# after=1 and the surviving marker is diffed, so "repaired it away" and
# "reported it" cannot be confused.
source "helpers.tcl"
read_lef sky130hd/sky130hd.tlef
read_def ns_metal_repair_blocked.def
set drc_file [make_result_file ns_metal_repair_blocked.drc]
drt::repair_ns_metal -output_file $drc_file
diff_files $drc_file ns_metal_repair_blocked.drcok
