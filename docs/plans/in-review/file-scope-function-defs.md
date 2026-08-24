# File-Scope Function Definitions In The Code Panel

## Status - PROJECT 1 COMPLETE; PROJECT 2 NOT APPROVED (2026-08-24)

Revision 2 closed the second read's blockers: the boundary helper's
comment rule (it ate body-attached comments) and the two placement calls
1h had left open. Revision 3 closed the third read's: the base-indent
vs. `cmd_indent` conflation in 1d, and the boundary comparison for a
not-yet-inserted row in 1e - which must compare the returned
`insert_pos` *untranslated*. Project 2 remains gated and must not be
folded in.

### Implementation log

**Stage 1 landed** - the boundary helper, the base-indent switch, the
three formatting paths of 1e, and the two re-derivation sites stage 1
turned up (1i below). Suite green, 35 of 40 panel goldens regenerated.

**Stage 2 landed** (`82c7085f`) - the panel chrome split (1b), the dump
twin (1c), the `.glr` writer strip (1f), the bootstrap splice (1g), and
the always-visible `display()` framing added in 1j.

**Completion review implemented** - explicit function-scene `.glr` frames
(1k), non-focus `void` decoration on editable user function headers (1l),
the host-effects 17th-callback rider, byte-stable scene re-save coverage,
focus/framing and insert-splice tests, and the missed scratch-array dedent.
The review also removed the former CAMERA -> FUNCS compatibility edge: the
new format has one shape, not a legacy alternate.

**Post-completion review implemented** - real end-of-document commits now
cover both the plain and leading-comment relocation cases from 1e; a
functions-only panel fixture covers the `at == document_count` trailing-slot
splice from 1b. The review also corrected two stale comments, routed the
commit test's mouse-row helper through the splice-aware layout API, and
limited `void` decoration to definitions that actually render at file scope.
The new relocation test also exposed that the attached comment text retained
its former body base even though the header and closer were rebased; the
commit plan now shifts the carried run by the same base delta.

**Corpus follow-up implemented** - the two remaining function-bearing opt-in
fixtures now use the required explicit frame, with globals/functions outside
and camera/body rows inside. The clear-pair decision text was also corrected:
the scanner's existing behavior is intentional because executable clear calls
belong inside `display()`; moving a tutorial function above the locked prelude
remains deferred to Project 2.

**Full trace-parity follow-up implemented** - catalog cases now use the real
`.glr` loader so camera and explicit-display rows are consumed as format syntax.
The run also exposed a pre-existing formatter conflation: bitmap `label(%f)`
is compact again, while only `console(%f)` keeps stable six-character fields;
the generated C helpers preserve the same split. Each case's trace-driver and
standalone compiler checks now run concurrently; the curated 12-case run fell
from 23.46s to 17.41s locally without reducing coverage.

**Mandatory-frame simplification implemented; corpus migration deferred** -
every `.glr` now requires exactly one `display() { ... }` frame, regardless of
whether it defines a function. The writer always emits it and the phase machine
no longer switches grammar based on function presence. Per request, the
mechanical wrapper insertion across most checked-in `.glr` scenes remains
outstanding; three representative catalog scenes were migrated to exercise the
real loader, and catalog/corpus gates are expected to reject the rest until the
bulk migration lands.

**Previous completion verification (before the mandatory-frame follow-up)** -
`make test-stubs` (86 binaries, 32,436 assertions),
`make test-scenes` (3 binaries, 8,302 assertions),
`make check-state-ownership`, `make check-c99`, `make check-formatted`, both
catalog/doc example guards, `git diff --check`, and
`REPL_SCENE_CORPUS=1 make run-test-export-trace-parity ARGS='--full'`
(159/159, including two documented XFAILs).

