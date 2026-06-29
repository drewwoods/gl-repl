# Built-In Examples

Built-in examples are authored as REPL scene snippets under `examples/scenes/`
and listed, in flat load order, by `examples/catalog.ini`.

Each catalog entry uses a stable section id plus explicit metadata:

```ini
[lit-cube]
file = scenes/lit-cube.c
name = Lit cube
tags = 3D, Polygons
group = Basics
```

- `name` is the user-visible name returned by `repl_example_name()`.
- `tags` must use the existing labels: `2D`, `3D`, `Polygons`, `Lines`.
  Do not list `All`; it is synthetic.
- `group` is the Scene menu flyout subheading returned by
  `repl_example_subheading()`.
- Section order is the F12 / `--example <idx>` order.

Scene files are plain REPL source lines. They may start with leading
`// @cfg <slug> = <value>` presentation metadata and then an optional
`// camera` block before geometry source. Names, tags, and groups belong in the
catalog, not in scene files.

Run `make check-examples-catalog` after editing the catalog or scene files. The
build generates `build/generated/repl_examples_data.inc` from this directory;
do not edit generated files.

## Example Color Language

The built-in example scenes share one coherent "Dusk" palette so the set reads
as a designed family instead of a grab-bag of saturated primaries. The rollout
began with the Scene menu "2D" tag and now also covers the line/surface 3D
scenes: animated ring, animated wave surface, GLU tessellator plus cutout, and
transform stress. Extend it to the rest of the catalog as examples are touched.

Every covered scene clears to the same deep cool ink and draws geometry from
this curated accent set:

| Role | RGB | Use |
|---|---|---|
| Canvas | `0.05 0.06 0.08` | Deep cool ink, all channels under the clear-color cap |
| CORAL | `0.98 0.46 0.36` | Warm key |
| AMBER | `0.98 0.76 0.36` | Gold highlight / control points |
| ROSE | `0.95 0.44 0.66` | Pink bridge |
| VIOLET | `0.62 0.52 0.95` | Purple bridge / faint guides |
| AZURE | `0.36 0.70 0.98` | Cool key |
| TEAL | `0.30 0.84 0.80` | Aqua secondary |
| MIST | `0.92 0.95 0.98` | Neutral near-white |

Procedural color sweeps should lerp between two anchors, with the signature
ramp being CORAL to AZURE. Flat-colored shape sets should pick distinct palette
members rather than ad-hoc primaries. Keep `glClearColor` channels at or below
`REPL_CLEAR_COLOR_MAX_V` (`0.1f`).
