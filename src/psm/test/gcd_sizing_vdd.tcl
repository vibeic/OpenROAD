source helpers.tcl

# Real-DB gate for the analysis-driven PDN sizing / decap-sizing commands.
#
# This drives size_pdn_for_droop off the worst static IR drop PSM ACTUALLY
# measures on the Nangate45 gcd design (not an assumed number), and exercises
# the three behaviours a correct sizer must show and a broken one cannot:
#   1. droop too high  -> FEASIBLE, strap must be WIDER than it is now,
#   2. target below the package floor -> INFEASIBLE (the naive
#      "scale width by measured/target" law would wrongly say FEASIBLE here),
#   3. target already met -> FEASIBLE, strap need not be widened.
# size_decap_for_droop is checked against hand-computable capacitances.

read_lef Nangate45/Nangate45.lef
read_def Nangate45_data/gcd.def
read_liberty Nangate45/Nangate45_typ.lib
read_sdc Nangate45_data/gcd.sdc

check_power_grid -net VDD -dont_require_terminals
analyze_power_grid -vsrc Vsrc_gcd_vdd.loc -net VDD

# --- PD2: strap sizing driven by the real measured droop -------------------
puts "### sizing: droop too high, must widen ###"
size_pdn_for_droop -net VDD -target_droop 2.5e-4 -current_width 0.2 \
  -irreducible_droop 1e-6

puts "### sizing: target below package floor, must be infeasible ###"
size_pdn_for_droop -net VDD -target_droop 2.0e-4 -current_width 0.2 \
  -irreducible_droop 3.0e-4

puts "### sizing: target already met, no widening ###"
size_pdn_for_droop -net VDD -target_droop 1.0e-3 -current_width 0.2 \
  -irreducible_droop 1e-6

# --- PD4: decap sizing with hand-computable numbers ------------------------
# UI units: current in mA, time in ns, resistance in kohm, voltage in V.
#   I = 40 mA, T = 0.2 ns, R = 0.002 kohm (2 ohm) -> DC droop I*R = 0.08 V.
#   Budget 0.04 V (half of DC droop):
#     C = T / (R * ln(I*R/(I*R-D))) = 2e-10 / (2*ln2) = 144.2695 pF,
#     charge bound I*T/D = 0.04*2e-10/0.04 = 200 pF.
puts "### decap: budget below DC droop, finite decap ###"
size_decap_for_droop -peak_current 40 -event_duration 0.2 \
  -resistance 0.002 -target_droop 0.04

puts "### decap: budget above DC droop, none needed ###"
size_decap_for_droop -peak_current 40 -event_duration 0.2 \
  -resistance 0.002 -target_droop 0.1