**Mandatory-frame verification** - the document-order, camera-apply,
REPL-state and scene-loader suites pass against the new grammar, including
function-free export, split-brace rejection inside `display()`, and physical
row-map coverage across the consumed wrapper. A three-scene runtime catalog
covering the migrated logo, scratch-array and analytic-normal scenes passes
export trace parity (15/15, including the existing curated XFAIL), and the C99,
formatting and state-ownership guards pass. The full catalog gates remain
intentionally red: 98 of the 148 checked-in `.glr` files still await the
separate mechanical wrapper insertion requested above.

### Corrections found while implementing

- **1g over-lists one site.** `tests/test_repl_code_panel_syntax.c:214`
  passes a string *literal* to `ui_repl_code_panel_generated_category()`,
  not `REPL_CODE_PANEL_SCRATCH_DECL_LINE`. Dropping the constant's two
  spaces does not touch it.
- **1f is a byte-stability requirement, not a regeneration.** Working
  the arithmetic through: a function-body row goes in-memory 4 -> 2 while
  its strip goes 2 -> 0; headers and declarations go 2 -> 0 with the strip
  2 -> 0. On-disk `.glr` output is unchanged in every phase. So the test
  is `git diff --exit-code` over `examples/scenes/` after a save
  round-trip, and *without* 1f the same round-trip flattens function
  bodies. The checked-in files staying untouched during stage 1 proves
  nothing on its own - nothing rewrote them.
- **The UI needs the boundary on the snapshot.** 1a caches `at` on
  `ReplSourceScopeView`, but `repl_code_panel.c` is pure over
  `UiRenderSnapshot` and never calls `repl_source_scope_*`. `at` reaches
  it as a snapshot field filled beside `active_indent_chars`
  (`glr_ctrl.c`), which is the existing precedent for exactly this.
- **1e needs a statement reorder in `commit.c`, not just a swapped
  argument.** `insert_pos` was computed *after* the indent block it has
  to feed; the call moves up.

## 1i. Re-derivation sites (found during stage 1, not in the original plan)

Making the base indent a function of the whole document creates an
obligation the original plan did not name: **any bulk mutation that can
move the boundary must re-derive indentation afterwards.** A row's stored
text carries its indent, but the boundary depends on rows that may not
exist yet at the moment that text is written. Two sites needed it, and
they need *different* tools:

- **The catalog loader** (`src/repl/example_loader.c`) now runs
  `repl_reformat_program()` at its finish, the same finish
  `src/repl/import.c` has always run. Without it the two loaders disagree
  on canonical text for the same scene and `test_camera_header_parity`
  fails on every function scene: the comment run introducing a scene's
  declarations only becomes file-scope prologue once those declarations
  have been fed, and the file path self-corrected via reformat while the
  catalog path kept its incremental guess. This is provably a no-op
  otherwise - the two paths agreed before, and the file path already
  reformatted, so reformat was already a no-op on catalog text.

- **Comment toggle** (`src/repl/comment_toggle.c`) must NOT use
  `repl_reformat_program()`. Uncommenting restores rows one at a time
  against a document where the block's closing brace is still a comment -
  i.e. against an *unclosed* definition - so every restored row gets the
  body base. But reformat re-canonicalizes text, and it trims a blank
  comment row's `// ` to `//`, after which the uncomment pass no longer
  recognizes its own prefix; a comment/uncomment round trip stops being
  byte-identical. The tool is instead
  `repl_reindent_after_boundary_move(at_before)`
  (`src/repl/reformat.c`): only the *base* term can change, so a row that
  changed side shifts by exactly +/-2 and every other row and byte is left
  alone. No per-command dispatch, no re-canonicalization.

The general rule to apply to any future site: **indent-only fixups use
`repl_reindent_after_boundary_move`; only a loader that is already
re-deriving canonical text may use `repl_reformat_program`.**

## 1j. Always-visible `display()` framing (added 2026-08-24)

Two requirements beyond the original plan:

