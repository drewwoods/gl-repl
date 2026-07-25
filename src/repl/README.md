# `src/repl` — the language pipeline (Draft)

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
| Lexer/parser → AST | [`parser.c`](parser.c) → [`GLCmd`](command.h#L97) records |
| Static validation / compile pass | [`compile.c`](compile.c) → [`ReplCompiledChange`](compile.h#L130) (pure, never mutates) |
| Expression evaluator | [`eval.c`](eval.c) (recursive descent; `sin`, `cos`, `%`, comparisons, vars) |
| IR / lowering | [`flatten.c`](flatten.c): unrolls loops, inlines functions, resolves `if` → a flat command stream; [`flatten_query.c`](flatten_query.c): live flat-program cost/cursor queries |
| Bytecode VM / executor | [`executor.c`](executor.c): walks the flat stream emitting GL calls |
| Symbol/spec table | [`command_spec.c`](command_spec.c) (per-command arity, arg kinds, highlight category) |

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
non-editor load transaction ([`compile.c`](compile.c) + [`load.c`](load.c) + [`apply.c`](apply.c)), variable
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
[`src/editor/commit.c`](../editor/commit.c) (editor business), so the demo hand-constructs commands
instead. [`tools/repl_demo/stubs.c`](../../tools/repl_demo/stubs.c) is empty — the pipeline has zero
backfill dependencies once host effects flow through the one
[`ReplHostEffects`](host_effects.h#L38) bridge.

## In the REPL app

Inside the full app this is **layers 1 and 3** of the ownership map:

- The editor proposes text; `repl_compile` validates it *purely* (it never
  edits state, never touches the cursor, never calls `set_status`).
- On success the editor applies the change to REPL runtime state via
  `repl_apply_*`, and `repl_command_store` does the low-level [`GLCmd`](command.h#L97) array
  shuffling.
- Each frame, [`repl_refresh_flat_program()`](pipeline.h) brings the flat
  program current: it either does nothing, rebakes values in place, or asks
  [`flatten.c`](flatten.c) for a full rebuild. [`autonormal.c`](autonormal.c)
  regenerates `glNormal3f`s when needed; [`executor.c`](executor.c) then
  renders the flat result.
- [`ReplRuntimeState`](state.h#L18) ([`state.c`](state.c)) owns the program model: parsed commands, the flat
  program, predefined variables, scratch arrays `A/B/C`, the `func0..func9`
  alias table, the `t` clock, and the runtime-mutated render tail
  (light-enable mask + clear color). The user-scene *catalog* slots live
  separately in [`scenes.c`](scenes.c) (as [`SceneSnapshot`](scene_snapshot.h#L17)s); [`ReplRuntimeState`](state.h#L18) only tracks the
  active example index and bound workspace dir.

[`GLCmd`](command.h#L97) is a pure parse result (type, args, flags, provenance) — it carries
**no source text**; the per-line text lives in the editor's buffer. That
split is what keeps the pipeline editor-agnostic (and what `repl_demo`
proves by supplying its own line store).

Beyond the core pipeline, this directory also owns program-adjacent services:
the generated built-in example facade [`examples.c`](examples.c)
(authored from repository-level `examples/catalog.ini` and
`examples/scenes/`), the [`tutorials.c`](tutorials.c) catalog, the save/load
file format (writer in [`export.c`](export.c), reader in [`import.c`](import.c)) and workspace I/O
([`scenes.c`](scenes.c) / [`workspace_io.c`](workspace_io.c)), the neutral F1
[`help_text.c`](help_text.c) tables, and the compiled-in right-click command
help facade ([`command_descriptions.c`](command_descriptions.c)). Command-help
text is authored outside C in
[`command_descriptions.txt`](command_descriptions.txt) and validated/generated
at build time, like the example catalog.

> For how all of this fits together — the two flows, the compile→apply
> seam, the flatten budgets, the state slices, and the host-effects
> bridge — see [`ARCHITECTURE.md`](ARCHITECTURE.md).

## File map

| File | Responsibility |
|---|---|
| [`command.h`](command.h) | Core types: [`CmdType`](command.h#L37), [`GLCmd`](command.h#L97), control-flow predicates |
| [`command_spec.c`](command_spec.c) / `.h` | Per-command descriptor tables (arity, enum args, highlight category) |
| [`control_flow.h`](control_flow.h), [`color_limits.h`](color_limits.h), [`util.h`](util.h) | Shared limits (goto cap, clear-color cap) and size-checked buffer helpers |
| **Edit flow** | *text → program model* |
| [`parser.c`](parser.c) / `.h` | One source line → [`GLCmd`](command.h#L97) + canonical text |
| [`normalize.c`](normalize.c) / `.h` | Parse-and-normalize pipeline |
| [`eval.c`](eval.c) / `.h` | Expression evaluator, predefined-variable lookup, REPL↔C translation |
| [`compile.c`](compile.c) / `.h` | Pure validators → [`ReplCompiledChange`](compile.h#L130) (never mutates) |
| [`apply.c`](apply.c) / `.h` | Applies a compiled change to REPL runtime state (cmd store + predef/scratch/alias ops) |
| [`command_store.c`](command_store.c) / `.h` | Low-level [`GLCmd`](command.h#L97) array mechanics (insert/replace/delete/load) |
| [`load.c`](load.c) / `.h` | Non-editor line loader + apply transaction (import/example/tutorial/tests) |
| [`visible_vars.c`](visible_vars.c) / `.h`, [`text_helpers.c`](text_helpers.c) / `.h` | Loop/func-local variable collection; parse/extract/canonical-text helpers |
| [`source_scope.c`](source_scope.c) / `.h`, [`format.c`](format.c) / `.h`, [`reformat.c`](reformat.c) / `.h`, [`bootstrap.c`](bootstrap.c) / `.h` | Depth/indent/block-lookup cache, pure indentation, source reformat, startup loading |
| **Frame flow** | *program model → GL* |
| [`flatten.c`](flatten.c) / `.h` | Source → flat program (unroll/inline/resolve `if`) |
| [`expr_program.c`](expr_program.c) / `.h` | Compiled expression programs and ephemeral per-line cache |
| [`flatten_expr.c`](flatten_expr.c) / `.h` | Internal cache/evaluation/dependency boundary used by flatten and rebake |
| [`flatten_query.c`](flatten_query.c) / `.h` | Live flat-program cost/cursor queries |
| [`init_state.h`](init_state.h) | Read-only access to the effective REPL-modifiable state commands applied by `init()` |
| [`gl_state_inspector.c`](gl_state_inspector.c) / `.h` | Pure source-checkpoint fold of every generated `init()`/`display()` state write plus REPL commands through the selected point; includes generated lights, camera/modelview, render toggles, and attribute-stack depth, and reports touched values, their latest source, and OpenGL 2.1 initial defaults without issuing GL calls |
| [`autonormal.c`](autonormal.c) | Auto-generated `glNormal3f` maintenance |
| [`executor.c`](executor.c) / `.h` | Walks the flat program emitting live GL calls (the only live-GL TU) |
| [`transform_utils.h`](transform_utils.h) | Shared GL matrix tracking helpers (no executor link dependency) |
| [`pipeline.h`](pipeline.h) | Controller-facing frame entry points (flatten/autonormal/refresh) |
| [`program_query.c`](program_query.c) / `.h`, [`geometry_query.h`](geometry_query.h) | Read-only queries over the source/flat program |
| **State & ownership** | |
| [`state.c`](state.c) / `.h`, [`state_views.h`](state_views.h), [`state_owners.h`](state_owners.h) | [`ReplRuntimeState`](state.h#L18) storage + capture/restore + typed read/mut facades |
| [`state_notify.h`](state_notify.h) | Dirty-flag invalidation entry points |
| [`time.c`](time.c) / `.h` | The predefined `t` animation clock |
| [`host_effects.c`](host_effects.c) / `.h` | Host side-effect bridge (status, cursor, completion, tutorial teardown) |
| **Persistence (save/load)** | |
| [`export.c`](export.c), [`import.c`](import.c) | Writer half (file emit, header refresh) and reader half (import state machine) |
| [`export_setup.c`](export_setup.c), [`export_prologue.c`](export_prologue.c), [`export_display.c`](export_display.c), [`export_cmd_writer.c`](export_cmd_writer.c) | C boilerplate, globals/predef prologue, `display()` body, per-command C emission |
| [`export.h`](export.h), [`export_internal.h`](export_internal.h), [`export_state.h`](export_state.h), [`export_format_shared.h`](export_format_shared.h) | Export/import API and shared state-text dimensions |
| **Scenes & workspaces** | |
| [`scenes.c`](scenes.c) / `.h`, [`scene_snapshot.c`](scene_snapshot.c) / `.h` | User-scene slots (LRU, promotion); copyable scene payload |
| [`workspace_io.c`](workspace_io.c) / `.h`, [`cfg_baseline.c`](cfg_baseline.c) / `.h` | Workspace filesystem + file-naming mechanics; flat key/value config bag |
| **Program-adjacent data** | |
| [`examples.c`](examples.c) / `.h`, [`example_loader.c`](example_loader.c) / `.h` | Built-in example catalog facade; example load + `.glr` snippets, `.c` import sources, and `@cfg` / `// camera` metadata. Authored source lives in repository-level `examples/catalog.ini` and `examples/scenes/`; `--examples-dir` can replace the compiled-in catalog at runtime for authoring |
| [`command_descriptions.c`](command_descriptions.c) / `.h` | Lookup facade for the generated right-click GL command help catalog. Authored source lives in [`command_descriptions.txt`](command_descriptions.txt); `glEnable`/`glDisable` select capability-specific entries by `args[0]` |
| [`tutorials.c`](tutorials.c) / `.h`, [`catalog_tags.h`](catalog_tags.h) | Tutorial catalog; shared example/tutorial tag-bit helper |
| [`help_text.c`](help_text.c) / `.h`, [`keymap_format.c`](keymap_format.c) | F1 help-text tables; user-facing keybinding labels |

**Boundary:** `src/repl` owns the program model and compiler. It does **not**
own editor state, UI state, replay *runtime* state (a `src/subsystems/` peer), or
live input dispatch. The only live GL in this layer is [`executor.c`](executor.c).
`ARCHITECTURE.md` §10 lists the guards that ratchet these boundaries.
