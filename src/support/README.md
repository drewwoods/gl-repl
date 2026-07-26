# `src/support/` — Neutral shared utilities (Draft)

Home for small, dependency-light helpers that don't belong to any of the
layered modules (`repl`, `editor`, `render3d`, `ui`, `app`, `subsystems`).
TUs here have no `repl_*` / `editor_*` / `render3d_*` / `ui_*` /
`glr_*` knowledge and must be linkable into any of the standalone
demos (`render3d_demo`, `repl_demo`, `editor_demo`) without dragging in
their respective layers.

## Contents

- `src/support/cpuprof.{c,h}` — CPU wall-time profiling instrumentation
  (per-section accumulators, frame tick). Public API: `prof_begin`,
  `prof_accum_end`, `prof_end`, `prof_frame_tick`, etc. Used by every
  layer that wants to measure work; the UI panel that visualises it
  lives at [`src/ui/support/cpuprof.c`](../ui/support/cpuprof.c).
- `src/support/gpuprof.{c,h}` — the asynchronous twin: GL timer queries
  bracketing the same `ProfSection` ids, so the panel can show GPU time
  next to CPU time. Deliberately a GL-free TU at the API level.
- `src/support/memprof.{c,h}` — process memory sampling (current signal,
  ring of historical samples, pure byte formatter). Mirrors cpuprof's
  conventions; panel at [`src/ui/support/memprof.c`](../ui/support/memprof.c).
- `src/support/mesh_ply.{c,h}` — pure PLY writer over a GL feedback
  (`GL_3D_COLOR`) float stream: inverts the ortho/viewport transform back
  to world space, fan-triangulates, optionally welds.

## How it is exercised

The profiling helpers each have a standalone driver under `tools/`, which
is what keeps them linkable with no owner layer:

- **`make cpuprof_demo`** ([`tools/cpuprof_demo/`](../../tools/cpuprof_demo/)) —
  a display-list micro-benchmark: the same teapots drawn immediate,
  from a reused list, and from a per-frame recompiled list, each timed as
  its own section.
- **`make memprof_demo`** ([`tools/memprof_demo/`](../../tools/memprof_demo/)) —
  the live memory panel with keys to allocate and free, so the signal and
  graph respond on demand.

Both link only the sampler here + its `src/ui/support/` panel + `ui/core`
theme, enforced by `check-subsystem-demo-isolation.sh` under
`make check-state-ownership`. `mesh_ply` has no demo; it is covered by the
PLY export tests. Index of every demo: [`tools/README.md`](../../tools/README.md).

## Why a dedicated directory

Before the `src/` restructure these files sat at the repo root with no
owner directory. Putting them under `src/support/` keeps the root
clean and gives future neutral helpers a clear landing spot — matching
the same pattern as `src/render3d/guides/`, `src/subsystems/`, and
similar self-contained pockets.

See [`MODULES.md`](../../docs/MODULES.md) for the full layered overview.
