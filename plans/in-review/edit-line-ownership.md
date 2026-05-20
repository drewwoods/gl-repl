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
  refactor touching ~142 call sites in `src/` (~293 incl. tests).
  Concentrated in `test_repl_editor.c` (~70), `input.c` (~49),
  and `flatten.c` (~17). The
  architecturally clean answer — edit-line lives where it
  conceptually belongs, the demo's shim drops to zero symbols,
  the existing `check-repl-no-direct-editor` invariant stays
  intact.
- **Option B (not chosen):** delete `edit_line_idx` from
  `EditorInputView` so `state.c`'s view builder stops calling
  `repl_state_edit_line`. ~5 call sites; demo's shim drops to
  zero; edit-line stays in `ReplState`. Faster, but doesn't fix
  the underlying layering. Not pursued.

This plan has been revised three times after review. Key points
versus the first draft:

1. **Layering invariant β is non-negotiable.** REPL pipeline
   code never calls into `EditorState`; it receives edit-line
   as an explicit parameter from the caller. No exception, no
   guard carve-out.
2. **The existing β guard is a paper guard today.** Phase 0
   fixes `check-repl-no-direct-editor.sh` to actually scan
   `src/repl/` (it currently scans root-only, finds nothing,
   and exits OK). Without this, every later phase's
   verification can pass while β silently breaks.
3. **`ReplCommandStore` + `repl_apply_compiled_change` cursor
   coupling is Phase 1, not a Phase 5 cleanup detail.** The
   store holds an `int *edit_line` pointer; the apply layer
   above it packages `change->adjust_edit_line` into the store
   flag. Both layers have to flip in the same phase — flipping
   just the store leaves apply.c in a state where it can't
   honor its own intent flag without violating β. Both end up
   taking an optional `int *cursor_inout` parameter; the store
   does the standard insert / delete math on the caller-owned
   int, and apply just forwards the pointer through. No
   global, no helper, no struct.
4. **Single source of truth, every phase.** No bidirectional
   forwarders. The editor accessor reads from REPL during the
   transition (editor → REPL, a forward dep that's already
   allowed); after the atomic flip, the editor accessor reads
   from EditorState and the REPL function is deleted in the
   same commit.
5. **`EditorInputView.edit_line_idx` is removed, not blessed.**
   Edit-line is document-cursor state, not input-row state;
   `EditorDocumentState` is the right home. Putting it on the
   input view too muddles slices. The dead
   `EditorInputState.edit_line_idx` storage (never written) also
   goes.
6. **REPL-side inventory.** Files touching `repl_state_edit_line*`
   today: compile.c, flatten.c (3 distinct entry points — see
   3.6.2), parser.c (single fallback site — see 3.6.3), scenes.c,
   core.c, example_loader.c, export.c, **load.c**, **autonormal.c**
   (β-bound direct store-mutator caller — see 3.6.0), plus
   command_store.c (via the store pointer, fixed in Phase 1)
   and the load.h / state_views.h / state_owners.h headers.
   Additional direct store-mutator sites caught by review:
   `tools/repl_demo/repl_demo.c` (3 sites; demo-local cursor),
   `src/repl/state.c:279` (`repl_state_document_reset`'s zero-cursor
   load), and `src/widgets/tutorial.c` (brackets
   `repl_load_apply_line` with explicit edit-line writes —
   see 3.3). `load.c` needs explicit cursor-in / cursor-out
   plumbing because it relies on `change.adjust_edit_line = 1`
   for its append-at-end loop semantics (load.c:107). The
   `EditorServices.apply_repl_change` function-pointer signature
   also changes in lockstep with `repl_apply_compiled_change`
   (see 1.3.1).

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

REPL pipeline code (`src/repl/*.c` and `src/repl/*.h`) does not
call `editor_state_edit_line()` or any other `editor_*` accessor.

The mechanism: every REPL pipeline function that needs edit-line
takes it as a parameter (typically `int edit_line_idx`). Functions
that *mutate* the cursor take an `int *cursor_inout` parameter
(see Phase 1); the store does the per-op insert / delete math
on the caller-owned int. Callers above
the pipeline boundary (controller, commit, glr_ctrl, editor)
supply the read value from the editor accessor and apply any
returned delta.

