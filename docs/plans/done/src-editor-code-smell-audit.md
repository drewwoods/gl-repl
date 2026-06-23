# `src/editor/` — Code-Smell Audit

> Audit produced 2026-05-23. Findings come from four parallel reviews
> of `src/editor/` (input + edit_ops; commit + services + reformat;
> state + clipboard + undo; search + inline overlays + completion +
> help_session) plus targeted spot-verification of the most actionable
> claims. File:line references are exact at the time of writing —
> check `git log` on the cited files before acting if this doc has
> aged.
>
> **Revision 2 (2026-05-23):** Corrections applied after reviewer
> feedback. The first draft had: severity inflation in the 🔴 bucket
> (#3 and #7 moved to 🟡, where "naming hazards and ambiguous-intent
> code" now lives); two stale "add a new helper" fixes for helpers
> that already exist publicly (#44, #46); a boundary-violating fix
> for undo-clear (#23); an under-scoped fix for context-purity (#11);
> a contract bug understated as just docstring drift (#39); a wrong
> symbol name (#10's Enter helper is the static `commit_current_input`,
> not `editor_commit_current_input`); a too-loose "pick one parse API"
> recommendation that would drop strict-refs semantics (#18); and an
> afternoon sequencing item (`EditorServices` delete) that has four
> production callers and is actually a multi-step week-pass refactor.
> All corrected inline.
>
> **Revision 3 (2026-05-23):** Second reviewer pass. Fixed: #13's
> "route through dispatch" recipe would have silently dropped every
> `EditorCommitPlan` post-effect (clear_input, cursor_target,
> commit_message, normals_dirty, and the `_var_statements_then_insert`
> insert-mode flip); reframed as two viable shapes (shared ordering
> table vs. adapter). #11 also missed `collect_visible_vars` at
> `core.c:721`, which has its own live-state reads — full extraction
> spans three modules. #15 conflicted with #27 (delete the only
> production user of `EditorCommitResult`) — picked direction (delete
> the type; reframe #15 around status-side-effect testability). #3
> covered only `editor_compile_close_brace` while the same ignored-err
> issue exists in `repl_compile_close_brace` (`compile.c:1485`) and
> propagates through `load_try_block` — fix now requires both. #29's
> test-call count was revised upward from the first draft (later
> corrected to 19 actual calls in Revision 4); afternoon sequencing
> now names the real churn so "build-safe" stays true.
>
> **Revision 4 (2026-05-23):** Third reviewer pass. Fixed: #4's
> "success check" was ambiguous — `repl_load_workspace()` returns the
> file count, 0 for empty, -1 on I/O error; the audit now specifies
> the recommended `>= 0` predicate and flags the empty-workspace
> behavior as a product decision. #11's interim doc-fix only covered
> the four `editor_compile_*` entry points; `repl_compile_var_assign`
> (`compile.c:858`) also calls live `collect_visible_vars` and was
> added to the doc-fix scope. #13's "static const ReplCompileFn
> k_var_statement_chain[]" was wrong (a `static` table isn't shareable
> across TUs) — reframed as a shared kind enum with per-TU dispatch
> tables. #16's "three apply-block sites" stops being true after
> #27 lands in the afternoon — finding now carries the conditional
> framing. #29's call-site count corrected (19, not 21 — the extra 2
> were doc-comments). #27's call-site list corrected (L621 was a
> doc-comment, not a call; the 5 actual calls are L630/L639/L662/L679/
> L724). Afternoon sequencing items 2, 3, and 8 updated to match.
>
> **Revision 5 (2026-05-24):** Fourth reviewer pass. Fixed two
> remaining sequencing drifts: #11's week-pass summary now includes
> the interim doc-comment work for `repl_compile_var_assign` as well
> as the four editor-side `editor_compile_*` entry points; #13's
> week-pass summary no longer recommends a shared static
> `k_var_statement_chain[]` and now matches the finding's "shared
> ordered kind list, per-TU handler tables" shape. Refreshed #4's
> `glr_actions.c` line reference to the current source and clarified
> its empty-workspace return-value semantics.
>
> **Revision 6 (2026-05-25):** Tier C review refresh against current
> source. #12 is now stale: cursor blink lives on `EditorState` and
> the README matches, so the old doc fix must not be applied. #13's
> preferred shape is the shared ordered kind-list; the adapter remains
> viable only if it preserves per-handler post-effects and can identify
> the matched var-statement kind. #23's fix should be a semantic
> editor-side wholesale-replacement API that clears plus bumps the undo
> generation, not scattered generation bumps beside raw clears. #28's
> step 1 already landed via `apply_compiled_three_halves`; the remaining
> direct-call migrations must update the `check-editor-repl-surface`
> ratchet/baseline. #40 should use a local `commit.c` context helper,
> not `services.h`, because #28 deletes services. #41 is partly stale:
> input-text paste and the stack snapshot are already extracted/fixed;
> only optional line-paste factoring remains.
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
>
> **Implementation status (2026-05-25 Tier C pass):** Full afternoon pass +
> most week-pass items completed. Tier C pass completed 12 additional
> findings: #21 (clipboard switch dispatch), #22 (input view aliasing
> doc), #24 (REPL_PREDEF_NAME_MAX constant), #26 (EditorServices removal
> from for-loop compile), #32 (search route wrapper inline), #35
> (editor_state_search const pointer return), #40 (compile context
> helper), #42 (mouse_func comment), #43 (MAX_INPUT_LEN-2 comment),
> #45 (shared identifier predicate), #47 (tutorial message unification),
> #48 (overlay list doc comment), #49 (clipboard setter contract doc),
> #50 (reformat save-set doc). Remaining items deferred to future
> passes: #13 (var-statement ordering: shared kind list vs. adapter),
> #15 (error reporting testability), #18 (`parse_for_commit`
> policy-flagged helper extraction), #28 (EditorServices
> dismantle — steps 1-2 landed, steps 3-4 remain),
> #41 (optional line-paste factoring — main fixes done).
> #20 resolved as correct layering (adapter comments added);
> #23 done (generation-counter safety + structural guard).
> All completed items are tested via the 7113-test suite and guarded
> against regression by structural guards including `check-editor-no-app`,
> `check-repl-no-app`, `check-scene-no-upper-layers`,
> `check-ui-core-no-upper-layers`, and `check-editor-repl-surface`.
>
> Headings below are marked ✅ (done) or ⏳ (deferred).

## Backlog closeout (2026-05-24)

The 2026-05-24 backlog pass that swept the sibling
`src-repl-code-smell-audit.md` and `src-ui-code-smell-audit.md`
classified their remaining findings by **Tier A/B/C/D** (see those
docs for definitions, reproduced below). Applying the same system
to this audit shows almost nothing left to bite off:

- **Tier A — small, real fix, near-zero risk.** None outstanding;
  the original afternoon pass already absorbed every Tier-A-sized
  item that could land safely. (Several Tier C items below are
  *individually* small but were deferred for cross-audit or product
  reasons — see notes per finding.)
- **Tier B — moderate effort, clear value.** None outstanding;
  the original week pass closed the layering inversions and
  dispatch-chain drift.