- The `void display(void) {` line and its matching `}` are **no longer
  chrome**. They render in `code_focus` mode too, so the body is always
  visibly enclosed and the file-scope declarations and function
  definitions are always visibly outside it. Everything else that is
  chrome today stays chrome: `glLoadIdentity`, `glPushAttrib`, the lights
  and camera stanzas, `glPopAttrib`, `glutSwapBuffers`, and the whole
  `reshape`/`keyboard`/`init` footer.
- This retires the plan's note that "`code_focus` already hides all
  chrome ... so that mode needs no case". Focus mode now has exactly one
  case: the two framing rows.

Note `g_header_post[]` is empty (`src/repl/export_setup.c:294`), so the
"display-open chrome" of 1b is `g_display_header` + lights + camera; the
display closer is `g_footer_pre_init[3]`.

## 1k. Mandatory explicit `display() { }` in `.glr` (added 2026-08-24)

The `.glr` format has one shape:

- Every scene carries exactly one explicit `display() {` ... `}` around its
  camera and body, including scenes with no function definitions.
- Function definitions sit outside it, exactly as they do in exported C and in
  the code panel.
- Camera rows live inside that explicit frame; top-level variable
  declarations live outside it with the function definitions.
- There is **no backward-compatible reader path**. CAMERA -> FUNCS and a
  file without the frame are rejected, which keeps the writer, importer,
  catalog loader, tutorial loader and linter on one grammar.

Rationale: making frame presence conditional on function presence leaves two
permanent grammars and requires EOF-sensitive `saw_function` state. The two
wrapper lines are cheap; requiring them makes the phase boundary explicit and
matches the C representation for every scene.

Touches the writer (`src/repl/export_glr.c`), the reader
(`src/repl/import.c`), the phase machine
(`src/repl/doc_order.{c,h}` - the `FUNCS -> BODY` transition becomes
observable rather than inferred), `--lint-scenes`
(`src/app/boot/glr_lint_scenes.c`), the scene formatter
(`scripts/format_scenes.py`), and requires a mechanical wrapper insertion in
every checked-in `.glr` that does not already carry one (`examples/scenes/`,
`tests/scenes/**`). That migration is deliberately separate from this change.

**Ordering against 1f.** 1f (strip the base indent per phase, not
uniformly) has to land first or with this: body rows inside an explicit
`display() {` are still written at column 0 on disk - the wrapper is
structure, not indentation, and the loader re-derives indent either way.

## 1l. Non-focus C return type on user function headers (added 2026-08-24)

Full-chrome mode projects file-scope editable `func0() {` / `name(args) {`
rows as `void func0() {` / `void name(args) {`. Focus mode keeps the editable
REPL spelling. A mid-document definition that remains inside `display()`
(currently possible through the tutorial clear-pair placement rule or paste)
also keeps its indented REPL spelling; decorating it would imply a nested C
definition. This is display decoration only: the buffer and parser never
receive the prefix.

`UiTextPanelRow.display_prefix_chars` carries the synthetic width. The panel
adds it to active-input length, cursor and anchor coordinates, ignores it when
searching, and subtracts it from committed and active-input hits before a
`char_idx` reaches the editor. The dump twin applies the same full-mode
decoration to `source_prologue` rows. Coverage asserts exact text for committed
and active rows plus a click on the synthetic prefix mapping to editor column
zero.


The work splits into **two projects**. Project 1 (the view) is
self-contained and specified. Project 2 (making the order total for
every entry point) is gated on a tutorial decision.

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
(`src/subsystems/tutorial/tutorial_runner.c`) and `--lint-scenes`
(`src/app/boot/glr_lint_scenes.c`). A hand-written `.glr` with a
mid-document definition is *rejected* (`REPL_DOC_ORDER_FUNC_LATE`), not
silently loaded. The writer half is `src/repl/export_glr.c`, which
re-emits the live document in three passes keyed on `GlrRowPhase`
(`glr_scene_write_phase`, `:171`) rather than verbatim.

