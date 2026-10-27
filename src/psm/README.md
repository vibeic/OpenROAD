# IR Drop Analysis

The IR Drop Analysis module in OpenROAD (`psm`) is based on PDNSim,
an open-source static IR analyzer.

Features:

- Report worst IR drop.
- Report worst current density over all nodes and wire segments in the
  power distribution network, given a placed and PDN-synthesized design.
- Check for floating PDN stripes on the power and ground nets.
- Spice netlist writer for power distribution network wire segments.

![picorv32](doc/picorv32.png)

## Commands

```{note}
- Parameters in square brackets `[-param param]` are optional.
- Parameters without square brackets `-param2 param2` are required.
```

### Analyze Power Grid

This command analyzes power grid for 2D and 3D chips.

```tcl
analyze_power_grid
    -net net_name
    [-corner corner]
    [-error_file error_file]
    [-voltage_file voltage_file]
    [-enable_em]
    [-em_outfile em_file]
    [-vsrc voltage_source_file]
    [-source_type FULL|BUMPS|STRAPS]
    [-allow_reuse]
```

#### Options

| Switch Name | Description |
| ----- | ----- |
| `-net` | Name of the net to analyze, power or ground net name. |
| `-corner` | Corner to use for analysis. |
| `-error_file` | File to write power grid errors to. For 3D ICs, errors are grouped by chiplet. |
| `-vsrc` | File to set the location of the power C4 bumps/IO pins. [Vsrc_aes.loc file](test/Vsrc_aes_vdd.loc) for an example with a description specified [here](doc/Vsrc_description.md). For 3D ICs see the example [here](doc/vsrc_desciption_3d.md). |
| `-enable_em` | Reports maximum and average power-grid segment currents. |
| `-em_outfile` | Write the per-segment current values into a file. This option is only available if used in combination with `-enable_em`. |
| `-voltage_file` | Write per-instance voltage into the file. |
| `-source_type` | Indicate the type of voltage source grid to [model](#source-grid-options). FULL uses all the nodes on the top layer as voltage sources, BUMPS will model a bump grid array, and STRAPS will model power straps on the layer above the top layer. |
| `-allow_reuse` | Allow the analysis to reuse a previous solution, if one exists. |

### Check Power Grid

This command checks power grid.

```tcl
check_power_grid
    -net net_name
    [-floorplanning]
    [-error_file error_file]
    [-dont_require_terminals]
```

#### Options

| Switch Name | Description |
| ----- | ----- |
| `-net` | Name of the net to analyze. Must be a power or ground net name. |
| `-floorplanning` | Ignore non-fixed instances in the power grid, this is useful during floorplanning analysis when instances may not be properly placed. |
| `-error_file` | File to write power grid errors to. For 3D, errors are grouped by chiplet, with coordinates relative to the assembly origin. |
| `-dont_require_terminals` | If specified, this will skip checking if there are terminals on the net. For 3D, this applies to each chiplet. |

### Check Current Density

This command checks the DC current density of every segment of a power or
ground net's grid against per-layer limits (EM signoff). It solves the grid
as `analyze_power_grid` does, judges each wire and via segment, logs the
worst utilization and the verdict (PASS, FAIL, or NOT_MEASURED), and returns
the number of violations.

```tcl
check_current_density
    -net net_name
    [-corner corner]
    [-em_limit limit]
    [-em_limits_file file]
    [-em_report report_file]
    [-vsrc voltage_source_file]
    [-source_type FULL|BUMPS|STRAPS]
    [-allow_reuse]
```

#### Options

| Switch Name | Description |
| ----- | ----- |
| `-net` | Name of the net to check, power or ground net name. |
| `-corner` | Corner to use for analysis. |
| `-em_limit` | Current-density limit in A/um^2 for every layer the limits file does not name. |
| `-em_limits_file` | File of per-layer limits, in the format described under [Limits File](#limits-file). |
| `-em_report` | Write every segment into a CSV file: layer, end points, current, cross-section, J, limit, ratio, status (OK, VIOLATED, NO_AREA, NO_LIMIT), cut count and basis (AREAL, PER_CUT, NO_AREA, NO_LIMIT). |
| `-vsrc` | File to set the location of the power C4 bumps/IO pins, as for `analyze_power_grid`. |
| `-source_type` | Type of voltage source grid to [model](#source-grid-options), as for `analyze_power_grid`. |
| `-allow_reuse` | Allow the check to reuse a previous solution, if one exists. |

A wire is judged on J = I / (width x thickness). The width is that of the
drawn metal carrying the segment, measured across it: the narrowest position
along the segment, each position as wide as the widest drawn shape covering
it. A via is judged on its own cut layer, either per cut or on its total cut
area (see [Limits File](#limits-file)). A segment with no cross-section
(NO_AREA) or no limit for its layer (NO_LIMIT) is counted, listed per layer in
the log and written to the report, and the verdict is then NOT_MEASURED
rather than PASS.

With neither `-em_limit` nor `-em_limits_file`, the limits are taken from the
tech LEF's `DCCURRENTDENSITY AVERAGE` values in the LEF reference units: a
routing layer's value (mA per um of width) is divided by the layer's
`THICKNESS`, and a cut layer's value is mA per um^2 of cut area.

#### Limits File

One limit per line; blank lines and text after `#` are ignored. A line takes
one of two forms:

```
# <layer> <limit in A/um^2>
metal1 1.0e-3
via1 2.0e-3
# <cut layer> per_cut <limit in A per cut>
via2 per_cut 1.5e-4
```

- `<layer> <A/um^2>` is an areal limit. A wire on a routing layer is judged
  on I / (width x thickness); a via on a cut layer is judged on
  I / (total area of its cuts).
- `<cut layer> per_cut <A per cut>` is a per-cut limit, such as a foundry
  maximum current per via. A via on that cut layer is judged on its current
  per cut, I / cuts, using the via's effective cut count, against the limit.
  The per-cut form takes precedence over an areal limit for the same cut
  layer. For such a segment (basis PER_CUT) the J and Jlimit columns of the
  `-em_report` CSV are in A per cut, and the log prints the worst J and its
  limit in `A/cut` when the worst segment is one.

### Check Current Density

This command checks the DC current density of every segment of a power or
ground net's grid against per-layer limits (EM signoff). It solves the grid
as `analyze_power_grid` does, judges each wire and via segment, logs the
worst utilization and the verdict (PASS, FAIL, or NOT_MEASURED), and returns
the number of violations.

```tcl
check_current_density
    -net net_name
    [-corner corner]
    [-em_limit limit]
    [-em_limits_file file]
    [-em_report report_file]
    [-vsrc voltage_source_file]
    [-source_type FULL|BUMPS|STRAPS]
    [-allow_reuse]
```

#### Options

| Switch Name | Description |
| ----- | ----- |
| `-net` | Name of the net to check, power or ground net name. |
| `-corner` | Corner to use for analysis. |
| `-em_limit` | Current-density limit in A/um^2 for every layer the limits file does not name. |
| `-em_limits_file` | File of per-layer limits, in the format described under [Limits File](#limits-file). |
| `-em_report` | Write every segment into a CSV file: layer, end points, current, cross-section, J, limit, ratio, status (OK, VIOLATED, NO_AREA, NO_LIMIT), cut count and basis (AREAL, PER_CUT, NO_AREA, NO_LIMIT). |
| `-vsrc` | File to set the location of the power C4 bumps/IO pins, as for `analyze_power_grid`. |
| `-source_type` | Type of voltage source grid to [model](#source-grid-options), as for `analyze_power_grid`. |
| `-allow_reuse` | Allow the check to reuse a previous solution, if one exists. |

A wire is judged on J = I / (width x thickness). The width is that of the
drawn metal carrying the segment, measured across it: the narrowest position
along the segment, each position as wide as the widest drawn shape covering
it. A via is judged on its own cut layer, either per cut or on its total cut
area (see [Limits File](#limits-file)). A segment with no cross-section
(NO_AREA) or no limit for its layer (NO_LIMIT) is counted, listed per layer in
the log and written to the report, and the verdict is then NOT_MEASURED
rather than PASS.

With neither `-em_limit` nor `-em_limits_file`, the limits are taken from the
tech LEF's `DCCURRENTDENSITY AVERAGE` values in the LEF reference units: a
routing layer's value (mA per um of width) is divided by the layer's
`THICKNESS`, and a cut layer's value is mA per um^2 of cut area.

#### Limits File

One limit per line; blank lines and text after `#` are ignored. A line takes
one of two forms:

```
# <layer> <limit in A/um^2>
metal1 1.0e-3
via1 2.0e-3
# <cut layer> per_cut <limit in A per cut>
via2 per_cut 1.5e-4
```

- `<layer> <A/um^2>` is an areal limit. A wire on a routing layer is judged
  on I / (width x thickness); a via on a cut layer is judged on
  I / (total area of its cuts).
- `<cut layer> per_cut <A per cut>` is a per-cut limit, such as a foundry
  maximum current per via. A via on that cut layer is judged on its current
  per cut, I / cuts, using the via's effective cut count, against the limit.
  The per-cut form takes precedence over an areal limit for the same cut
  layer. For such a segment (basis PER_CUT) the J and Jlimit columns of the
  `-em_report` CSV are in A per cut, and the log prints the worst J and its
  limit in `A/cut` when the worst segment is one.

### Write Spice Power Grid

This command writes the `spice` file for power grid.

```tcl
write_pg_spice
    -net net_name
    [-vsrc vsrc_file]
    [-corner corner]
    [-source_type FULL|BUMPS|STRAPS]
    spice_file
```

#### Options

| Switch Name | Description |
| ----- | ----- |
| `-net` | Name of the net to analyze. Must be a power or ground net name. |
| `-vsrc` | File to set the location of the power C4 bumps/IO pins. See [Vsrc_aes.loc file](test/Vsrc_aes_vdd.loc) for an example and its [description](doc/Vsrc_description.md). |
| `-corner` | Corner to use for analysis. |
| `-source_type` | Indicate the type of voltage source grid to [model](#source-grid-options). FULL uses all the nodes on the top layer as voltage sources, BUMPS will model a bump grid array, and STRAPS will model power straps on the layer above the top layer. |
| `spice_file` | File to write spice netlist to. |

### Set PDNSim Net voltage

This command sets PDNSim net voltage. For 3D analysis, define external sources with `analyze_power_grid -vsrc`.

```tcl
set_pdnsim_net_voltage
    -net net_name
    -voltage volt
    [-corner corner]
```

#### Options

| Switch Name | Description |
| ----- | ----- |
| `-net` | Name of the net to analyze. It must be a power or ground net name. |
| `-voltage` | Sets the voltage on a specific net. If this option is not given, the Liberty file's voltage value is obtained from operating conditions. |
| `-corner` | Corner to use this voltage. If not specified, this voltage applies to all corners. |

### Set PSM Inter-chip Resistance

Set the resistance value used for inter-chip bump pair.

```tcl
set_psm_interchip_resistance -resistance resistance [-scene scene]
```

#### Options

| Switch Name | Description |
| ----- | ----- |
| `-resistance` | Resistance in ohms for each aligned inter-chip bump pair. It must be positive. |
| `-scene` | Scene to use this resistance. If omitted, the resistance applies to all defined scenes. |

### Set PDNSim Instance power

This command sets PDNSim instance power.
This should only be used when needing to override the computed power.

```tcl
set_pdnsim_inst_power
    -inst inst_name
    -power power
    [-corner corner]
```

#### Options

| Switch Name | Description |
| ----- | ----- |
| `-inst` | Name of the instance to set the power dor. |
| `-power` | Sets the power on a specific instance. |
| `-corner` | Corner to use this power. If not specified, this power applies to all corners. |

### Set PDNSim Power Source Settings

Set PDNSim power source setting.

```tcl
set_pdnsim_source_settings
    [-bump_dx pitch]
    [-bump_dy pitch]
    [-bump_size size]
    [-bump_interval interval]
    [-strap_track_pitch pitch]
    [-external_resistance resistance]
```

#### Options

| Switch Name | Description |
| ----- | ----- |
| `-bump_dx`,`-bump_dy` | Set the bump pitch to decide the voltage source location. The default bump pitch is 140um. |
| `-bump_size` | Set the bump size. The default bump size is 70um. |
| `-bump_interval` | Set the bump population interval, this is used to depopulate the bump grid to emulate signals and other power connections. The default bump pitch is 3. |
| `-strap_track_pitch` | Sets the track pitch to use for modeling voltage sources as straps. The default is 10x. |
| `-external_resistance` | Set to model the resistance of the package or power network outside the chip/block. The default value is 0.0. |

### Insert Decap Cells
The `insert_decap` command inserts decap cells in the areas with the highest
IR Drop. The number of decap cells inserted will be limited to the target
capacitance defined in the `-target_cap` option. `list_of_decap_with_cap`
is a list of even size of decap master cells and their capacitances,
e.g., `<cell1> <decap_of_cell1> <cell2> <decap_of_cell2> ...`. To insert decap
cells in the IR Drop of a specific net (power or ground) use `-net <net_name>`,
if not defined the default power net will be used.
To use this command, you must first execute the `analyze_power_grid` command
with the net to have the IR Drop information.

```tcl
insert_decap -target_cap target_cap [-net net_name] -cells list_of_decap_with_cap
```

#### Options
| Switch Name | Description |
| ----- | ----- |
| `-target_cap` | Target capacitance to insert os decap cells. |
| `-net` | Power or ground net name. The decap cells will be inserted near the IR Drops of the net. |
| `-cells` | List of even size of decap master cells and their capacitances. |

## Source grid options

The source grid models how power is going be delivered to the power grid.
The image below illustrate how they can be modeled, the red elements are the source models, the black horizontal boxes represent the top metal layer of the power grid, and the gray boxes indicate these are not used in the modeling.

| Bumps with 2x interval | Bumps with 3x interval | Straps | Full |
| - | - | - | - |
| ![Image 1](doc/top_grid_bumps_2x.png) | ![Image 2](doc/top_grid_bumps_3x.png) | ![Image 1](doc/top_grid_straps.png) | ![Image 2](doc/top_grid_full.png) |

### Selectively disconnecting sources

If you need to be able to disconnect some of the terminals in the design, such as in the case of "what-if" analysis or different chip packaging options.
This can be done by assigning `PSM_DISCONNECT` to a terminal or shape in a terminal will cause PDNSim to leave that object disconnected from the analysis.

<!-- checker: skip -->
```tcl
# Assumes bpin is the block pin to be disconnected
odb::dbBoolProperty_create $bpin PSM_DISCONNECT 1

# Assumes box the box shape in the bpin to be disconnected
odb::dbBoolProperty_create $box PSM_DISCONNECT 1
```

## Useful Developer Commands

If you are a developer, you might find these useful. More details can be found in the [source file](./src/pdnsim.cpp) or the [swig file](./src/pdnsim.i).

| Command Name | Description |
| ----- | ----- |
| `find_net` | Get a reference to net name. |

## Example scripts

Example scripts demonstrating how to run PDNSim on a sample design on `aes` as follows:

```
./test/aes_test_vdd.tcl
./test/aes_test_vss.tcl
```

## Regression tests

There are a set of regression tests in `./test`. For more information, refer to this [section](../../README.md#regression-tests).

Simply run the following command:

```shell
bazelisk test //src/psm/...
```

## Limitations

The following 3D extensions are not currently supported:

- Exporting 3D IR and EM results to JSON.
- GUI IR-drop heatmaps.
- Source generation through `-source_type FULL|BUMPS|STRAPS`.
- SPICE export of the connected chiplet network through `write_pg_spice`.
- Chiplet-aware instance lookup in `set_pdnsim_inst_power`.

## References

1. PDNSIM [documentation](doc/PDNSim-documentation.pdf)
1. Chhabria, V.A. and Sapatnekar, S.S. (no date) The-openroad-project/pdnsim: Power Grid Analysis, GitHub. Available at: https://github.com/The-OpenROAD-Project/PDNSim (Accessed: 24 July 2023). [(link)](https://github.com/The-OpenROAD-Project/PDNSim)

## License

BSD 3-Clause License. See [LICENSE](../../LICENSE) file.
