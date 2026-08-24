# File-Scope Function Definitions In The Code Panel

## Status - IN REVIEW (revised 2026-08-24 after design read)

Investigation only; nothing implemented. Revised from the first draft,
which invented a "prologue invariant" that already exists as
[`doc_order.h`](../../../src/repl/doc_order.h), treated a tutorial
contract change as a "hardening" step of the same project, and proposed
a parallel row array where two scalars do.

The work splits into **two projects**. Project 1 (the view) is
self-contained and ready to specify. Project 2 (making the order total
for every entry point) is gated on a tutorial decision and must not be
folded into it.

## Summary

Make the code panel render user function definitions - and the global
`static float` declarations that precede them - *above* the generated
`void display(void) {` line instead of nested inside it, so the panel
reads like the C the exporter already writes. The document stays a
single linear `GLCmd[]`. What changes is where the generated chrome is
spliced and what base indent a row carries.

## The Order This Projects Already Exists

This plan does not introduce an ordering rule. `.glr` has a phase
machine ([`src/repl/doc_order.h`](../../../src/repl/doc_order.h)):

    DECLS -> FUNCS -> CAMERA -> BODY

`repl_doc_order_offer()` runs in the catalog loader
(`src/repl/example_loader.c`), the file importer gated on a `.glr`
source (`src/repl/import.c`), the tutorial setup-scaffold loader
(`src/subsystems/tutorial/tutorial_runner.c`) and
`--lint-scenes` (`src/app/boot/glr_lint_scenes.c`). A hand-written
`.glr` with a mid-document definition is *rejected*
(`REPL_DOC_ORDER_FUNC_LATE`), not silently loaded. The writer half is
`src/repl/export_glr.c`, which re-emits the live document in three
passes keyed on `GlrRowPhase` (`DECLS` / `FUNCS` / `BODY`) rather than
verbatim.

Camera rows never enter the editor document - the camera reader consumes
them - so the *live* document is `[comments][decls][funcs][body]`.

That is the boundary the panel should project. It is **not**
`compile_decl_prologue_end` (decls only, `src/repl/compile.c:1311`) and
**not** the commit hoist walk in `src/editor/commit.c:508-556` (which
includes the tutorial clear pair).

Consequences already true, and load-bearing for the sizing:

- **Export already hoists.** `write_func_defs_as_c()`
  (`src/repl/export_prologue.c:422`) emits each `CMD_FUNC_DEF` as a
  file-scope `static void`, forward declarations first. The
  `/* @func-body N */` markers in the display body
  (`src/repl/export_cmd_writer.c:290`, `:682`) exist only so `.c` import
  can restore document position.
- **Flatten is position-independent.** `flatten_range()` skips whole
  func-def blocks (`src/repl/flatten.c:1737`) and resolves calls through
  `ctx.func_def_idx[slot]` (`:1887`).
- **Hit-testing, replay markers, assign-plot and edit guides are keyed
  on `row->source_line_idx`**, so splicing chrome mid-walk does not
  touch them.
- **`code_focus` already hides all chrome** via
  `repl_code_panel_chrome_visible()`, so that mode needs no case.

**Declarations are not an optional companion.** The live document is
`[decls][funcs][body]`; one splice point after the function prefix
necessarily puts both above `display()`. "Functions only" would need a
second splice point or an illegal `[funcs][decls]` order. They move
together or not at all.

## Project 1 - The View

One change, one golden regeneration. Indent, chrome and dump ship
together: an indent-first intermediate renders column-0 function headers
*inside* `void display(void) {`, which is a worse lie than the one being
fixed, and costs a second golden regeneration.

### 1a. One boundary helper

Add to `src/repl/source_scope.c`, e.g.
`repl_source_scope_view_display_body_start(view)`: from row 0, skip
comments, blanks, top-level `CMD_VAR_DECLARE`, and **closed** depth-0
`CMD_FUNC_DEF` blocks; stop at the first executable row or at an
unclosed function header. Returns one document index, `at`.

