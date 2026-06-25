# `repl_live_demo` — live, file-watching REPL demo

A standalone driver that bootstraps the **REPL pipeline + the variable-panel
peer** from a one-file controller and drives them under a real
external-editor workflow:

- You edit **scene `.c` files** in your own editor (vim, or anything).
- The demo **watches the active scene's mtime** and re-imports it on save,
  drawing the geometry live.
- The scene's **predefined variables** appear in the floating slider panel —
  drag a row and the geometry reshapes in real time.
- Each scene's saved **`// camera` block** sets the initial view.

It is the *composition* counterpart to [`repl_demo`](../repl_demo/): where
`repl_demo` proves the REPL language pipeline links with **no** editor /
controller / UI, this demo proves the pipeline and the variable-panel subsystem
compose without the app shell — no `src/editor`, `src/app`, `src/render3d`, or
`src/ui/app` in the link set (`check-repl-live-demo-no-editor` enforces the
editor exclusion).

## Build & run

```bash
make repl_live_demo            # real GL
./repl_live_demo               # default INI (repl_live_demo.ini, or the bundled
                               # tools/repl_live_demo/repl_live_demo.ini)
./repl_live_demo my.ini        # explicit INI
./repl_live_demo a.c b.c       # use these scene files directly (bypass the INI)

make repl_live_demo USE_GL_STUBS=1   # headless build; runs the import path in
                                     # main() and prints diagnostics, then exits
                                     # (handy as a scene "does it parse?" check)
```

## Controls

| Key / mouse | Action |
|---|---|
| `[` / `]` | Previous / next scene (re-imports) |
| `r` | Force-reload the active scene |
| `v` | Toggle the variable panel |
| `space` | Pause / resume the animation clock `t` |
| LMB drag | Orbit camera |
| RMB drag | Pan camera |
| wheel | Zoom |
| drag a slider row | Change a variable (LMB = linear, RMB = log) |
| `q` / Esc | Quit |

## The live-edit loop

1. `./repl_live_demo` (from the repo root — the bundled INI resolves its scene
   paths relative to itself, so this just works).
2. In another terminal: `vim tools/repl_live_demo/scenes/ring.c`, tweak a vertex
   or a `float` value, `:w`.
3. The window updates within `poll_ms`. Drag the `n` / `r` sliders to reshape.

**Reload is not transactional.** The importer resets live state before loading
and treats per-line parse failures as *warnings* while still succeeding if the
file merely opened — so a malformed save can leave a partial or empty scene with
no rollback. That is by design: the file is the source of truth and you
fix/undo in your editor. The demo's job is diagnostic clarity — every reload
prints a banner and the importer's per-line warnings + load summary go to the
terminal, so a partial load is never silent.

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

## Scene file format

Scenes are in the app's **save/export `.c` format** — exactly what
`./gl-repl scene.c` reads and what File → Save writes. The easiest way to make a
new scene is to build it in `gl-repl` and save it; the bundled
[`scenes/`](scenes/) files are minimal hand-written examples. The geometry lives
between `// Snippet start` and `// Snippet end`; a leading `// camera` block sets
the view, and `float name = value;` lines inside the snippet declare the
predefined variables that drive the sliders. `t` is the animation clock (space
toggles it) and is intentionally **not** shown as a slider.