- **Tier C — defer (high cost or cross-cutting).** The 2 ⏳-marked
  items (#13, #18) plus #28 and 17 unmarked findings that were
  triaged-skipped during the original closeout but aren't worth a
  dedicated pass on their own. Most are real wins; they wait for
  the surrounding code to be reworked. See individual headings.
- **Tier D — keep deferred / stale.** #12 is stale after later
  cursor-blink ownership work and needs no fix. #25 — the
  `editor_search_find_{next,prev}_in_text` pass-through wrappers,
  same call as `src-ui` #37 (also Tier D), is kept to preserve the
  `editor_*` ↔ `ui_text_*` prefix boundary. Deleting it would force
  editor-namespaced callers to reach across.

### Tier system

Used during the 2026-05-24 backlog review to triage what was left:

- **Tier A — small, real fix, near-zero risk.** 5–30 line changes
  with no architectural exposure. Always worth doing once identified.
- **Tier B — moderate effort, clear value.** 50–200 line changes,
  one or two files of churn, a real test impact. Worth doing as a
  focused pass when next in the area.
- **Tier C — defer (high cost, low payoff).** Real wins but touch a
  wide surface, or need cross-audit/product coordination; revisit
  when the surrounding code is being reworked anyway.
- **Tier D — keep deferred.** Findings the audit landed wrong, or
  where the original intent (a kept-on-purpose trampoline, a
  test-pinned bug) makes "fixing" them worse than living with them.

### Headline take

This audit is the most-closed of the four 2026-05-23 audits. The
active leftovers are mostly "doc/comment fix or small helper
extraction that depends on a decision the original closeout deferred."
None are bugs. They sit in Tier C waiting for either (a) the relevant
area being touched for other reasons, or (b) the cross-audit work that
resolves their upstream dependency (e.g., #45 waits on `src-repl`
#18, which is done — so #45 is unblocked but still cheap enough to
skip until someone opens `input.c` again). Revision 6 reclassifies
#12 as stale/Tier D and leaves #25 as intentionally kept.

Headings below carry either ✅ / ⏳ (from the original closeout) or
**Tier C / D** (from this backlog pass).

## How to read this

Severity grouping mirrors the previous two audits:

- **🔴 Actual bugs / hazards (verified)** — correctness or
  memory-safety issues with a concrete failure mode that exists in
  current production code. Pick these up first.
- **🟡 Drift / boundary hazards** — parallel structures, layering
  inversions, dispatch reorderings, contract mismatches, naming
  hazards, and ambiguous-intent code that works today but is one
  edit away from misbehaving. Working today; one-side edit will
  silently diverge.
- **🟢 Dead code / dead fields** — code with no callers, unused
  parameters, redundant wrappers. Pure surface reduction.
- **🔵 Structural concerns** — long functions, misnamed entry
  points, magic numbers, hand-rolled patterns. Bigger refactors;
  higher cost.

Each finding cites file + line, names the smell, says why it
matters, and suggests a one-line fix.

## 🔴 Actual bugs / hazards (verified)

### 1. ✅ 2 MB `CommitAttemptState` allocated on the stack

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

### 2. ✅ 1 MB `MAX_COMMANDS × MAX_LINE_LEN` snapshot on the stack in paste

**Where:** `src/editor/clipboard.c:384` inside
`editor_clipboard_paste_current`

**Smell:** `char buf[MAX_COMMANDS][MAX_LINE_LEN]` = 4096 × 256 =
1 MB stack allocation, sized for the worst case regardless of the
actual `count` of pasted lines.

**Why it matters:** Same risk as #1 — the entire snapshot fits on
the stack of a typical desktop thread but fails on smaller-stack
deployments; ASan flags it.

**Fix:** Size by `count` via heap
(`char (*buf)[MAX_LINE_LEN] = malloc((size_t)count * MAX_LINE_LEN)`)
with a null check and early return; or promote to a file-scope
static mirroring the #1 pattern (avoids the `free()` obligation).
The implementation chose heap — see `clipboard.c:380-384`.

### 3. ✅ Close-brace compile functions accept `err` / `err_size` then ignore them

**Where:** `src/editor/commit.c:285-290`
(`editor_compile_close_brace`) AND `src/repl/compile.c:1485-1490`
(`repl_compile_close_brace`). The latter is called via
`load_try_block` at `src/repl/load.c:94`, so the import path is
affected too.

**Smell:** Both signatures take `char *err, int err_size` with
`(void)err; (void)err_size;` as the first statements. The sibling
structured-compile functions (`if_block`, `_func_def`, `_for_loop`)
in both modules use them. Close-brace *can* fail (stray `}` with
no open block returns NO_CHANGE silently).

**Why it matters:** Callers who want "why didn't close-brace
match?" can't — the diagnostic is unreachable from interactive
commit *and* from file load. Future debugging of brace-mismatch
issues will require re-plumbing both err buffers.

**Fix:** Fix both functions together. Either drop the `(void)`
casts and fill `err` for the stray-`}` case in both, or remove
`err`/`err_size` from both signatures and split each typedef. Do
**not** fix only the editor side — that leaves
`repl_compile_close_brace` in the same shape and
`load_try_block`'s diagnostic still discarded.

### 4. ✅ Workspace-load wipes undo *before* validating; per-file load preserves it on failure

**Where:** `src/app/glr_actions.c:589-590` vs.
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

**Fix (predicate matters):** Move `editor_undo_clear()` after
the `repl_load_workspace(dir)` call and gate it on the return
value. `repl_load_workspace()` returns the number of files
loaded, **0 for a NULL/empty path argument or for a workspace with
no `.c` files**, and **-1 on I/O error** (`src/repl/core.h:52`
documents the NULL/empty-path half; implementation also returns 0
after loading no scenes). A truthy C check (`if (n)`) would still
clear undo on -1; a `> 0` check would skip empty-workspace cases
that do replace workspace/user-scene state before returning 0.
The intended predicate depends on whether "workspace exists but
contained no scenes" should reset undo (arguably yes — workspace
state changed) or preserve it (arguably no — no scene was actually
loaded). Audit's recommendation for the menu path, which supplies
a non-empty directory, is `if (n >= 0) editor_undo_clear();` —
i.e. clear on success **and** on empty, only preserve on I/O
error. But this is a product decision, not a mechanical fix;
document the intended empty-workspace behavior in the
`repl_load_workspace` docstring at the same time.

### 5. ✅ `editor_state_capture` skips the lazy-init that fixes selection sentinels

**Where:** `src/editor/state.c:27-38, 60-64`

**Smell:** `editor_state_get_defaults()` already self-guards with an
`inited` flag (L27-38) — it patches `g_editor_state` with the
non-zero sentinels (`anchor_pos = -1`, etc.) the *first time* it's
called. The first call is inside `_reset` / `_search_clear` /
`_autocomplete_clear`. `editor_state_capture()` (L60-64) skips
that lazy-init call and copies BSS-zero state.

**Why it matters:** A test or early-startup caller running
`editor_state_capture(&snap)` before any reset/clear captures
`anchor_pos = 0` instead of `-1`, which makes
`editor_input_selection_active()` (`anchor_pos >= 0`) erroneously
report an active selection.

**Fix:** Either call `editor_state_get_defaults()` (which is
idempotent after the first call) at the top of
`editor_state_capture()`, or introduce a real
`editor_state_init()` the controller / tests must call at startup
and remove the lazy-init pattern entirely.

## 🟡 Drift / boundary hazards

### 6. ✅ `editor_clipboard_clear_selection` does **not** clear the clipboard

**Where:** `src/editor/clipboard.c:33-35`, `clipboard.h:30`

**Smell:** The name says "clipboard clear selection", but the body
is exactly one line: `editor_state_selection_clear()` — it clears
the line-range *selection* anchors only. The clipboard payload is
untouched. A sibling function `editor_state_clipboard_clear()`
*does* clear the payload, so the file has two near-identical names
with opposite behaviors.

