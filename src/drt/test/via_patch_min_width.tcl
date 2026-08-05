# A via whose patch on the UPPER routing layer is narrower than that layer's own
# declared WIDTH cannot legally terminate a route on it: where the wire ENDS on
# the via, the part of the patch protruding past the wire end is below min
# width. sky130 M4M5_PR is such a via -- met5 patch 1.42 um, `LAYER met5 ...
# WIDTH 1.6`. The three nets differ ONLY in where the via sits:
#
#   n_end  wire ends on the via   -> 0.56 x 1.42 protrusion -> Min Width
#   n_mid  via mid-wire           -> patch inside the wire  -> clean
#   n_ext  wire flush with patch  -> patch inside the wire  -> clean
#
# which is exactly what the two shipped sky130 sign-off decks say about the same
# three geometries. This pins the min-width check to the MERGED wire-union-via
# polygon; a change that checked the wire and the via patch separately would
# lose n_end and pass this file silently otherwise.
source "helpers.tcl"
read_lef sky130hd/sky130hd.tlef
read_def via_patch_min_width.def
set drc_file [make_result_file via_patch_min_width.drc]
drt::check_drc -output_file $drc_file
diff_files $drc_file via_patch_min_width.drcok