Camera rows and explicit display-wrapper rows never enter the editor document -
the loader consumes them - so the *live* document remains
`[comments][decls][funcs][body]`.

That is the boundary the panel should project. It is **not**
`compile_decl_prologue_end` (decls only, `src/repl/compile.c:1311`) and
**not** the commit hoist walk in `src/editor/commit.c:508-556` (which
includes the tutorial clear pair).

Consequences already true, and load-bearing for the sizing:

- **Export already hoists.** `write_func_defs_as_c()`
  (`src/repl/export_prologue.c:422`) emits each `CMD_FUNC_DEF` as a
  file-scope `static void`, forward declarations first. The
  `/* @func-body N */` markers in the display body
  (`src/repl/export_cmd_writer.c:290`, call at `:683`) exist only so
  `.c` import can restore document position.
- **Flatten is position-independent.** `flatten_range()` skips whole
  func-def blocks (`src/repl/flatten.c:1711`) and resolves calls through
  `ctx.func_def_idx[slot]`, filled at `:1862-1864`.
- **Hit-testing, replay markers, assign-plot and edit guides are keyed
  on `row->source_line_idx`**, so splicing chrome mid-walk does not
  touch them.
- **`code_focus` hides derived chrome but retains the frame.** It renders
  `display() {` / `}` and keeps editable user function headers free of the
  synthetic `void` prefix. Scratch arrays remain full-mode-only.

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

### 1a. One boundary helper, with forward comment attachment

Add to `src/repl/source_scope.c`, e.g.
`repl_source_scope_view_display_body_start(view)`, returning one
document index `at`.

**A comment/blank run belongs to the row it precedes**, which is the
rule `glr_scene_write_phase()` (`src/repl/export_glr.c:171-225`) already
uses when it carries each row's leading run into that row's phase. A
walk that skips comments unconditionally until the first executable row
gets this wrong in the common case:

    tri() { ... }
                                        <- blank
    // --- Render State ----------      <- belongs to glClearColor
    glClearColor(...);

The naive walk lands `at` on `glClearColor` and strands
`// --- Render State ---` above `void display(void) {`. Every catalog
function scene has this shape.

The rule:

- Buffer a pending comment/blank run rather than consuming it.
- A pending run is prologue **iff** its next non-comment row is a
  top-level `CMD_VAR_DECLARE` or a closed depth-0 `CMD_FUNC_DEF`.
- Otherwise the run *starts the body*: `at` is the run's first row, not
  the executable row after it.
- Stop at the first executable row, an unclosed `CMD_FUNC_DEF`, or a
  body-attached comment run.

Closed-block only, because `find_block_end()` returns `document_count`
for an unclosed block and a naive boundary would swallow the document.
The unclosed case is *not* reachable by interactive typing -
`editor_compile_func_def` inserts `fd` and `fe` together
(`src/editor/commit.c:640-672`) - it is reachable via `--watch`, a
partial paste, and the load path's header-only `INSERT_ONE`.

Point the implementation at `glr_scene_write_phase` for the attachment
rule, but **do not import `GlrRowPhase` into the UI or into
source_scope** - the helper answers one question with one index.

**Compute `at` once, at view-build time**, and cache it as a scalar on
`ReplSourceScopeView` (`src/repl/source_scope.h:41-50`) beside the
`built` flag, next to where the four prefix arrays are filled. Still not
a fifth prefix array - one `int`. Recomputing the walk inside each
per-row indent helper would make a reformat pass O(n^2).

### 1b. Panel chrome split (`src/ui/app/repl_code_panel.c`)

`repl_code_panel_add_header_rows()` (`:2376`) and its row-count twin
`repl_code_panel_header_row_count()` (`:211`) currently emit all chrome
before row 0. Split into:

- **file-scope chrome** - workspace header, includes, GL vector helpers,
  and the scratch decoration line (see 1h) - before row 0. This stays
  `layout.header_rows`.
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
cursor/follow helpers.