**Why it matters:** Nothing breaks today — every caller of
`editor_clipboard_clear_selection` actually wants the selection
cleared and does not expect the payload cleared. But it's a
classic naming-vs-behavior footgun: the next contributor who reads
the name will write code that wrongly assumes the payload gets
cleared too, or wrongly omits a payload clear that they thought
this call provided.

**Fix:** Rename to `editor_selection_clear_line_range()` (or
similar without `clipboard_` prefix); update the 10 call sites.

### 7. ✅ `KEY_BACKSPACE` and `KEY_DELETE` collapsed to one behavior

**Where:** `src/editor/input.c:949-951, 964-965, 1175`

**Smell:** Every site mentioning one mentions the other with `||`.
`edit_op_buffer_delete_left_of_cursor` ignores the distinction —
Delete behaves as Backspace. No comment in the source acknowledges
the conflation. Could be intentional (the missing
`delete_right_of_cursor` primitive is a deferred feature) or a
silent bug.

**Why it matters:** Intent is unknown — the audit cannot
classify this as a verified bug without a product decision. But
the code reads as if a future contributor will assume Delete is
broken and "fix" it (changing behavior), or assume the conflation
is deliberate and propagate it further.

**Fix:** Confirm intent first. If Delete *should* delete-right
(standard editor semantics), add `edit_op_buffer_delete_right_of_cursor`
and dispatch on key. If intentional, add a one-line comment at
each dispatch site explaining why.

### 8. ✅ Editor reaches up into `app/glr_*` (layering inversion)

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

### 9. ✅ `EditorCompletionProvider` is missing `accept`

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

### 10. ✅ Documented canonical chain order does not match the real Enter path

**Where:** CLAUDE.md says the chain is
`float_decl → assign → close_brace → for → func → if → parse`.
`editor_try_commit_any` (`commit.c`) matches that. But the static
`commit_current_input(int enter_mode)` in `src/editor/input.c:633`
(used by both insert-mode Enter at L880 and overwrite-mode Enter at
L1315) runs `editor_try_commit_block_structs()` *first*, then
calls `editor_try_commit_var_statements_then_insert()` (overwrite)
or `editor_try_commit_var_statements()` (insert) deeper. The
overwrite path uses the `_then_insert` variant specifically for its
post-effects — flipping into insert mode and clearing the input
buffer after a successful var commit (`commit.c:1350`,
`input.c:779`).

**Smell:** The "canonical chain" mental model isn't preserved by
`commit_current_input`. Two of the four dispatch sites
(`;` key and `editor_feed_line`) use the canonical ordering;
the two Enter sites use a block-first variant.

**Why it matters:** The README and CLAUDE.md flag ordering as
load-bearing — but the load-bearing claim only really applies to
the var-statement pair (`float_decl` before `assign`). The
block-vs-var top-level reordering under Enter is genuinely
intentional (so a `}` on Enter closes the block instead of being
misread as a free-floating var statement). What's missing is the
*written* invariant that explains the variant.

**Fix:** *Not* "replace with `editor_try_commit_any`" — that loses
the `_var_statements_then_insert` post-effects (insert-mode flip,
input clear) the overwrite path depends on. Instead: (a) document
the block-first variant inline at `commit_current_input` with a
comment naming the two ordering invariants (load-bearing
`float_decl → assign` *within* var-statements; intentional
`block_structs → var_statements` *under Enter*), and (b) add a
unit test that exercises both Enter helpers and pins their
ordering. If you want a single canonical helper, the cleaner
shape is `editor_try_commit_block_structs_then_var_statements_insert()`
that wraps the existing pair and is called from both Enter sites,
keeping the post-effects intact.

### 11. ✅ `editor_compile_*` functions advertised as context-pure but read live state

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

**Fix (under-scoped — read first):** A proper fix is a real
extraction, not a small refactor. The live-state coupling is
broader than just `source_scope_*`:

- `src/repl/source_scope.h:73` and `source_scope.c:153` currently
  expose only live-state queries (they read from the live
  `g_repl_state` document); adding a "view" variant means
  duplicating each query to take a `const GLCmd *cmds, int n` (or
  a `SourceTextView`) and routing the live versions through it.
- `collect_visible_vars` (`src/repl/core.c:721`, declared at
  `src/repl/core_internal.h:110`) reads `source_document_view()`,
  `repl_state_document_cmds()`, and `repl_state_document_count()`
  directly. `editor_compile_if_block` (`commit.c:404`) and
  `editor_compile_for_loop` (`commit.c:887`) call it. A
  `source_scope_view` extraction alone won't make these
  functions context-pure. The author of `collect_visible_vars`
  already left a comment at `core.c:732` acknowledging it should
  take a view parameter.
- Similar live-scope reads exist in `src/repl/compile.c` (e.g.
  `repl_compile_close_brace`).

A proper extraction needs all three migrated together:
`source_scope_*_view(...)`, `collect_visible_vars_view(view,
pos, ...)`, and the compile.c live-scope readers.

The cheap interim is to **document** the contract loudly at
**every entry point that takes a `ReplCompileContext *` but
secretly reads live state**:

- the four `editor_compile_*` entry points in `commit.c`;
- **also `repl_compile_var_assign` in `src/repl/compile.c:858`**,
  which also calls live `collect_visible_vars` and is reached
  from both the editor commit path and the file-load path via
  `load_try_block`.

Doc-comment at each: "context-pure for document data,
live-state-coupled for scope queries and visible-var
collection — callers must apply the change to the live
document before the next scope-dependent or visible-vars
call." That's a one-pass doc-comment change today, with the
full extraction queued as its own week of work.

### 12. [Tier D — stale after later refactor] Cursor blink ownership mismatch is no longer present

**Where:** `src/editor/README.md:88` and `src/editor/state.h`
(`EditorCursorBlinkState` inside `EditorState`)

**Original smell:** README said state.c owned "cursor blink" while
`state.h` allegedly disclaimed it to `UiState`.

**Current state (verified 2026-05-25):** The editor-side ownership
claim is now true. `EditorState` carries `EditorCursorBlinkState`
and the README's file map includes cursor blink. Do **not** apply the
old fix that strikes "cursor blink" from the README.

**Disposition:** No editor-side action. If `src-ui` audit #17 is
still open, resolve it from the UI side against the current source;
this editor finding is stale.

### 13. ⏳ [Tier C — deferred] Two parallel encodings of `float_decl → var_assign` order

**Where:** `src/repl/compile.c:60-87` (in `repl_compile_dispatch`)
vs. `src/editor/commit.c:1335-1339` (in
`editor_try_commit_var_statements`)

**Smell:** `compile_dispatch` encodes float_decl → var_assign.
`editor_try_commit_var_statements` independently calls
`editor_try_commit_float_decl` → `editor_try_commit_assign_variable`,
each of which calls `repl_compile_float_decl` /
`repl_compile_var_assign` directly (not through dispatch).

**Why it matters:** A future "insert a new var-statement handler"
requires touching two unrelated functions; the load-bearing
ordering comment in CLAUDE.md only covers one.

**Fix (carefully scoped — read first):** Do **not** "route through
`repl_compile_dispatch`" naively. `repl_compile_dispatch`
(`compile.c:60`) only returns a bare `ReplCompiledChange`. The
two editor handlers do considerably more than compile:
`editor_try_commit_float_decl` (`commit.c:1181`) builds an
`EditorCommitPlan` with `clear_input`, `cursor_target`,
`load_line_after_apply`, a `commit_message`, and ends with
`repl_mark_normals_dirty()`. `editor_try_commit_assign_variable`
(`commit.c:1228`) does the same plus assign-specific effects.
The `_var_statements_then_insert` variant (`commit.c:1350`) layers
in the insert-mode flip and input clear that overwrite-Enter
depends on.

