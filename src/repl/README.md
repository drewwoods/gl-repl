# `src/repl` — the language pipeline

> Part of the OpenGL Immediate-Mode REPL. This README is the module-local
> orientation: what a REPL pipeline *is*, how the standalone demo exercises
> it, and what it does inside this app. For the deep dive — the data model,
> the edit/frame flows, each pipeline stage, and the state-ownership
> boundaries — read [`ARCHITECTURE.md`](ARCHITECTURE.md).
>
> Whole-tree context lives one level up: the ownership map is in
> [`../../docs/MODULES.md`](../../docs/MODULES.md) and the per-frame *app* narrative is
> in [`../../docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md).

## What this is, in general

A **REPL** (read–eval–print loop) is the classic shape of an interactive
language tool: read source text, turn it into a structured form, evaluate
it, show the result, repeat. Strip away the editor and the screen and what
remains is an ordinary **interpreter pipeline**:

```
source text ──parse──▶ command/AST ──compile/validate──▶ IR ──execute──▶ effects
```

`src/repl` is exactly that pipeline for a small domain-specific language.
The "language" is a friendly subset of **immediate-mode OpenGL** —
`glBegin`/`glVertex3f`/`glColor3f`/`glRotatef`/… — plus light control flow
(`for`, `if`, `func0..func9`), scalar variables, and fixed scratch arrays.
The "effects" are live GL calls that draw geometry.

The pieces map onto standard interpreter parts:

| General concept | Here |
|---|---|
| Lexer/parser → AST | [`parser.c`](src/repl/parser.c) → [`GLCmd`](src/repl/command.h#L86) records |
| Static validation / compile pass | [`compile.c`](src/repl/compile.c) → [`ReplCompiledChange`](src/repl/compile.h#L129) (pure, never mutates) |
| Expression evaluator | [`eval.c`](src/repl/eval.c) (recursive descent; `sin`, `cos`, `%`, comparisons, vars) |
| IR / lowering | [`flatten.c`](src/repl/flatten.c): unrolls loops, inlines functions, resolves `if` → a flat command stream; [`flatten_query.c`](src/repl/flatten_query.c): live flat-program cost/cursor queries |
| Bytecode VM / executor | [`executor.c`](src/repl/executor.c): walks the flat stream emitting GL calls |
| Symbol/spec table | [`command_spec.c`](src/repl/command_spec.c) (per-command arity, arg kinds, highlight category) |

The defining design choice is a **two-level command model**:
*source commands* (what the user wrote, with loops and calls intact) are
lowered into a *flat program* (everything unrolled/inlined) that the
executor runs every frame. Re-flattening each frame is what lets `t`-driven
animation and variable edits take effect live.

## The demo: `repl_demo`

[`tools/repl_demo/`](../../tools/repl_demo/) drives the pipeline —
parse → command store → flatten → execute — from **hard-coded static text**,
with no editor, controller, or UI in the link set. It is the load-bearing
proof that the language pipeline stands on its own.

```bash
make repl_demo                  # real GL (--render opens a window)
./repl_demo                     # print parse + flatten summaries for samples 1-3
./repl_demo --execute           # also run repl_execute_program() against GL stubs
./repl_demo --trace             # narrated end-to-end walkthrough of every stage
./repl_demo --render            # real GL window; keys 1/2/3/4 switch sample, space pauses, q quits
make repl_demo USE_GL_STUBS=1   # headless build (no GL dev libs needed)
```

**Start with `--trace`.** It loads one representative program through the
*real* non-editor compile→apply path (`repl_load_apply_line`) and narrates
every stage the backend runs — text → compile → apply → source program →
flatten (with provenance + local-var snapshots) → per-frame re-evaluation.
It's the executable companion to [`ARCHITECTURE.md` §11](ARCHITECTURE.md),
which walks the same output prose-side. Render sample 4 draws the same
program as a rotating ring.

The demo is representative of the **language pipeline**, not of the whole
application. `--trace` is the broadest sample because it exercises the
non-editor load transaction ([`compile.c`](src/repl/compile.c) + [`load.c`](src/repl/load.c) + [`apply.c`](src/repl/apply.c)), variable
side effects, typed block syntax, flatten provenance, local-var snapshots, and
per-frame expression re-evaluation. The default samples are intentionally
narrower boundary probes: one parses plain GL lines directly, one
hand-constructs a loop to isolate flattening, and one registers variables
directly to isolate `has_vars` re-evaluation. None of these demo modes cover
editor undo/cursor/input effects, UI/controller routing, full import/export
metadata, scene-tab LRU behavior, or tutorial/replay presentation; those stay
with their owning modules in the full app and tests.

The three print-summary samples each isolate one pipeline behavior:

1. **Plain commands** — a `glBegin/glColor/glVertex/glEnd` triangle parsed
   straight through `repl_parser_parse_command_ctx` + the command store.
2. **Hand-built for-loop** — a `CMD_FOR_BEGIN`/body/`CMD_FOR_END` triplet
   constructed directly so `repl_flatten_program` is shown unrolling 4
   vertices (no editor commit path involved).
3. **Variable-driven re-evaluation** — declares `r`, parses
   `glVertex3f(r*sin(t), r*cos(t), 0)` with the expression *preserved*, then
   bumps `t` and re-flattens to watch `has_vars` expressions recompute
   against the live variable table.

What the demo deliberately *does not* link tells you where the boundary is:
the `float x;` / `x = expr;` / typed-as-text `for(...) {` flows live in
[`src/editor/commit.c`](src/editor/commit.c) (editor business), so the demo hand-constructs commands
instead. [`tools/repl_demo/stubs.c`](tools/repl_demo/stubs.c) is empty — the pipeline has zero
backfill dependencies once host effects flow through the one
[`ReplHostEffects`](src/repl/host_effects.h#L38) bridge.

## In the REPL app

Inside the full app this is **layers 1 and 3** of the ownership map:

- The editor proposes text; `repl_compile` validates it *purely* (it never
  edits state, never touches the cursor, never calls `set_status`).
- On success the editor applies the change to REPL runtime state via
  `repl_apply_*`, and `repl_command_store` does the low-level [`GLCmd`](src/repl/command.h#L86) array
  shuffling.
- Each frame, if the program is dirty, [`flatten.c`](src/repl/flatten.c) rebuilds the flat program
  and [`autonormal.c`](src/repl/autonormal.c) regenerates `glNormal3f`s; [`executor.c`](src/repl/executor.c) then renders it.
- [`ReplRuntimeState`](src/repl/state.h#L18) ([`state.c`](src/repl/state.c)) owns the program model: parsed commands, the flat
  program, predefined variables, scratch arrays `A/B/C`, the `func0..func9`
  alias table, the `t` clock, and the runtime-mutated render tail
  (light-enable mask + clear color). The user-scene *catalog* slots live
  separately in [`scenes.c`](src/repl/scenes.c) (as [`SceneSnapshot`](src/repl/scene_snapshot.h#L17)s); [`ReplRuntimeState`](src/repl/state.h#L18) only tracks the
  active example index and bound workspace dir.

[`GLCmd`](src/repl/command.h#L86) is a pure parse result (type, args, flags, provenance) — it carries
**no source text**; the per-line text lives in the editor's buffer. That
split is what keeps the pipeline editor-agnostic (and what `repl_demo`
proves by supplying its own line store).

Beyond the core pipeline, this directory also owns program-adjacent data and
services: built-in [`examples.c`](src/repl/examples.c), the [`tutorials.c`](src/repl/tutorials.c) catalog, the save/load
file format (writer in [`export.c`](src/repl/export.c), reader in [`import.c`](src/repl/import.c)) and workspace I/O
([`scenes.c`](src/repl/scenes.c) / [`workspace_io.c`](src/repl/workspace_io.c)), and the neutral F1 [`help_text.c`](src/repl/help_text.c) tables.

> For how all of this fits together — the two flows, the compile→apply
> seam, the flatten budgets, the state slices, and the host-effects
> bridge — see [`ARCHITECTURE.md`](ARCHITECTURE.md).

## File map

| File | Responsibility |
|---|---|
| [`command.h`](src/repl/command.h) | Core types: [`CmdType`](src/repl/command.h#L37), [`GLCmd`](src/repl/command.h#L86), control-flow predicates |
| [`command_spec.c`](src/repl/command_spec.c) / `.h` | Per-command descriptor tables (arity, enum args, highlight category) |
| [`control_flow.h`](src/repl/control_flow.h), [`color_limits.h`](src/repl/color_limits.h), [`util.h`](src/repl/util.h) | Shared limits (goto cap, clear-color cap) and size-checked buffer helpers |
| **Edit flow** | *text → program model* |
| [`parser.c`](src/repl/parser.c) / `.h` | One source line → [`GLCmd`](src/repl/command.h#L86) + canonical text |
| [`normalize.c`](src/repl/normalize.c) / `.h` | Parse-and-normalize pipeline |
| [`eval.c`](src/repl/eval.c) / `.h` | Expression evaluator, predefined-variable lookup, REPL↔C translation |
| [`compile.c`](src/repl/compile.c) / `.h` | Pure validators → [`ReplCompiledChange`](src/repl/compile.h#L129) (never mutates) |
| [`apply.c`](src/repl/apply.c) / `.h` | Applies a compiled change to REPL runtime state (cmd store + predef/scratch/alias ops) |
| [`command_store.c`](src/repl/command_store.c) / `.h` | Low-level [`GLCmd`](src/repl/command.h#L86) array mechanics (insert/replace/delete/load) |
| [`load.c`](src/repl/load.c) / `.h` | Non-editor line loader + apply transaction (import/example/tutorial/tests) |
| [`visible_vars.c`](src/repl/visible_vars.c) / `.h`, [`text_helpers.c`](src/repl/text_helpers.c) / `.h` | Loop/func-local variable collection; parse/extract/canonical-text helpers |
| [`source_scope.c`](src/repl/source_scope.c) / `.h`, [`format.c`](src/repl/format.c) / `.h`, [`reformat.c`](src/repl/reformat.c) / `.h`, [`bootstrap.c`](src/repl/bootstrap.c) / `.h` | Depth/indent/block-lookup cache, pure indentation, source reformat, startup loading |
| **Frame flow** | *program model → GL* |
| [`flatten.c`](src/repl/flatten.c) / `.h` | Source → flat program (unroll/inline/resolve `if`) |
| [`flatten_query.c`](src/repl/flatten_query.c) / `.h` | Live flat-program cost/cursor queries |
| [`autonormal.c`](src/repl/autonormal.c) | Auto-generated `glNormal3f` maintenance |
| [`executor.c`](src/repl/executor.c) / `.h` | Walks the flat program emitting live GL calls (the only live-GL TU) |
| [`transform_utils.h`](src/repl/transform_utils.h) | Shared GL matrix tracking helpers (no executor link dependency) |
| [`pipeline.h`](src/repl/pipeline.h) | Controller-facing frame entry points (flatten/autonormal/refresh) |
| [`program_query.c`](src/repl/program_query.c) / `.h`, [`geometry_query.h`](src/repl/geometry_query.h) | Read-only queries over the source/flat program |
| **State & ownership** | |
| [`state.c`](src/repl/state.c) / `.h`, [`state_views.h`](src/repl/state_views.h), [`state_owners.h`](src/repl/state_owners.h) | [`ReplRuntimeState`](src/repl/state.h#L18) storage + capture/restore + typed read/mut facades |
| [`state_notify.h`](src/repl/state_notify.h) | Dirty-flag invalidation entry points |
| [`time.c`](src/repl/time.c) / `.h` | The predefined `t` animation clock |
| [`host_effects.c`](src/repl/host_effects.c) / `.h` | Host side-effect bridge (status, cursor, completion, tutorial teardown) |
| **Persistence (save/load)** | |
| [`export.c`](src/repl/export.c), [`import.c`](src/repl/import.c) | Writer half (file emit, header refresh) and reader half (import state machine) |
| [`export_setup.c`](src/repl/export_setup.c), [`export_prologue.c`](src/repl/export_prologue.c), [`export_display.c`](src/repl/export_display.c), [`export_cmd_writer.c`](src/repl/export_cmd_writer.c) | C boilerplate, globals/predef prologue, `display()` body, per-command C emission |
| [`export.h`](src/repl/export.h), [`export_internal.h`](src/repl/export_internal.h), [`export_state.h`](src/repl/export_state.h), [`export_format_shared.h`](src/repl/export_format_shared.h) | Export/import API and shared state-text dimensions |
| **Scenes & workspaces** | |
| [`scenes.c`](src/repl/scenes.c) / `.h`, [`scene_snapshot.c`](src/repl/scene_snapshot.c) / `.h` | User-scene slots (LRU, promotion); copyable scene payload |
| [`workspace_io.c`](src/repl/workspace_io.c) / `.h`, [`cfg_baseline.c`](src/repl/cfg_baseline.c) / `.h` | Workspace filesystem + file-naming mechanics; flat key/value config bag |
| **Program-adjacent data** | |
| [`examples.c`](src/repl/examples.c) / `.h`, [`example_loader.c`](src/repl/example_loader.c) / `.h` | Built-in example data; example load + `@cfg` / `// camera` metadata |
| [`tutorials.c`](src/repl/tutorials.c) / `.h`, [`catalog_tags.h`](src/repl/catalog_tags.h) | Tutorial catalog; shared example/tutorial tag-bit helper |
| [`help_text.c`](src/repl/help_text.c) / `.h`, [`keymap_format.c`](src/repl/keymap_format.c) | F1 help-text tables; user-facing keybinding labels |

**Boundary:** `src/repl` owns the program model and compiler. It does **not**
own editor state, UI state, replay *runtime* state (a `src/subsystems/` peer), or
live input dispatch. The only live GL in this layer is [`executor.c`](src/repl/executor.c).
`ARCHITECTURE.md` §10 lists the guards that ratchet these boundaries.
