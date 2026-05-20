# Edit-line ownership migration (editor-demo Option A follow-up)

## Summary

Move the active edit-line cursor from `ReplState` to `EditorState`.
This closes out the last stub in `tools/editor_demo/repl_shim.c`
(`repl_state_edit_line`) and aligns ownership with the layering
claim: the editor (text-document model + controller) owns the
cursor over the document; the REPL pipeline takes that cursor as
explicit input to its parse / flatten / compile / load / export work.

Picked from the two options the prior `editor-demo.md` plan named:

- **Option A (this plan):** full ownership migration. Multi-day
  refactor touching ~176 call sites across the codebase. The
  architecturally clean answer — edit-line lives where it
  conceptually belongs, the demo's shim drops to zero symbols,
  the existing `check-repl-no-direct-editor` invariant stays
  intact.
- **Option B (not chosen):** delete `edit_line_idx` from
  `EditorInputView` so `state.c`'s view builder stops calling
  `repl_state_edit_line`. ~5 call sites; demo's shim drops to
  zero; edit-line stays in `ReplState`. Faster, but doesn't fix
  the underlying layering. Not pursued.

This plan revises an earlier draft after review. Key revisions
versus the first draft:

1. **Layering invariant β is non-negotiable.** REPL pipeline
   code never calls into `EditorState`; it receives edit-line
   as an explicit parameter from the caller. No exception, no
   guard carve-out.
2. **`ReplCommandStore` cursor coupling is Phase 1, not a
   Phase 5 cleanup detail.** The store currently holds an
   `int *edit_line` pointer into `ReplDocumentState`; that
   pointer is the structural reason ownership can't move
   atomically. The store has to stop owning cursor adjustment
   *before* the storage flip.
3. **Single source of truth, every phase.** No bidirectional
   forwarders. The editor accessor reads from REPL during the
   transition (editor → REPL, a forward dep that's already
   allowed); after the atomic flip, the editor accessor reads
   from EditorState and the REPL function is deleted in the
   same commit.
4. **`EditorInputView.edit_line_idx` is removed, not blessed.**
   Edit-line is document-cursor state, not input-row state;
   `EditorDocumentState` is the right home. Putting it on the
   input view too muddles slices. The dead
   `EditorInputState.edit_line_idx` storage (never written) also
   goes.
5. **REPL-side inventory expanded.** The earlier draft listed
   only compile.c / flatten.c / parser.c / scenes.c; the actual
   set also includes core.c, example_loader.c, export.c, plus
   the load.h / state_views.h / state_owners.h headers and
   command_store.c via the store's pointer.

## Context

`tools/editor_demo/repl_shim.c` currently contains one symbol:

```c
static int g_demo_edit_line = 0;
int repl_state_edit_line(void) { return g_demo_edit_line; }
void demo_edit_line_set(int n) { ... }
```

The shim exists because `src/editor/state.c`'s `EditorInputView`
builder reads `repl_state_edit_line()` to populate the view's
`edit_line_idx` field, and because the REPL pipeline reads
edit-line during compile / flatten / load / export. After this
migration:

- Editor owns the storage (`EditorState.document.edit_line_idx`).
- Editor exposes `editor_state_edit_line()` / `_set()` /
  `_clamp()` accessors.
- REPL pipeline functions take edit-line as a parameter from the
  caller; no `repl_state_edit_line*` exists.
- `ReplCommandStore` no longer holds an `int *edit_line`
  pointer; mutating operations report a cursor delta that the
  caller (always editor-side) applies.
- `tools/editor_demo/repl_shim.c` is gone.

## Layering invariant (β, non-negotiable)

REPL pipeline code (`src/repl/*.c`) does not call
`editor_state_edit_line()` or any other `editor_*` accessor.
This is the existing `check-repl-no-direct-editor` invariant —
this migration preserves it.

The mechanism: every REPL pipeline function that needs edit-line
takes it as a parameter (typically `int edit_line_idx`). Callers
above the pipeline boundary (controller, commit, glr_ctrl,
editor) supply the value from the editor accessor.