Closed-block only, because `find_block_end()` returns `document_count`
for an unclosed block and a naive boundary would swallow the document.
Note the unclosed case is *not* reachable by interactive typing -
`editor_compile_func_def` inserts `fd` and `fe` together
(`src/editor/commit.c:640-660`) - it is reachable via `--watch`, a
partial paste, and the load path's header-only `INSERT_ONE`.

Not frame-hot: reformat walks each row once per edit. A step function
over `at` is enough; do not add a fifth prefix array to
`ReplSourceScopeView`.

### 1b. Panel chrome split (`src/ui/app/repl_code_panel.c`)

`repl_code_panel_add_header_rows()` (`:2373`) and its row-count twin
`repl_code_panel_header_row_count()` (`:211`) currently emit all chrome
before row 0. Split into:

- **file-scope chrome** - workspace header, includes, GL vector helpers -
  before row 0, as today. This stays `layout.header_rows`.
- **display-open chrome** - `g_display_header`, lights, camera,
  `g_header_post` - spliced at `at`.

Layout representation: keep `header_rows`, add `display_open_rows` and
`display_open_at` to `UiReplCodePanelLayout`
(`src/ui/app/repl_code_panel.h:86`). Prefix sum becomes
`header_rows + sum(cmd+replay before i) + (i >= at ? display_open_rows : 0)`.
Three fields that cannot disagree; a third parallel array sized
`MAX_EDITOR_COMMANDS` to encode one splice as zeros plus a spike can.

Sites: 12 `header_rows` uses in `repl_code_panel.c`, principally
`repl_code_panel_rows_before_cmd()` (`:318`),
`ui_repl_code_panel_target_for_doc_line()` (`:490`) and the
cursor/follow helpers. Keeping `header_rows` meaning "file-scope chrome
before row 0" keeps the existing test consumers compiling and mostly
correct (see Test Impact).

### 1c. Dump twin (`src/repl/export.c:526-586`)

`repl_dump_code_panel_text()` re-derives the same sections
independently. Section order becomes:

    --- header_pre ---
    --- gl_vector_helpers ---
    --- source_prologue ---      rows [0, at); omitted when at == 0
    --- display_header ---
    --- lights_pre_camera ---
    --- camera ---
    --- header_post ---
    --- source ---               rows [at, count)
    --- render_state ---

`--- source ---` keeps its name for the body rows so tests that inspect
the dump do not churn on a rename. The 40 goldens are byte-exact, so the
new section name is load-bearing - fix it here before regenerating.

### 1d. Indent base

Six helpers in `src/repl/source_scope.c` start at `2 +` (`:261`, `:271`,
`:284`, `:301`, `:322`, `:339`). Base becomes
`pos < at ? 0 : 2`, via one shared `source_scope_base_indent(view, pos)`.

**Use the boundary, not an "inside a depth-0 `CMD_FUNC_DEF`"
predicate.** The boundary form covers three cases the narrow predicate
misses: top-level declarations; the comment/blank run attached to a
function definition, which export deliberately emits *with* the function
(`src/repl/export_prologue.c:441`, `comment_run_attached_func_idx()` in
`export_cmd_writer.c:277`) but which `reformat.c:332` indents from its
own position; and a mid-document function the panel still draws inside
`display()`, which the narrow predicate would wrongly dedent.

The formula is otherwise unchanged and stays consistent:
`base + 2*td + 2*bd + 2*kd + 2*md`. A top-level `def` gets 0, its body 2,
a loop inside it 4, the closing `}` 0; a top-level body row still gets 2;
a function-scoped local (`REPL_VAR_IDX_LOCAL`) has `kd >= 1` and lands at
2.

### 1e. Direct commit paths that bypass the helper

Two sites format indentation without asking the source scope:

- `src/repl/compile.c:1593` - `format_decl_text(&parsed, "  ", ...)`
  hard-codes the two-space indent for a new global declaration. Once
  declarations sit at base 0 this is wrong; it must ask for the indent at
  the insert position.
