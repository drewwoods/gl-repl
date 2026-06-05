# `src/repl` — the language pipeline

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../MODULES.md`](../../MODULES.md); the per-frame pipeline narrative
> is in [`../../ARCHITECTURE.md`](../../ARCHITECTURE.md). This README is the
> module-local view: what a REPL pipeline *is*, how the standalone demo
> exercises it, and what it does inside this app.

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
| Lexer/parser → AST | `parser.c` → `GLCmd` records |
| Static validation / compile pass | `compile.c` → `ReplCompiledChange` (pure, never mutates) |
| Expression evaluator | `eval.c` (recursive descent; `sin`, `cos`, `%`, comparisons, vars) |
| IR / lowering | `flatten.c`: unrolls loops, inlines functions, resolves `if` → a flat command stream |
| Bytecode VM / executor | `executor.c`: walks the flat stream emitting GL calls |
| Symbol/spec table | `command_spec.c` (per-command arity, arg kinds, highlight category) |

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
./repl_demo                     # print parse + flatten summaries for 3 samples
./repl_demo --execute           # also run repl_execute_program() against GL stubs
./repl_demo --render            # real GL window; keys 1/2/3 switch sample, space pauses, q quits
make repl_demo USE_GL_STUBS=1   # headless build (no GL dev libs needed)
```

Three samples isolate three pipeline behaviors:

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
`src/editor/commit.c` (editor business), so the demo hand-constructs commands
instead. `tools/repl_demo/stubs.c` is empty — the pipeline has zero
backfill dependencies once host effects flow through the one
`ReplHostEffects` bridge.

## In the REPL app

Inside the full app this is **layers 1 and 3** of the ownership map:

- The editor proposes text; `repl_compile` validates it *purely* (it never
  edits state, never touches the cursor, never calls `set_status`).
- On success the editor applies the change to `ReplState` via
  `repl_apply_*`, and `repl_command_store` does the low-level `GLCmd` array
  shuffling.
- Each frame, if the program is dirty, `flatten.c` rebuilds the flat program
  and `autonormal.c` regenerates `glNormal3f`s; `executor.c` then renders it.
- `ReplState` (`state.c`) owns the program model: parsed commands, the flat
  program, predefined variables, scratch arrays `A/B/C`, the `func0..func9`
  alias table, user scenes, and persisted render config.

`GLCmd` is a pure parse result (type, args, flags, provenance) — it carries
**no source text**; the per-line text lives in the editor's buffer. That
split is what keeps the pipeline editor-agnostic (and what `repl_demo`
proves by supplying its own line store).

Beyond the core pipeline, this directory also owns program-adjacent data and
services: built-in `examples.c`, the `tutorials.c` catalog, save/load and
workspace I/O in `export.c`, and the neutral F1 `help_text.c` tables.

## File map

| File | Responsibility |
|---|---|
| `parser.c` / `.h` | One source line → `GLCmd` + canonical text |
| `eval.c` / `.h` | Expression evaluator, predefined-variable lookup, REPL↔C translation |
| `command.h` | Core types: `CmdType`, `GLCmd`, control-flow predicates |
| `command_spec.c` / `.h` | Per-command descriptor tables (arity, enum args, highlight category) |
| `compile.c` / `.h` | Pure validators → `ReplCompiledChange` (never mutates) |
| `apply.c` / `.h` | Applies a compiled change to `ReplState` |
| `command_store.c` / `.h` | Low-level `GLCmd` array mechanics (insert/replace/delete/load) |
| `flatten.c` / `.h` | Source → flat program (loop unroll, func inline, `if` resolve) |
| `executor.c` / `.h` | Walks the flat program emitting live GL calls |
| `autonormal.c` | Auto-generated `glNormal3f` maintenance |
| `source_scope.c` / `.h` | Source depth / indentation / block lookup cache |
| `format.c` / `.h` | Pure indentation/depth computation |
| `core.c` / `.h`, `core_internal.h`, `pipeline.h` | Normalization pipeline + lifecycle/frame surface |
| `state.c` / `.h`, `state_views.h`, `state_owners.h` | `ReplState` storage + typed read/mut facades |
| `scenes.c`, `examples.c`, `example_loader.c` | User-scene slots, built-in example data + loading |
| `tutorials.c`, `help_text.c` | Tutorial catalog, F1 help-text tables |
| `export.c` / `.h`, `export_state.h`, `load.c` | Save/load, workspace headers, single-file round-trip |

**Boundary:** `src/repl` owns the program model and compiler. It does **not**
own editor state, UI state, replay *runtime* state (a `src/subsystems/` peer), or
live input dispatch. The only live GL in this layer is `executor.c`.