This is what makes Phase 4's atomic flip simple: when the storage
moves, no REPL-side code has to change. Only callers of the
deleted `repl_state_edit_line()` need to update — and after
Phases 2 and 3, there are no callers of `repl_state_edit_line()`
outside `src/repl/state.c` itself, where it gets deleted.

## Phase 1 — `ReplCommandStore` cursor decoupling

The store currently exposes:

```c
typedef struct {
    GLCmd *cmds;
    int   *count;
    int    capacity;
    int   *edit_line;        /* <-- the structural problem */
} ReplCommandStore;
```

and `repl_command_store_live()` returns a store with
`.edit_line = &g_repl_state.document.edit_line_idx`. Mutating
operations (`_insert_one`, `_insert_many`, `_delete_range`,
`_load`) with `REPL_COMMAND_STORE_ADJUST_EDIT_LINE` adjust
`*store->edit_line` automatically.

After Phase 4 the storage moves to `EditorState`, so the pointer
would either (a) point into editor state from REPL code (which
violates β) or (b) need to be re-plumbed. The right answer per
β is **(c) the store stops owning cursor adjustment**.

### 1.1 — Drop the `edit_line` pointer from `ReplCommandStore`

```c
typedef struct {
    GLCmd *cmds;
    int   *count;
    int    capacity;
} ReplCommandStore;
```

`repl_command_store_live()` returns a store without the pointer.
The `REPL_COMMAND_STORE_ADJUST_EDIT_LINE` flag is renamed (or kept
as a no-op for source compatibility during transition) and the
store no longer reads or writes the cursor.

### 1.2 — Return a cursor delta from mutating ops

The mutating operations gain an optional out-parameter:

```c
typedef struct {
    int kind;       /* INSERT / DELETE / REPLACE / LOAD / CLEAR */
    int pos;
    int count;
} ReplCommandStoreCursorDelta;

int repl_command_store_insert_one(ReplCommandStore *store,
                                  int pos, const GLCmd *cmd,
                                  int flags,
                                  ReplCommandStoreCursorDelta *out_delta);
/* ... same for _insert_many, _delete_range, _replace_one, _load */
```

