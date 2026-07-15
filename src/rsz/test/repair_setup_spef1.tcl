# repair_timing -setup on POST-DETAILED-ROUTE real SPEF parasitics (kDetailedRouting).
#
# Locks vibeic fork fix cf06074139 ("rsz/est: enable post-detailed-route repair on
# annotated parasitics"). When the resizer repairs a design whose parasitics come
# from a routed DEF + read_spef (ParasiticsSrc::kDetailedRouting) there is NO live
# GlobalRouter route map. Stock OpenROAD routes kDetailedRouting through
# makeBufferedNetGroute(), which derefs that absent map inside
# GlobalRouter::getPinGridPositions() -> Signal 11. The fork builds the buffer tree
# with the placement-Steiner topology instead (RC/violation still from the real SPEF).
#
# Self-checking: if the resizer segfaults the marker line never prints and the
# process exits non-zero; if repair worsens slack the test errors.
source "helpers.tcl"
read_lef Nangate45/Nangate45.lef
read_liberty Nangate45/Nangate45_typ.lib
read_def repair_setup_spef1.def
create_clock -period 0.45 [get_ports clk]
source Nangate45/Nangate45.rc
set_wire_rc -signal -layer metal3
set_wire_rc -clock -layer metal5
# Real detailed-route parasitics extracted by OpenRCX on the routed DEF.
read_spef repair_setup_spef1.spef
# Fork flag: trust the annotated detailed-route RC (locks the EstimateParasitics half).
estimate_parasitics -detailed_routing
set pre [sta::worst_slack -max]
if { $pre >= 0.0 } {
  error "test setup error: expected a pre-repair setup violation, got slack $pre"
}
# Buffer-only sequence isolates the makeBufferedNet dispatch that cf06074139 fixes.
repair_timing -setup -sequence "buffer"
set post [sta::worst_slack -max]
if { $post < $pre } {
  error "regression: post-SPEF repair_timing -setup worsened slack $pre -> $post"
}
puts "POST_SPEF_REPAIR_SETUP no_crash improved"