**`at == document_count` needs its own path.** A declarations- and
functions-only document puts the whole display-open chrome after the
last command and *before* the trailing input/placeholder row
(`repl_code_panel_trailing_row_count()` `:394`,
`repl_code_panel_add_trailing_document_row()` `:2590`). The trailing
slot is the `cmd_idx == document_count` arm of both the emission walk
and `target_for_doc_line`'s loop, so the `i >= at` term has to be
applied there too, not only inside the command loop. `total_lines`,
target lookup, cursor-follow and emission all need it. Test with a
scene that is nothing but a function.

### 1c. Dump twin (`src/repl/export.c:506-594`)

`repl_dump_code_panel_text()` re-derives the same sections
independently. Section order becomes:

    --- header_pre ---
    --- gl_vector_helpers ---
    (scratch decoration line, untagged - as today)
    --- source_prologue ---      rows [0, at); omitted when at == 0
    --- display_header ---
    --- lights_pre_camera ---
    --- camera ---
    --- header_post ---
    --- source ---               rows [at, count)
    --- render_state ---

The scratch line is printed untagged today, after `--- header_post ---`
(`src/repl/export.c:577`); 1h moves it up with the file-scope chrome, so
it moves here too. `--- source ---` keeps its name for the body rows so
tests that inspect the dump do not churn on a rename. The 40 goldens are
byte-exact, so both the new section name and the scratch line's new
position are load-bearing - settle them here before regenerating.

### 1d. Indent base

Six helpers in `src/repl/source_scope.c` open their sum with a literal
`2 +` (`:257`, `:276`, `:292`, `:313`, `:329`, `:344`). That leading 2
becomes `source_scope_base_indent(view, pos)` = `pos < at ? 0 : 2`.

A seventh literal is the early-out in
`repl_source_scope_view_cmd_indent_chars()` (`:325`), which returns a
bare `2` when the view is not built. Either keep it as the conservative
fallback or route it through the base helper once a view is bound - but
do not miss it while changing the other six.

**Use the boundary, not an "inside a depth-0 `CMD_FUNC_DEF`"
predicate.** The boundary form covers three cases the narrow predicate
misses: top-level declarations; the comment/blank run attached to a
function definition, which export deliberately emits *with* the function
(`src/repl/export_prologue.c:441`, `comment_run_attached_func_idx()` in
`export_cmd_writer.c:277`) but which `reformat.c:332` indents from its
own position; and a mid-document function the panel still draws inside
`display()`, which the narrow predicate would wrongly dedent.

**Do not touch the block-closer special case, and do not fold it into
the base.** `source_scope_base_indent` returns 0 or 2 and nothing else.
The closer's extra level comes from the existing formula: `cmd_indent`
is `base + 2*kd + ...`, and `kd` is still 1 on a `CMD_FUNC_END` row, so a
top-level closer computes `base + 2`. `reformat.c:250-264` subtracts 2
from that, and `commit.c:671` copies the header's indent onto the `fe`
text; a prefix `}` reaches column 0 through the subtraction.

Two ways to break this, both of which produce a `}` one level too deep:
baking the closer's `+2` into `source_scope_base_indent` (the closer then
computes 4, minus 2 is 2), or "simplifying away" reformat's minus-2 after
the base change.

### 1e. Direct formatting paths that bypass the helper

Three sites format indentation without asking the source scope. All are
*formatting* fixes; none of them changes where a row is placed (that is
Project 2):

- `src/repl/compile.c:1593` - `format_decl_text(&parsed, "  ", ...)`
  hard-codes two spaces for a new global declaration.
- `src/repl/import.c:754` - `@declare` reconstruction hard-codes
  `"  static float"`, with a comment claiming it matches
  `format_decl_text`. `repl_reformat_program()` rewrites it at
  `import.c:3338`, so this is not a live-panel bug as long as reformat
  always runs - but it is a lying intermediate and the comment rots.
  Same patch as the line above, or neither is true.
