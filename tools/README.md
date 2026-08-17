# `tools/` - standalone demos + developer tooling

Every binary here is built from the repo root Makefile and lands at the repo
root (`./repl_demo`, `./render3d_demo`, ...). Nothing in `tools/` is linked into
`gl-repl` itself.

The demos are not samples: each one is the **executable proof that a module
links without its upper layers**, enforced by a guard in
`make check-state-ownership`. That is why they live outside `src/` - the
isolation guards match on path prefixes (`src/app/`, `src/repl/`, ...), and a
demo TU with its own `main()` inside one of those trees would need a carve-out
in every guard that scans the module.

Where the docs live, so nothing is written twice:

| Layer | Holds |
|-------|-------|
| [`docs/MODULES.md`](../docs/MODULES.md#standalone-demo-binaries-layer-independence-proofs) | Why the demos exist, boundary rules - the authority |
| `src/<module>/README.md` | The module-side narrative (linked per demo below) |
| The demo's own source header | Keys, CLI, what the scene shows |
| This file | The directory index: what's here, what proves what |

Screenshots below are generated assets - see [Regenerating the
screenshots](#regenerating-the-screenshots).

## Boundary demos

The four that define a layer by exclusion. All build `USE_GL_STUBS=1`-clean.

### `render3d_demo` - [`src/render3d/README.md`](../src/render3d/README.md#the-demo-render3d_demo)

<img src="../docs/images/demos/render3d.png" alt="render3d_demo: a lit teapot on the adaptive grid with axes and a camera/theme HUD" width="100%">

Drives [`src/render3d/`](../src/render3d/) with a non-REPL geometry callback
and its own camera + HUD shell. The grid, axes, backdrop, lights and overlays
in that frame are the same code the app runs - proof they carry no REPL
dependency. [`render3d_asset_builder/`](render3d_asset_builder/README.md) holds the
reloadable half of `make render3d-hot`, the dlopen live-reload variant
([`hot reload`](../src/render3d/README.md#hot-reload-make-render3d-hot)).

### `repl_demo` - [`src/repl/README.md`](../src/repl/README.md#the-demo-repl_demo)

No screenshot: `repl_demo` is **headless in every build** - it opens no window
and creates no GL context, which is itself the point. The pipeline runs with
no host at all ([`stubs.c`](repl_demo/stubs.c) is empty, and a ratchet keeps it
that way), backing source lines with its own editor-free
[`source_document.c`](repl_demo/source_document.c). Its picture is
`./repl_demo --trace`, which narrates one program through every stage:

```
 STAGE 2   the source program              repl_state_document_cmds()
------------------------------------------------------------------------
  idx type                 has_vars args
  4   CMD_BEGIN            -        2
  5   CMD_FOR_BEGIN        -
  6   CMD_VERTEX3F         yes      0, 1.5, 0
  7   CMD_FOR_END          -

 STAGE 3   source -> flat program          repl_flatten_commands()
------------------------------------------------------------------------
9 source commands expand to 11 flat commands. The loop body unrolled;
each flat command remembers its source line (src_idx) and a frozen
snapshot of the variables in scope when it was emitted:
```

### `repl_live_demo` - [`README`](repl_live_demo/README.md) | [`src/repl/README.md`](../src/repl/README.md#the-demo-repl_demo)

<img src="../docs/images/demos/repl-live.png" alt="repl_live_demo: a whale scene rendered from a watched .c file, with a 26-row variable slider panel" width="100%">

The *composition* counterpart to `repl_demo`: pipeline + variable-panel peer +
a demo-local [`ReplExportCameraBridge`](../src/repl/export.h#L93), driven by
whatever editor you like over file-watch - and still no app shell. Shown with
the bundled `scenes/whale-full-c.c`, whose 26 declared variables fill the
slider panel; drag a row and the geometry reshapes. It watches `.glr` scene
source as well as `.c` saves, each through the loader the app uses for that
format, so it doubles as a live preview window for authoring a built-in
example.

### `editor_demo` - [`src/editor/README.md`](../src/editor/README.md#the-demo-editor_demo) | [`src/ui/README.md`](../src/ui/README.md#how-it-is-exercised)

<img src="../docs/images/demos/editor.png" alt="editor_demo: a plain-text document with line numbers, a File menu, and the cursor on the last row" width="100%">

The text-document model driven by a *different* controller: this window is
[`EditorState`](../src/editor/state.h#L199) plus `ui/core/text_panel`, with the
demo's own dispatcher ([`input.c`](editor_demo/input.c)) and File menu
([`menu.c`](editor_demo/menu.c)) - which is what makes `src/editor/input.c` the
REPL editor's controller rather than the model's. It links `src/ui/core` but
never `src/ui/app`: no [`UiRenderSnapshot`](../src/ui/app/snapshot.h#L90), no
menu bar, no app chrome.

`src/app` is the one module with no demo, by design - it is what the four
above exclude. See [`src/app/README.md`](../src/app/README.md#how-it-is-exercised--the-inverse-of-the-demos).

## Single-module demos

Each links only its peer + that peer's renderer + `src/ui/core` theme (plus a
neutral `src/support` helper where the peer has one), and is guarded by
`check-subsystem-demo-isolation.sh` (Makefile dep list, demo includes, and an
`nm` sweep for `repl_`/`editor_`/`glr_` symbols).

<img src="../docs/images/demos/variable-panel.png" alt="variable_panel_demo: a torus with a three-row slider panel" width="49%"> <img src="../docs/images/demos/color-picker.png" alt="color_picker_demo: five colored shapes with the floating picker open on the torus" width="49%">

<img src="../docs/images/demos/assign-plot.png" alt="assign_plot_demo: a wave clamped at the program counter, with three assignment rows plotted and their values read out at the PC" width="49%"> <img src="../docs/images/demos/cpuprof.png" alt="cpuprof_demo: fifteen teapots with a compute-profile panel and section histograms" width="49%">

<img src="../docs/images/demos/memprof.png" alt="memprof_demo: a teapot with a memory panel showing RSS, baseline, delta and a rising graph" width="49%">

| Demo | Drives | Module doc |
|------|--------|-----------|
| [`variable_panel_demo/`](variable_panel_demo/) | The variable-panel peer over an in-memory `VariablePanelValueSource` and a hand-built `UiVariablePanelView` - no REPL eval table, no snapshot. | [`src/subsystems/README.md`](../src/subsystems/README.md#how-it-is-exercised) |
| [`color_picker_demo/`](color_picker_demo/) | The color-picker peer over a [`ColorPickerHostBridge`](../src/subsystems/color_picker/color_picker_state.h#L116) backed by a plain color array. The popup anchors left because the demo's bridge reports no code panel. | [`src/subsystems/README.md`](../src/subsystems/README.md#how-it-is-exercised) |
| [`assign_plot_demo/`](assign_plot_demo/) | The value-capture peer + its panel over an [`AssignPlotHostBridge`](../src/subsystems/assign_plot/assign_plot.h) fed by a trace the demo's own three-row loop generates - the curve on screen and the plot are the same numbers. `r` walks a program counter through the frame, which is how the replay PC rules and value readouts run with no replay peer linked. | [`src/subsystems/README.md`](../src/subsystems/README.md#how-it-is-exercised) |
| [`cpuprof_demo/`](cpuprof_demo/) | `prof_*` wall-time sampling, shaped as a display-list micro-benchmark: the same teapots drawn immediate, from a reused list, and from a per-frame recompiled list. | [`src/support/README.md`](../src/support/README.md#how-it-is-exercised) |
| [`memprof_demo/`](memprof_demo/) | `memprof_*` sampling + the live memory panel: current / baseline / delta over a time-anchored graph. | [`src/support/README.md`](../src/support/README.md#how-it-is-exercised) |

## Other

- [`render3d_asset_builder/`](render3d_asset_builder/README.md) - REPL-language
  recreations of render3d elements (grids, backdrops) with their own example
  catalog; needs a raised flat-command budget.
- [`capacity_matrix.c`](capacity_matrix.c) - `make capacity-matrix`; prints the
  per-unit memory cost of every tunable `MAX_*` constant. Hand-curated table:
  add a row when you add a `MAX_*`.
- [`keymap.sh`](keymap.sh) - `make check-keymap-no-dup` / `make keymap-list`
  over [`keymap.h`](../keymap.h).
- [`glprobe/`](glprobe/README.md) — `make glprobe SAMPLE=<file.c>`; a
  `GL_FEEDBACK` geometry probe for the loose fixed-function samples in this
  tree. Captures a draw callback twice — once under a known ortho with lighting
  off, once under the live camera and lights — so "why can't I see it?" splits
  into "is the mesh wrong?" and "is the shading wrong?". Dumps PLY through the
  same pure writer as the app's mesh export. `make glprobe-preload` builds the
  same probe as an `LD_PRELOAD`/`DYLD_INSERT_LIBRARIES` library that hooks
  `glutDisplayFunc` and needs no source change at all. Not linked into any
  demo.

## Build

```bash
make repl-demo USE_GL_STUBS=1   # any demo, headless - no GL dev libs needed
make render3d-demo              # real GL, opens a window
make check-state-ownership      # runs every demo isolation guard
```

A demo that stops linking is the guard doing its job: something in the module
grew a dependency on a layer above it. Fix the dependency, not the demo's
object list.

## Regenerating the screenshots

```bash
make render3d-demo repl-live-demo editor-demo variable-panel-demo \
     color-picker-demo assign-plot-demo cpuprof-demo memprof-demo
scripts/docs-assets.sh --demos          # or one: demo-render3d, demo-memprof, ...
```

Assets land in `docs/images/demos/` (Git LFS, like the rest of `docs/images/`).
The `demo-*` names are their own category in
[`scripts/docs-assets.sh`](../scripts/docs-assets.sh) rather than more
`--pngs`, because they come from these binaries instead of `gl-repl` - so
`--pngs` still needs only `make gl-repl`.

Two things the script has to work around, both documented at the helpers:

- **Frame budget is not a free knob.** Record mode renders exactly N frames and
  exits, which needs the demo to keep asking for frames. `editor_demo` and
  `color_picker_demo` are event-driven, so only `N=1` terminates - anything
  higher hangs forever. Their content is static, so one frame is the picture.
- **Timing-based panels need wall clock.** `cpuprof_demo` and `memprof_demo`
  capture through the SIGUSR1 one-shot path at live cadence instead: record
  mode's per-frame readback would be measured as app time, and memprof's ring
  samples every ~5 s of *monotonic* time, so a fast N-frame run only ever
  reads "(collecting samples)".

Demos with nothing on screen cold are staged through their own `GLR_DEMO_*`
startup hooks - the demo-side equivalents of the app's `GLR_OPEN_COLOR_PICKER`
/ `GLR_TYPE_KEYS` capture hooks:

| Hook | Demo | Effect |
|------|------|--------|
| `GLR_DEMO_TEXT=<path>` | `editor_demo` | Types the file in through the demo's own dispatcher (an empty document is otherwise the whole shot). |
| `GLR_DEMO_OPEN_PICKER=<shape>` | `color_picker_demo` | Opens the picker on that shape; it otherwise only opens on a click. |
| `GLR_DEMO_ALLOC=<n>` | `memprof_demo` | Allocates n blocks, standing in for n presses of `a`. Waits for the first ring sample, because memprof defers its baseline to that moment and allocating earlier would fold the blocks into the baseline and report a delta of zero. |
