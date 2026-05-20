# Edit-line ownership migration (editor-demo Option A follow-up)

## Summary

Move the active edit-line cursor from `ReplState` to `EditorState`.
This closes out the last stub in `tools/editor_demo/repl_shim.c`
(`repl_state_edit_line`) and aligns ownership with the layering
claim: the editor (text-document model + controller) owns the cursor
over the document; the REPL pipeline takes that cursor as *input* to
its parse/flatten/compile work.

Picked from the two options the prior `editor-demo.md` plan named:

- **Option A (this plan):** full ownership migration. Multi-day
  refactor touching ~176 call sites across 33 files. Architecturally
  clean — edit-line lives where it conceptually belongs, the demo's
  shim drops to zero symbols.
- **Option B (not chosen):** delete `edit_line_idx` from
  `EditorInputView` so `state.c`'s view builder stops calling
  `repl_state_edit_line`. ~5 call sites; demo's shim drops to zero;
  edit-line stays in `ReplState`. Faster, but doesn't fix the
  underlying layering. Documented for context; not pursued.

Goal of this plan: lay out Option A as a series of committable
phases that each leave the tree green, with the architectural
question pinned down up front so the migration doesn't stall
mid-way.

## Context

`tools/editor_demo/repl_shim.c` currently contains one symbol:

```c
static int g_demo_edit_line = 0;
int repl_state_edit_line(void) { return g_demo_edit_line; }
void demo_edit_line_set(int n) { ... }
```

The shim exists because `src/editor/state.c`'s `EditorInputView`
builder reads `repl_state_edit_line()` to populate the view's
`edit_line_idx` field. Every editor controller, the app shell, the
REPL pipeline, the replay widget, ~10 tests, and the demo all read
`repl_state_edit_line` (or its `_set` / `_clamp` counterparts)
directly — 176 call sites across 33 files as of branch
`editor-demo-multiline`.

Audit note (this plan, before starting): `EditorInputView.edit_line_idx`
exists today as a "view symmetry" field but has exactly **one
user** — `tools/editor_demo/editor_demo.c:107`. Every REPL editor
caller queries `repl_state_edit_line()` directly. The migration
either makes this field the canonical read API (everyone migrates
to it where they have the view) or removes it. See Phase 4.

## The architectural question (decide before Phase 3)

After the migration, code that currently reads `repl_state_edit_line()`
must either:

- **Option α — Accept "REPL takes editor cursor as input."**
  REPL pipeline files (`src/repl/compile.c`, `flatten.c`, `parser.c`,
  `scenes.c`) read from `EditorState` directly via the new editor
  accessor. This creates a backward dependency (REPL → editor) that
  the existing `check-repl-no-direct-editor` and
  `check-repl-no-direct-buffer-read` guards explicitly prevent.
  We'd need a narrowly-scoped exception ("read-only edit_line
  query is allowed; nothing else") and to document it.

- **Option β — Push the reads up the call stack.** REPL pipeline
  functions that need edit-line take it as a parameter from the
  caller. The controller / commit code (which knows both editor and
  REPL state) becomes the bridge. Function signatures change;
  callers thread the value through.

**Recommendation:** **Option β.** Reasons:
- Keeps the existing layering invariant intact. The
  `check-repl-no-direct-editor` guard stays useful and we don't
  introduce a "one-symbol exception" that will rot into more
  exceptions later.
- Edit-line is a natural piece of input data for REPL pipeline
  functions; passing it explicitly makes the dependency visible
  at the call site instead of hidden behind a getter.
- The REPL pipeline call sites are concentrated (5 files) and the
  callers above them (controller, commit, glr_ctrl) already have
  the value available.

Trade-off: Option β changes more function signatures than Option α
(parameter additions in ~5 REPL pipeline files plus their callers).
That's the cost of staying clean.

Pin this decision in commit message of Phase 1 so subsequent
phases don't re-litigate it.

## Phase 1 — Editor-side storage + accessors

Add edit-line ownership to `EditorState`. No call sites move yet
beyond `state.c`'s view builder; this phase establishes the new
home so subsequent phases can migrate readers one group at a time.

### 1.1 — Pick the storage slice

Two reasonable spots:

- **`EditorInputState`** (existing): already has `edit_line_idx`
  as a *view* field, but storage today lives on `ReplState`.
  Move the storage in here. Pro: locality with the input slice
  the editor already owns. Con: muddles "input row state"
  (text/cursor/anchor) with "document cursor" (which line of the
  document is active).

- **New `EditorDocumentState`** slice on `EditorState`: dedicated
  document-level state. The cursor *over the document* (active
  edit_line, possibly future: selection range across lines)
  lives here. Pro: clean separation of input-row vs document
  cursor. Con: one more slice to plumb through capture/restore/reset.

**Recommendation:** `EditorDocumentState`. The semantic distinction
matters once we have multi-line nav and selection, both of which
are document-level concerns. The `EditorInputState.edit_line_idx`
in the input *view* stays as a copy populated from the
document slice (existing "view symmetry" pattern).

### 1.2 — Add accessors

```c
/* src/editor/state.h */

typedef struct {
    int edit_line_idx;
    /* Future: cross-line selection anchor / end could land here. */
} EditorDocumentState;

typedef struct {
    int edit_line_idx;
} EditorDocumentView;

/* On EditorState */
EditorDocumentState document;

/* Slice API */
EditorDocumentView   editor_state_document(void);
EditorDocumentState *editor_state_document_mut(void);
void                 editor_state_document_reset(void);

/* Convenience getters/setters (preferred over reaching into the
 * slice; same pattern as editor_input_text() / _set_text()). */
int  editor_state_edit_line(void);
void editor_state_edit_line_set(int line);
void editor_state_edit_line_clamp(void);  /* clamp to [0, doc count] */
```

### 1.3 — Migrate `state.c`'s view builder

`src/editor/state.c`'s `editor_state_input()` builder currently:

```c
return (EditorInputView){
    ...
    .edit_line_idx = repl_state_edit_line(),
};
```

After this phase:

```c
.edit_line_idx = editor_state_edit_line(),
```

`editor_state_edit_line()` reads `g_editor_state.document.edit_line_idx`.

### 1.4 — Initial value + sync

For now, keep `repl_state_edit_line` / `_set` / `_clamp` on
`ReplState` *and* their editor-side counterparts in sync via a
forwarder: have the editor accessors call the REPL ones (or vice
versa). This lets Phase 3 migrate readers without behavioral
change. The forwarder direction is decided by Phase 2.

### 1.5 — Verify

`make sample USE_GL_STUBS=1` clean, `make test-stubs` green, full
`make check-state-ownership` clean.

Commit message records the architectural decision (α vs β).

## Phase 2 — Decide and execute the `repl_state_edit_line*` fate

Two sub-options:

- **Phase 2-Keep:** keep `repl_state_edit_line` / `_set` / `_clamp`
  as thin forwarders to the editor accessors. Lets Phase 3 migrate
  callers incrementally with the rest of the codebase still
  green. Final step (Phase 5) deletes the forwarders.

- **Phase 2-Inline:** rewrite the three functions to read/write
  the editor's storage directly (so `ReplState.document.edit_line_idx`
  goes away in this phase, replaced by `EditorState.document`).

**Recommendation:** **Phase 2-Keep.** Smaller per-commit diffs;
each subsequent reader migration is independently revertible.
Phase 5 cleanup deletes the forwarders.

## Phase 3 — Migrate readers, by ownership group

Split into committable chunks. Each chunk migrates one ownership
group from `repl_state_edit_line()` to `editor_state_edit_line()`
(or to the explicit-parameter form if Phase α/β was Option β for
REPL files).

The grep baseline (branch `editor-demo-multiline` head) gives the
chunk sizes:

| Chunk | Files | ~Sites |
|-------|-------|--------|
| 3.1 — Editor controllers | input.c, commit.c, clipboard.c, undo.c, reformat.c, search.c, inline_file_prompt.c, state.c | ~80 |
| 3.2 — App shell | glr_ctrl.c, glr_actions.c, glr_debug.c | ~25 |
| 3.3 — REPL pipeline | compile.c, flatten.c, parser.c, scenes.c | ~30 (Option β: parameter additions) |
| 3.4 — Widgets | replay.c (snapshot field) | ~5 |
| 3.5 — Tests | ~10 test files | ~30 |
| 3.6 — Demo | editor_demo.c, repl_shim.c | ~3 |