When `out_delta` is non-NULL, the function fills in what happened
so the caller can adjust whichever cursor it cares about.
`out_delta == NULL` is allowed (caller doesn't care).

### 1.3 — Editor-side delta apply helper

Add a single small function in `src/editor/state.c`:

```c
/* Adjust the editor's edit-line cursor in response to a command
 * store mutation. Pure cursor-math; no REPL involvement. */
void editor_state_edit_line_apply_store_delta(
    const ReplCommandStoreCursorDelta *delta);
```

The math:
- INSERT at pos: if `pos <= current_line`, `current_line += count`.
- DELETE at pos: if `pos < current_line`, `current_line -=
  min(count, current_line - pos)`. Clamp to [0, doc_count].
- LOAD: caller passes the new edit_line as part of the load
  call's separate `edit_line` parameter (existing pattern).
- REPLACE / CLEAR: no cursor adjustment.

This helper lives editor-side; callers in `src/editor/` use it,
callers outside (none expected — the store's auto-adjust callers
today are all editor-side commit / clipboard / undo code) update
their own cursor however they like.

### 1.4 — Migrate every `REPL_COMMAND_STORE_ADJUST_EDIT_LINE` caller

Grep call sites (`src/editor/clipboard.c`, `commit.c`, `undo.c`,
`input.c`, plus tests). Each becomes:

```c
ReplCommandStoreCursorDelta delta;
repl_command_store_insert_one(store, pos, &cmd, flags, &delta);
editor_state_edit_line_apply_store_delta(&delta);
```

For callers that don't care about the cursor (some test fixtures),
pass `NULL`.

### 1.5 — Verification

`make sample USE_GL_STUBS=1`, `make test-stubs`, full
`check-state-ownership` clean. Behavior unchanged: every call
site still adjusts the same cursor by the same amount, just
through an explicit apply call instead of an auto-pointer.

Commit message confirms: the store no longer holds a cursor
pointer; ownership of cursor adjustment lives editor-side.

## Phase 2 — Editor-side accessor that reads from REPL (transition adapter)

Add the editor-side API without moving storage yet. Storage stays
in `ReplState.document.edit_line_idx`. The editor accessor reads
from REPL via `repl_state_edit_line()`.

This is **editor → REPL**, a forward dependency that's already
allowed (editor depends on REPL by the existing layering). No
backward dep is introduced.

### 2.1 — Define `EditorDocumentState`

```c
/* src/editor/state.h */
typedef struct {
    int edit_line_idx;
    /* Future: cross-line selection anchor / end could land here.
     * Out of scope for this plan. */
} EditorDocumentState;

typedef struct {
    int edit_line_idx;
} EditorDocumentView;
```

Add to `EditorState`:
```c
EditorDocumentState document;
```

### 2.2 — Define the accessors

```c
int  editor_state_edit_line(void);
void editor_state_edit_line_set(int line);
void editor_state_edit_line_clamp(void);

EditorDocumentView   editor_state_document(void);
EditorDocumentState *editor_state_document_mut(void);
void                 editor_state_document_reset(void);
```

### 2.3 — Phase 2 implementation (transition mode)

During this phase only, the accessors forward to REPL:

```c
/* src/editor/state.c (transitional — flips in Phase 4) */
int editor_state_edit_line(void) {
    return repl_state_edit_line();
}
void editor_state_edit_line_set(int line) {
    repl_state_edit_line_set(line);
}
void editor_state_edit_line_clamp(void) {
    repl_state_edit_line_clamp();
}
```

The `EditorDocumentState document;` field on `EditorState`
exists but is unused while the forwarders are in effect. This
is *deliberate*: it's the destination for the Phase 4 atomic
flip. We define the shape now so Phase 3 callers compile against
the final API.

### 2.4 — `EditorInputView` builder

`src/editor/state.c`'s view builder switches:

```c
.edit_line_idx = repl_state_edit_line(),  /* before */
.edit_line_idx = editor_state_edit_line(),  /* after — same value via forwarder */
```

(`EditorInputView.edit_line_idx` is removed in Phase 4 per
"Path 4-Remove" below; this Phase-2 change is transitional.)

### 2.5 — Verification

Behavior identical: every editor accessor call returns exactly
the same value the REPL accessor would have. Build + test +
check-state-ownership clean.

## Phase 3 — Migrate readers in 6 ownership chunks

Each chunk is one or more commits; tree stays green between.
The phasing order matters: editor / app / widget / tests / demo
chunks first (their migration is mechanical — just call the
editor accessor), REPL pipeline chunk last (it's the
architecturally interesting one — parameter additions).

By the end of Phase 3, the *only* caller of `repl_state_edit_line()`
is `editor_state_edit_line()` (the transitional forwarder from
Phase 2). Every other site has migrated.

### 3.1 — Editor controllers

Files: `src/editor/{input,commit,clipboard,undo,reformat,search,inline_file_prompt}.c`,
plus `src/editor/state.c` (for non-view-builder reads).

Pure mechanical rename: `repl_state_edit_line()` →
`editor_state_edit_line()` and `_set()` / `_clamp()`
counterparts.

Also during this chunk: callers that need both the view and the
edit-line can drop to one call — but since
`EditorInputView.edit_line_idx` is being removed in Phase 4,
prefer the dedicated accessor `editor_state_edit_line()` so the
chunk-3 changes don't need re-touching in Phase 4.

### 3.2 — App shell

Files: `src/app/glr_ctrl.c`, `glr_actions.c`, `glr_debug.c`.

Same mechanical rename. Snapshot builders (`glr_ctrl_build_ui_snapshot`,
`build_guide_snapshot`) fill `.edit_line_idx =
repl_state_edit_line()` — change to `editor_state_edit_line()`.

Snapshot field name stays the same (`edit_line_idx` on
`UiRenderSnapshot` and `SceneGuideSnapshot`); only the *source*
of the value changes.

### 3.3 — Widgets

Files: `src/widgets/replay.c`.

`replay_walk_*` functions already take edit_line as a snapshot
field, not by direct accessor call. The snapshot population sites
(in app shell, covered by 3.2) are where the change happens. This
chunk is small or empty depending on whether `replay.c` itself
calls `repl_state_edit_line` (audit during chunk).

### 3.4 — Tests

Files: ~10 test files using `repl_state_edit_line` / `_set` for
setup. Mechanical rename to the editor accessor. Tests should
pass unchanged once the forwarders are in place.

### 3.5 — Demo

Files: `tools/editor_demo/{editor_demo,input,repl_shim}.c`,
`tools/editor_demo/input.h`.

- `editor_demo.c:107`: `int edit_line = input.edit_line_idx;` →
  `int edit_line = editor_state_edit_line();`
- `tools/editor_demo/input.c`: uses `repl_state_edit_line()`
  through a forward declaration. Switch to the editor accessor.
- `tools/editor_demo/repl_shim.c`: still provides
  `repl_state_edit_line()` and `demo_edit_line_set()`. The
  shim's `repl_state_edit_line()` returns demo-local storage;
  the editor accessor's forwarder calls it. So the demo's
  edit_line still works through the shim during transition.
  Phase 5 deletes the shim entirely.

### 3.6 — REPL pipeline (β: parameter passing)

Files: `src/repl/{compile,flatten,parser,scenes,core,example_loader,export,command_store}.c`,
plus the `src/repl/{load,state_owners,state_views}.h` headers.

This chunk is the architecturally interesting one. Sub-steps:

**3.6.1 — `compile.c`.** `repl_compile_context_from_live()` populates
`.edit_line` from `repl_state_edit_line()`. Replace with a function
that takes `int edit_line_idx` as a parameter — or, more conservatively,
add a new variant `repl_compile_context_from_input(edit_line_idx)`
and migrate callers (app shell / editor) to use it. Internal compile
helpers already take `ReplCompileContext`, so no further plumbing
beyond the context-builder change.

**3.6.2 — `flatten.c`.** `repl_state_edit_line()` is called in
multiple places: cursor highlight tracking, block range
detection, autonormal staleness. The cleanest path: add an
explicit `int edit_line_idx` parameter to `repl_flatten_program()`
and its internal helpers. The controller calls
`repl_flatten_program(editor_state_edit_line())`. Inside
`flatten.c`, no `repl_state_edit_line` calls remain.

**3.6.3 — `parser.c`.** Audit; if there's a direct call (likely
in `ReplParseContext.source_line_idx` defaulting), switch to
the value already on `ReplParseContext` — the caller provides it.

