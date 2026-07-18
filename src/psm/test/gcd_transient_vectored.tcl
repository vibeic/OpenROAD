source helpers.tcl

read_lef Nangate45/Nangate45.lef
read_def Nangate45_data/gcd.def
read_liberty Nangate45/Nangate45_typ.lib
read_sdc Nangate45_data/gcd.sdc

set voltage_file [make_result_file gcd_transient_vectored-voltage.rpt]

# Per-instance VECTORED profile (as a VCD/SAIF derivation would produce): each
# instance gets its own switching phase so switching is no longer assumed
# simultaneous, plus an activity weight and a duty.  The columns are
#   <instance> <activity_scale> <phase_center> <duty>.
set prof [make_result_file gcd_vectored_profile.txt]
set fh [open $prof w]
puts $fh "# instance activity_scale phase_center duty"
puts $fh "_440_ 1.5 0.10 0.30"
puts $fh "_441_ 1.2 0.30 0.30"
puts $fh "_442_ 0.8 0.55 0.30"
puts $fh "_443_ 1.0 0.75 0.30"
puts $fh "_444_ 0.9 0.90 0.30"
close $fh

check_power_grid -net VDD -dont_require_terminals

# Dynamic IR-drop with the vectored per-instance current model AND a lumped
# package/board series R + L, so the report includes the di/dt inductive-droop
# ("first droop") term on top of the resistive drop.
analyze_power_grid -vsrc Vsrc_gcd_vdd.loc -voltage_file $voltage_file -net VDD \
  -transient -period 2 -steps 100 -node_cap 0.001 \
  -vectored_profile $prof -package_r 0.1 -package_l 2e-10

diff_files $voltage_file gcd_transient_vectored-voltage.rptok