Each chunk: build clean, tests green, check-state-ownership clean.

### 3.1 — Editor controllers

Easiest group; same module already owns EditorState. Pure rename:
`repl_state_edit_line()` → `editor_state_edit_line()` and
`repl_state_edit_line_set(n)` → `editor_state_edit_line_set(n)`.
No semantic change while Phase 2-Keep forwarders are in place.

Also during this chunk: identify call sites in `input.c` /
`commit.c` etc. that have an `EditorInputView` in scope and can
read `view.edit_line_idx` instead of calling the accessor. Either
is fine; pick consistency.

### 3.2 — App shell

Same mechanical rename. Watch for snapshot builders
(`glr_ctrl_build_ui_snapshot`, `build_guide_snapshot`) that
currently fill `.edit_line_idx = repl_state_edit_line()` —
migrate to the editor accessor.

### 3.3 — REPL pipeline (this is the architecturally interesting one)

If Option α (accept backward dep): mechanical rename, same as
above; add `check-repl-no-direct-editor` exception for
`editor_state_edit_line` only.

If Option β (push reads up): add an `int edit_line_idx`
parameter to the REPL pipeline functions that need it. Callers
(controller / commit / glr_ctrl) pass the value through. Function
signatures change; this chunk is the biggest in churn.

### 3.4 — Widgets

`src/widgets/replay.c` already takes `edit_line_idx` as a snapshot
field rather than calling `repl_state_edit_line` directly. The
snapshot population site moves from REPL accessor to editor
accessor (or to a parameter, per Option α/β).

### 3.5 — Tests

~10 test files set up state via `repl_state_edit_line_set(n)`.
Migrate to `editor_state_edit_line_set(n)`. Tests should pass
unchanged once accessors are in place (Phase 1 / 2 keeps
behavior identical).

### 3.6 — Demo

`tools/editor_demo/editor_demo.c:107` currently reads
`input.edit_line_idx`. After Phase 4 decision, this stays or
migrates to `editor_state_edit_line()`. `tools/editor_demo/input.c`
uses `repl_state_edit_line()` via forward declaration — migrate
to `editor_state_edit_line()`.

`tools/editor_demo/repl_shim.c` stops being needed: the storage
moves to EditorState, the setter goes away, the shim file is
deleted (or kept as a zero-stub ledger; see Phase 5).

## Phase 4 — `EditorInputView.edit_line_idx` audit

Decision point named by the user: the field is currently used by
exactly one caller (the demo). Two paths:

- **Path 4-Use:** make the field the publicly-blessed read API.
  Migrate any REPL editor caller that has an `EditorInputView` in
  scope from `editor_state_edit_line()` to `view.edit_line_idx`.
  The view stays a single-struct snapshot of "everything about
  the active input row," including which document line it
  represents. (This is the "view symmetry" argument the existing
  comment in `editor/state.h:45` makes.)

- **Path 4-Remove:** delete `edit_line_idx` from `EditorInputView`.
  The field is conceptually document-level, not input-row-level;
  putting it on the input view muddles the slice's purpose.
  Callers use `editor_state_edit_line()` or `editor_state_document().edit_line_idx`.
  Update the demo's editor_demo.c:107 to call the accessor.

**Recommendation:** **Path 4-Use.** The view-symmetry argument
holds: a caller iterating editor state will reasonably want
"which row is active" alongside "what's in the input." Removing
the field forces callers to make two queries for related data.
The mismatch the user flagged ("field exists but isn't used")
gets resolved by *making* it used, not by removing it.

Concretely: during Phase 3.1/3.2/3.3, prefer `view.edit_line_idx`
over `editor_state_edit_line()` at call sites that already have
the view. The field gains real users.

## Phase 5 — Drop the shim and forwarders

After Phase 3 migrations, no editor / app / REPL / widget / test
code calls `repl_state_edit_line*` directly. At this point:

1. Delete `repl_state_edit_line` / `_set` / `_clamp` from
   `src/repl/state.c` and `src/repl/state_owners.h` /
   `src/repl/state_views.h`.
2. Delete the `g_edit_line` macro and the `edit_line_idx` field
   from `ReplDocumentState` (or whatever struct holds it in
   `ReplState`).