This is what makes Phase 4's atomic flip simple: when the storage
moves, no REPL-side code has to change. Only callers of the
deleted `repl_state_edit_line()` need to update — and after
Phases 2 and 3, there are no callers of `repl_state_edit_line()`
outside `src/repl/state.c` itself, where it gets deleted.

### Caveat: the existing `check-repl-no-direct-editor` guard is a
### paper guard today

`scripts/check-repl-no-direct-editor.sh` scans `repl_*.c` /
`repl_*.h` *at the repo root*. The REPL code moved to `src/repl/`
during the source-document-port work; the root-level files no
longer exist, so the guard's `nullglob` finds zero files and exits
"repl-no-direct-editor OK (no repl_* sources found)" *regardless
of what's actually in `src/repl/`*. Verified by reading the
script (lines 23–28).

That means right now `make check-state-ownership` passes even if
REPL code calls editor accessors. The migration cannot rely on
this guard until it actually scans the right directory. Phase 0
below fixes the guard *before* Phase 1 starts, so every
subsequent phase's verification step has real teeth.

## Phase 0 — Fix `check-repl-no-direct-editor` to scan `src/repl/`

Before any code moves. This phase has its own commit and lands
on `main` ahead of the migration PR if at all possible (it's a
strictly-improving guard tightening with zero behavior change to
shipped code).

### 0.1 — Audit current REPL → editor coupling

Run a manual grep equivalent to what the fixed guard will scan:

```
grep -rnE '#include "(src/)?editor/|editor_buffer_|editor_state_|editor_cursor_|EditorBufferView|EditorBuffer' src/repl/
```

Expected: zero hits. If anything turns up, that's pre-existing
β coupling that the paper guard was silently allowing; resolve
it before this phase lands (separate commit / PR if needed).

### 0.2 — Update `scripts/check-repl-no-direct-editor.sh`

Replace the root-level glob:

```bash
shopt -s nullglob
files=( repl_*.c repl_*.h )
```

with the actual layout:

```bash
shopt -s globstar nullglob
files=( src/repl/**/*.c src/repl/**/*.h )
```

