# File-Scope Function Definitions In The Code Panel

## Status - IN REVIEW (2026-08-24 investigation)

Investigation only; nothing implemented. The finding is that the
semantic half of this is already done - export, flatten and the editor
commit hoist already treat a `CMD_FUNC_DEF` as file-scope. What remains
is the code-panel projection (which still draws function bodies inside
`display()`) and the indentation baked into canonical row text.

## Summary

Make the code panel render user function definitions - and, by the same
argument, global `static float` declarations - *above* the generated
`void display(void) {` line instead of nested inside it, so the panel
reads like the C the exporter already writes. The document stays a
single linear `GLCmd[]`; only the placement of the generated chrome and
the base indent change.

## What Is Already True

These are load-bearing for the sizing, and each was verified:

- **Export already hoists.** `write_func_defs_as_c()`
  (`src/repl/export_prologue.c:422`) emits every `CMD_FUNC_DEF` as a
  file-scope `static void name(float a, ...)`, forward declarations
  first. The display body writes a `/* @func-body N */` marker in its
  place (`src/repl/export_cmd_writer.c:290`, `:682`) purely so import
  can restore the original document position.
- **Flatten is position-independent.** `flatten_range()` skips whole
  func-def blocks (`src/repl/flatten.c:1737`) and resolves calls through
  `ctx.func_def_idx[slot]` (`:1887`). A definition's document position
  carries no execution meaning, which is what makes this change
  presentational rather than semantic.
- **The editor already hoists new definitions.**
  `editor_compile_func_def` relocates a newly typed def to just past the
  declaration prologue and any existing defs
  (`src/editor/commit.c:508-556`).
- **The corpus already obeys the invariant.** All 40
  `examples/scenes/*.glr` and every scene in `tests/scenes/{stress,general}`
  have nothing but comments, blank lines, `float` declarations and
  `@camera` rows before the first function definition.
- **Declarations have the same story.** Export emits `static float`
  decls as file-scope globals plus `@declare` markers
  (`src/repl/export_cmd_writer.c:380-400`), and the decl prologue is
  already a hoisted contiguous prefix (`compile_decl_prologue_end`,
  `src/repl/compile.c:1311`). The panel is the only place that still
  draws them inside `display()`.

## Key Changes

### 1. Panel chrome placement (`src/ui/app/repl_code_panel.c`)

`repl_code_panel_add_header_rows()` (`:2373`) emits all chrome before
document row 0: workspace header, includes, GL vector helpers,
`g_display_header` (`void display(void) {`, `src/repl/export_setup.c:132`),
lights, camera, `g_header_post`, scratch-decl line. Its row-count twin is
`repl_code_panel_header_row_count()` (`:211`).

Split that into two groups:

- **file-scope chrome** - workspace header, includes, GL vector helpers -
  emitted before row 0, as today.
- **display-open chrome** - `g_display_header`, lights, camera,
  `g_header_post`, scratch-decl line - emitted at a computed boundary
  index, after the declaration + function prologue.

The layout math is the real cost. `header_rows`
(`src/ui/app/repl_code_panel.h:86`) is one scalar that
`repl_code_panel_rows_before_cmd()` (`:318`),
`ui_repl_code_panel_target_for_doc_line()` (`:490`) and the
cursor/follow helpers prefix-sum against. Preferred shape: a
`cmd_pre_rows[]` array parallel to the existing `cmd_main_rows[]` /
`replay_extra_rows[]`, so the forward prefix-sum and the reverse
row-to-command lookup each stay one loop and cannot disagree. Roughly 15
`header_rows` sites, all inside that one file.

Nothing else in the panel needs work: hit-testing, replay markers,
assign-plot targets and edit guides are all keyed on
`row->source_line_idx`, not on row arithmetic. `code_focus` hides chrome
entirely, so that mode needs no special case.

### 2. The parallel dump (`src/repl/export.c:526-586`)

`repl_dump_code_panel_text()` re-derives the same sections
independently for the goldens. It needs the matching split - a
`--- source_prologue ---` section emitted ahead of
`--- display_header ---`.

### 3. Indentation (`src/repl/source_scope.c`)