The actual fix has two viable shapes; prefer (1) unless the commit
transaction is already being reworked:
1. **Share a kind list, map locally to handlers.** Define a
   single ordered enum (`enum ReplVarStmtKind { VAR_STMT_FLOAT_DECL,
   VAR_STMT_ASSIGN }`) in a shared header. `compile.c` keeps
   its own `static const ReplCompileFn` table mapping kinds to
   pure compilers; `commit.c` keeps its own `static` table
   mapping kinds to editor wrappers. Both iterate the kind list
   in order. One source of truth for ordering; each TU keeps its
   per-kind dispatch table local (a `static` table isn't
   shareable across TUs, and the editor side needs per-handler
   post-effects with different shapes anyway).
2. **Adapter that preserves post-effects.** Introduce a
   `editor_compile_var_statement(...)` that calls
   `repl_compile_dispatch` for the compile step then layers the
   right post-effects per kind. Bigger move; only safe if the
   adapter can identify which var-statement kind matched and preserve
   `clear_input`, cursor/load-line behavior, commit messages, source-
   dirty marking, and the overwrite-Enter insert-mode flip. #28 by
   itself is not enough reason to take this shape.

Either way, the audit's first-draft "route through dispatch and
remove the direct calls" recipe is **wrong** — it silently drops
every `EditorCommitPlan` effect listed above and breaks overwrite
Enter's insert-mode flip.

### 14. ✅ `editor_try_assign_variable` breaks the `editor_try_commit_*` naming convention

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

### 15. [Tier C — deferred] Error reporting drifts across handlers

**Where:** `src/editor/commit.c` try-commit handlers that call
`repl_set_status_error(...)` / `repl_set_status(...)` directly.

**Original smell:** The now-deleted `editor_commit_current_input`
returned an `EditorCommitResult` diagnostic, while the live
`editor_try_commit_*` chain wrote status as an implicit side effect.

**Current state (verified 2026-05-25):** `editor_commit_current_input`,
`EditorCommitResult`, and `editor_commit_apply_compiled_change` are
gone from source and tests. The only remaining policy is status side
effects from the live handlers.

**Why it matters:** The hidden status dependency still makes isolated
handler tests awkward: callers cannot observe a diagnostic except via
the installed status host effect.

**Fix:** Do **not** resurrect `EditorCommitResult` just to make this
symmetrical. If testability becomes painful, route handler status
publication through a narrow capture-friendly sink/host effect, or add
test helpers that install the existing host-effects status callbacks.

### 16. ✅ Apply-block call sites reach the primitives through different surfaces

**Where:** `src/editor/commit.c:116-123, 148-156, 250-257`

**Smell:** `editor_commit_current_input`,
`editor_commit_apply_external_change`, and
`editor_commit_apply_plan` each contain the same 4-step apply
sequence (`apply_predef_ops` → `apply_scratch_ops` →
`editor_buffer_apply_compiled_change` → `apply_repl_change`).
But each reaches the underlying primitives differently — one via
passed-in `services`, one via a fresh `svc =
editor_services_default()`, one via direct `repl_apply_*`.

**Why it matters:** A fix to the apply sequence needs coordinated
changes across the call sites.

