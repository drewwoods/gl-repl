---
name: gl-repl-capture
description: Run gl-repl headless and capture frames - CLI flags, GLR_* env hooks, OSMesa builds, screenshots, GIF/video recording, pointer scripts, tours, and docs-media regeneration. Use when asked to screenshot the app, record a GIF or video, regenerate docs/images, verify a change visually without a window, or debug startup stalls.
---

# Running & capturing gl-repl

Full reference: `docs/ADVANCED_USAGE.md`. This is the working subset.

## CLI

```bash
./gl-repl                  # Fresh session
./gl-repl output.c         # Reload saved session
./gl-repl workspace/       # Load every *.c as a user scene
./gl-repl --example torus  # Built-in example (name or 1-based index)
./gl-repl --list-examples
./gl-repl --tour editing   # Guided tour on launch (name or 1-based index)
./gl-repl --list-tours
./gl-repl --no-audio       # Skip audio init
./gl-repl --assets <dir>   # Music dir override (also GLR_ASSETS_DIR)
./gl-repl --time 5         # Initial animation t (also GLR_TIME; --time wins)
./gl-repl --example 9 --export-ply out.ply [--export-ply-srgb]
./gl-repl --dump-code      # Print loaded buffer; --dump-* family honors --example
./gl-repl --no-accum       # Disable accumulation buffer (AA + blur)
./gl-repl --accum          # Force it on - required under OSMesa, where the
                           # auto probe disables software-emulated accum

./gl-repl --detailed-prof  # Fine-grained init-trace phases (also GLR_DETAILED_PROF)
```

`--time` / `GLR_TIME` applies *after* `--example` load, so the override sticks.

## Headless capture env hooks

Line numbers are the code panel's own (1-based); below 1 is refused.

| Var | Effect |
|---|---|
| `GLR_EDIT_LINE=<n>` | park cursor and scroll it into view → cursor-bound overlays render headlessly |
| `GLR_TYPE_KEYS='...'` | feed keystrokes after load |
| `GLR_OPEN_COLOR_PICKER=<line>` | open the floating picker on frame 1 |
| `GLR_OPEN_GL_STATE=<line>` | open the GL-state popup on frame 1 |
| `GLR_OPEN_ASSIGN_PLOT=<line>[,<line>…]` | open the value plot; extra lines join as series (max 4) |
| `GLR_OPEN_COMMAND_HELP=<line>[,<dx>]` | right-click a GL row for its help card; `dx` slides it right - pose it after the plot, which closes it |
| `GLR_ASSIGN_PLOT_RATE=once/1hz/frame` | plot capture rate - `frame` is what makes a plotted shot deterministic |
| `GLR_ASSIGN_PLOT_LOG=1` / `_EXPANDED=1` | the plot's `log` and `2x` chips |
| `GLR_CODE_FOCUS=0/1` | code focus as a level - `0` shows the generated C (`init()`, `display()` prologue) |
| `GLR_CODE_SCROLL=<row>` | park the panel's top row (panel rows, so it reaches generated C no cursor can) |
| `GLR_ACCUM_PASSES=1/2/4/8/12/16` | accumulation passes |
| `GLR_ACCUM_EFFECT=off/aa/blur/'blur cam'` | accum effect by Config-menu name - the only capture route to motion blur |
| `GLR_TICK_PER_FRAME=1` | fixed-dt sim advances per rendered frame - **deterministic offline capture** |
| `GLR_VIEW_TOGGLE_AT=<t1,t2,…>` | 2D/3D swatch transition; implies tick-per-frame |
| `GLR_POINTER_SCRIPT=<file>` | scripted pointer/keyboard + cursor overlay |
| `GLR_NO_SPLASH=1` | suppress the startup splash |

Other env: `GLR_NO_POINT_PARAMETER=1` (force the no-`glPointParameterfv`
fallback path - runtime-gated, no build flag), `GLR_NO_GPU_PROF=1`,
`GLR_AUDIO_HITCH_MS=<ms>` (worker hitch log threshold, default 50).

Implementation: `src/app/boot/glr_capture_env.{c,h}` - `_apply` handles
bootstrap hooks (time / pointer-script / splash / tick-per-frame / edit-line /
type-keys / accum), `_frame_hook` handles per-frame ones (color-picker /
GL-state / assign-plot / command-help / help / view-toggle) - and their order
there is load-bearing where two popups fight.

## Headless OSMesa build

```bash
brew install mesa mesa-glu
make gl-repl FREEGLUT_OSMESA=1
```

Renders with no window - for headless geometry/feedback tests and captures.
Separate `build-osmesa/` dirs, so Cocoa and OSMesa builds coexist.
Compatibility GL only.

## Frame capture

The vendored freeglut carries SIGUSR1/record support on **all** backends - no
re-vendor needed.

- `kill -USR1 <pid>` → writes a PPM frame (native and OSMesa)
- `FREEGLUT_CAPTURE_FRAMES=N` → records N frames, then exits
- `FREEGLUT_CAPTURE_STREAM` → pipes frames to ffmpeg

## Scripts

| Script | Purpose |
|---|---|
| `scripts/docs-assets.sh` | regenerates `docs/images/`; deterministic frame-based settling |
| `scripts/record-gif.sh` | GIF capture |
| `scripts/record-video.sh` | video capture |

Scripted interaction uses `GLR_POINTER_SCRIPT` pointer scripts with symbolic
targets. The same engine powers the in-app **Tours** menu: `tours/*.pointer` +
`tours/catalog.ini`, compiled by `scripts/gen_tours.py`, validated by
`make check-tours-catalog`.

## Staging good shots

- Pose shots via snippet files carrying `@cfg` headers rather than by driving
  the UI.
- **Vertex outlines default ON** and pollute reference shots - disable them.
- Theme fades need ~12 s to settle; eased example cameras need ~240 frames.
- Prefer `GLR_TICK_PER_FRAME=1` for anything that must be reproducible.

## Startup diagnostics

An always-on `[init +N.NNNs] <phase>` stderr trace runs in `main()`.
`--no-audio` isolates `ma_engine_init()` stalls; `--detailed-prof` adds
glutInit / playlist / first-two-frames phases. The audio worker logs
`worker hitch: <op> took N ms` for blocking ops over `GLR_AUDIO_HITCH_MS`.

## Music (affects startup timing)

`glr_audio_bootstrap()` in `src/app/glr_audio.c` concatenates three sources,
each sorted by filename: the primary assets dir (`./assets`, overridden by
`--assets` > `GLR_ASSETS_DIR`), the bundled `<exe>/../Resources/assets` (the
`.app` case), and the per-user folder (`~/Library/Application Support/gl-repl/Music`
on macOS, `$XDG_DATA_HOME/gl-repl/music` elsewhere; created on first run). Zero
mp3s → fallback `assets/song.mp3`. `make fetch-music` pulls the pack.

All of it lives in `glr_audio.c` statics; the `#ifdef` platform branches must
stay C99/portable and localized.