(Or `find src/repl -name '*.c' -o -name '*.h'` if bash globstar
isn't reliable in CI.) The "no files found" success path is
removed since the directory is known-populated; if it ever
empties, that's a real signal worth surfacing.

### 0.3 — Verify the guard exits OK on current `main`

After the script update, `make check-repl-no-direct-editor` must
still pass on current main. If it doesn't, Step 0.1 missed
something — fix the offending coupling before merging this phase.

### 0.4 — Wire `check-repl-no-direct-editor` into
### `check-state-ownership` (if not already)

Already wired, per the Makefile audit during the editor-demo
plan. Confirm during this phase.

This phase is small (~10 lines of bash) but load-bearing — every
later phase's verification implicitly depends on it.

## Phase 1 — `ReplCommandStore` + `repl_apply_compiled_change` cursor decoupling

### Why this phase has to cover two layers, not just the store

The cursor adjustment goes through two stacked APIs in
`src/repl/`:

1. **`ReplCommandStore`** (`src/repl/command_store.c`). Holds an
   `int *edit_line` pointer:

   ```c
   typedef struct {
       GLCmd *cmds;
       int   *count;
       int    capacity;
       int   *edit_line;        /* <-- the pointer */
   } ReplCommandStore;
   ```

   Mutating ops with `REPL_COMMAND_STORE_ADJUST_EDIT_LINE`
   adjust `*store->edit_line` automatically.

2. **`repl_apply_compiled_change`** (`src/repl/apply.c:73`). The
   transactional wrapper most callers actually use. It calls
   `repl_command_store_live()`, packages
   `change->adjust_edit_line` into the store flag, and forwards
   the mutating op:

   ```c
   int repl_apply_compiled_change(const ReplCompiledChange *change) {
       ReplCommandStore store = repl_command_store_live();
       int flags = change->adjust_edit_line
                       ? REPL_COMMAND_STORE_ADJUST_EDIT_LINE : 0;
       /* ... switch on change->kind, call store ops with flags ... */
   }
   ```

   After the store stops owning cursor writes (1.1 below), this
   function has no way to honor `change->adjust_edit_line`
   without either (a) calling editor (β violation — apply.c
   lives in `src/repl/`) or (b) silently ignoring the flag
   (behavior change for `load.c`, `commit.c`, `clipboard.c`,
   `undo.c` callers).

   The right answer: **apply also returns the cursor delta**,
   propagating it to its caller. Apply doesn't touch the
   cursor; it reports what the cursor *would* have changed by
   if the old auto-adjust were still in effect.

Both layers have to flip in the same phase or the codebase ends
up in a state where `adjust_edit_line` is set but never applied.

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
store no longer reads or writes any global cursor.

### 1.2 — Mutating ops take an `int *cursor_inout`

Every store mutating op gains an optional `int *cursor_inout`
parameter. When non-NULL *and* the caller passes
`ADJUST_EDIT_LINE`, the store applies the standard insert/delete
math to the caller-owned int in place. When NULL, no cursor
mutation — pure data op.

```c
int repl_command_store_insert_one(ReplCommandStore *store,
                                  int pos, const GLCmd *cmd,
                                  int flags,
                                  int *cursor_inout);
int repl_command_store_insert_many(ReplCommandStore *store,
                                   int pos, const GLCmd *cmds, int count,
                                   int flags,
                                   int *cursor_inout);
int repl_command_store_delete_range(ReplCommandStore *store,
                                    int start, int count,
                                    int *cursor_inout);
/* _replace_one and _load: see note on _load below. */
```

The math lives in the store (it knows what each op means);
ownership of the cursor itself lives with the caller (caller-owned
stack int, editor field, or wherever). No global, no helper, no
struct.

Per-op math the store implements:
- **INSERT at pos by `count`:** if `pos <= cursor`, `cursor += count`.
- **DELETE at pos by `count`:** three cases —
  (a) `cursor < pos`: no change (cursor is before the range);
  (b) `pos <= cursor < pos+count`: snap to `pos` (cursor was
  inside the deleted range);
  (c) `cursor >= pos+count`: `cursor -= count` (cursor was
  past the range). Clamp the result to `[0, new line_count]`.
- **REPLACE:** no cursor change. `cursor_inout` ignored even if
  non-NULL.

LOAD is the absolute-set case. Reviewer flagged the existing API:

```c
int repl_command_store_load(ReplCommandStore *store,
                            const GLCmd *cmds, int count,
                            int edit_line);
```

The `edit_line` parameter was only ever meaningful *because of*
the `*store->edit_line` pointer — the store wrote that value
through. After Phase 1, the store no longer holds a cursor.

**API decision pinned: drop the `edit_line` parameter from
`_load()` entirely.** Callers set their own cursor before/after
the call. Concretely:

```c
int repl_command_store_load(ReplCommandStore *store,
                            const GLCmd *cmds, int count);
```

This is cleaner than threading the value through `cursor_inout`
because the store has no concept of "the load's target cursor" —
that was always caller policy. Sites that previously passed a
specific value (e.g., `scenes.c:259` passing the scene's saved
edit_line, `state.c:279` passing 0 for reset) now do:

```c
ok = repl_command_store_load(&store, cmds, count);
if (ok) editor_state_edit_line_set(target);  /* caller policy */
```

In β-bound REPL files that can't call editor (autonormal, load,
scenes restore paths), the caller's "target" lives in an
`int *out_new_edit_line` parameter the function surfaces to its
own caller — see 3.6.4 and 3.6.5.

### 1.2.1 — LOAD_ALL gate semantics in `repl_apply_compiled_change`

`apply.c:103` invokes `repl_command_store_load` for the
`REPL_COMPILED_LOAD_ALL` kind. Today, that path forwards
`change->pos` (used as the absolute cursor target) into the
store, which writes through `*store->edit_line` *unconditionally*
— the `adjust_edit_line` flag is irrelevant for LOAD_ALL today
because the store always sets the cursor when loading.