- `src/editor/commit.c:647` - `repl_source_scope_cmd_indent(0, ...)` for
  a new function header. Position 0 is not guaranteed to be a file-scope
  position (it is not, if the clear-pair exception survives - see
  Project 2). Query the indent at the computed insert position instead.

### 1f. `REPL_GLR_BASE_INDENT`

`glr_scene_write_line()` (`src/repl/export_glr.c:43-53`) strips
`REPL_GLR_BASE_INDENT` (2) from **every** row on save, which is correct
today because every row carries the display-body indent. With mixed
bases it flattens function bodies (in-memory 2 -> on-disk 0) and
declarations. Strip 2 only from `GLR_ROW_PHASE_BODY` rows; write
`DECLS` and `FUNCS` rows verbatim.

Load re-derives indentation, so this is a fidelity bug in checked-in
files rather than a runtime one - but every scene regenerated after the
change would be flattened, so it must land in the same patch.

### 1g. Missed call sites

- **`scroll_to_display_function()`** (`src/repl/bootstrap.c:29-42`)
  scans `g_header_pre[]` for `REPL_EXPORT_DISPLAY_OPEN_LINE`, which is
  not in that array (it is `g_display_header[0]`,
  `src/repl/export_setup.c:132`). The loop never matches and the
  function lands at the end of the includes - accidentally near
  `display()` today, and at the *start of the declarations* after the
  split. It has to target the splice.
- **`tests/test_repl_editor.c:260,332`** reimplements the header counter
  (`code_panel_header_row_count` / `code_panel_mouse_y_for_cmd`) for
  mouse-Y math and assumes all chrome precedes command 0. It aims at the
  wrong row once display chrome moves.
- **`tests/test_repl_core_extra.c:130`** hard-asserts
  `repl_source_scope_cmd_indent_chars(0) == 2`.

### 1h. Two placement calls to make

- **The scratch decoration line.** The panel draws
  `"  float A[16], B[16], C[16];"`
  (`REPL_CODE_PANEL_SCRATCH_DECL_LINE`) as display-body chrome; export
  emits `static float A[16] = {0};` at file scope
  (`emit_export_scratch_globals_section`, `src/repl/export.c:329-342`).
  If the point is that the panel reads like the C, this belongs with the
  file-scope chrome and should lose its two-space indent (or take the
  `static float` spelling). Keeping it display-adjacent is a proximity
  hint at the cost of one remaining lie. **Decide before coding.**
- **The insert ghost at the splice.** `target_for_doc_line()` emits the
  insert row before `cmd_idx`. At `cmd_idx == at` it is ambiguous
  whether the new row is the last file-scope row or the first display
  body row. **Pick one and test it.**

## Decisions That Block Project 1

| Decision | Resolution |
| --- | --- |
| Closed-block boundary | Yes. Last closed depth-0 func block; stop at first executable row or unclosed header. |
| Unclosed trailing function | Boundary stops *at* it, so it renders inside the body; chrome shifts down by its row count when `}` lands. Reachable only from `--watch` / partial paste / header-only load, all of which already reflow the panel wholesale. State it and test it. |
| Scratch decoration line | Open - see 1h. |
| Insert ghost at the splice | Open - see 1h. |
| Clear-pair exception | Survives Project 1. Tutorials keep showing their clear pair above `display()`. |
| Load-time hoist | Not in Project 1. |

## Project 2 - Making The Order Total (not approved)

Project 1 projects the order that already holds for `.glr`. It does not
hold for every path into the live document. Each of these is a separate
decision, and none of them is a step of Project 1:

- **The commit hoist vs. the tutorial contract.** Every tutorial injects
  four locked prelude rows (`g_tutorial_scene_prelude`,
  `src/subsystems/tutorial/tutorial_runner.c:722`: clear-color comment,
  `glClearColor`, comment, `glClear`), and `tutorial_guard_source_change`
  rejects any insert at or above a locked line. The current hoist works
  *because* it parks functions below them
  (`src/editor/commit.c:536-539`). The catalog validator already forbids
  a func-open step combined with a setup scaffold, and forbids a
  func-open after other top-level commands, in both cases because "func
  relocation would desync rows" (`src/repl/tutorials.c:2146-2151`,
  `:2227-2243`). Changing the rule to "above all executable rows" is
  either rejected by that guard or becomes a tutorial project: remapping
  locked indices, instruction-comment attachment, and the validator.
