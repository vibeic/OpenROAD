# Metal fill

This module inserts floating metal fill shapes to meet metal density
design rules while obeying DRC constraints. It is driven by a `json`
configuration file.

## Commands

```{note}
- Parameters in square brackets `[-param param]` are optional.
- Parameters without square brackets `-param2 param2` are required.
```

### Density Fill

This command performs density fill to meet metal density DRC rules.

```tcl
density_fill
    [-rules rules_file]
    [-area {lx ly ux uy}]
    [-min_density density]
    [-max_density density]
    [-density_window window]
    [-density_step step]
    [-critical_nets nets]
    [-critical_halo halo]
```

#### Options

| Switch Name | Description | 
| ----- | ----- |
| `-rules` | Specify `json` rule file. |
| `-area` | Optional. If not specified, the core area will be used. |
| `-min_density` | Optional. Only fill density windows measuring below this density (0.0-1.0). |
| `-max_density` | Optional. Never let fill push a density window above this density (0.0-1.0). |
| `-density_window` | Required with `-min_density`/`-max_density`. Window edge length in microns. |
| `-density_step` | Optional. Window slide in microns; defaults to `-density_window` (non-overlapping windows). |
| `-critical_nets` | Optional. List of coupling-sensitive nets to hold fill away from. |
| `-critical_halo` | Required with `-critical_nets`. Extra keep-out in microns around those nets. |

Without `-min_density`/`-max_density` every fillable location is filled, which
is the historical behavior. With them, the layer is divided into sliding
windows and only the windows measuring below `-min_density` are filled, with
each fill shape charged against every window it touches so that no window is
driven past `-max_density`.

`-critical_nets` holds fill an extra `-critical_halo` away from the named nets
so that filling cannot load a timing-critical net with sidewall capacitance.
The halo is applied on top of the rule file's `space_to_non_fill`.

### Check Metal Density

Reports the measured metal density of each sliding window and returns the
number of windows falling outside `[-min_density, -max_density]`. Routing,
special wires, instance shapes and existing fill all count as metal.

```tcl
check_metal_density
    -window window
    [-step step]
    [-area {lx ly ux uy}]
    [-min_density density]
    [-max_density density]
    [-layer layer]
```

#### Options

| Switch Name | Description | 
| ----- | ----- |
| `-window` | Window edge length in microns. |
| `-step` | Optional. Window slide in microns; defaults to `-window`. |
| `-area` | Optional. If not specified, the core area will be used. |
| `-min_density` | Optional. Lower bound, default `0.0`. |
| `-max_density` | Optional. Upper bound, default `1.0`. |
| `-layer` | Optional. Restrict the check to one layer; default is every routing layer. |

Only windows lying entirely inside the area are evaluated, so a window never
reports an artificially low density from hanging off the die edge.

## Example scripts

The rules `json` file controls fill and you can see an example
[here](https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts/blob/master/flow/platforms/sky130hd/fill.json).

The schema for the `json` is:

```json
{
  "layers": {
    "<group_name>": {
      "layers": "<list of integer gds layers>",
      "names": "<list of name strings>",
      "opc": {
        "datatype":  "<list of integer gds datatypes>",
        "width":   "<list of widths in microns>",
        "height":   "<list of heightsin microns>",
        "space_to_fill": "<real: spacing between fills in microns>",
        "space_to_non_fill": "<real: spacing to non-fill shapes in microns>",
        "space_line_end": "<real: spacing to end of line in microns>"
      },
      "non-opc": {
        "datatype":  "<list of integer gds datatypes>",
        "width":   "<list of widths in microns>",
        "height":   "<list of heightsin microns>",
        "space_to_fill": "<real: spacing between fills in microns>",
        "space_to_non_fill": "<real: spacing to non-fill shapes in microns>"
      }
    }, ...
  }
}
```

The `opc` section is optional depending on your process.

The width/height lists are effectively parallel arrays of shapes to try
in left to right order (generally larger to smaller).

The layer grouping is for convenience. For example in some technologies many
layers have similar rules so it is convenient to have a `Mx`, `Cx` group.

This all started out in `klayout` so there are some obsolete fields that the
parser accepts but ignores (e.g., `space_to_outline`).

## Regression tests

There are a set of regression tests in `./test`. For more information, refer to this [section](../../README.md#regression-tests). 

Simply run the following script: 

```shell
./test/regression
```

## Limitations

## FAQs

Check out [GitHub discussion](https://github.com/The-OpenROAD-Project/OpenROAD/discussions/categories/q-a?discussions_q=category%3AQ%26A+metal%20fill+in%3Atitle)
about this tool.

## License

BSD 3-Clause License. See [LICENSE](../../LICENSE) file.
