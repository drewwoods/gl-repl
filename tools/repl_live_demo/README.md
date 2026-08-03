# `repl_live_demo` - live, file-watching REPL demo

A standalone driver that bootstraps the **REPL pipeline + the variable-panel
peer** from a one-file controller and drives them under a real
external-editor workflow:

- You edit **scene files** in your own editor (vim, or anything) - either `.c`
  (the app's standalone-C save/export format) or `.glr` (the scene source the
  built-in examples under [`examples/scenes/`](../../examples/scenes/) are
  written in, so the demo doubles as an example-authoring window).
- The demo **watches the active scene's mtime** and re-imports it on save,
  drawing the geometry live.
- The scene's **predefined variables** appear in the floating slider panel -
  drag a row and the geometry reshapes in real time.
- Each scene's saved **`// camera` block** sets the initial view.

It is the *composition* counterpart to [`repl_demo`](../repl_demo/): where
`repl_demo` proves the REPL language pipeline links with **no** editor /
controller / UI, this demo proves the pipeline and the variable-panel subsystem
compose without the app shell - no `src/editor`, `src/app`, `src/render3d`, or
`src/ui/app` in the link set (`check-repl-live-demo-no-editor` enforces the
editor exclusion).

## Build & run

```bash
make repl-live-demo            # real GL
./repl_live_demo               # default INI (repl_live_demo.ini, or the bundled
                               # tools/repl_live_demo/repl_live_demo.ini)
./repl_live_demo my.ini        # explicit INI
./repl_live_demo a.c b.glr     # use these scene files directly (bypass the INI)
./repl_live_demo examples/scenes/rotating-cube.glr   # author a built-in example

./repl_live_demo --dump-code s.glr        # round-trip the scene to stdout, exit
./repl_live_demo --dump-code s.glr | diff - s.glr    # ...and check the diff

make repl-live-demo USE_GL_STUBS=1   # headless build; runs the import path in
                                     # main() and prints diagnostics, then exits
                                     # (handy as a scene "does it parse?" check)
```

## Controls

| Key / mouse | Action |
|---|---|
| `[` / `]` | Previous / next scene (re-imports) |
| `r` | Force-reload the active scene |
| `e` | Export current state to `./<scene>.roundtrip.c` (or `.roundtrip.glr` for a `.glr` scene - the writer follows the source format, so the diff vs the source stays like-for-like) |
| `v` | Toggle the variable panel |
| `space` | Pause / resume the animation clock `t` |
| LMB drag | Orbit camera |
| RMB drag | Pan camera |
| wheel | Zoom |
| drag a slider row | Change a variable (LMB = linear, RMB = log) |
| `q` / Esc | Quit |

`--dump-code` is the same round-trip as `e` with no window in the way: it
imports the **first** scene, writes what `e` would write to **stdout**, and
exits. Same writers, so the same caveats (no `@cfg` rows - see below); it skips
`glutInit()` entirely, so it works over ssh with no display. `--dump-code
scene.glr | diff - scene.glr` is the whole import/export check.

## The live-edit loop

1. `./repl_live_demo` (from the repo root - the bundled INI resolves its scene
   paths relative to itself, so this just works).
2. In another terminal: `vim tools/repl_live_demo/scenes/ring.c`, tweak a vertex
   or a `float` value, `:w`.
3. The window updates within `poll_ms`. Drag the `n` / `r` sliders to reshape.

**Reload is not transactional.** Live state is reset before loading and neither
loader rolls back, so a malformed save can replace a good scene. The two fail
differently, because that is how the loaders themselves behave: a `.c` import
treats per-line parse failures as *warnings* and still succeeds if the file
merely opened (partial scene), while a `.glr` goes through the example loader,
which rejects the whole body on the first bad line (empty scene). That is by
design: the file is the source of truth and you fix/undo in your editor. The
demo's job is diagnostic clarity - every reload prints a banner and the
per-line warnings / parse errors go to the terminal, so a partial or dropped
load is never silent.

## INI format

Flat `key = value`; `#`/`;` comments; repeatable `scene=`. Relative scene paths
resolve against the INI's own directory.

```ini
window=960x720      # initial window size WxH
poll_ms=250         # mtime poll interval (ms)
panel=on            # variable panel visible at startup (on/off)

scene=scenes/triangle.c
scene=scenes/ring.c
scene=scenes/torus.c
```

## Scene file formats

Two, told apart by extension, each loaded exactly the way the app loads it.

### `.c` - the app's save/export format

Exactly what `./gl-repl scene.c` reads and what File -> Save (`Ctrl+S`) writes.

**Full standalone exports load directly.** A complete `output.c` - with its
`#include` / `display()` / `main()` scaffold, global variable declarations,
function definitions, and `// camera` block - imports the same as a minimal
file: the demo uses the same `repl_export_load_from_file` reader as
`./gl-repl output.c`, so a saved scene round-trips byte-for-byte through the
geometry. Just point the demo at one (`./repl_live_demo output.c`) or add a
`scene=` line for it. So the natural workflow is: build a scene in `gl-repl`,
`Ctrl+S`, then watch/iterate on it here.

The bundled [`scenes/`](scenes/) files are deliberately **minimal** so they are
pleasant to hand-edit in vim: the geometry lives between `// Snippet start` and
`// Snippet end`, a leading `// camera` block sets the view, and
`float name = value;` lines declare the predefined variables that drive the
sliders. The animation clock `t` is shown as a slider too - dragging it scrubs
the clock (playback then continues from the scrubbed value); space pauses it.

### `.glr` - scene source (built-in examples)

The format the built-in examples under [`examples/scenes/`](../../examples/scenes/)
are authored in: REPL source at column 0, optionally preceded by `// @cfg`
rows and a `// camera` block. Point the demo at one and you get a live preview
window for the example you are writing, refreshed on every `:w`.

These load through the **example loader** (`repl_load_example_lines`), not the
`.c` importer, which is what makes them behave as they do in `gl-repl`: the
`@cfg` + `// camera` headers are consumed as metadata, and the body emits in
two passes (function definitions first), so a scene may call a function or read
a `static float` declared further down the file. Feeding the same text through
the `.c` importer would reject those forward references.

Two things the demo cannot reproduce, both because it has no controller: the
`@cfg` rows apply through the app's config bridge, which the demo does not
install, so presentation settings are ignored; and for the same reason the `e`
round-trip writes no `@cfg` rows back. Diff a `.roundtrip.glr` against its
source with that in mind - the geometry and camera are the parts under test.

## Rendering notes & limitations

The demo executes **only the geometry program** (the snippet + the scene's
functions). It does *not* run an export's `init()` / `display()` scaffold, which
in the real app is the job of the `render3d` layer the demo deliberately omits.
To keep lit scenes looking right anyway, the demo installs a small per-frame GL
baseline that approximates `render3d`'s defaults: `GL_COLOR_MATERIAL` (so
`glColor3f` tints lit surfaces - otherwise it is ignored under `GL_LIGHTING` and
geometry renders as the default white-ish material), `GL_NORMALIZE`, a neutral
key + fill light, and a small global ambient. The scene still enables
`GL_LIGHTING` and its own lights. The *exact* light colors/positions from a
scene's `init()` are not reproduced, so a heavily lit export may look close but
not pixel-identical to `gl-repl`. The animation clock `t` is advanced in place
in the REPL variable table (like the app), so a scene's own `t = 0` reset takes
effect - the panel `t`, the HUD `t`, and the geometry all stay in sync.
