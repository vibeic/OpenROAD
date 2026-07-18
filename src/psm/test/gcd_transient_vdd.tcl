source helpers.tcl

read_lef Nangate45/Nangate45.lef
read_def Nangate45_data/gcd.def
read_liberty Nangate45/Nangate45_typ.lib
read_sdc Nangate45_data/gcd.sdc

set voltage_file [make_result_file gcd_transient_vdd-voltage.rpt]

check_power_grid -net VDD -dont_require_terminals

# Dynamic (transient) IR-drop: static DC operating point + backward-Euler RC
# time-stepping under the vectorless per-clock triangular current model.  The
# worst per-instance dynamic voltage is written to $voltage_file.
analyze_power_grid -vsrc Vsrc_gcd_vdd.loc -voltage_file $voltage_file -net VDD \
  -transient -period 2 -steps 100 -node_cap 0.001

diff_files $voltage_file gcd_transient_vdd-voltage.rptok
