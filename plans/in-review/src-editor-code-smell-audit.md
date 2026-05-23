# `src/editor/` — Code-Smell Audit

> Audit produced 2026-05-23. Findings come from four parallel reviews
> of `src/editor/` (input + edit_ops; commit + services + reformat;
> state + clipboard + undo; search + inline overlays + completion +
> help_session) plus targeted spot-verification of the most actionable
> claims. File:line references are exact at the time of writing —
> check `git log` on the cited files before acting if this doc has
> aged.
>
> Scope: every file under `src/editor/`. Tests under `tests/` were
> read where they document a contract, but not audited.
>
> The contract: **the editor is the model/controller for text; UI is
> its view; `commit.c` is the only path crossing into the REPL
> pipeline; undo restores both halves atomically.** The dominant
> themes in the findings are *layering inversion back into `app/`*,
> *dispatch-chain drift between the documented canonical order and
> the real Enter path*, and *stack-allocated mega-buffers*.

## How to read this

Severity grouping mirrors the previous two audits:

- **🔴 Actual bugs / hazards (verified)** — correctness or
  memory-safety issues with a concrete failure mode. Pick these up
  first.
- **🟡 Drift / boundary hazards** — parallel structures, layering
  inversions, dispatch reorderings, contract mismatches. Working
  today; one-side edit will silently diverge.
- **🟢 Dead code / dead fields** — code with no callers, unused
  parameters, redundant wrappers. Pure surface reduction.
- **🔵 Structural concerns** — long functions, misnamed entry
  points, magic numbers, hand-rolled patterns. Bigger refactors;
  higher cost.

Each finding cites file + line, names the smell, says why it
matters, and suggests a one-line fix.

## 🔴 Actual bugs / hazards (verified)

### 1. 2 MB `CommitAttemptState` allocated on the stack

**Where:** `src/editor/input.c:1333` (`handle_semicolon_commit_key_route`)

**Smell:** `CommitAttemptState before;` is a stack local. The
file-scope rationale block at L550-553 explicitly documents the
struct as huge — its `EditorUndoSnapshot.cmds[MAX_COMMANDS]` plus
`editor_lines[MAX_COMMANDS][MAX_LINE_LEN]` (~1 MB each) total ~2 MB.
The other two call sites (L663, L862) correctly use the file-scope
`g_commit_attempt_before` / `g_navigation_commit_before` statics for
exactly this reason.

**Why it matters:** Every `;` keystroke pushes 2 MB onto the call
stack — ASan-trap-bait, and a guaranteed overflow on threads with
default 512 KB stacks (some embedded / sandboxed environments).

**Fix:** Reuse `g_commit_attempt_before` (or add a third
`g_semicolon_commit_before` static) at L1333.

### 2. 1 MB `MAX_COMMANDS × MAX_LINE_LEN` snapshot on the stack in paste

**Where:** `src/editor/clipboard.c:384` inside
`editor_clipboard_paste_current`

**Smell:** `char buf[MAX_COMMANDS][MAX_LINE_LEN]` = 4096 × 256 =
1 MB stack allocation, sized for the worst case regardless of the
actual `count` of pasted lines.

**Why it matters:** Same risk as #1 — the entire snapshot fits on
the stack of a typical desktop thread but fails on smaller-stack
deployments; ASan flags it.

**Fix:** Size by `count` (`char (*buf)[MAX_LINE_LEN] = malloc(count * MAX_LINE_LEN)`)
or impose a small `MAX_CLIPBOARD_PASTE` cap.

### 3. `editor_clipboard_clear_selection` does **not** clear the clipboard

**Where:** `src/editor/clipboard.c:33-35`, `clipboard.h:30`

**Smell:** The name says "clipboard clear selection", but the body
is exactly one line: `editor_state_selection_clear()` — it clears
the line-range *selection* anchors only. The clipboard payload is
untouched. A sibling function `editor_state_clipboard_clear()`
*does* clear the payload, so the file has two near-identical names
with opposite behaviors.

**Why it matters:** This function is called from 10+ sites
(`src/app/glr_ctrl.c`, `clipboard.c`, `input.c`) — every reader
parses the wrong intent. Verified: every caller wants the selection
cleared and does not expect the payload cleared, so today nothing
breaks; the smell is purely a footgun for the next change.

**Fix:** Rename to `editor_selection_clear_line_range()` (or
similar without `clipboard_` prefix); update the 10 call sites.

### 4. `editor_compile_close_brace` accepts `err` / `err_size` then ignores them

**Where:** `src/editor/commit.c:285-290`

**Smell:** Signature is `ReplCompileResult editor_compile_close_brace(... char *err, int err_size)` with `(void)err; (void)err_size;` as the first statements. The sibling structured-compile functions
(`editor_compile_if_block`, `_func_def`, `_for_loop`) use them; the
shared signature requires them. Close-brace *can* fail (stray `}`
with no open block returns NO_CHANGE silently).