- `src/editor/commit.c:654` - `repl_source_scope_cmd_indent(0, ...)` for
  a new function header. **This one is load-bearing, not tidy-up.**
  After the indent change position 0 is base 0, but with the clear-pair
  exception surviving Project 1 a new definition in a catalog scene
  still parks *below* the clear pair, i.e. at `pos >= at`. Querying
  position 0 would emit a column-0 header inside `display()`. Query the
  computed insert position instead - and take both the indent and `at`
  from `ctx->source_scope` (`src/repl/compile.h:187`), the bound view
  that caches `at`, rather than the live wrapper. Reading a live `at`
  against a snapshot document is how the comparison drifts.

**The comparison at the boundary differs for an existing row and a
not-yet-inserted one**, and reformat is not a follow-up that would paper
over getting it wrong: `repl_reformat_program()` runs on Ctrl+\
(`src/editor/input.c:1317-1328`), so whatever indent commit writes is
canonical until the user asks for a reformat.

- **Existing row**: `pos < at ? 0 : 2`. At `pos == at` the row *is* the
  first body row.
- **New global declaration or `CMD_FUNC_DEF`**: `insert_pos <= at ? 0 : 2`.
  Equality means the insert *extends* the prefix. A functions-only
  document has `at == count` and the hoist returns `count`; so does a
  `[decls][funcs][glBegin...]` document where the hoist stops on the
  first body row. In both cases the new closed function becomes prologue
  once inserted, and a `<` test would leave its header at indent 2 above
  `display()`.
- `insert_pos > at` stays 2 - that is the clear-pair case described
  above.

The insert-ghost tie-break in 1h is about where the live cursor's ghost
row draws, not about formatting a row that does not exist yet. They are
different questions and can disagree at the same index.

**Coordinate contract: compare the returned `insert_pos` as-is. Do not
translate it.** `at` comes from a scope view bound to the *pre-change*
document, and `compile_function_decl_insert_pos_after_delete()` returns
`insert_pos` in *post-delete* coordinates
(`src/editor/commit.c:551-554`), so adding `delete_count` back looks like
the correcting move. It is not - that shift exists to place the insert,
not to classify prologue vs. body, and inverting it fails the very test
this section adds.

The reason is the relocation rule: **the deleted comment run is not
dropped, it rides with the function.** `editor_compile_func_def` emits
one `REPL_COMPILED_INSERT_MANY` whose `cmds[]` is
`comments + fd + fe` at `insert_pos`, with `delete_pos`/`delete_count`
naming the run it lifted (`src/editor/commit.c:676-699`). So the
post-delete `insert_pos` is exactly where the whole block lands in the
final document, which is the coordinate the classification wants.

Worked example - a functions-only scene with a comment above the cursor,
rows `0:fd 1:fe 2:// note`, cursor at 3:

| | |
| --- | --- |
| `at` (pre-change) | 2 - the trailing run starts the body (1a) |
| hoist walk | skips the func, skips the comment, stops at 3 |
| subtracts `delete_count` | yes (3 >= 2+1) -> `insert_pos = 2` |
| after insert | the comment rides with the def; both are prologue |

Untranslated: `2 <= 2` -> base 0. Correct. Translated: `3 <= 2` is false
-> base 2, and the new header sits two spaces too deep above
`display()` - the exact bug this section exists to prevent.

The untranslated `<=` also holds for the other two shapes: the clear-pair
case (the walk stops on the executable row before the cursor's comments,
nothing is subtracted, `insert_pos > at` -> base 2) and functions-only
with no comment run (`insert_pos == at == count` -> base 0). Global
declarations have no such delete and are already in current-document
coordinates.

Do **not** build a virtual post-change view to format one row.

Tests must cover the exact-boundary commit both ways: a second function
committed at end-of-document in a functions-only scene (assert the new
header is column 0 *without* pressing Ctrl+\), and the same with a
leading comment run above the cursor, which is what makes the delete
translation non-trivial.