Reviewer flagged: after Phase 1 the gating logic in 1.3 below
("apply passes NULL to the store when `change->adjust_edit_line`
is false") could silently drop the LOAD cursor-target.

**Resolution:** with `_load()` no longer taking an edit_line
parameter (above), there's no cursor in the load path for apply
to gate. The `change->pos` field for LOAD_ALL becomes a
caller-policy hint that the caller may apply with its own
`editor_state_edit_line_set(change.pos)` after a successful
apply. Phase 1.5 caller migration explicitly handles this — the
editor's apply wrapper for LOAD_ALL changes is two calls:
`apply` then `set`. Other kinds don't need the post-step.

### 1.3 — `repl_apply_compiled_change` gets the same shape

```c
/* Before: */
int  repl_apply_compiled_change(const ReplCompiledChange *change);

/* After: */
int  repl_apply_compiled_change(const ReplCompiledChange *change,
                                int *cursor_inout);
```

Apply forwards `cursor_inout` to the store mutating ops it
invokes, honoring `change->adjust_edit_line` as the gate
(INSERT_ONE / INSERT_MANY pass NULL when the flag is off).
The `change->adjust_edit_line` field stays — it's still the
*intent* flag. LOAD_ALL is handled per 1.2.1 above (caller
applies `change.pos` separately after a successful apply).

Apply itself never reads or writes editor state. The cursor
lives where the caller put it.

### 1.3.1 — `EditorServices.apply_repl_change` function-pointer

The seam at `src/editor/services.h:48` and the default binding
at `src/editor/services.c:41` must also gain the `int *cursor_inout`
parameter. The current shape:

```c
int (*apply_repl_change)(const ReplCompiledChange *change, void *user);
```

becomes:

```c
int (*apply_repl_change)(const ReplCompiledChange *change,
                         int *cursor_inout, void *user);
```

`editor_services_default()` updates accordingly. Editor commit
code already calls `repl_apply_compiled_change` directly in some
sites (commit.c:237), but the services seam is the test-double
boundary — both signatures must stay in sync.

### 1.4 — Migrate every direct store-mutator caller

Grep direct call sites of the store ops (not via apply). Full
inventory by file:

| File | Sites | Cursor source |
|------|-------|---------------|
| `src/editor/clipboard.c`, `commit.c`, `undo.c`, `input.c` | several | `editor_state_edit_line()` |
| `src/repl/autonormal.c` (lines 71, 254) | 2 | β-bound — caller-passed `int *edit_line_inout` (see 3.6 below) |
| `src/repl/state.c` (line 279, `repl_state_document_reset`) | 1 (via `_load(... 0)`) | reset always zeroes; passes `NULL` or local `int = 0` |
| `tools/repl_demo/repl_demo.c` (lines 172, 203, 267) | 3 | demo-local stack int; demo doesn't share cursor with anything |
| Tests | many | per-test; some `NULL`, some local int |

Editor-side and `repl_demo` sites become:

```c
int cur = editor_state_edit_line();   /* or demo-local */
int ok = repl_command_store_insert_one(store, pos, &cmd, flags,
                                        flags & ADJUST_EDIT_LINE ? &cur : NULL);
if (ok && (flags & ADJUST_EDIT_LINE))
    editor_state_edit_line_set(cur);   /* or write demo-local */
```

For callers that don't care about the cursor (some test fixtures
and `_load(... 0)` reset paths), pass `NULL`.

`src/repl/autonormal.c` is β-bound — see Phase 3.6.0 below for
its specific plumbing.

### 1.5 — Migrate every `repl_apply_compiled_change` caller

Grep:

```
grep -rn 'repl_apply_compiled_change\b' --include="*.c" --include="*.h" .
```

Expected sites:
- `src/editor/commit.c` — main editor commit path.
- `src/editor/clipboard.c` — paste path.
- `src/editor/undo.c` — undo restore path.
- `src/repl/load.c` — REPL-side line loader (β-bound;
  see Phase 3.6.5).
- Various tests.

Editor-side sites become:

