# `src/support/` — Neutral shared utilities

Home for small, dependency-light helpers that don't belong to any of the
layered modules (`repl`, `editor`, `scene`, `ui`, `app`, `subsystems`).
TUs here have no `repl_*` / `editor_*` / `scene_*` / `ui_*` /
`glr_*` knowledge and must be linkable into any of the standalone
demos (`scene_demo`, `repl_demo`, `editor_demo`) without dragging in
their respective layers.

## Contents

- `src/support/cpuprof.{c,h}` — CPU wall-time profiling instrumentation
  (per-section accumulators, frame tick). Public API: `prof_begin`,
  `prof_accum_end`, `prof_end`, `prof_frame_tick`, etc. Used by every
  layer that wants to measure work; the UI panel that visualises it
  lives at [`src/ui/support/cpuprof.c`](src/ui/support/cpuprof.c).

## Why a dedicated directory

Before the `src/` restructure these files sat at the repo root with no
owner directory. Putting them under `src/support/` keeps the root
clean and gives future neutral helpers a clear landing spot — matching
the same pattern as `src/scene/guides/`, `src/subsystems/`, and
similar self-contained pockets.

See [`MODULES.md`](../../docs/MODULES.md) for the full layered overview.