### 1f. `REPL_GLR_BASE_INDENT`

`glr_scene_write_line()` (`src/repl/export_glr.c:43-53`) strips
`REPL_GLR_BASE_INDENT` (2) from **every** line it writes, which is
correct today because every document row carries the display-body
indent. With mixed bases it flattens function bodies (in-memory 2 ->
on-disk 0) and declarations. Strip 2 only from `GLR_ROW_PHASE_BODY`
rows; write `DECLS` and `FUNCS` rows verbatim.

**Camera rows keep the current behaviour.** `glr_scene_write_line` is
also the writer for the generated camera block (`:101`), whose lines come
from `IMPORT_EXPORT_VIEW.cam_lines[]` and are not document rows - they
still carry two spaces and must still be stripped. A phase-keyed strip
must not accidentally cover them. Add a camera round-trip assertion.

Load re-derives indentation, so 1f is a fidelity bug in checked-in files
rather than a runtime one - but every scene regenerated after the change
would be flattened, so it lands in the same patch.

### 1g. Missed call sites

- **`scroll_to_display_function()`** (`src/repl/bootstrap.c:29-42`)
  scans `g_header_pre[]` for `REPL_EXPORT_DISPLAY_OPEN_LINE`, which is
  not in that array (it is `g_display_header[0]`,
  `src/repl/export_setup.c:132`). The loop never matches and the
  function lands at the end of the includes - accidentally near
  `display()` today, and at the *start of the declarations* after the
  split. **Swapping the needle is not the fix**: the splice is
  `header_rows` (workspace + includes + helpers + scratch) plus the
  *wrapped* row count of the prologue rows `[0, at)`, and bootstrap has
  no layout snapshot to ask. It has to count that prefix, which means
  either taking a layout or asking the panel for the splice row.
- **`tests/test_repl_editor.c:260,332`** reimplements the header counter
  (`code_panel_header_row_count` / `code_panel_mouse_y_for_cmd`) for
  mouse-Y math and assumes all chrome precedes command 0. It aims at the
  wrong row once display chrome moves.
- **`tests/test_repl_core_extra.c:130`** hard-asserts
  `repl_source_scope_cmd_indent_chars(0) == 2`.
- **`tests/test_repl_code_panel_syntax.c:214`** asserts the scratch line
  string `"  float A[16], B[16], C[16];"` verbatim; 1h drops its two
  leading spaces.

### 1h. The two placement calls, decided

- **The scratch decoration line goes to file-scope chrome**, after the
  GL vector helpers and before `--- source_prologue ---`, with its two
  leading spaces dropped. It keeps the panel-only
  `float A[16], B[16], C[16];` spelling; export still emits
  `static float A[16] = {0};` on demand
  (`emit_export_scratch_globals_section`, `src/repl/export.c:329-342`).
  Rationale: proximity-to-source was the argument for keeping it in the
  body, and that argument is spent once the declarations it decorates
  sit above `display()` anyway. Leaving it below would preserve exactly
  the lie this change removes for decls.
- **The insert ghost at `at` is the first display-body row.**
  `target_for_doc_line()` emits the ghost before `cmd_idx`, so at
  `cmd_idx == at` it reads as "insert a body row here." A declaration or
  function typed there still hoists into the prologue under the existing
  commit rules, so nothing is lost by the tie-break. Test: insert mode
  with the cursor on the first body command of a scene with a non-empty
  prologue.

## Decisions