- **`.c` import ignoring `@func-body` position.** If a hoist happens on
  load it needs its own helper (or `export_glr.c`'s phase walk) called
  from the `.c` import finish path **after**
  `editor_undo_note_wholesale_replacement()`. Do **not** piggy-back it
  on `repl_reformat_program()` (`src/repl/import.c:3338`): that function
  rewrites canonical text and indentation only, and making it a
  reorderer would move functions on an ordinary `;` reformat, outside
  the undo transaction. Do not hoist `.glr` - keep rejecting
  non-canonical files.
- **`ReplSceneRowMap` (`src/repl/scene_load.h:65-82`)** documents that
  entries are written as rows are fed **with no fixup pass, because the
  importer only ever appends**. A load-time hoist breaks that: `doc_row`
  and `hole_doc_row` would name the wrong commands, and the `--watch`
  external-editor cursor mapping (`src/app/glr_extedit.c`) resolves
  through it. Either hoist before mappings are recorded, or add the
  fixup pass that header says would be required.
- **Paste of a func-def range.** `repl_compile_func_def` inserts at the
  cursor with no hoist, so pasting a definition into the body produces
  exactly the non-prefix document Project 1's boundary cannot represent
  (it renders inside `display()`). If the order must be total, paste
  should re-hoist rather than refuse.
- **Cut/delete.** For the record, the first draft had this wrong:
  `repl_compile_delete_range()` does not block cutting declarations -
  it enforces reference integrity. Copy/cut of decl rows is blocked in
  `src/editor/clipboard.c:450` via `repl_range_contains_var_decl`.
  Function definitions can be cut today. Do not invent a second
  refuse-reason modeled on declarations unless "delete a definition with
  live calls" is independently a thing we want to block.

Leave commit, import, clipboard and tutorials alone until Project 2 is an
explicit yes.

## Test And Golden Impact (Project 1)

- 40 byte-exact panel goldens,
  `tests/testdata/repl_examples_ui/00.golden.txt`-`39.golden.txt`. The
  section split and the dedent land together; regenerate once, from a
  debug build.
- `tests/test_repl_core_extra.c:130` - the `indent_chars(0) == 2`
  assertion becomes 0 (or is re-pinned to a body position).
- `tests/test_repl_editor.c:260,332` - the private header counter and
  its mouse-Y helper.
- `layout.header_rows` consumers: `tests/test_ui.c:921,1569,1728,1766`,
  `tests/test_repl_core_commit.c:154`,
  `tests/test_repl_code_panel_document.c:806-918,1072`. These scroll past
  chrome; with `header_rows` retained as file-scope chrome they still
  compile, but several now scroll to the declarations rather than to the
  first body row and need re-pinning against `display_open_at`.
- `test_repl_core_format` (indentation), `test_camera_header_parity`
  (canonical text), `test_repl_core_examples` and `test_repl_core_io`
  (definition-order and round-trip assertions beyond the panel goldens).
- `test_export_trace_parity` compares argument values and is unaffected.
- `examples/scenes/*.glr` and `tests/scenes/**` regenerate cosmetically
  once 1f is in.

New coverage: the boundary helper itself; an unclosed trailing function;
the insert ghost at the splice; `code_focus`; a `.glr` save round-trip
proving function-body indent survives (the 1f regression).

## Not Touched

Flatten, executor, replay, edit guides, the export writer's structure,
and - in Project 1 - commit, import, clipboard and tutorials. No
`CmdType`, spec-table, config-key or keymap changes; no `@cfg` slug, so
no config-golden churn beyond the panel goldens.

Tours are unaffected: `code:N` is a document index, and moving chrome
does not renumber documents. A Project 2 hoist would retarget them, but
catalog function scenes are already ordered, so it is identity there.