**3.6.4 — `scenes.c`, `core.c`, `example_loader.c`, `export.c`.**
These are load / import / lifecycle paths. They call
`repl_state_edit_line_set()` (write) to position the cursor
after a load or scene switch. Per β, the *write* should happen
editor-side too: these functions return "the cursor should land
at line N" and the caller (controller / commit code) calls
`editor_state_edit_line_set(N)`. Concretely: add an `int
*out_new_edit_line` parameter to the load functions, or rely on
the existing return shape if there's one. Specific sites:

- `src/repl/core.c:772, 777`: `repl_state_edit_line_set(repl_state_document_count())`
  — likely in an example/scene load. Return the value instead.
- `src/repl/example_loader.c:462, 464`: same pattern; comment
  acknowledges `repl_state_edit_line_set` "stays" REPL-state.
  Update the comment and surface the new edit_line through the
  load API.
- `src/repl/export.c`: import path sets edit_line on
  load-from-file. Same refactor.
- `src/repl/scenes.c`: user-scene save/restore saves/restores
  edit_line. Save → read via parameter; restore → return value
  for caller to apply.

**3.6.5 — `command_store.c`**. Phase 1 already removed the
`*edit_line` pointer. This sub-step verifies no remaining
`repl_state_edit_line*` calls in command_store.c.

**3.6.6 — Header cleanup.** `src/repl/load.h`,
`src/repl/state_views.h`, `src/repl/state_owners.h` no longer
need to declare `repl_state_edit_line*`. Phase 4 will delete
the declarations along with the definitions.

By end of Phase 3: zero `repl_state_edit_line*` calls outside
`src/repl/state.c` itself. `make check-repl-no-direct-editor`
still passes (REPL never calls editor).

## Phase 4 — Atomic flip + EditorInputView.edit_line_idx removal

Single commit. This is the moment the source of truth moves.

### 4.1 — Move storage

- Add a write to `EditorState.document.edit_line_idx` wherever
  the previous storage was written.
