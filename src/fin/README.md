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

Measures per-window, per-layer metal density and flags every window outside the
per-layer `[min, max]` band. This is the signoff half of the fill flow: it is
the measurement that decides whether fill is needed at all, whether the fill
that ran was enough, and whether it overshot.

The measured quantity is the UNION area of all metal on the layer — signal
routing, special (power) routing, instance pins and OBS, and any dummy fill
already inserted — clipped to the window, so overlapping shapes are counted
once. That is the same quantity a foundry density deck measures.

```tcl
check_metal_density
    -window window
    [-step step]
    [-min_density density]
    [-max_density density]
    [-limits_file file]
    [-report_file file]
    [-area {lx ly ux uy}]
```

#### Options

| Switch Name | Description | 
| ----- | ----- |
| `-window` | Window edge length in microns. |
| `-step` | Optional. Window slide in microns; a non-positive step means `step = window` (tiled, no overlap). |
| `-min_density` | Optional. Default lower bound as a fraction in `[0,1]`. |
| `-max_density` | Optional. Default upper bound as a fraction in `[0,1]`. |
| `-limits_file` | Optional. Per-layer bands, which override the defaults for the layers they name. |
| `-report_file` | Optional. Write the per-window report to this file. |
| `-area` | Optional. If not specified, the core area will be used. |

The band is foundry data. Where none is supplied the check reports every window
as `NO_LIMIT` and cannot fail — it will not manufacture a verdict it has no
data for. Bounds are inclusive. An edge window is clipped to the check area and
measured against its own smaller area, so an under-dense strip at the die edge
is still caught rather than silently dropped; a window that clips away entirely
is skipped, never flagged.

### How the two halves fit together

`density_fill` and `check_metal_density` share ONE measurement core
(`fin/density_check.h` plus `DensityCheck`). When `density_fill` is given a
density target it runs that same core to decide which windows are short and how
much room each has left, so the windows the fill tops up are exactly the windows
the check later judges. They cannot disagree about what a window is or what it
contains.

The division of labour:

| | `check_metal_density` | `density_fill` |
| ----- | ----- | ----- |
| Role | signoff — measure and judge | actuator — make it compliant |
| Answers | is this design within the band? | fill the short windows, without overshooting |
| Needs a band? | yes, or it reports `NO_LIMIT` | only if you pass a density target |

#### The cap is guaranteed on the window grid you filled with

`-max_density` is enforced against the windows the fill was driven over, and
only those. A window that exists only at some *other* offset was never in the
budget, so it can exceed the cap. This is measurable on the fixture in
`density_geometry_mismatch`: fill driven at window/step 100/100 measures a peak
of **0.399998** and PASSES its 0.40 cap on that grid, and the very same design
measured at 100/50 — where a window straddles two separately-budgeted regions —
peaks at **0.400200** and FAILS.

That is not the budget misbehaving; it is what a per-window budget can promise.

It has a sharper consequence for how you sign off. `DensityBudget` rejects any
shape that would push a budgeted window past the cap, so after the fill every
budgeted window is within the cap **by construction**. Re-checking those same
windows against that same cap therefore *cannot* fail — it reads the constraint
back rather than testing it. **A check on the grid the fill used is not an
independent signoff.** `check_metal_density` says so (FIN-0053) when it detects
that case.

The practical rules:

- **Sign off at a step finer than you filled with.** Only a finer or offset grid
  reaches the windows that straddle two separately-budgeted regions — the ones
  that can actually be over the cap.
- If you must sign off at step S, drive fill at a step no coarser than S.

Divergent geometry is therefore the *useful* case, not the error case, which is
why FIN-0052 is a warning and not a hard failure: making divergence an error
would push callers toward the same-grid check, i.e. toward the tautological one.

Note this is the DEF-stage pair. It is complementary to, not a replacement for,
a post-streamout GDS density pass: fill inserted here is visible to routing and
extraction, which is precisely why it has to be bounded by `-max_density`.

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
