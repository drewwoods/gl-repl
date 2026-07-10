# Built-In Examples

> For a rendered gallery of these scenes — screenshots, animations, and the
> source behind them — see the [Showcase](../docs/SHOWCASE.md).

Built-in examples are listed, in flat load order, by `examples/catalog.ini`.
Their source files live under `examples/scenes/` and use one of two formats:

- `.glr` for concise REPL scene snippets. This is the default authoring format.
- `.c` for full exported/importable C files. These load through the same import
  path as `./gl-repl output.c`.

Each catalog entry uses a stable section id plus explicit metadata:

```ini
[lit-cube]
file = scenes/lit-cube.glr
name = Lit cube
tags = 3D, Polygons
group = Basics
```

- `name` is the user-visible name returned by [`repl_example_name()`](../src/repl/examples.h#L68).
- `file` must be a relative path under `scenes/` and must end in `.glr` or
  `.c`.
- `tags` must use the existing labels: `2D`, `3D`, `Polygons`, `Lines`.
  Do not list `All`; it is synthetic.
- `group` is the Scene menu flyout subheading returned by
  [`repl_example_subheading()`](../src/repl/examples.h#L109).
- Section order is the F12 / `--example <idx>` order.

`.glr` files are plain REPL source lines. They may start with leading
`// @cfg <slug> = <value>` presentation metadata and then an optional
`// camera` block before geometry source. Names, tags, and groups belong in the
catalog, not in scene files.

`.c` files should be normal exported C files with `// Snippet start` /
`// Snippet end` markers. They are useful when an example is easier to keep as
a complete import fixture than as the shortened REPL form.

Run `make check-examples-catalog` after editing the catalog or scene files. The
build generates `build/generated/repl_examples_data.inc` from this directory;
do not edit generated files.

For live iteration without regenerating or rebuilding, run:

```bash
./gl-repl --examples-dir examples --list-examples
./gl-repl --examples-dir examples --example lit
```

The app still uses the compiled-in generated catalog by default, so app bundles
and installed binaries do not need the repository `examples/` directory at
runtime.

## Authoring notes

No predefined `goto` examples are shipped: `goto` support is partial — top-level
only (flatten rejects labels/gotos inside functions), not replay-safe (replay
intentionally does not model dynamic goto traces), and not suitable for
variable-driven geometry loops. Keep `goto` coverage in tests and docs rather
than in F12 examples.

Keep each scene under the 8192 flattened-command budget; hoist loop-invariant
work out of `for` bodies so a dense example stays well clear of the cap.

## Example Color Language

The built-in example scenes share one coherent "Dusk" palette so the set reads
as a designed family instead of a grab-bag of saturated primaries. The rollout
began with the Scene menu "2D" tag and now also covers the line/surface 3D
scenes: animated ring, animated wave surface, GLU tessellator plus cutout, and
transform stress. Extend it to the rest of the catalog as examples are touched.

Beyond the examples, the palette also anchors the default **XZ Ruler grid
theme** (`grid_ruler_line_color` + the axis/tick colors in
`src/render3d/grid.c`: CORAL X axis, AZURE Z axis, near-neutral field lines)
and every staged snippet object in `scripts/docs-assets.sh` — so the docs
screenshots, the showcase, and a fresh session all read as one family. Keep
those on the palette too when touching them.

Every covered scene clears to the same deep cool ink and draws geometry from
this curated accent set — mid-bright, lightly desaturated, harmonious rather
than garish:

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

Procedural color sweeps lerp between two anchors — `color = A + (B - A) * s`
with `s` in `[0, 1]`. The signature ramp is CORAL → AZURE (warm to cool); a few
scenes use VIOLET → AZURE or an AMBER accent for variety while staying in the
family. Flat-colored discrete sets (triangle trios, transform groups, tess
stops) pick distinct palette members rather than ad-hoc primaries.

`glClearColor` channels are capped at `REPL_CLEAR_COLOR_MAX_V` (`0.1f`) so the
canvas always stays dark; the accents above are full-range `glColor`. When
touching these colors, keep the whole covered set on the palette.