- Delete `edit_line_idx` from `ReplDocumentState` (or whatever
  REPL struct holds it).
- Delete `g_edit_line` macro in `src/repl/state.c`.

### 4.2 — Rewire the editor accessors (no more forwarders)

```c
/* src/editor/state.c */
int editor_state_edit_line(void) {
    return g_editor_state.document.edit_line_idx;
}
void editor_state_edit_line_set(int line) {
    g_editor_state.document.edit_line_idx = clamp_nonneg(line);
}
void editor_state_edit_line_clamp(void) {
    /* clamp to [0, document count] using EditorState only */
}
```

### 4.3 — Delete `repl_state_edit_line*` entirely

- Delete from `src/repl/state.c`.
- Delete declarations from `src/repl/state_owners.h` and
  `src/repl/state_views.h`.
- Delete the relevant `repl_state_edit_line*` mentions from
  `src/repl/state_views.h`'s `ReplDocumentView`.

### 4.4 — Remove `EditorInputView.edit_line_idx` (Path 4-Remove)

Decision recorded earlier: the field is conceptually
document-cursor state, not input-row state. With
`EditorDocumentState` and `editor_state_edit_line()` in place,
the field on the input view is redundant. Remove:

- `EditorInputView.edit_line_idx` (field).
- `EditorInputState.edit_line_idx` (the dead storage —
  confirmed never written; only the *view* field was populated
  by the builder).
- `state.h:264` comment about populating the field — delete.
- `state.h:45` comment claiming the field is for "view symmetry" —
  delete; the symmetry was specious because the storage was dead.

The only call site reading the view's field today is
`tools/editor_demo/editor_demo.c:107`; Phase 3.5 already
migrated it to `editor_state_edit_line()`.

### 4.5 — Update `repl_command_store_live()`

The store no longer takes a cursor pointer (Phase 1). If
anything in its construction referenced the now-deleted
`document.edit_line_idx` for sizing or other internal reasons,
clean that up.

### 4.6 — Verification

Build clean. Full test suite. `check-state-ownership` clean.
`check-repl-no-direct-editor` still green (REPL has no editor
includes; pipeline files take edit_line as parameter).

Confirm: `grep -rn 'repl_state_edit_line' .` returns zero hits
in `src/` and `tools/` and `tests/`. The function is gone.

## Phase 5 — Demo cleanup + shim deletion

### 5.1 — Delete `tools/editor_demo/repl_shim.c`

The shim's only function was `repl_state_edit_line` (which no
longer exists) and `demo_edit_line_set` (now redundant — the
demo's input dispatcher calls `editor_state_edit_line_set`
directly).

Delete the file.

### 5.2 — Update `Makefile`

- Remove `tools/editor_demo/repl_shim.c` from `EDITOR_DEMO_OBJS`.
- Update the comment block above `EDITOR_DEMO_DEP_SRCS` (Phase 6
  also touches this).

### 5.3 — Verification

`make editor_demo USE_GL_STUBS=1` clean. `make editor_demo` (real
GL) clean. `./editor_demo` smoke runs.

If `tools/editor_demo/repl_shim.c` is retained as a zero-stub
ledger comment file (matching `tools/repl_demo/stubs.c`), update
its content to reflect "no symbols needed; left as a record."
Otherwise just delete.

## Phase 6 — Guards and documentation

### 6.1 — Ratchet `check-editor-repl-surface`

`scripts/baselines/editor-repl-surface.txt`: drop the
`repl_state_edit_line*` counts. They should be zero in
`src/editor/input.c` and `commit.c` (and everywhere else).
Update the baseline.

### 6.2 — Update file-layout docs

`CLAUDE.md`:
- Remove the `repl_state_edit_line*` references from any module
  description.
- Update the `src/editor/state.c` row to note edit-line
  ownership (`EditorDocumentState`).
- Add a `src/editor/state.h` mention of the new
  `EditorDocumentState` slice if appropriate.

`MODULES.md`:
- Update the editor_demo entry: zero-shim link set; the
  `repl_shim.c` ledger note is gone.
- Update the editor section's "EditorState owns ..." sentence
  to include edit-line.
- Naming-conventions / cross-cutting section: note that
  edit-line is editor-owned.