3. Update `repl_command_store.c:10` which references
   `&document->edit_line_idx` (it gets the new editor-side
   pointer instead, or the command store's bookkeeping moves).
4. Delete `tools/editor_demo/repl_shim.c` outright (or replace
   with a zero-stub ledger comment file, the way
   `tools/repl_demo/stubs.c` is — same purpose: an empty file
   that documents "we are honest about what we don't link").
5. Remove the file from `Makefile`'s `EDITOR_DEMO_OBJS`.

## Phase 6 — Update guards + docs

1. `scripts/baselines/editor-repl-surface.txt` ratchets down.
   `repl_state_edit_line*` references in `src/editor/input.c`
   and `commit.c` should be zero; lower the baseline counts.
2. `CLAUDE.md` file-layout table:
   - Remove the `repl_state_edit_line` row from the REPL state
     section if present.
   - Update the `src/editor/state.c` row to note edit-line
     ownership.
   - Update the `tools/editor_demo` rows if `repl_shim.c` is
     deleted.
3. `MODULES.md`:
   - Update the editor_demo entry to describe the zero-shim link
     set.
   - Note in the naming-conventions / cross-cutting section that
     edit-line is editor-owned.
4. `plans/done/editor-demo.md` already documents Options A/B in
   the "What's still open" section; add a one-line pointer to
   this plan and to its eventual done/ location.

## Phase 7 — Full verification

- `make sample` (real GL) and `make sample USE_GL_STUBS=1` clean.
- `make editor_demo` (real GL) and `make editor_demo USE_GL_STUBS=1`
  clean. Smoke test: window opens, typing works, Enter splits
  lines, arrow keys navigate, clicks place cursor (all of
  Phase 8b behavior preserved).
- `make repl_demo` / `make scene_demo` clean (sanity — these
  shouldn't be affected but the migration touches REPL state).
- `make test-stubs`: full regression (6292 tests at baseline).
- `make check-state-ownership` clean. Includes
  `check-edit-ops-pure`, `check-editor-repl-surface` (with the
  new lower baseline), and any new exception added if Option α
  was picked in the architectural decision.
- Confirm the shim file is gone: `ls tools/editor_demo/repl_shim.c`
  should fail.
- Confirm zero `repl_state_edit_line*` references remain
  (`grep -rn 'repl_state_edit_line' --include="*.c" --include="*.h" .`).

## Risk / open questions to revisit before starting

1. **Architectural decision (Option α vs β)** — pinned at start
   of Phase 1. Don't drift.
2. **EditorDocumentState scope creep** — keep it to just
   `edit_line_idx` in this migration. Future selection state
   etc. is out of scope.
3. **ReplCommandStore edit_line pointer** — the command store
   takes a `*edit_line` pointer (see `src/repl/command_store.c:10`)
   so its insert/delete operations can adjust the cursor. After
   migration, that pointer comes from `EditorState.document`.
   Verify command_store still works without semantic change.
4. **Snapshot lifecycle** — `UiRenderSnapshot.edit_line_idx`,
   `SceneGuideSnapshot.edit_line_idx`, and `ReplayWalkContext.edit_line_idx`
   are populated by snapshot builders. The builders move from
   `repl_state_edit_line()` to `editor_state_edit_line()` (Option α)
   or get the value as a parameter (Option β). The snapshot
   consumers don't need to change.
5. **Test fixtures** — many tests use
   `repl_state_init_defaults` + `repl_state_edit_line_set(n)` as
   setup. After Phase 5, those calls become
   `editor_state_reset()` + `editor_state_edit_line_set(n)`.
   Verify test ordering / dependencies stay correct.

## Landing strategy

- Phases 1, 2, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 5, 6 each as
  separate commits. Phase 7 verification runs after each.
- Sequence: 1 → 2 → 3 (in order 3.1, 3.2, 3.4, 3.5, 3.6,
  3.3) → 5 → 6 → 7. (REPL pipeline last in the 3.x chunk
  so any architectural surprise surfaces only after the
  editor + app + widget + tests + demo are already
  migrated and green.)
- One PR for the whole migration, since the phases share the
  invariant "edit-line storage moved to editor"; merging
  partial would leave the codebase in a forwarder state that
  isn't useful long-term.