| Decision | Resolution |
| --- | --- |
| Comment attachment at the boundary | Forward: a run belongs to the row it precedes (1a). |
| Closed-block boundary | Yes. Last closed depth-0 func block; stop at first executable row, unclosed header, or body-attached comment run. |
| Unclosed trailing function | Boundary stops *at* it, so it renders inside the body; chrome shifts down when `}` lands. Reachable only from `--watch` / partial paste / header-only load, all of which already reflow the panel wholesale. |
| Scratch decoration line | File-scope chrome, two spaces dropped (1h). |
| Insert ghost at the splice | First display-body row (1h). |
| Indent at the boundary | Existing row `pos < at`; new decl/func `insert_pos <= at`, compared in pre-change coordinates (1e). |
| Block-closer indent | Unchanged; keep reformat's minus-2 (1d). |
| Clear-pair exception | Survives Project 1. The executable clear pair renders inside `display()`; a function deliberately parked after it remains inside too. Moving tutorial functions above the locked prelude is a Project 2 placement change. |
| Load-time hoist | Not in Project 1. |

## Project 2 - Making The Order Total (not approved)

Project 1 projects the order that already holds for `.glr`. It does not
hold for every path into the live document. Each of these is a separate
decision, and none is a step of Project 1:

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
- **`.c` import ignoring `@func-body` position.** A load hoist needs its
  own helper (or `export_glr.c`'s phase walk) called from the `.c`
  import finish path **after**
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
  cursor with no hoist, so pasting a definition into the body produces a
  non-prefix document. Project 1 tolerates it - the boundary stops at the
  first executable row and the pasted definition simply renders inside
  `display()`, correctly indented - but if the order must be total,
  paste should re-hoist rather than refuse.
- **Cut/delete.** For the record, the first draft had this wrong:
  `repl_compile_delete_range()` does not block cutting declarations - it
  enforces reference integrity. Copy/cut of decl rows is blocked in
  `src/editor/clipboard.c:450` via `repl_range_contains_var_decl`.
  Function definitions can be cut today. Do not invent a second
  refuse-reason modeled on declarations unless "delete a definition with
  live calls" is independently worth blocking.

Leave commit *placement*, import, clipboard and tutorials alone until
Project 2 is an explicit yes. Project 1's edits to `commit.c` and
`import.c` (1e) are indentation-formatting only.

## Test And Golden Impact (Project 1)

- 40 byte-exact panel goldens,
  `tests/testdata/repl_examples_ui/00.golden.txt`-`39.golden.txt`. The
  section split, the scratch line's new position and the dedent land
  together; regenerate once, from a debug build.
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
- Function-bearing files in `examples/scenes/*.glr` and `tests/scenes/**`
  gain the explicit frame, with camera inside and globals/functions outside.
  Four older no-function examples were also normalized once so the new
  byte-stability assertion covers all 40 shipped scenes without exceptions.

New coverage:

- The boundary helper, **including a `// --- Render State ---` comment
  row between a function's `}` and the first `glClearColor`** - the case
  the naive walk gets wrong.
- A functions-only document (`at == document_count`), for the trailing
  input row. **End it on `}`, not on a trailing blank** - a trailing
  comment/blank run belongs to the body (1a), so `at` would be the blank
  and the `at == document_count` path never runs.
- A second function committed at end-of-document in that scene, asserting
  a column-0 header with no Ctrl+\ reformat; and the same with a leading
  comment run, for the delete-coordinate translation (1e).
- An unclosed trailing function.
- The insert ghost at the splice, using a non-empty prologue.
- Exact full/focus frame spellings, focus-only chrome suppression, scratch
  visibility and column-zero alignment, and `void` decoration/hit translation.
- A byte-for-byte `.glr` save round-trip across all 40 shipped scenes proving
  function-body indent survives (1f), with a camera-inside-display assertion
  for every function-bearing scene.

## Not Touched

Flatten, executor, replay, edit guides, the export writer's structure,
and all *placement* logic in commit, import, clipboard and tutorials. No
`CmdType`, spec-table, config-key or keymap changes; no `@cfg` slug, so
no config-golden churn beyond the panel goldens.

Tours are unaffected: `code:N` is a document index, and moving chrome
does not renumber documents. A Project 2 hoist would retarget them, but
catalog function scenes are already ordered, so it is identity there.