### 6.3 — Plan disposition

- `plans/in-review/edit-line-ownership.md` → `plans/done/`.
- The note in `plans/done/editor-demo.md` "What's still open"
  about edit-line cleanup gets a one-line update pointing at
  this plan's done location.

### 6.4 — Stale path references (cleanup carried in this PR's first commit)

Pre-existing references to `plans/active/editor-demo.md` in the
codebase need updating to `plans/done/editor-demo.md`. Locations
flagged in review:

- `Makefile` (around line 545).
- `MODULES.md` (around line 190).
- `tools/editor_demo/editor_demo.c` (lines 4 and 35).
- `tools/editor_demo/repl_shim.c` (line 18 — this file may
  itself be deleted by Phase 5; the reference only matters if
  the zero-stub-ledger variant is kept).

These are P3; folded into the migration PR so the docs stay
coherent.

## Phase 7 — Full verification

- `make sample` (real GL) and `make sample USE_GL_STUBS=1`
  clean.
- `make editor_demo` (real GL) and `make editor_demo USE_GL_STUBS=1`
  clean.
- `make repl_demo` / `make scene_demo` clean.
- `make test-stubs`: full regression (6292 tests at baseline).
- `make check-state-ownership` clean, including
  `check-edit-ops-pure`, `check-editor-repl-surface` (lowered
  baseline), `check-repl-no-direct-editor` (still no carve-out).
- `grep -rn 'repl_state_edit_line' --include="*.c" --include="*.h" .`:
  zero hits.
- `ls tools/editor_demo/repl_shim.c`: file gone (or contains the
  zero-stub-ledger comment).
- Manual smoke (real GL): editor_demo opens, multi-line editing
  works (Enter splits, arrows nav, click positions cursor, File
  menu opens). All of Phase 8b's behavior preserved.

## Risk / open questions to pin before starting

1. **`ReplCommandStore` API change blast radius.** Phase 1 adds
   an out-parameter to the mutating ops. All callers update —
   editor side is straightforward; tests need to migrate too.
   Estimate ~30 call sites. The cursor-delta apply helper
   centralizes the math so callers don't reimplement.

2. **`example_loader.c` / `core.c` / `export.c` API shape.**
   Phase 3.6.4 changes these from "calls
   `repl_state_edit_line_set` internally" to "returns a target
   line for the caller to set." Decide between (a) a new
   `int *out_new_edit_line` parameter, (b) returning a struct
   that already exists (e.g., `ReplLoadResult`) with an
   `edit_line_after_load` field, or (c) just returning the value
   from functions that currently return void or status int.
   Pick (b) if there's a natural return struct, else (c) with a
   small status struct.

3. **`UiRenderSnapshot` and `SceneGuideSnapshot` field names.**
   These already use `edit_line_idx`. The name stays; only the
   source changes (from `repl_state_edit_line` to
   `editor_state_edit_line`).

4. **Test fixture ordering.** Many tests use
   `repl_state_init_defaults` + `repl_state_edit_line_set(n)` as
   setup. After Phase 5, those calls become `editor_state_reset()`
   + `editor_state_edit_line_set(n)`. Verify test ordering /
   dependencies stay correct — some tests may rely on REPL state
   being initialized before editor state.

5. **`source_document.c` interaction.** `tools/repl_demo/source_document.c`
   provides a standalone implementation. It may also reference
   edit-line; audit during Phase 3.6 and migrate if so.

## Landing strategy

- Phase 1, 2, 3.x, 4, 5, 6 as separate commits on one PR. Each
  commit leaves the tree buildable + tests green.
- Sequence: 1 → 2 → 3.1 → 3.2 → 3.3 → 3.4 → 3.5 → 3.6 → 4 → 5 → 6 → 7.
- The atomic flip (Phase 4) is the critical commit — easy to
  revert if anything breaks. The Phase 3 chunks ahead of it are
  all behavior-preserving forwarder migrations; reverting one is
  cheap.
- One PR for the whole migration. Partial landings would leave
  the codebase in a forwarder state where editor accessors
  forward to REPL accessors that still exist — that's
  internally consistent (it's the Phase 2-3 transition state)
  but not a useful long-term shape.