```c
int cur = editor_state_edit_line();
int ok = repl_apply_compiled_change(&change,
                                     change.adjust_edit_line ? &cur : NULL);
if (ok && change.adjust_edit_line)
    editor_state_edit_line_set(cur);
```

`src/repl/load.c` (which can't call editor) threads the cursor
through its loop as a local int (`int running_edit_line`), passes
`&running_edit_line` to apply, and returns the final value to
its caller. See Phase 3.6.5.

### 1.6 — Verification

`make sample USE_GL_STUBS=1`, `make test-stubs`, full
`check-state-ownership` clean (including the now-real
`check-repl-no-direct-editor` from Phase 0). Behavior unchanged:
every call site still adjusts the same cursor by the same
amount, just through an explicit apply call instead of an
auto-pointer.

Commit message confirms: neither `ReplCommandStore` nor
`repl_apply_compiled_change` holds a cursor pointer; ownership
of cursor adjustment lives editor-side.

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
EditorDocumentState document;  /* populated in Phase 4; reads
                                * during Phases 2-3 still resolve
                                * to ReplState via the forwarders. */
```

The "currently unused" comment is important: the half-state
where the field exists but isn't read from is intentional, lasts
only for the duration of this PR (Phases 2-3), and is the
deliberate cost of an atomic Phase 4 flip. A reviewer who lands
on this struct between phases shouldn't think it's a mistake.

**Capture/restore invariant during transition.** Because
`editor_state_capture` / `_restore` (existing functions at
`src/editor/state.c:60, 66`) memcpy the whole `EditorState`,
they'll snapshot the unwritten `document.edit_line_idx = 0`
during Phases 2-3 and write it back on restore. The forwarders
mask this (reads route through REPL accessors that ignore the
editor field) but it would silently activate if anything reads
the field directly before Phase 4. Mitigation: keep the field
strictly forwarder-only until Phase 4a; do not introduce
direct field reads in Phase 3. Phase 4a flip is the moment
direct reads become safe.

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

Files: `src/widgets/replay.c`, `src/widgets/tutorial.c`.

**`replay.c`:** `replay_walk_*` functions already take edit_line
as a snapshot field, not by direct accessor call. The snapshot
population sites (in app shell, covered by 3.2) are where the
change happens. This is small or empty depending on whether
`replay.c` itself has direct calls (audit during chunk).

**`tutorial.c`:** brackets `repl_load_apply_line` with explicit
`repl_state_edit_line_set` calls — line 151 sets the cursor to
the instruction line before loading, line 179 sets it back
afterward. Phase 3.6.5 changes `repl_load_apply_line` to take
`int *edit_line_inout`; the tutorial migration is:

```c
/* Before: */
repl_state_edit_line_set(instruction_line);
repl_load_apply_line(comment, err, sizeof(err));
/* ... */
repl_state_edit_line_set(state->expected_commit_line);

/* After: */
int line = instruction_line;
repl_load_apply_line(comment, err, sizeof(err), &line);
/* tutorial doesn't need the post-load cursor value here; the
 * second set call replaces it anyway. */
editor_state_edit_line_set(state->expected_commit_line);
```

`tutorial.c` is editor-inherent (it's a widget the editor knows
about, not REPL pipeline), so it CAN call the editor accessor.
β invariant unaffected.

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
  through a forward declaration. Switch reads to
  `editor_state_edit_line()` and writes to
  `editor_state_edit_line_set()`. The local
  `demo_edit_line_set()` setter (declared by forward decl in
  input.c) is removed from the demo's source — there's no need
  for a demo-internal name once the editor accessor exists.
- `tools/editor_demo/repl_shim.c`: during Phases 2-3 the
  forwarder chain is:
    - Read: `editor_demo` calls `editor_state_edit_line()` →
      forwarder → `repl_state_edit_line()` (shim) →
      `g_demo_edit_line` (shim's static int).
    - Write: `editor_demo` calls `editor_state_edit_line_set(n)`
      → forwarder → `repl_state_edit_line_set(n)` → ...
  For reads and writes to land on the *same* storage, the shim
  must provide BOTH `repl_state_edit_line` and
  `repl_state_edit_line_set` (and `_clamp`). Add the setter and
  the clamp to the shim as part of this chunk; both update
  `g_demo_edit_line`. Without this, the demo's writes go
  nowhere (or to a different copy) while reads return stale
  values — the migration would silently break the demo's
  multi-line editing.

  Phase 5 deletes the shim entirely (storage moves to
  EditorState in Phase 4).

### 3.6 — REPL pipeline (β: parameter passing)

Files: `src/repl/{compile,flatten,parser,scenes,core,example_loader,export,command_store}.c`,
plus the `src/repl/{load,state_owners,state_views}.h` headers.

This chunk is the architecturally interesting one. Sub-steps:

**3.6.0 — `autonormal.c`.** β-bound REPL caller that today
free-rides on the store's auto-adjust flag (autonormal.c:71
inserts with `REPL_COMMAND_STORE_ADJUST_EDIT_LINE`; line 254
replaces, no adjust). After Phase 1 the store can't auto-adjust
on its own, and autonormal can't fetch the cursor itself
(β-bound, in `src/repl/`).

Fix:
```c
/* Before: */
void repl_recompute_autonormals(int autonormal_enabled);
/* After: */
void repl_recompute_autonormals(int autonormal_enabled,
                                int *edit_line_inout);
```

Caller plumbing: `src/app/glr_ctrl.c:1559` (the one frame-loop
site) already has `editor_state_edit_line()` available; pass
`&local_edit_line` and write it back via
`editor_state_edit_line_set(local_edit_line)` after the call.
Other callers (if any — audit) pass a stack-local int through.

**3.6.1 — `compile.c`.** `repl_compile_context_from_live()` populates
`.edit_line` from `repl_state_edit_line()`. Replace with a function
that takes `int edit_line_idx` as a parameter — or, more conservatively,
add a new variant `repl_compile_context_from_input(edit_line_idx)`
and migrate callers (app shell / editor) to use it. Internal compile
helpers already take `ReplCompileContext`, so no further plumbing
beyond the context-builder change.

**3.6.2 — `flatten.c` (largest sub-step).** 17 calls across
**three distinct public-API entry points** (declared in
`flatten.h`):

- `repl_flatten_refresh_current_block_highlight()` (~6 calls
  in `flatten.c:149-187`): block-range detection.
- `repl_flat_cmd_matches_cursor()` (~10 calls in `flatten.c:685-791`):
  cursor matching + func-scope resolution + attribute tracking.
- `repl_flatten_program()` itself: 1 call site (often via the
  two functions above as internal helpers).

All three gain an explicit `int edit_line_idx` parameter (they
do not delegate to a shared internal). Callers (controller at
`glr_ctrl.c:1212` and similar) supply the value. The single
largest REPL-side sub-step — straightforward but tedious. Run
the existing flatten-related tests after this chunk to confirm
no behavior drift.

Per-call usage that originally read `repl_state_edit_line()` is
replaced with the parameter directly; do not introduce a local
helper that hides the parameter as global state — that would
defeat β.

**3.6.3 — `parser.c`.** Single site at `parser.c:251`:

```c
int source_line_idx = ctx ? ctx->source_line_idx
                          : repl_state_edit_line();
```

The fallback exists for legacy no-ctx wrapper paths. After
β migration, neither the REPL accessor (deleted) nor the editor
accessor (forbidden in REPL files) is callable here.

**Resolution pinned: remove the fallback; require a non-NULL
context.** Audit `repl_parser_parse_command_ctx` callers; if any
still pass `ctx == NULL`, migrate them to construct a context
first. The legacy wrappers (`repl_parser_parse_command` /
`_with_vars`) were already retired in earlier phases per the
existing tree comment; the remaining `ctx ? :` ternary is a
defensive vestige.

Add a runtime assert (`assert(ctx)`) at the top of
`repl_parser_parse_command_ctx` to catch any stragglers loudly
during the migration. Remove the ternary on line 251.

**3.6.4 — `scenes.c`, `core.c`, `example_loader.c`, `export.c`.**
These are load / import / lifecycle paths. They call
`repl_state_edit_line_set()` (write) to position the cursor
after a load or scene switch. Per β, the *write* should happen
editor-side too: these functions return "the cursor should land
at line N" and the caller (controller / commit code) calls
`editor_state_edit_line_set(N)`.

**API shape pinned: return the new edit_line as an int.** For
functions currently returning `void`, change the return type to
`int`. For functions already returning a meaningful status
(typically `int success`), add an `int *out_new_edit_line`
parameter. Do NOT introduce a new `ReplLoadResult` struct for a
single int — it's heavier than the migration warrants and would
ripple into every caller for no extra information. Specific
sites:

- `src/repl/core.c:772, 777`: `repl_state_edit_line_set(repl_state_document_count())`
  — example/scene load via the
  `glr_ctrl.c:2233 → core.c:798 (load_initial_commands) →
  core.c:789 (repl_load_example) → core.c:772/777` chain.
  Each frame needs to forward the target cursor; the outermost
  controller call site applies via `editor_state_edit_line_set`.
- `src/repl/example_loader.c:462, 464`: same pattern; comment
  acknowledges `repl_state_edit_line_set` "stays" REPL-state.
  Update the comment and surface the new edit_line through the
  load API.
- `src/repl/export.c`: import path sets edit_line on
  load-from-file. Same refactor.
- `src/repl/scenes.c`: user-scene save/restore saves/restores
  edit_line. Save → read via parameter; restore → return value
  for caller to apply.

**3.6.5 — `load.c`.** The REPL-side line loader is structurally
similar to the editor commit path but lives in `src/repl/` and
relies heavily on auto-advance — line 107's explicit
`change.adjust_edit_line = 1` is exactly the
"loader's append-at-end semantics, edit_line must auto-advance
line-by-line so the next call sees insert_idx = document_count"
pattern Phase 1 broke.

With Phase 1's `int *cursor_inout` shape, the fix is
straightforward:

1. `repl_load_*` entry points add an `int *edit_line_inout`
   parameter (caller's cursor; the loader threads it through
   each per-line apply).
2. The loop's per-line body becomes:
   ```c
   repl_apply_compiled_change(&change,
                              change.adjust_edit_line ? edit_line_inout
                                                      : NULL);
   ```
   Each iteration advances `*edit_line_inout` by the store's
   standard math; no REPL → editor call, no struct, no helper.
3. Existing internal references to `ctx.edit_line`
   (load.c:144-145) migrate to dereferencing the running
   `*edit_line_inout` instead.

This is the most behaviorally-sensitive sub-step in Phase 3 — a
silent off-by-one in the cursor accumulation will make
multi-line loads land on the wrong row. Belt-and-suspenders:
add a focused unit test (`tests/test_repl_core_io.c` or
similar) that loads a 5-line file and asserts the post-load
edit_line equals the expected value, before refactoring.

**3.6.6 — `repl_state_capture` / `repl_state_restore`.** These
copy/restore `g_repl_state` wholesale, which today includes
`document.edit_line_idx`. After Phase 4 the field is gone from
`ReplState`. Decide before Phase 3.6.6 starts:

- Option (a) (preferred): `repl_state_capture` /
  `repl_state_restore` simply stop touching edit_line — their
  job becomes "snapshot REPL state only," edit_line is
  EditorState concern. The single caller that uses these for
  undo (in `src/editor/undo.c`) already snapshots editor state
  separately; it threads edit_line through the editor snapshot
  path.
- Option (b): the functions gain `int *edit_line_inout` (in for
  restore, out for capture). Simpler for callers but spreads
  edit-line awareness across two snapshot mechanisms.

(a) is cleaner: edit_line is editor-owned, so editor-side
capture/restore handles it. Audit `editor_state_capture` /
`editor_state_restore` (in `src/editor/state.c`) — they may
already snapshot the new `EditorDocumentState` once Phase 2
adds it.

**3.6.7 — `command_store.c` audit.** Phase 1 already removed
the `*edit_line` pointer. This sub-step verifies no remaining
`repl_state_edit_line*` calls in command_store.c.

**3.6.8 — Header inventory check.** `src/repl/load.h`,
`src/repl/state_views.h`, `src/repl/state_owners.h` are the
remaining declaration sites for `repl_state_edit_line*`. Verify
no other header re-declares them (grep). The deletion itself
happens in Phase 4a (4.3 below).

Also at Phase 4a: the forward declaration at
`src/editor/state.c:257` (`int repl_state_edit_line(void);`)
becomes unnecessary once the editor accessor no longer
forwards. Delete it then.

By end of Phase 3: zero `repl_state_edit_line*` calls outside
`src/repl/state.c` itself, and zero REPL → editor symbol
references (the fixed `check-repl-no-direct-editor` guard from
Phase 0 enforces this).

## Phase 4 — Atomic flip

**Two commits**, sequenced for bisect-friendliness:

- **Commit 4a (4.1 + 4.2 + 4.3 + 4.5):** the storage flip.
  Storage moves from `ReplState` to `EditorState`; editor
  accessors stop forwarding; `repl_state_edit_line*` is deleted;
  `repl_command_store_live()` no longer references the removed
  REPL field.
- **Commit 4b (4.4 alone):** remove `EditorInputView.edit_line_idx`
  (Path 4-Remove). The dead `EditorInputState.edit_line_idx`
  storage also goes.

Splitting 4b out means if anything regresses after 4a, the bisect
points at the storage move; if a regression shows up only after
4b, it's because something *did* depend on the view field after
Phase 3 supposedly migrated all readers, and that's a smaller
search space to investigate.

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

### 6.4 — Stale path references (already cleaned)

A previous review round flagged `plans/active/editor-demo.md`
references in `Makefile`, `MODULES.md`,
`tools/editor_demo/editor_demo.c`, and
`tools/editor_demo/repl_shim.c`. Those references were already
updated to `plans/done/editor-demo.md` in the
`plan-edit-line-migration` branch's first commit. Re-grep at
PR start to confirm; this sub-step is conditional on whether
they survived rebasing.

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
   an optional `int *cursor_inout` parameter to the mutating ops
   plus the same to `repl_apply_compiled_change`. All callers
   update — editor side is straightforward; tests need to
   migrate too. Estimate ~30 call sites. The per-op math lives
   in the store; callers just pass a stack-local int through.

2. **`example_loader.c` / `core.c` / `export.c` / `scenes.c`
   API shape.** Pinned: functions currently returning `void`
   change to return `int` (the new edit_line); functions already
   returning a status int gain an `int *out_new_edit_line`
   parameter. No new result struct. See Phase 3.6.4.

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

- Phase 0 lands on `main` ahead of the migration PR if at all
  possible (it's a strictly-improving guard fix with no behavior
  change to shipped code). If timing doesn't permit, it's the
  first commit on the migration PR.
- Phase 1, 2, 3.x, 4a, 4b, 5, 6 as separate commits on one PR
  after Phase 0. Each commit leaves the tree buildable + tests
  green.
- Sequence: 0 → 1 → 2 → 3.1 → 3.2 → 3.3 → 3.4 → 3.5 → 3.6.0 →
  3.6.1 → 3.6.2 → 3.6.3 → 3.6.4 → 3.6.5 → 3.6.6 → 3.6.7 → 3.6.8 →
  4a → 4b → 5 → 6 → 7. (3.6.0 is autonormal.c, added after
  the latest review.)
- Phase 6.4 (stale `plans/active/editor-demo.md` references)
  folds into Phase 0's commit, since both are independent of
  the migration itself.
- The atomic flip is split into 4a (storage move + accessor
  rewire) and 4b (Path 4-Remove on `EditorInputView.edit_line_idx`).
  Splitting 4b out gives a cleaner bisect target if a regression
  shows up post-storage-move vs. post-field-removal.
- One PR for the whole migration (besides the optional Phase 0
  precursor). Partial landings would leave the codebase in a
  forwarder state where editor accessors forward to REPL
  accessors that still exist — that's internally consistent
  (it's the Phase 2-3 transition state) but not a useful
  long-term shape.