**Fix (conditional — depends on #27):** This is "three call sites"
*only* if `editor_commit_current_input` is still alive. The
afternoon sequence deletes it via #27, dropping the count to two
(`editor_commit_apply_external_change` and
`editor_commit_apply_plan`). Two viable framings:

1. **If #27 lands first (recommended):** rewrite this finding as
   "dedupe the two remaining apply paths
   (`editor_commit_apply_external_change` and
   `editor_commit_apply_plan`) — both still reach
   `repl_apply_*` differently after services dismantle."
2. **If #27 is dropped:** keep the three-site extraction
   (`apply_compiled_three_halves`), but read #15's keep-#15 branch
   first — `EditorCommitResult` survives and the three sites stay
   asymmetric on diagnostic shape too.

Either way the fix is "extract one canonical four-step apply
helper"; the count just changes based on what else lands.

### 17. ✅ Editor undo reaches into `g_predef_vars` / `g_num_predef_vars` directly

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

### 18. ⏳ [Tier C — deferred] Five near-identical "collect-visible-vars → parse → place" sequences

**Where:** `src/editor/input.c:437-486, 694-751, 1351-1394,
1496-1538`; also `commit_current_input`'s overwrite branch at
L763-803

**Smell:** Same shape — `collect_visible_vars(insert_idx, ...)` →
`parse_command_ctx`-or-`repl_parse_and_normalize_strict` two-branch
→ `editor_place_parsed_command` → status — replicated 4-5×. Two
different parse APIs are used across them.

**Why it matters:** Any tweak to the parse policy needs to be
replicated 4-5×. **But the two parse APIs are not interchangeable.**
`repl_parse_and_normalize_strict` (`src/repl/core.c:421, 475`)
sets `strict_refs` and preserves variable expressions through
`parse_and_normalize_impl`; the strict path is what rejects
undefined top-level function calls (see `core_internal.h:50`).
The plain `parse_command_ctx` path doesn't. The audit cannot
recommend "pick one parse entry point" — that would silently drop
strict-mode rejections from the affected dispatch sites.

**Fix:** Extract `parse_for_commit(insert_idx, GLCmd *out, char
*text_out, size_t, /* policy: */ int strict_refs, int preserve_var_exprs)`
that takes the strict/permissive and preserve-expression flags
**as inputs**, not as a hard-coded choice; each caller still names
its own policy. Audit the 4-5 call sites first to enumerate which
combination each currently uses (the audit didn't determine this);
the helper signature is the documentation contract. This extraction
also owns the two remaining `EditorServices.parse_command_ctx` call
sites, so coordinate with #28 and update the `check-editor-repl-surface`
ratchet/baseline if direct `repl_*` calls replace service calls.

### 19. ✅ Three open-coded "skip leading whitespace, then trim trailing `;`/whitespace"

**Where:** `src/editor/input.c:296-336, 396-403, 582-589`
(`editor_load_line_to_input`, `rewrite_source_text_with_indent`,
`input_matches_committed_line`)

**Smell:** Same canonical strip implemented three times inline; no
shared helper.

**Fix:** Extract a single helper (probably in `src/repl/`, given
`parser.c` also has canonical-form duties).

### 20. [Tier C — ✅ done] Tutorial commit-gating helpers live in `input.c` as file-locals

**Where:** `src/editor/input.c:1233-1299`, call sites at L875,
L1277, L1294-1297, L1312-1322, L1335-1346, L1396-1397

**Smell:** `tutorial_precheck_current_input` and
`tutorial_advance_if_commit_ok` are file-local helpers reading
tutorial state. The README's sanctioned
`glr_ctrl_router_handle_tutorial_ack_key` path correctly hoists
ack-key routing — but commit-time tutorial gating still relies on
input.c locals.

**Resolution:** Assessed as correct layering. These are editor
adapters, not misplaced tutorial policy. The real matching, step
advancement, and locked-line rules live in `tutorial.c`. The two
statics bridge editor facts (input text, cursor position, insert
mode) into the tutorial policy API and translate the result back
into editor side effects (completion clear, status message). They
stay in `input.c` because they read editor state that `tutorial.c`
must not include directly. Moving them to `tutorial.c` would invert
the dependency (tutorial → editor) or require heavy parameter
plumbing for minimal gain. Documented with adapter-intent comments
at both the forward declarations and the definitions.

### 21. [Tier C — ✅ done] Tagged-union `EditorClipboardKind` is never consulted via switch

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

### 22. [Tier C — ✅ done] `EditorInputView.input` aliases live storage with no documented liveness

**Where:** `src/editor/state.h:64-74`, `state.c:300-313`

**Smell:** `view.input` and `view.pending_newline` point at live
`g_editor_state` buffers. Hold the view, mutate via
`editor_input_set_text(...)`, your `view.input_len` is stale but
`view.input` still tracks the live mutation. Half by-value, half
by-reference.

**Why it matters:** Subtle aliasing footgun. `editor_state_input()`
is called repeatedly per function so the bug only surfaces when
someone stashes the view.

**Fix:** Prefer a doc-first fix unless a real stashed-view bug appears:
document that `input` and `pending_newline` are borrowed live pointers
while the scalar fields are by-value snapshots. A fully by-value view
is also valid, but it copies two `MAX_INPUT_LEN` buffers per call and
should be justified by an actual caller need.

### 23. [Tier C — ✅ done] `editor_undo_clear` is contract-required but compile-unenforced

**Where:** `src/editor/undo.h:97-102`, call sites at
`src/app/glr_actions.c:360, 365, 569, 585`,
`src/app/glr_ctrl.c:2088, 2719, 2762`,
`src/editor/inline_file_prompt.c:154`

**Smell:** Header documentation says it "must be called on
wholesale replacement"; nothing checks it. Failure mode is silent
— Ctrl+Z restores foreign-scene state into the new scene.

**Resolution:** Added `editor_undo_note_wholesale_replacement()` which
clears both undo/redo rings AND bumps a generation counter.
`EditorUndoSnapshot` now carries a `generation` field stamped at push
time; `pop` and `redo` peek the next snapshot's generation before
restoring and refuse cross-generation restores (clearing the ring and
reporting "Nothing to undo/redo"). All 8 production call sites
migrated from raw `editor_undo_clear()` to the semantic API.
`editor_undo_clear()` is retained for `undo.c` internals and test
scaffolding. Structural guard `check-no-raw-undo-clear` (in
`check-state-ownership`) enforces the ban in `src/app/` and
`src/editor/` (excluding `undo.c`). Tests in `test_repl_editor.c`
(3c/3d/3e) cover: cross-generation pop/redo blocked, same-generation
undo/redo works, multiple wholesale replacements isolate each world.

### 24. [Tier C — ✅ done] Three sites hardcode `name[16]` instead of a shared constant

**Where:** `src/editor/undo.h:58`, `src/editor/state.h:147`,
`src/repl/eval.h:133`

**Smell:** Magic constant duplicated across module boundaries;
the same finding as `src-repl` audit #28 and `src-ui` not-applicable.

**Fix:** `#define REPL_PREDEF_NAME_MAX 16` in `eval.h` (the upstream
source); reference the predef-variable name buffers from there. Do
not blindly replace every local `char name[16]` in the tree — several
are parser scratch buffers or command-shaped names and should only
move if they are semantically tied to the predef table.

### 25. [Tier D — kept on purpose] Wrapped editor search functions are pass-throughs to `ui_text_*`

**Where:** `src/editor/search.c:57-65`

**Smell:** `editor_search_find_next_in_text` /
`_prev_in_text` are one-line wrappers around
`ui_text_find_{next,prev}_in_text` with identical signature.
Tests use both names; production callers could use `ui_text_*`
directly.

**Fix:** Either delete the wrappers and have callers use
`ui_text_*` directly, or inline them as `static` inside
`search.c`. (Same as the `src/ui/` audit's #37.)

### 26. [Tier C — ✅ done] `editor_compile_for_loop` is the only structured-compile that uses `EditorServices`

**Where:** `src/editor/commit.c:872`

**Smell:** `EditorServices svc = editor_services_default(); ... svc.parse_command_ctx(...)`.
The other three structured-compile functions call `repl_eval_*` /
`parse_repl_func_signature` directly.

**Why it matters:** Noise that suggests these compile functions
might be substitutable; they aren't (the others bypass services
entirely).

**Fix:** Replace with `repl_parser_parse_command_ctx(body, &body_pl,
&parse_ctx)` directly when doing #28. Because this adds a direct
`repl_*` symbol to `commit.c` while removing an `EditorServices`
callback path, update the `check-editor-repl-surface` ratchet/baseline
in the same change.

## 🟢 Dead code / dead fields

### 27. ✅ `editor_commit_current_input` has zero production callers

**Where:** `src/editor/commit.c:65-130` (definition);
`commit.h:55` (declaration). Verified: only
`tests/test_repl_compile.c` calls it — **5 call sites at L630,
L639, L662, L679, L724** (the doc-comment block at L621 referenced
the function name without calling it). They exercise the
compile-failure, NO_CHANGE, and success paths.

**Smell:** Documented as "the common path for the live input row"
in `commit.h:10`, but the four real production dispatch sites all
bypass it and go through `editor_try_commit_*` + the open-coded
`repl_parse_and_normalize_strict` tail. The whole `EditorServices`
indirection (#28) exists primarily for this function.

**Fix:** Delete the function (and the `services` indirection it
requires) and update the 5 test call sites — they're a small
cluster in one file and each test currently asserts something
about the wrapper's return shape rather than the underlying
behavior, so the tests should be rewritten to exercise the live
dispatch sites instead. Or, alternatively, route the four
production dispatch sites through it (the harder direction, since
those sites have post-effects the wrapper currently doesn't model).

### 28. ⏳ [Tier C — deferred] `EditorServices` is a one-implementation abstraction (with production callers — not pure cleanup)

**Where:** `src/editor/services.h:28-74`, `services.c:65-76`.
Remaining production callers verified at `src/editor/commit.c`
(`editor_compile_for_loop`) and `src/editor/input.c`
(`parse_for_overwrite_enter`, `commit_current_input`).

**Smell:** `editor_services_default()` is the only `EditorServices`
factory in the tree. The header at L6-9 admits: "gives the
controller a single place to substitute test doubles or alternate
implementations" — no such substitution exists in `src/` or
`tests/`.

**Why it matters:** Adds context churn (svc construction,
`void *user` plumbing) for no substitution benefit. **But unlike
#27, this is not a self-contained delete** — the four production
call sites use the services struct's function pointers; ripping
the struct out means migrating each of them to the direct
underlying calls (`repl_compile_dispatch`,
`repl_parser_parse_command_ctx`, etc.) at the same time.

**Fix (multi-step, not afternoon-safe):**
1. **Already landed:** `editor_commit_apply_external_change` and
   `editor_commit_apply_plan` now share `apply_compiled_three_halves`,
   which calls the `repl_apply_*` primitives directly.
2. Migrate `commit.c:872` (in `editor_compile_for_loop`) per
   finding #26 — replace `svc.parse_command_ctx(body, ...)` with
   `repl_parser_parse_command_ctx(body, ...)` directly.
3. Migrate `input.c:439, 634` together — these are the
   parse-and-place sites that finding #18 also touches; do them
   in the same change as the strict/permissive helper extraction.
4. Only then delete `EditorServices` + `editor_services_default`
   + the `user`-pointer plumbing in `commit.h`.

This sequence preserves the build at every step, but it must also
preserve the boundary guards: direct `repl_*` calls added while
removing service calls need a matching update to
`scripts/check-editor-repl-surface.sh` and
`scripts/baselines/editor-repl-surface.txt` so the ratchet reflects
the intended new surface instead of failing unexpectedly. The audit's
original framing as "delete in an afternoon" was incorrect — it's a
week-pass refactor that touches #26, #18, and the remaining production
sites. Current source no longer has the #27 / #29 APIs, so they are no
longer prerequisites.

### 29. ✅ `editor_commit_apply_compiled_change` is a test-only wrapper

**Where:** `src/editor/commit.c:1117-1119`, declared at
`commit.h:79`. Verified: only `tests/test_repl_compile.c` calls
it — **19 call sites** (plus 2 doc-comment references; the
21-match `grep` count includes both). Production code uses
`editor_commit_apply_external_change(change, 0)` directly.

**Smell:** Public wrapper with the body
`editor_commit_apply_external_change(change, 0)`. 19 test
references couple the test cluster to the wrapper shape rather
than to the underlying API.

**Fix:** Delete the wrapper; rewrite the 19 test sites to call
`editor_commit_apply_external_change(change, 0)` directly. The
rewrite is mechanical search-and-replace, but the LOC churn is
non-trivial — name it in the sequencing so "build-safe" stays
true.

### 30. ✅ `EditorInputState.input_capacity` and `pending_newline_capacity` are write-only

**Where:** `src/editor/state.h:49, 59`, `state.c:15-17, 304-309`

**Smell:** Initialized in sentinels and re-derived in
`editor_state_input()`, but the only reads are inside `state.c`
itself. The view hardcodes `MAX_INPUT_LEN` instead of reading the
stored fields. Three sources of truth for "input capacity": the
struct field, the view field, and the raw `MAX_INPUT_LEN`.

**Fix:** Delete both fields; use `MAX_INPUT_LEN` directly.

### 31. ✅ `EditorVariableDragState` typedef stranded in `state.h` after storage moved

**Where:** `src/editor/state.h:142-149, 195-197`

**Smell:** Comment at L195-197 says `variable_drag` lives on the
variable-panel peer. Yet the typedef still sits in
`editor/state.h`, used only by `src/subsystems/variable_panel/`.

**Fix:** Move the typedef to
`src/subsystems/variable_panel/variable_panel_state.h`.

### 32. [Tier C — ✅ done] Two pointless one-liner route wrappers in input.c

**Where:** `src/editor/input.c:992-994, 1594-1596`

**Smell:** `handle_search_key_route(key) { return editor_search_handle_key(key); }`
and `handle_search_special_route(key) { return editor_search_handle_special(key); }`
exist purely so dispatcher reads `if (handle_search_key_route(key)) return;`
symmetrically.

**Fix:** Inline both.

### 33. ✅ `special_begin_key` takes a param it ignores

**Where:** `src/editor/input.c:1554-1560`

**Smell:** Signature is `static void special_begin_key(int key)`
with `(void)key;` as first statement. Either use `key` (clear
line-range selection like `keyboard_begin_key` does) or drop the
parameter — currently a documentation lie.

**Fix:** `static void special_begin_key(void)`.

### 34. ✅ `editor_get_modifiers()` and `editor_input_active_modifiers()` are the same function

**Where:** `src/editor/input.c:140, 174`,
`src/editor/input.h:59, 63`

**Smell:** Two names for one operation, split so external callers
use the long name and internal handlers the short. Indistinguishable
in behavior; both go through the same test seam.

**Fix:** Pick `editor_input_active_modifiers` (matches file's prefix
convention); delete the wrapper.

### 35. [Tier C — ✅ done] `editor_state_search()` returns a 1 KB struct by value per call

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

### 36. ✅ Forward declarations in `state.c` for functions already in `state.h`

**Where:** `src/editor/state.c:245-250`

**Smell:** Three forward declarations
(`editor_input_clear`, `editor_pending_newline_clear`,
`editor_cursor_pos_set`) for functions defined later in the same
file. They're already declared via `state.h` which the file
includes.

**Fix:** Delete the forward-decl block.

### 37. ✅ `editor_state_input_reset` mutates `insert_mode` directly, bypassing setter

**Where:** `src/editor/state.c:319-323`

**Smell:** `g_editor_state.input.insert_mode = 0;` instead of
`editor_insert_mode_set(0)`. Every other caller goes through the
setter.

**Fix:** `editor_insert_mode_set(0);`.

### 38. ✅ Comments reference "legacy" code that is the live path

**Where:** `src/editor/commit.c:88, 190, 609, 1202`

**Smell:** Several comments call the existing primary path
"legacy" (`"Caller falls through to the legacy try_commit_* chain"`,
`"The legacy guard returns 0 from editor_try_commit_func_def"`)
when the "legacy" name *is* the live path. Future readers will
look for a "modern" path that doesn't exist.

**Fix:** Search-and-replace "legacy" → "current" / "live"; or
delete the comments.

### 39. ✅ `editor_commit_apply_plan` header says it captures undo; the body explicitly disclaims it

**Where:** `src/editor/commit.h:178` vs. `commit.c:238`

**Smell:** The header lists step 2 of `editor_commit_apply_plan`
as "Capture undo pre-state." The body says the opposite:
> "NOTE: undo capture is the caller's responsibility. The ;-key
> / Enter / `editor_feed_line` dispatch sites in
> `src/editor/input.c` push a snapshot before invoking the
> try_commit_* chain; this helper deliberately does NOT push a
> second snapshot, to avoid double-capture."

This is a **false transaction contract** in the public-facing
docstring. Any new caller reading the header will assume the apply
function handles undo, will skip their own undo push, and silently
won't get undo. Past the docstring drift, the post-effect order
also has additional steps (`clear_autocomplete →
func_decl_resume_publish`) the header doesn't list, and the
"status" step is in the caller (`editor_commit_apply_plan:262`),
not in `apply_post_effects` itself.

**Fix:** Strike "Capture undo pre-state" from the header
description; replace with "Undo capture is the caller's
responsibility (see body comment for rationale)." While editing,
expand the post-effect list to all 8 effects and note where status
is applied. **Then** add a new finding-of-its-own (or fold into
#23): every call site that doesn't push a snapshot before
`editor_commit_apply_plan` is a latent undo-skip bug.

### 40. [Tier C — ✅ done] `apply_compiled_change` cluster has 3 distinct context constructions for the same payload

**Where:** `src/editor/commit.c:1182, 1229, 1283`

**Smell:** Each try-commit handler opens with the same two
statements on one line:
`ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line()); ctx.insert_mode = editor_insert_mode();`.
This duplicates the body of `default_context()` in
`services.c:21-30`.

**Fix:** Add a local helper in `commit.c` (for example
`static ReplCompileContext editor_compile_context_live(void)`) and
call it from the try-commit handlers. Do **not** put the helper in
`services.h`: #28 deletes `EditorServices`, and adding new public
surface there points future cleanup in the wrong direction.

## 🔵 Structural concerns

### 41. [Tier C — deferred, optional] `editor_clipboard_paste_current` decision logic is opaque

**Where:** `src/editor/clipboard.c:245-413`

**Smell:** Original finding mixed three concerns: input-text paste,
line-range paste, and the 1 MB stack snapshot (#2).

**Current state (verified 2026-05-25):** Input-text paste is already
split into `editor_clipboard_paste_input_text()`, and #2's stack
snapshot is now heap-sized by pasted line count. The remaining smell is
only the line-range paste tail in `editor_clipboard_paste_current`.

**Fix:** Optional opportunistic cleanup: extract the remaining line
paste path into `paste_lines(state)` and `finalize_after_paste(...)`;
do not treat input-text extraction or stack-snapshot removal as still
pending.

### 42. [Tier C — ✅ done] `mouse_func` signature has 3 silenced unused parameters

**Where:** `src/editor/input.c:1823-1832`

**Smell:** `static void mouse_func(int button, int state, int x,
int y) { (void)button; (void)x; (void)y; if (state != GLUT_UP) return; ... }`.
Three `(void)` casts on one line make this read like a stub. The
signature is required by GLUT shape; legitimate, but un-commented.

**Fix:** Add `/* GLUT callback shape; only state == GLUT_UP is
editor-relevant. */`; lay the casts out one per line.

### 43. [Tier C — ✅ done] Edit-line override path edge: `MAX_INPUT_LEN - 2` vs `- 1` is silently load-bearing

**Where:** `src/editor/input.c:305-336`
(`editor_load_line_to_input`)

**Smell:** Clamps to `MAX_INPUT_LEN - 1` for plain and
`:`-prefixed-already paths but `MAX_INPUT_LEN - 2` for the path that
prepends `:`. Correct (one byte reserved for the leading `:`) but
uncommented; future-edit hazard.

**Fix:** Add `/* -2: reserve one byte for the leading ':' below */`;
or compute via `MAX_INPUT_LEN - 1 /* NUL */ - 1 /* leading ':' */`.

### 44. ✅ Four hand-written `pending_newline` clears bypass the existing helper

**Where:** `src/editor/input.c:273-274, 826-827, 849-850,
1385-1386`. Helper already exists at `src/editor/state.h:339`
(declaration) and `state.c:493` (definition); it's `void
editor_pending_newline_clear(void)`.

**Smell:** Four sites open-code `inp->pending_newline[0] = '\0';
inp->pending_newline_len = 0;` while the canonical helper has
existed publicly the whole time. `state.c` itself uses the helper
(L319).

**Fix:** Replace the four open-coded clears in `input.c` with
calls to `editor_pending_newline_clear()`.

### 45. [Tier C — ✅ done] `is_word_char` re-implements the project's identifier predicate

**Where:** `src/editor/input.c:1407-1410`

**Smell:** Yet another
`(c >= 'a' && c <= 'z') || ... || c == '_'` inline. `src/repl/eval.c`
has the same predicate inlined in 6 places. The `src-repl` audit
#18 calls this out too.

**Fix:** Add `int repl_is_ident_continue(unsigned char c)` to
`src/repl/eval.h`; use from both.

### 46. ✅ Tutorial-Tab branch hand-writes the input buffer instead of using the existing helper

**Where:** `src/editor/input.c:1213-1222`. Helper already exists
at `src/editor/state.h:299` (declaration) and `state.c:352`
(definition); it's `void editor_input_set_text(const char *text)`.

**Smell:** The Tab handler open-codes a `strncpy + input_len =
strlen + cursor_pos_set` sequence directly on
`editor_state_input_mut()->input` while the canonical
`editor_input_set_text` helper has existed the whole time.

**Fix:** Replace the open-coded sequence with a single
`editor_input_set_text(text)` call.

### 47. [Tier C — ✅ done] Tutorial-locked guard messages drift in wording

**Where:** `src/editor/input.c:186, 292`, `clipboard.c:29`

**Smell:** Three guards say roughly the same thing — two with
"comment", one with "instruction". Trivial but real — they'll
show up next to each other in status text.

**Fix:** Pick one phrasing; ideally a small tutorial-subsystem helper
that publishes the status text. Avoid adding a new generic
`repl_set_status_tutorial_locked()` special case unless other modules
need that exact status policy.

### 48. [Tier C — ✅ done (doc)] `EditorState` carries ~1.2 MB of per-frame transient overlay lists

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

### 49. [Tier C — ✅ done (doc)] `state.c:248-250` exposes `EditorClipboardKind` setter without payload-validity guarantee

**Where:** `src/editor/state.c:545-563`
(`editor_state_clipboard_count_set`)

**Smell:** Setting `n > 0` writes `line_count = n` and
`kind = EDITOR_CLIPBOARD_LINES`, but doesn't ensure
`lines[0..n)` has fresh data; relies on callers to write them first
(production does this in `clipboard.c`, while tests also poke the
setter directly).

**Why it matters:** Brittle informal contract. The tagged-union
invariant ("kind matches payload") nominally holds, but content
can be undefined if a future caller forgets to populate.

**Fix:** Make this private; or take
`(const char *const *lines, int count)` and write both inside.

### 50. [Tier C — ✅ done] Reformat saves five fields; the doc claim is broader

**Where:** `src/editor/reformat.c:19-40`, `reformat.h:1-14`

**Smell:** Header says it "saves/restores the typed input buffer"
and "keeps the editor session intact." Actually saves: `edit_line`,
`inserting`, `input[1024]`, `input_len`, `cursor_pos`. NOT saved:
selection, search, autocomplete, scroll, pending_newline, undo
ring. If `repl_reformat_program` ever mutates any of those, the
user-visible session silently loses those parts.

**Fix:** Either document "reformat doesn't touch X, Y, Z so we
don't save them"; or expand the save set.

### 51. ✅ Stale doc reference to a non-existent line

**Where:** `src/editor/commit.h:267-270`

**Smell:** Comment says "editor_commit_func_decl_resume_set is
declared at line 190" — actual declaration is at L176.

**Fix:** Drop the line-number reference, or delete the comment
(the symbol is declared once at L176 with its own docblock; no
missing declaration to explain).

## Sequencing

The sequencing below has been corrected from the audit's first
draft. Findings that originally lived in the afternoon pass but
turned out to be either not self-contained, or to recommend fixes
that would break the build / invert the boundary, have been moved
or rewritten. Read every item before treating this as an
implementation checklist.

### One-afternoon pass — truly self-contained, build-safe

1. **#1** + **#2** — Move the 2 MB and 1 MB stack allocations to
   file-scope statics or heap. Each is a 2-line change at a single
   site.
2. **#3** — Either fill `err` for the stray-`}` case in **both**
   `editor_compile_close_brace` (`commit.c:285`) **and**
   `repl_compile_close_brace` (`compile.c:1485`), or drop the
   unused parameters in both (and split the typedef). The REPL
   variant is reached via `load_try_block` (`load.c:94`), so
   editor-only fix preserves the file-load diagnostic gap.
3. **#4** — Add `editor_undo_clear()` after the
   `repl_load_workspace(dir)` call, gated on the return value.
   Recommended: `if (n >= 0) editor_undo_clear();` (clear on
   success and on empty workspace; preserve only on I/O error).
   But this is a product call on empty-workspace semantics —
   read finding #4 for the predicate options.
4. **#5** — Make `editor_state_capture()` call
   `editor_state_get_defaults()` (idempotent after first call) so
   captured state always has the correct sentinels.
5. **#6** — Rename `editor_clipboard_clear_selection` to
   `editor_selection_clear_line_range` (or similar without
   `clipboard_` prefix); update the 10 call sites. Mechanical.
6. **#14** — Rename `editor_try_assign_variable` →
   `editor_try_commit_assign_variable`. 11 references across src/
   and tests/.
7. **#44** + **#46** — Replace the four open-coded
   `pending_newline` clears with the existing
   `editor_pending_newline_clear()` (state.h:339); replace the
   tutorial-Tab open-coded buffer write with the existing
   `editor_input_set_text()` (state.h:299). The audit's first
   draft proposed adding new helpers; the helpers already exist
   publicly.
8. **#27** + **#29** — Delete two test-only API shapes:
   - `editor_commit_current_input`: **5 calls** in
     `tests/test_repl_compile.c` at **L630, L639, L662, L679,
     L724** (the doc-comment at L621 references the function
     name but does not call it). Rewrite each test to exercise
     the live dispatch sites.
   - `editor_commit_apply_compiled_change`: **19 calls** in the
     same test file — rewrite each to call
     `editor_commit_apply_external_change(change, 0)` directly.
     Mechanical search-and-replace but ~19 LOC churn.

   **Current status (2026-05-25):** #27 and #29 have landed:
   `editor_commit_current_input`, `EditorCommitResult`, and
   `editor_commit_apply_compiled_change` are gone from source/tests.
   That unblocks #28's remaining services migration; no decision on
   the old value-typed commit API is still pending.

   **Test-cost note:** the 5 `editor_commit_current_input` calls
   in `test_repl_compile.c` aren't just shape-testing the
   wrapper — they cover the compile→undo→preflight→apply
   transaction boundary that the wrapper sequences. Rewriting
   them to use `editor_commit_apply_external_change(change, 0)`
   directly loses that coverage; the rewrite needs to exercise
   the live dispatch sites (`editor_try_commit_*` chain) to
   preserve it. This is a meaningful redesign of what those
   tests cover, not a mechanical substitution.
9. **#30** + **#31** + **#33** + **#34** + **#36** + **#37** —
   Dead-field and naming cleanup. All mechanical.
10. **#38** + **#39** (docstring half only) + **#51** —
    Documentation fixes: replace "legacy" comments, strike
    "Capture undo pre-state" from `editor_commit_apply_plan`'s
    header (#39's full fix continues in the week pass), drop the
    stale line-number reference in `commit.h:267-270`.

### One-week pass — multi-site refactors

The dominant work is **closing the layering inversion**,
**resolving dispatch-chain drift**, and **dismantling the
`EditorServices` indirection without breaking the build**.

- **#7** — *Product decision first.* Decide whether Delete should
  delete-right. If yes, add `edit_op_buffer_delete_right_of_cursor`
  and dispatch on key. If no, document the conflation at each of
  the three dispatch sites. Either way, the audit cannot proceed
  without a product call.
- **#8** + **#9** — Hoist `editor_reset_transients` and
  `editor_input_restore_hidden_code_panel` into
  `src/app/glr_ctrl.c`; add `accept` to
  `EditorCompletionProvider`. Together they remove the four
  `app/` includes from `editor/input.c`.
- **#10** — Do **not** "replace with `editor_try_commit_any`" as
  the first-draft fix said — that would lose the
  `_var_statements_then_insert` post-effects the overwrite Enter
  path depends on. Instead, add an inline comment at
  `commit_current_input` (`input.c:633`) naming both ordering
  invariants (the load-bearing `float_decl → assign` and the
  intentional `block_structs → var_statements` under Enter), and
  add a unit test that exercises both Enter helpers and pins
  their ordering. Optionally extract
  `editor_try_commit_block_structs_then_var_statements_insert()`
  as a single canonical helper that both Enter sites call.
- **#11** — *Larger than first stated.* The live-state coupling
  spans three modules, not one: `source_scope.h` exposes only
  live-state queries today; `collect_visible_vars`
  (`src/repl/core.c:721`) reads the live document directly and
  is called from `editor_compile_if_block` /
  `editor_compile_for_loop`; similar reads exist in
  `src/repl/compile.c`. Two-stage approach:
  (a) document the live-state coupling at each affected
  `editor_compile_*` entry point **and** at
  `repl_compile_var_assign` (one-pass doc fix);
  (b) queue the full extraction (`source_scope_*_view`,
  `collect_visible_vars_view`, and the compile.c migration)
  as its own week.
- **#13** — *Do not naively route through `repl_compile_dispatch`*
  — `repl_compile_dispatch` only returns `ReplCompiledChange`,
  while the editor wrappers build `EditorCommitPlan` effects
  (clear_input, cursor_target, load_line_after_apply,
  commit_message, normals dirty) plus the
  `_var_statements_then_insert` insert-mode flip. Preferred shape:
  (a) define one shared ordered var-statement kind list
  (`VAR_STMT_FLOAT_DECL`, `VAR_STMT_ASSIGN`) and let each TU map
  those kinds to its own local handler table — pure compilers in
  `compile.c`, editor wrappers in `commit.c`. Alternative:
  (b) introduce
  `editor_compile_var_statement(...)` adapter that calls
  dispatch then layers per-kind post-effects — bigger, and only
  safe if it can identify the matched kind and preserve all editor
  post-effects. Do not take (b) merely because #28 is in flight.
- **#16** — *Site count depends on #27.* If #27 lands in the
  afternoon (audit recommendation), `editor_commit_current_input`
  is already gone and this becomes a two-site dedupe between
  `editor_commit_apply_external_change` and
  `editor_commit_apply_plan`. If #27 is dropped, it stays a
  three-site extraction. Either way: extract one canonical
  four-step apply helper. Read the finding for the conditional
  framing.
- **#17** — Add `repl_eval_predef_copy` seam; route undo through
  it.
- **#18** — *Preserve strict/permissive semantics.* Extract a
  `parse_for_commit(..., int strict_refs, int preserve_var_exprs)`
  helper that takes the policy as flags. Audit the 4-5 call sites
  first to enumerate which combination each currently uses; the
  helper signature is the documentation contract. Do **not**
  silently collapse to a single parse API. This is also where the
  remaining `EditorServices.parse_command_ctx` call sites should
  disappear; update `check-editor-repl-surface` and its baseline if
  the direct `repl_*` surface changes.
- **#19** — Extract the canonical strip helper (probably in
  `src/repl/`).
- **#23** — ✅ Done. See finding above.
- **#28** — *Multi-step `EditorServices` dismantling.* Order:
  (1) already landed: `apply_compiled_three_halves` is the shared
  direct `repl_apply_*` sequence;
  (2) migrate `commit.c:872` (#26: replace `svc.parse_command_ctx`
  with `repl_parser_parse_command_ctx`); (3) migrate `input.c:439,
  634` together with the #18 helper extraction; (4) only then
  delete `EditorServices` + `editor_services_default` + the
  `user`-pointer plumbing. The build stays green at every step, but
  the ratchet must be updated deliberately: direct `repl_*` calls that
  replace service calls affect `scripts/check-editor-repl-surface.sh`
  / `scripts/baselines/editor-repl-surface.txt`. #27 + #29 have
  already landed in current source and no longer block this work.
- **#39** (full fix) — Once the docstring is corrected
  (afternoon), audit every call site of
  `editor_commit_apply_plan` that doesn't push an undo snapshot
  beforehand. Each is a latent undo-skip bug.
  **Verified (2026-05-25):** all 3 production call sites
  (`commit.c:1166`, `1209`, `1246`) are inside
  `editor_try_commit_*` handlers, which are only reached from
  dispatch sites (`input.c`) that push undo first. The undo
  contract is documented at `commit.c:1118-1124`.
  `editor_feed_line` intentionally skips per-line undo (bulk
  loader — callers bracket the load). No latent undo-skip bugs
  found.

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
  `UiState`) was originally the inverse of editor finding #12, but
  the editor side is now resolved/stale: cursor blink lives on
  `EditorState` and the README matches. Re-check the UI finding
  against current source before applying any old cross-audit recipe.
- `src-ui-code-smell-audit.md` finding #37 (search wrapper
  pass-throughs) is the same as editor finding #25 — fix once at
  the editor side.