**Why it matters:** Callers who want "why didn't close-brace
match?" can't — the diagnostic is unreachable. Future debugging of
brace-mismatch issues will require re-plumbing the err buffer.

**Fix:** Drop the `(void)` casts and fill `err` for the stray-`}`
case, or remove `err`/`err_size` from the signature and split the
typedef.

### 5. Workspace-load wipes undo *before* validating; per-file load preserves it on failure

**Where:** `src/app/glr_actions.c:569-570` vs.
`src/editor/inline_file_prompt.c:138-144` (cautionary comment)

**Smell:** `editor_undo_clear()` runs *before*
`repl_load_workspace(dir)` is called — so failed workspace loads
also blow away undo. The inline file prompt's authors explicitly
warned against this in a comment and modeled their own ordering on
"Load Workspace's adjacent-to-success-branch policy" — but that
policy never actually existed; Load Workspace clears unconditionally
upfront.

**Why it matters:** Mismatched user contract: try-to-load-bad-file
loses undo for one entry point but not the other.

**Fix:** Move `editor_undo_clear()` after the
`repl_load_workspace(dir)` success check.

### 6. `g_search_*` / `g_ac_*` sentinel initialization missed by `editor_state_capture`

**Where:** `src/editor/state.c:27-38, 60-64`

**Smell:** First call to `_get_defaults()` (inside `_reset` /
`_search_clear` / `_autocomplete_clear`) patches both `defaults`
and `g_editor_state` with `anchor_pos = -1` and other sentinels.
`editor_state_capture()` skips that side-effect entirely.

**Why it matters:** A test or early-startup caller running
`editor_state_capture(&snap)` before any reset/clear captures
BSS-zero state — `anchor_pos = 0` instead of `-1` — which makes
`editor_input_selection_active()` (`anchor_pos >= 0`) erroneously
report an active selection.

**Fix:** Move sentinel application to an explicit
`editor_state_init()` the controller runs at startup; or have
`_get_defaults()` no-op once initialized.

### 7. `KEY_BACKSPACE` and `KEY_DELETE` collapsed to one behavior

**Where:** `src/editor/input.c:949-951, 964-965, 1175`

**Smell:** Every site mentioning one mentions the other with `||`.
`edit_op_buffer_delete_left_of_cursor` ignores the distinction —
Delete behaves as Backspace.

**Why it matters:** Either intentional and the missing
`delete_right_of_cursor` primitive is a deferred feature, or a
silent bug. No comment in the source acknowledges the conflation.

**Fix:** Confirm intent. If Delete *should* delete-right, add
`edit_op_buffer_delete_right_of_cursor` and dispatch on key. If
intentional, document at the dispatch site.

## 🟡 Drift / boundary hazards

### 8. Editor reaches up into `app/glr_*` (layering inversion)

**Where:** `src/editor/input.c:53-56` (#includes
`app/glr_completion.h`, `app/glr_state.h`, `app/glr_camera.h`,
`app/glr_ctrl.h`); calls at L353-356, L927-940, L1225, L1307

**Smell:** `src/editor/` is layer 2 in `MODULES.md`; non-editor
concerns are supposed to be filtered by the controller *before* the
editor sees them. Yet `input.c` calls
`glr_camera_controls_reset` (L353),
`glr_ctrl_router_reset_code_panel_drag` (L356), reads/writes
`glr_state_presentation*().code_panel_layout` (L927-939), invokes
`glr_ctrl_sync_ui_chrome()` (L940), and calls
`glr_completion_accept_autocomplete()` (L1225, L1307).

**Why it matters:** The file comment at L29-34 acknowledges
`editor_reset_transients` as a single "boundary exception" — but
the inversion is now much wider than the docstring admits. Future
contributors look at the includes and assume the layering is
informal.