Six indent helpers all start at `2 +` (`:261`, `:271`, `:284`, `:301`,
`:322`, `:339`). That 2 *is* the display-body indent, and it is baked
into every row's canonical text. Rows at or inside a depth-0
`CMD_FUNC_DEF` need base 0 instead.

Preferred shape: one more prefix array in the source-scope view
alongside `block_depth_prefix[]` and friends, plus a single
`source_scope_base_indent(view, pos)` the six helpers share. Everything
downstream then follows for free - canonical text
(`src/repl/reformat.c:217`), the live input line
(`glr_ctrl_active_indent_chars`, `src/app/glr_ctrl.c:2107`), the
autocomplete ghost.

Side benefit: exported C copies canonical text verbatim, so today's
top-level function bodies in the generated `.c` are over-indented by 2.
This fixes that at the same time.

### 4. Harden the prologue invariant

Three ways a non-prefix function definition can still exist:

- The commit hoist deliberately parks functions *below* a leading
  `glClearColor` / `glClear` pair (`src/editor/commit.c:536-539`, for the
  tutorial-injected prelude rows). Those clears would then be stranded
  above `display() {`. The rule has to become "above all executable
  rows", which changes established ordering for scenes that relied on
  the old placement.
- **Import restores functions to their original mid-document slot** via
  the `@func-body` markers (`src/repl/import.c:909`, `:2447`, `:3403`),
  so an older exported `.c` can produce a non-prefix document. Fix is a
  hoist on the load path; `src/repl/import.c:3338` already calls
  `repl_reformat_program()` at the end and is the natural hook. Keep
  parsing the markers for back-compat, ignore the position they name.
- A hand-written `.glr` with a definition in the middle hits the same
  thing.

Also worth deciding: `repl_compile_delete_range()` blocks cutting decl
rows; func-def rows have no equivalent guard, and cut/paste across the
new boundary acquires a meaning it did not have before.

### 5. Do the global declarations at the same time

Recommended, not optional-feeling: the decl prologue is already a
hoisted contiguous prefix, and export already emits those rows as
file-scope globals. Moving functions alone leaves
globals-inside-`display()` as the remaining lie, and makes the boundary
two special cases instead of one index -
`[decls][funcs] | display() {`.

## Decisions To Make Before Coding

- **Boundary is defined against *closed* blocks.** While a function
  block is being typed at end-of-document, `find_block_end()` returns
  `document_count`, so a naive boundary swallows the rest of the
  document and `display() {` renders at the bottom of the panel, jumping
  back when the block closes. Use the end of the last *closed* prologue
  function block.
- **Whether the clear-pair prologue exception survives.** Changing the
  commit hoist rule to "above all executable rows" is what makes the
  invariant total, but it reorders existing tutorial scaffolds.
- **Whether a load-time hoist is acceptable.** It changes document row
  indices at load. `@plot` tags ride the row's canonical text so they
  are safe; the hoist happens before the document goes live, so the undo
  ring is not implicated - but this should be confirmed against
  `editor_undo_clear()` call sites.

## Test And Golden Impact

- 40 panel goldens in `tests/testdata/repl_examples_ui/*.golden.txt` -
  both the section split and the dedent land here. Regeneration needs a
  debug build.
- `test_repl_core_format` (indentation), `test_camera_header_parity`
  (canonical text), `test_repl_core_commit` / `test_repl_editor` (hoist
  rule), the `test_ui_*` row-layout tests.
- `test_export_trace_parity` compares argument values, so it is
  unaffected.
- `examples/scenes/*.glr` and `tests/scenes/**` are cosmetic only -
  indentation is recomputed at load - but the checked-in text should be
  regenerated to stay honest.

New coverage worth adding: the boundary computation itself, an open
function block at end-of-document, an imported non-prefix document, and
`code_focus`.

## Not Touched

Flatten, executor, replay, edit guides, and the structure of the export
writer. No `CmdType`, spec-table, config-key or keymap changes; no
`@cfg` slug and therefore no config-golden churn beyond the panel
goldens above.

## Suggested Sequencing

1. Indent base (`source_scope.c`) - visible, self-contained, forces the
   golden regeneration exactly once.
2. Panel chrome split plus the `repl_dump_code_panel_text()` twin.
3. Invariant hardening in the commit and import hoists.
