# `tools/` — standalone demos + developer tooling

Every binary here is built from the repo root Makefile and lands at the repo
root (`./repl_demo`, `./render3d_demo`, …). Nothing in `tools/` is linked into
`gl-repl` itself.

The demos are not samples: each one is the **executable proof that a module
links without its upper layers**, enforced by a guard in
`make check-state-ownership`. That is why they live outside `src/` — the
isolation guards match on path prefixes (`src/app/`, `src/repl/`, …), and a
demo TU with its own `main()` inside one of those trees would need a carve-out
in every guard that scans the module.

Where the docs live, so nothing is written twice:

| Layer | Holds |
|-------|-------|
| [`docs/MODULES.md`](../docs/MODULES.md#standalone-demo-binaries-layer-independence-proofs) | Why the demos exist, boundary rules — the authority |
| `src/<module>/README.md` | The module-side narrative (linked per demo below) |
| The demo's own source header | Keys, CLI, what the scene shows |
| This file | The directory index: what's here, what proves what |

## Boundary demos

The four that define a layer by exclusion. All build `USE_GL_STUBS=1`-clean.

| Demo | Proves | Module doc |
|------|--------|-----------|
| [`repl_demo/`](repl_demo/) | The language pipeline links with **no** editor / controller / UI. [`stubs.c`](repl_demo/stubs.c) is empty — zero host dependencies — and a ratchet keeps it that way. Backs source lines with its own editor-free [`source_document.c`](repl_demo/source_document.c). | [`src/repl/README.md`](../src/repl/README.md#the-demo-repl_demo) · [`ARCHITECTURE.md §11`](../src/repl/ARCHITECTURE.md) |
| [`render3d_demo/`](render3d_demo/) | `src/render3d` has no REPL dependency; supplies its own camera + HUD shell. | [`src/render3d/README.md`](../src/render3d/README.md#the-demo-render3d_demo) |
| [`editor_demo/`](editor_demo/) | The text-document model works with a *different* input dispatcher ([`input.c`](editor_demo/input.c)) and File menu ([`menu.c`](editor_demo/menu.c)) — so `src/editor/input.c` is the REPL editor's controller, not the model's. Links `src/ui/core` but never `src/ui/app`. | [`src/editor/README.md`](../src/editor/README.md#the-demo-editor_demo) · [`src/ui/README.md`](../src/ui/README.md#how-it-is-exercised) |
| [`repl_live_demo/`](repl_live_demo/README.md) | The *composition* counterpart: pipeline + variable-panel peer + a demo-local camera bridge, driven by an external editor over file-watch — with no app shell. | [`src/repl/README.md`](../src/repl/README.md#the-demo-repl_demo) |

`src/app` is the one module with no demo, by design — it is what the four
above exclude. See [`src/app/README.md`](../src/app/README.md#how-it-is-exercised--the-inverse-of-the-demos).

## Single-module demos

Each links only its peer + that peer's renderer + `src/ui/core` theme, and is
guarded by `check-subsystem-demo-isolation.sh` (Makefile dep list, demo
includes, and an `nm` sweep for `repl_`/`editor_`/`glr_` symbols).

| Demo | Drives | Module doc |
|------|--------|-----------|
| [`variable_panel_demo/`](variable_panel_demo/) | The variable-panel peer over an in-memory `VariablePanelValueSource` — no REPL eval table. | [`src/subsystems/README.md`](../src/subsystems/README.md#how-it-is-exercised) |
| [`color_picker_demo/`](color_picker_demo/) | The color-picker peer over a `ColorPickerHostBridge` backed by a plain color array. | [`src/subsystems/README.md`](../src/subsystems/README.md#how-it-is-exercised) |
| [`cpuprof_demo/`](cpuprof_demo/) | `prof_*` wall-time sampling, shaped as a display-list micro-benchmark. | [`src/support/README.md`](../src/support/README.md#how-it-is-exercised) |
| [`memprof_demo/`](memprof_demo/) | `memprof_*` sampling + the live memory panel. | [`src/support/README.md`](../src/support/README.md#how-it-is-exercised) |

## Other

- [`render3d-elements/`](render3d-elements/README.md) — REPL-language
  recreations of render3d elements (grids, backdrops) with their own example
  catalog; needs a raised flat-command budget. Also the reloadable half of
  `make render3d-hot` ([`src/render3d/README.md`](../src/render3d/README.md#hot-reload-make-render3d-hot)).
- [`capacity_matrix.c`](capacity_matrix.c) — `make capacity-matrix`; prints the
  per-unit memory cost of every tunable `MAX_*` constant. Hand-curated table:
  add a row when you add a `MAX_*`.
- [`keymap.sh`](keymap.sh) — `make check-keymap-no-dup` / `make keymap-list`
  over [`keymap.h`](../keymap.h).

## Build

```bash
make repl_demo USE_GL_STUBS=1   # any demo, headless — no GL dev libs needed
make render3d_demo              # real GL, opens a window
make check-state-ownership      # runs every demo isolation guard
```

A demo that stops linking is the guard doing its job: something in the module
grew a dependency on a layer above it. Fix the dependency, not the demo's
object list.