**Fix:** Hoist `editor_reset_transients` and
`editor_input_restore_hidden_code_panel` into
`src/app/glr_ctrl.c`; keep their thin editor-text counterparts in
`input.c`. Add `accept` to `EditorCompletionProvider` (#9) so the
two autocomplete call sites don't need `glr_completion.h`.

### 9. `EditorCompletionProvider` is missing `accept`

**Where:** `src/editor/completion.h:17-29` (no `accept` field);
`src/editor/input.c:1225, 1307` (Tab + Enter call
`glr_completion_accept_autocomplete()` directly)

**Smell:** The provider seam exposes `update`,
`update_selected_preview`, `clear` — but no `accept`. So the Tab
and Enter routes bypass the seam and call into the REPL-flavored
provider directly, breaking the abstraction the rest of the
registry maintains.

**Why it matters:** `editor_demo` correctly works without this
because it has no completion at all; the REPL editor leaks. A
future "alternate provider" (e.g., the GL command spec from
tests) has no `accept` hook to register.

**Fix:** Add `void (*accept)(void);` to the struct; expose
`editor_completion_accept()`; have `input.c` call that.

### 10. Documented canonical chain order does not match the real Enter path

**Where:** CLAUDE.md says the chain is
`float_decl → assign → close_brace → for → func → if → parse`.
`editor_try_commit_any` (`commit.c`) matches that. But
`editor_commit_current_input` for Enter (`input.c:667`) runs
`editor_try_commit_block_structs()` *first*, then
`editor_try_commit_var_statements()` deeper at L713. Overwrite
Enter (L775) likewise runs block-structs first.

**Smell:** Two parallel encodings of the load-bearing order. The
canonical doc applies to the `;` and `editor_feed_line` paths but
not the two Enter paths.

**Why it matters:** The README explicitly says ordering is
load-bearing and warns against drift; this is exactly the drift.
The empty-line branch sits between the two halves under Enter,
breaking the "canonical chain" mental model.

**Fix:** Restructure so Enter paths use `editor_try_commit_any`
(removing the empty-line carve-out, or moving it into the helper),
or document explicitly that Enter has a block-first variant and
why it's safe.

### 11. `editor_compile_*` functions advertised as context-pure but read live state

**Where:** `src/editor/commit.c:271, 306, 464, 550, 583, 736, 792,
905`

**Smell:** `editor_compile_close_brace`, `_if_block`, `_func_def`,
`_for_loop` take a `ReplCompileContext *ctx` (carrying a document
snapshot) but then call `repl_source_scope_nearest_open_block_at(pos)`,
`repl_source_scope_cmd_indent(pos, ...)`,
`repl_source_scope_block_depth_at(...)`,
`repl_source_scope_find_block_end(...)` — which read live REPL
state, not the context snapshot.

**Why it matters:** The "REPL-half is pure" claim
(`commit.c:6-12`) is partially false. Tests that construct a
synthetic context but skip live-state setup will silently misbehave
(the live REPL document is empty / out-of-sync with the context).

**Fix:** Take a `repl_source_scope_view` reading from
`ctx->document_cmds`, or document loudly that these are
"context-pure for document data, live-state-coupled for scope
queries."

### 12. README claims `EditorState` owns "cursor blink"; `state.h` disclaims it

**Where:** `src/editor/README.md:88` vs. `src/editor/state.h:152-157`,
write sites at `src/editor/input.c:959, 1557`

**Smell:** README says state.c owns "cursor blink". `state.h`
explicitly disclaims it, pointing to `UiCodePanelRuntimeState` on
`UiState` — and the live writes (`cursor_visible`, `blink_tick`)
happen via `ui_state_code_panel_mut()`. The `src/ui/` audit's
finding #17 is about this same coupling from the UI side.

**Why it matters:** Doc disagreement with code; the audit-prompt
itself was steered by the wrong claim. (The `src/ui/` audit
recommends moving the fields to an editor-session slice, which would
make the README claim correct after the fact.)

**Fix:** Strike "cursor blink" from `README.md:88`'s `EditorState`
ownership line until the `UiState` carve-out is resolved. See
`src-ui-code-smell-audit.md` #17 for the resolution.

### 13. Two parallel encodings of `float_decl → var_assign` order

**Where:** `src/repl/compile.c:60-87` (in `repl_compile_dispatch`)
vs. `src/editor/commit.c:1335-1339` (in
`editor_try_commit_var_statements`)

**Smell:** `compile_dispatch` encodes float_decl → var_assign.
`editor_try_commit_var_statements` independently calls
`editor_try_commit_float_decl` → `editor_try_assign_variable`,
each of which calls `repl_compile_float_decl` /
`repl_compile_var_assign` directly (not through dispatch).

**Why it matters:** A future "insert a new var-statement handler"
requires touching two unrelated functions; the load-bearing
ordering comment in CLAUDE.md only covers one.

**Fix:** Route `editor_try_commit_var_statements` through
`repl_compile_dispatch`; remove the direct calls.

### 14. `editor_try_assign_variable` breaks the `editor_try_commit_*` naming convention

**Where:** `src/editor/commit.c:1228`

**Smell:** Every other handler in the chain is `editor_try_commit_*`
(`_float_decl`, `_for_loop`, `_func_def`, `_if_block`,
`_close_brace`). The assign handler is `editor_try_assign_variable`
(no `commit_`). Verified: 11 references in src/ and tests/, all
using the non-canonical name. CLAUDE.md occasionally drops the
`commit_` infix for this handler too.

**Why it matters:** Grep for "what handlers are in the chain" misses
this one.

**Fix:** Rename to `editor_try_commit_assign_variable`.

### 15. Error reporting drifts across handlers

**Where:** `src/editor/commit.c:83` vs. L1192, L1219, L1239,
L1262, L1295, L1299

**Smell:** `editor_commit_current_input` writes diagnostics into
`result.diagnostic[]` (via `result_set_diagnostic`) and never
touches status. Every `editor_try_commit_*` instead calls
`repl_set_status_error(err)` directly. Two policies in the same
module.

**Why it matters:** Callers of one path get a value-typed
`EditorCommitResult`; callers of the other get an implicit
status side-effect. Hard to test the chain handlers in isolation
because they have a hidden status dependency.

**Fix:** Pick one (return-the-result is cleaner given
`EditorCommitResult` already exists); route everyone through it.

### 16. Three apply-block call sites reach the primitives through three different surfaces

**Where:** `src/editor/commit.c:116-123, 148-156, 250-257`

**Smell:** `editor_commit_current_input`,
`editor_commit_apply_external_change`, and
`editor_commit_apply_plan` each contain the same 4-step apply
sequence (`apply_predef_ops` → `apply_scratch_ops` →
`editor_buffer_apply_compiled_change` → `apply_repl_change`).
But each reaches the underlying primitives differently — one via
passed-in `services`, one via a fresh `svc =
editor_services_default()`, one via direct `repl_apply_*`.

**Why it matters:** A fix to the apply sequence needs three
coordinated changes.

**Fix:** Extract `apply_compiled_three_halves(const ReplCompiledChange *)`;
call from all three.

### 17. Editor undo reaches into `g_predef_vars` / `g_num_predef_vars` directly

**Where:** `src/editor/undo.c:46-50, 73-77`

**Smell:** Undo uses external REPL eval globals by name. Two lines
below, the same module uses the proper seam
(`repl_eval_copy_scratch_arrays`, `repl_func_alias_get/set`).

**Why it matters:** If `g_predef_vars` storage layout moves, undo
silently breaks. The seam pattern is already established next to it
— this is the one outlier.

**Fix:** Add `repl_eval_predef_copy(EditorUndoSnapshot *)` /
`_restore` seam (mirror the scratch-array helpers); route undo
through it.

### 18. Five near-identical "collect-visible-vars → parse → place" sequences

**Where:** `src/editor/input.c:437-486, 694-751, 1351-1394,
1496-1538`; also commit_current_input's overwrite branch at L763-803

**Smell:** Same shape — `collect_visible_vars(insert_idx, ...)` →
`parse_command_ctx`-or-`repl_parse_and_normalize_strict` two-branch
→ `editor_place_parsed_command` → status — replicated 4-5×. Two
different parse APIs (`svc.parse_command_ctx` vs.
`repl_parse_and_normalize_strict`) are used across them with no
documented reason.

**Why it matters:** Any tweak to the parse policy needs to be
replicated 4-5×.

**Fix:** Extract `parse_for_commit(insert_idx, GLCmd *out, char
*text_out, size_t)`; pick one parse entry point.

### 19. Three open-coded "skip leading whitespace, then trim trailing `;`/whitespace"

**Where:** `src/editor/input.c:296-336, 396-403, 582-589`
(`editor_load_line_to_input`, `rewrite_source_text_with_indent`,
`input_matches_committed_line`)

**Smell:** Same canonical strip implemented three times inline; no
shared helper.

**Fix:** Extract a single helper (probably in `src/repl/`, given
`parser.c` also has canonical-form duties).

### 20. Tutorial commit-gating helpers live in `input.c` as file-locals

**Where:** `src/editor/input.c:1233-1299`, call sites at L875,
L1277, L1294-1297, L1312-1322, L1335-1346, L1396-1397

**Smell:** `tutorial_precheck_current_input` and
`tutorial_advance_if_commit_ok` are file-local helpers reading
tutorial state. The README's sanctioned
`glr_ctrl_router_handle_tutorial_ack_key` path correctly hoists
ack-key routing — but commit-time tutorial gating still relies on
input.c locals.

**Why it matters:** Editor input dispatcher carries tutorial
policy that should live in the tutorial subsystem.

**Fix:** Move both helpers into
`src/subsystems/tutorial/tutorial.{c,h}` as public entry points
(`tutorial_precheck_commit(input, ...)`,
`tutorial_after_commit(result)`); call from `input.c` through the
public API.

### 21. Tagged-union `EditorClipboardKind` is never consulted via switch

**Where:** `src/editor/state.c:534-595`, `clipboard.c:212,
245-413`

**Smell:** `kind` is set 4 places and read only at `state.c:585`
(`has_input_text`). The paste dispatcher uses two booleans
(`has_input_text()` and `count > 0`) instead of `switch (kind)`.

**Why it matters:** Adding a third kind (e.g., a `BLOCK` kind for
rectangular selections) requires touching every getter/predicate
and is invisible to the compiler — no `-Wswitch` safety net.

**Fix:** Switch read sites to `switch (kind)` for exhaustiveness,
or drop the enum and use the two-flag invariant explicitly.

### 22. `EditorInputView.input` aliases live storage with no documented liveness

**Where:** `src/editor/state.h:64-74`, `state.c:300-313`

**Smell:** `view.input` and `view.pending_newline` point at live
`g_editor_state` buffers. Hold the view, mutate via
`editor_input_set_text(...)`, your `view.input_len` is stale but
`view.input` still tracks the live mutation. Half by-value, half
by-reference.

**Why it matters:** Subtle aliasing footgun. `editor_state_input()`
is called repeatedly per function so the bug only surfaces when
someone stashes the view.

**Fix:** Make the view fully by-value (copy `input[]` and
`pending_newline[]` into fixed-size buffers), or document the
aliasing in the typedef comment.

### 23. `editor_undo_clear` is contract-required but compile-unenforced

**Where:** `src/editor/undo.h:97-102`, call sites at
`src/app/glr_actions.c:360, 365, 569, 585`,
`src/app/glr_ctrl.c:2088, 2719, 2762`,
`src/editor/inline_file_prompt.c:154`

**Smell:** Header documentation says it "must be called on
wholesale replacement"; nothing checks it. Failure mode is silent
— Ctrl+Z restores foreign-scene state into the new scene.

**Why it matters:** Any future wholesale-replacement site (e.g., a
new "Reset to default scene" button) must remember to call it; the
README repeatedly flags it as load-bearing.

**Fix:** Either bump a generation counter on replacement and have
`editor_undo_pop_snapshot` refuse cross-generation pops, or move
the calls *into* `repl_load_workspace` / `_load_user_scene_idx` /
`_load_example` so callers can't forget.

### 24. Three sites hardcode `name[16]` instead of a shared constant

**Where:** `src/editor/undo.h:58`, `src/editor/state.h:147`,
`src/repl/eval.h:133`

**Smell:** Magic constant duplicated across module boundaries;
the same finding as `src-repl` audit #28 and `src-ui` not-applicable.

**Fix:** `#define REPL_PREDEF_NAME_MAX 16` in `eval.h` (the upstream
source); reference everywhere.

### 25. Wrapped editor search functions are pass-throughs to `ui_text_*`

**Where:** `src/editor/search.c:57-65`

**Smell:** `editor_search_find_next_in_text` /
`_prev_in_text` are one-line wrappers around
`ui_text_find_{next,prev}_in_text` with identical signature.
Tests use both names; production callers could use `ui_text_*`
directly.

**Fix:** Either delete the wrappers and have callers use
`ui_text_*` directly, or inline them as `static` inside
`search.c`. (Same as the `src/ui/` audit's #37.)

### 26. `editor_compile_for_loop` is the only structured-compile that uses `EditorServices`

**Where:** `src/editor/commit.c:872`

**Smell:** `EditorServices svc = editor_services_default(); ... svc.parse_command_ctx(...)`.
The other three structured-compile functions call `repl_eval_*` /
`parse_repl_func_signature` directly.

**Why it matters:** Noise that suggests these compile functions
might be substitutable; they aren't (the others bypass services
entirely).

**Fix:** Replace with `repl_parser_parse_command_ctx(body, &body_pl,
&parse_ctx)` directly.

## 🟢 Dead code / dead fields

### 27. `editor_commit_current_input` has zero production callers

**Where:** `src/editor/commit.c:65-130` (definition);
`commit.h:55` (declaration). Verified: only
`tests/test_repl_compile.c:621` calls it.

**Smell:** Documented as "the common path for the live input row"
in `commit.h:10`, but the four real dispatch sites all bypass it
and go through `editor_try_commit_*` + the open-coded
`repl_parse_and_normalize_strict` tail. The whole `EditorServices`
indirection (#28) exists primarily for this function.

**Fix:** Delete it (and the `services` indirection it requires),
or route the four dispatch sites through it.

### 28. `EditorServices` is a one-implementation abstraction

**Where:** `src/editor/services.h:28-74`, `services.c:65-76`

**Smell:** `editor_services_default()` is the only `EditorServices`
factory in the tree. The header at L6-9 admits: "gives the
controller a single place to substitute test doubles or alternate
implementations" — no such substitution exists in `src/` or
`tests/`.

**Why it matters:** Adds context churn (svc construction,
`void *user` plumbing) for no substitution benefit.

**Fix:** Drop the services table; call `repl_compile_dispatch`,
`repl_apply_*`, `repl_parser_parse_command_ctx` directly from
`commit.c`. Paired with #27.

### 29. `editor_commit_apply_compiled_change` is a test-only wrapper

**Where:** `src/editor/commit.c:1117-1119`, declared at
`commit.h:79`

**Smell:** Defined in production but only `tests/test_repl_compile.c`
calls it. Production code uses
`editor_commit_apply_external_change(change, 0)` directly.

**Fix:** Delete (have tests call the canonical form), or make
the canonical form private.

### 30. `EditorInputState.input_capacity` and `pending_newline_capacity` are write-only

**Where:** `src/editor/state.h:49, 59`, `state.c:15-17, 304-309`

**Smell:** Initialized in sentinels and re-derived in
`editor_state_input()`, but the only reads are inside `state.c`
itself. The view hardcodes `MAX_INPUT_LEN` instead of reading the
stored fields. Three sources of truth for "input capacity": the
struct field, the view field, and the raw `MAX_INPUT_LEN`.

**Fix:** Delete both fields; use `MAX_INPUT_LEN` directly.

### 31. `EditorVariableDragState` typedef stranded in `state.h` after storage moved

**Where:** `src/editor/state.h:142-149, 195-197`

**Smell:** Comment at L195-197 says `variable_drag` lives on the
variable-panel peer. Yet the typedef still sits in
`editor/state.h`, used only by `src/subsystems/variable_panel/`.

**Fix:** Move the typedef to
`src/subsystems/variable_panel/variable_panel_state.h`.

### 32. Two pointless one-liner route wrappers in input.c

**Where:** `src/editor/input.c:992-994, 1594-1596`

**Smell:** `handle_search_key_route(key) { return editor_search_handle_key(key); }`
and `handle_search_special_route(key) { return editor_search_handle_special(key); }`
exist purely so dispatcher reads `if (handle_search_key_route(key)) return;`
symmetrically.

**Fix:** Inline both.

### 33. `special_begin_key` takes a param it ignores

**Where:** `src/editor/input.c:1554-1560`

**Smell:** Signature is `static void special_begin_key(int key)`
with `(void)key;` as first statement. Either use `key` (clear
line-range selection like `keyboard_begin_key` does) or drop the
parameter — currently a documentation lie.

**Fix:** `static void special_begin_key(void)`.

### 34. `editor_get_modifiers()` and `editor_input_active_modifiers()` are the same function

**Where:** `src/editor/input.c:140, 174`,
`src/editor/input.h:59, 63`

**Smell:** Two names for one operation, split so external callers
use the long name and internal handlers the short. Indistinguishable
in behavior; both go through the same test seam.

**Fix:** Pick `editor_input_active_modifiers` (matches file's prefix
convention); delete the wrapper.

### 35. `editor_state_search()` returns a 1 KB struct by value per call

**Where:** `src/editor/state.c:597-599`; callers at
`src/app/glr_ctrl.c:1505, 2485, 3621-3623` and
`src/editor/search.c:78, 96, 115, 128, 148, 169`

**Smell:** `EditorSearchState` has `char query[MAX_INPUT_LEN]`
(1024 B) inside; every call copies 1 KB.
`src/app/glr_ctrl.c:3621-3623` calls it twice consecutively;
`search.c` re-fetches per function entry.

**Fix:** Return `const EditorSearchState *`, mirroring
`EditorBufferView`; or expose narrow getters for the handful of
`.active` / `.hit_*` checks.

### 36. Forward declarations in `state.c` for functions already in `state.h`

**Where:** `src/editor/state.c:245-250`

**Smell:** Three forward declarations
(`editor_input_clear`, `editor_pending_newline_clear`,
`editor_cursor_pos_set`) for functions defined later in the same
file. They're already declared via `state.h` which the file
includes.

**Fix:** Delete the forward-decl block.

### 37. `editor_state_input_reset` mutates `insert_mode` directly, bypassing setter

**Where:** `src/editor/state.c:319-323`

**Smell:** `g_editor_state.input.insert_mode = 0;` instead of
`editor_insert_mode_set(0)`. Every other caller goes through the
setter.

**Fix:** `editor_insert_mode_set(0);`.

### 38. Comments reference "legacy" code that is the live path

**Where:** `src/editor/commit.c:88, 190, 609, 1202`

**Smell:** Several comments call the existing primary path
"legacy" (`"Caller falls through to the legacy try_commit_* chain"`,
`"The legacy guard returns 0 from editor_try_commit_func_def"`)
when the "legacy" name *is* the live path. Future readers will
look for a "modern" path that doesn't exist.

**Fix:** Search-and-replace "legacy" → "current" / "live"; or
delete the comments.

### 39. `apply_post_effects` docstring is out of date

**Where:** `src/editor/commit.c:175-220` vs. `commit.h:178-188`

**Smell:** Header says the effect order is `cursor_target → resume
→ insert_mode_target → clear_input → clear_pending_newline →
load_line_after_apply → status`. Actual order adds
`clear_autocomplete → func_decl_resume_publish` between
`load_line_after_apply` and the externally-applied status, and
status is in the caller (`editor_commit_apply_plan:262`), not here.

**Fix:** Update the docstring to list all 8 effects and note where
status is applied.

### 40. `apply_compiled_change` cluster has 3 distinct context constructions for the same payload

**Where:** `src/editor/commit.c:1182, 1229, 1283`

**Smell:** Each try-commit handler opens with the same two
statements on one line:
`ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line()); ctx.insert_mode = editor_insert_mode();`.
This duplicates the body of `default_context()` in
`services.c:21-30`.

**Fix:** Expose `editor_compile_context_live(void)` in
`services.h`; call it.

## 🔵 Structural concerns

### 41. `editor_clipboard_paste_current` decision logic is opaque

**Where:** `src/editor/clipboard.c:245-413`

**Smell:** ~170 lines mixing kind dispatch, input-text vs.
line-range paste, post-paste cursor/insert-mode policy, and the
1 MB stack snapshot (#2). The branching on `kind` is structural
but the post-paste behavior depends on a mix of input-mode,
trailing-blank-line detection, and the same-row selection-replace
case.

**Fix:** Extract `paste_input_text(state)`, `paste_lines(state)`,
`finalize_after_paste(...)`; let the entry point be ~10 lines.

### 42. `mouse_func` signature has 3 silenced unused parameters

**Where:** `src/editor/input.c:1823-1832`

**Smell:** `static void mouse_func(int button, int state, int x,
int y) { (void)button; (void)x; (void)y; if (state != GLUT_UP) return; ... }`.
Three `(void)` casts on one line make this read like a stub. The
signature is required by GLUT shape; legitimate, but un-commented.

**Fix:** Add `/* GLUT callback shape; only state == GLUT_UP is
editor-relevant. */`; lay the casts out one per line.

### 43. Edit-line override path edge: `MAX_INPUT_LEN - 2` vs `- 1` is silently load-bearing

**Where:** `src/editor/input.c:305-336`
(`editor_load_line_to_input`)

**Smell:** Clamps to `MAX_INPUT_LEN - 1` for plain and
`:`-prefixed-already paths but `MAX_INPUT_LEN - 2` for the path that
prepends `:`. Correct (one byte reserved for the leading `:`) but
uncommented; future-edit hazard.

**Fix:** Add `/* -2: reserve one byte for the leading ':' below */`;
or compute via `MAX_INPUT_LEN - 1 /* NUL */ - 1 /* leading ':' */`.

### 44. Four hand-written `pending_newline` clears

**Where:** `src/editor/input.c:273-274, 826-827, 849-850,
1385-1386`

**Smell:** `inp->pending_newline[0] = '\0'; inp->pending_newline_len = 0;`
repeated four times.

**Fix:** `static void editor_pending_newline_clear(void)` helper.

### 45. `is_word_char` re-implements the project's identifier predicate

**Where:** `src/editor/input.c:1407-1410`

**Smell:** Yet another
`(c >= 'a' && c <= 'z') || ... || c == '_'` inline. `src/repl/eval.c`
has the same predicate inlined in 6 places. The `src-repl` audit
#18 calls this out too.

**Fix:** Add `int repl_is_ident_continue(unsigned char c)` to
`src/repl/eval.h`; use from both.

### 46. Tutorial-Tab branch hand-writes the input buffer

**Where:** `src/editor/input.c:1213-1222`

**Smell:** Open-codes a `strncpy + input_len = strlen +
cursor_pos_set` sequence directly on
`editor_state_input_mut()->input`. Every other code path that
replaces the input buffer with a known string uses
`editor_input_clear()` + `editor_feed_line()`-ish helpers.

**Fix:** Add `editor_input_set_text(const char *)` helper;
call it here.

### 47. Tutorial-locked guard messages drift in wording

**Where:** `src/editor/input.c:186, 292`, `clipboard.c:29`

**Smell:** Three guards say roughly the same thing — two with
"comment", one with "instruction". Trivial but real — they'll
show up next to each other in status text.

**Fix:** Pick one phrasing; ideally a shared
`repl_set_status_tutorial_locked()` helper.

### 48. `EditorState` carries ~1.2 MB of per-frame transient overlay lists

**Where:** `src/editor/state.h:191-194`

**Smell:** `transformers` (64 items), `highlights` (256),
`virtual_lines` (512 × 384 B ≈ 192 KB), `line_overrides`
(4096 × 256 B = 1 MB) all live inside `EditorState`, despite being
refilled every frame by the controller. `editor_state_capture()`
deep-copies them as part of the state copy.

**Why it matters:** Today `EditorUndoSnapshot` doesn't capture
these (otherwise undo rings would be ~40 MB). The capture symmetry
the function name implies is broken; readers don't know.

**Fix:** Move the four overlay lists into a separate per-frame
struct (e.g., `EditorOverlaySnapshot`) the controller refills
directly; or document that `editor_state_capture()` deliberately
covers them.

### 49. `state.c:248-250` exposes `EditorClipboardKind` setter without payload-validity guarantee

**Where:** `src/editor/state.c:545-563`
(`editor_state_clipboard_count_set`)

**Smell:** Setting `n > 0` writes `line_count = n` and
`kind = EDITOR_CLIPBOARD_LINES`, but doesn't ensure
`lines[0..n)` has fresh data; relies on callers (only one site:
`clipboard.c:76-86`) to write them first.

**Why it matters:** Brittle informal contract. The tagged-union
invariant ("kind matches payload") nominally holds, but content
can be undefined if a future caller forgets to populate.

**Fix:** Make this private; or take
`(const char *const *lines, int count)` and write both inside.

### 50. Reformat saves five fields; the doc claim is broader

**Where:** `src/editor/reformat.c:19-40`, `reformat.h:1-14`

**Smell:** Header says it "saves/restores the typed input buffer"
and "keeps the editor session intact." Actually saves: `edit_line`,
`inserting`, `input[1024]`, `input_len`, `cursor_pos`. NOT saved:
selection, search, autocomplete, scroll, pending_newline, undo
ring. If `repl_reformat_program` ever mutates any of those, the
user-visible session silently loses those parts.

**Fix:** Either document "reformat doesn't touch X, Y, Z so we
don't save them"; or expand the save set.

### 51. Stale doc reference to a non-existent line

**Where:** `src/editor/commit.h:267-270`

**Smell:** Comment says "editor_commit_func_decl_resume_set is
declared at line 190" — actual declaration is at L176.

**Fix:** Drop the line-number reference, or delete the comment
(the symbol is declared once at L176 with its own docblock; no
missing declaration to explain).

## Sequencing

### One-afternoon pass

1. **#1** + **#2** — Move the 2 MB and 1 MB stack allocations to
   file-scope statics or heap. Each is a 2-line change at a single
   site.
2. **#3** — Rename `editor_clipboard_clear_selection` to
   `editor_selection_clear_line_range` (or similar); update the
   10 call sites. Mechanical.
3. **#4** — Either fill `err` for the stray-`}` case in
   `editor_compile_close_brace`, or drop the unused parameters.
4. **#5** — Move `editor_undo_clear()` after the
   `repl_load_workspace` success check.
5. **#14** — Rename `editor_try_assign_variable` →
   `editor_try_commit_assign_variable`. 11 references.
6. **#7** — Decide whether Delete should delete-right; if yes, add
   the primitive. If no, document at the dispatch site.
7. **#27** + **#28** + **#29** — Delete the unused
   `editor_commit_current_input` + `EditorServices` indirection +
   `editor_commit_apply_compiled_change` cluster, or wire them up.
   Together this is ~150 LOC.
8. **#30** + **#31** + **#33** + **#34** + **#36** + **#37** —
   Dead-field and naming cleanup. All mechanical.

### One-week pass

The dominant work is **closing the layering inversion** and
**resolving dispatch-chain drift**:

- **#8** + **#9** — Hoist `editor_reset_transients` and
  `editor_input_restore_hidden_code_panel` into `src/app/glr_ctrl.c`;
  add `accept` to `EditorCompletionProvider`. Together they remove
  the four `app/` includes from `editor/input.c`.
- **#10** — Restructure the two Enter paths to use
  `editor_try_commit_any`, removing the block-first carve-out (or
  documenting it explicitly with the safety rationale).
- **#11** — Take a `repl_source_scope_view` reading from the
  context's `document_cmds` in the four `editor_compile_*`
  structured handlers.
- **#13** — Route var-statement dispatch through
  `repl_compile_dispatch`; remove direct calls to
  `repl_compile_float_decl` / `_var_assign`.
- **#16** — Extract `apply_compiled_three_halves` and use across
  all three apply-block sites.
- **#17** — Add `repl_eval_predef_copy` seam; route undo through
  it.
- **#18** + **#19** — Extract `parse_for_commit` and the canonical
  strip helper.
- **#23** — Either bump a generation counter on wholesale
  replacements and enforce in undo pop, or push
  `editor_undo_clear()` into the load APIs.

### Out of scope

- The 32-deep undo ring depth — it's a long-standing constant; not
  flagged.
- The `editor_demo` link surface — it's working as designed
  (`state.c` + `edit_ops.c` only). All the findings about input/
  commit/clipboard apply to the REPL editor; the demo proves the
  cleavage works.
- `help_session.c` (51 lines) — the abstraction is thin but real
  (read-only editor session over a content provider); don't merge
  it into something larger.
- The `EditorClipboardKind` tagged-union — keep the kind for the
  future `BLOCK` use case, but enforce it via `switch` (#21).

## Method note

This audit was produced by four parallel review agents:

- `input.c` (73KB, 1904 lines — the heaviest file in `src/editor/`)
  + `edit_ops.c`
- `commit.c` (53KB, 1376 lines) + `services.c` + `reformat.c`
- `state.{c,h}` (state.h is 461 lines — unusually large for a
  header) + `clipboard.c` + `undo.c`
- `search.c` + the two inline overlays (`inline_rename.c`,
  `inline_file_prompt.c`) + `help_session.c` + `completion.c`

Each agent was asked for highest-signal findings only. The most
actionable claims (stack allocations, missing production callers,
naming drift, layering inversion) were verified against the source.
The 🟡 / 🟢 / 🔵 findings are reported as the agents framed them;
spot-check before acting on the more mechanical ones.

Cross-references to the other two audits in this directory:

- `src-repl-code-smell-audit.md` finding #28
  (`REPL_PREDEF_NAME_MAX`) is the upstream for editor finding #24.
- `src-ui-code-smell-audit.md` finding #17 (cursor blink on
  `UiState`) is the inverse of editor finding #12 (README claims
  cursor blink on `EditorState`). The resolution is to move the
  fields off `UiState` and update both docs.
- `src-ui-code-smell-audit.md` finding #37 (search wrapper
  pass-throughs) is the same as editor finding #25 — fix once at
  the editor side.
