# Editor Input Selection — Anchor Model

A character-range selection model for the editor's active input buffer
(`ReplEditorInputState.input[]`). Builds the standard "anchor + cursor"
pair so that shift+arrow, drag-to-select, double-click word, and select-
all-and-replace behaviors all share one mechanism instead of growing as
three parallel slot families.

## Goal

Make this work like every other editor:

| Action | Result |
|---|---|
| **Double-click on a word** | Selects the word (anchor at word start, cursor at word end). |
| **Shift + Arrow / Home / End** | Extends/contracts the selection by moving the cursor while leaving the anchor in place. |
| **Click and drag** | Sets anchor at press, extends selection as cursor follows the drag. |
| **Cmd/Ctrl + A** (currently jumps to line start) | Reinterpret as "select entire input line"; the existing line-start jump stays available via Home. |
| **Type a printable char with selection active** | Replace the selected range with the typed char. |
| **Backspace / Delete with selection active** | Delete the selected range; cursor lands at the range start. |
| **Tab / `;` / Enter with selection active** | Selection is cleared; the key performs its normal commit/accept behavior on the resulting buffer. |
| **Any plain cursor move (no shift)** | Clears the selection. |
| **Click without drag** | Clears the selection and places the cursor. |
| **Escape** | Clears the selection (in addition to its existing behaviors). |

Mouse selection lives entirely on the **active input row** — the row the
edit cursor is currently on. Selecting across multiple source lines is
out of scope; the existing line-range selection (clipboard scope) still
handles that.

## Current State

`ReplEditorInputState` (in `src/editor/state.h`) carries:

```c
char input[MAX_INPUT_LEN];   /* the editable buffer */
int  input_len;
int  cursor_pos;             /* live cursor — char offset into input[] */
...
```

There's no character-range selection. The only "selection" concept is
`ReplSelectionState { anchor_idx; end_idx; }` which is **line-range**
(used by clipboard cut/copy/paste, see `src/editor/clipboard.c`). That
state stays as-is; it is *different* from what this feature adds.

Cursor mutations are scattered. `grep -n editor_cursor_pos_set` shows
~37 call sites across `src/editor/{input,undo,commit,clipboard}.c`,
`glr_ctrl.c`, `glr_completion.c`. Typed-char insertion happens in one
place (around `src/editor/input.c:1186` — `memmove` + `inp->input[cur]
= (char)key`); backspace lives in `handle_text_delete_key_route`
(~line 1043). Every cursor move clears the existing line-range
selection via `editor_clipboard_clear_selection()` calls in
`editor_handle_key` (around line 845).

## Target Model

Add an **anchor** to the input buffer. The cursor stays where it is;
the anchor is the *other end* of the selection.

```c
typedef struct {
    char input[MAX_INPUT_LEN];
    int  input_capacity;
    int  input_len;
    int  cursor_pos;
    int  anchor_pos;     /* NEW: -1 = no selection, [0..input_len] = selection ends here */
    int  edit_line_idx;
    char pending_newline[MAX_INPUT_LEN];
    int  pending_newline_capacity;
    int  pending_newline_len;
    int  insert_mode;
} ReplEditorInputState;
```

**Invariants:**

- `anchor_pos == -1` means **no selection**; the cursor behaves normally.
- `anchor_pos >= 0` means the selection is `[lo, hi)` where
  `lo = min(anchor_pos, cursor_pos)` and `hi = max(anchor_pos, cursor_pos)`.
- An empty selection (`anchor_pos == cursor_pos`) is **not allowed** —
  the moment they collide, the anchor is cleared. This keeps "has
  selection" a single test (`anchor_pos >= 0`) rather than two.
- `anchor_pos` is always in `[0, input_len]`. Any mutation that shrinks
  the input below `anchor_pos` clears the anchor (cheap safety net).

The selection range is **derived**, never stored separately. There's
one source of truth.

## Phase Plan

### Phase A — State + accessors

`src/editor/state.h` / `state.c`:

1. Add `int anchor_pos;` to `ReplEditorInputState` and its view
   `ReplEditorInputView`.
2. Initialize to `-1` in `editor_state_input_reset()`.
3. Add accessors mirroring the cursor:
   - `int  editor_input_anchor(void);`
   - `void editor_input_anchor_set(int pos);`     /* clamps to [0, input_len], stores `-1` for "clear" */
   - `void editor_input_anchor_clear(void);`
   - `int  editor_input_selection_active(void);`  /* `anchor_pos >= 0` */
   - `int  editor_input_selection_lo(void);`       /* derived; returns `-1` if inactive */
   - `int  editor_input_selection_hi(void);`       /* derived; returns `-1` if inactive */
4. Add `editor_state_capture` / `editor_state_restore` symmetry: the
   anchor is part of the editor session and must round-trip through
   undo snapshots. The `ReplUndoSnapshot` already captures the input
   buffer — extending it with one int is local to `src/editor/undo.c`.

Hard-guard nothing yet — the field is unused after Phase A.

### Phase B — The "clear anchor on cursor move" rule

This is the source of every IDE bug in this area: forget to clear, and
the highlight goes stale; over-eager clear and shift+arrow doesn't
work. Get it right once, in one place.

1. Introduce a private helper inside `state.c`:
   ```c
   static void cursor_pos_set_internal(int pos, int keep_anchor);
   ```
   `editor_cursor_pos_set(int pos)` becomes a thin wrapper that calls
   `cursor_pos_set_internal(pos, /*keep_anchor=*/0)`. This is the
   **default cursor move**: anchor is cleared.
2. Add `void editor_cursor_pos_set_keep_anchor(int pos)` calling
   `cursor_pos_set_internal(pos, /*keep_anchor=*/1)`. This is the
   **shift-extended move**: anchor stays, selection grows/shrinks.
3. Audit all 37 cursor-set call sites:
   - Programmatic cursor moves that should clear the anchor (the
     vast majority — load_line_to_input, navigate_to_line, undo
     restore, autocomplete accept, etc.) keep calling the default
     `editor_cursor_pos_set`.
   - Shift+arrow / shift+home / shift+end handlers in
     `editor_handle_special` (and `editor_handle_key` for Cmd-shifted
     variants) call `_keep_anchor`. **Before** the move, they set the
     anchor if it's currently `-1` (anchor pinned to the *pre-move*
     cursor position), then move the cursor.
   - Plain arrow / Home / End keep the default behavior — they clear
     the anchor as a side effect of the standard `_set`.

Net effect: every existing cursor move automatically clears the anchor.
Only the new shift-variants keep it.

### Phase C — Selection-aware text mutations

Two places need to know about the anchor:

1. **Typed-char insert** (`src/editor/input.c:~1186`, the
   `memmove + inp->input[cur] = (char)key` path).
2. **Backspace / Delete** (`handle_text_delete_key_route`,
   `src/editor/input.c:~1043`).

Add a pre-step helper:

```c
/* If a selection is active, delete [lo, hi) from input[] and place
 * cursor at lo. Clears the anchor. Returns 1 if anything was deleted. */
static int input_consume_selection(void);
```

In the typed-char path: call `input_consume_selection()` first; the
subsequent insert proceeds as today on the now-shorter buffer.

In the backspace path: if selection active, just call
`input_consume_selection()` and return (the existing "delete one char
left" branch is skipped).

Delete (forward) is symmetric to backspace — same pre-step.

Tab / Enter / `;`: clear the anchor *without* deleting — the
commit-or-accept behavior runs on the input as it stands. This matches
common editor convention (typing `;` after selecting "foo" inside `bar
foo;` commits `bar foo;` cleanly, not `bar ;`).

### Phase D — Mouse and keyboard input handlers

`src/editor/input.c::editor_handle_mouse`:

1. **Single click on a code-panel char**: existing behavior (move
   cursor) — plus clear the anchor. Already handled by the default
   `editor_cursor_pos_set` from Phase B.
2. **Double click**: detect via a click-time + click-position record
   stored in a `static struct { unsigned int t_ms; int x, y; }` inside
   input.c. A second click within ~400ms and ~3px of the first counts
   as a double-click. On detection:
   - Navigate to the clicked line and load its text via
     `load_line_to_input` (current single-click behavior).
   - Walk left/right from the clicked char over the word-character
     class (`[A-Za-z0-9_]`).
   - `editor_input_anchor_set(word_start)`, `editor_cursor_pos_set_keep_anchor(word_end)`.
3. **Click + drag**: when a primary-button drag begins inside the code
   panel on the active edit row, set anchor at the press position.
   Each `editor_handle_motion` call updates the cursor with
   `_keep_anchor`. Drag ending inside the same row leaves the
   selection live; dragging outside the active row is a no-op for the
   selection (we don't extend across source lines in this feature).

`src/editor/input.c::editor_handle_special`:

1. **Shift + Arrow Left/Right**: anchor-pin if needed, then move
   cursor by one char with `_keep_anchor`.
2. **Shift + Arrow Up/Down**: out of scope — vertical motion would
   mean switching the active edit line, which the existing
   line-range selection already half-owns. Document as "ignored;
   plain Up/Down clears any input selection and changes the edit
   line."
3. **Shift + Home/End**: anchor-pin if needed, then move cursor to
   0 / `input_len`.

`src/editor/input.c::editor_handle_key`:

1. **Ctrl+A**: keep current behavior of "jump to start of input" unless
   user opts in to "select all." Two options:
   - (a) Leave Ctrl+A as line-start jump; let macOS Cmd+A be "select
     all input line" if/when Cmd-shortcuts grow.
   - (b) Reinterpret Ctrl+A as select-all (matches most editors).
   Recommend (b): set `anchor_pos = 0; cursor_pos = input_len`. The
   line-start jump is still reachable via Home.

   *Decision deferred — see Open Questions §1.*

2. **Escape**: extend the existing Escape handler to clear the anchor
   along with the other transient states it drops.

### Phase E — Render the selection band

`src/ui/panels.c::render_active_input_rows()` already paints the
active-input row glyph-by-glyph. Extend it to paint a colored
background band over `[lo, hi)` characters when
`editor_input_selection_active()`.

Implementation shape:

1. Add `selection_lo` and `selection_hi` to the per-frame input view
   the controller builds for the snapshot (`UiRenderSnapshot.editor_*`
   slice that today carries `input_len`, `cursor_pos`, etc.). Or
   compute it in panels.c via `editor_input_selection_*()` directly —
   the controller's snapshot pattern is the cleaner long-term shape,
   but a live read is fine for v1 since the UI rendering already
   reads `editor_state_input()` for the active row.
2. In the per-glyph render loop, if `i` is in `[lo, hi)`, draw the
   band background under that glyph in the same color used for
   line-range selection bands (search the file for the existing
   `editor_clipboard_sel_active` band-paint to keep the visual
   consistent).
3. The cursor's caret keeps blinking; it sits at `cursor_pos`, which
   is one end of the selection range, so visually the user sees the
   highlighted run with a caret at the moving end.

### Phase F — Tests

Add focused unit tests under `tests/`:

1. `tests/test_editor_input_selection.c` (new):
   - Anchor lifecycle: set, clear, auto-clear when cursor collides,
     auto-clear when input shrinks below anchor.
   - Selection derivation: lo/hi/active for various anchor/cursor
     orderings.
   - Selection-consuming mutations: typed char replaces, backspace
     deletes, delete deletes, mixed orderings.
   - Capture/restore symmetry (anchor survives undo/redo round-trip).
2. Extend `tests/test_repl_editor.c`:
   - Shift+arrow extends, then a plain arrow clears.
   - Typed printable replaces the selection.
   - Backspace replaces, then a follow-up typed char inserts at lo.
   - Tab/Enter/`;` clear without deleting.
   - Double-click selects the expected word in a representative
     input line (use the existing modifier-provider seam plus a
     small double-click-time-source seam — see Open Questions §2).
3. Touch `tests/test_repl_autocomplete.c` if Tab+selection interaction
   needs explicit coverage (selection clears, then ghost expansion
   runs).

### Phase G — Documentation

1. Update `CLAUDE.md` "Key Controls" with the new shift-modified
   navigation keys and the double-click behavior.
2. Update `MODULES.md` `EditorState` row to mention the new anchor
   field on `ReplEditorInputState`.
3. A one-paragraph note in `ARCHITECTURE.md` near the existing
   "Editing Existing Lines" section explaining that input-buffer
   selection is character-range and distinct from line-range
   clipboard selection.

## File Touch Inventory

| File | What changes | Approx LOC |
|---|---|---|
| `src/editor/state.h` | Add `anchor_pos` field; declare accessors. | +15 |
| `src/editor/state.c` | Define accessors; init in reset. | +40 |
| `src/editor/undo.c` | Snapshot/restore the anchor. | +5 |
| `src/editor/input.c` | Cursor-set wrappers; double-click detection; shift-extended moves; selection-aware text mutations; Esc clears anchor. | +150 |
| `src/editor/code_panel_document.c` | If/when the controller-snapshot route is taken (Phase E option), publish lo/hi. | +20 (optional) |
| `src/ui/panels.c` | Paint the selection band in `render_active_input_rows`. | +30 |
| `tests/test_editor_input_selection.c` | New focused suite. | +250 |
| `tests/test_repl_editor.c` | Add cases for shift+arrow / double-click / replace. | +120 |
| `Makefile` | Wire the new test binary. | +3 |
| `CLAUDE.md`, `MODULES.md`, `ARCHITECTURE.md` | Doc updates. | +30 |

**Total estimate: ~650 LOC across 11 files.** The single-feature
churn is concentrated in `input.c`, `state.c`, and the new test file;
everything else is small touch-ups.

## Invariants and Hard Guards

After this feature lands, the following must always hold:

1. **`anchor_pos == -1` OR `anchor_pos != cursor_pos`** — empty
   selections collapse immediately. Add an assertion inside
   `editor_input_anchor_set` and the cursor-move helpers.
2. **`anchor_pos` is always in `[0, input_len]`** — clamp on every
   write. Any mutation that shrinks the buffer past `anchor_pos`
   clears the anchor.
3. **The only path that mutates the cursor without clearing the
   anchor is `editor_cursor_pos_set_keep_anchor()`** — all other
   call sites use the default and inherit auto-clear.

Optional ratchet to consider: a Makefile check that no file outside
`src/editor/state.c` calls `editor_cursor_pos_set_keep_anchor` from
more than a documented allowlist (so the "anchor preservation" path
stays narrow and auditable). Probably overkill for v1 — leave as a
future hardening.

## Open Questions

1. **Ctrl+A semantics.** Currently Ctrl+A jumps to input-line start
   (Emacs-style). Standard editors interpret Ctrl+A as "select all."
   Options:
   - Reinterpret: `cursor=input_len, anchor=0`. Line-start jump moves
     to Home (or stays available as a no-op of "press Ctrl+A then
     press Right" with the bonus that Right collapses the selection
     to the right end — actually that goes to *end*, not start, so
     this breaks the existing muscle memory).
   - Conservative: leave Ctrl+A alone; bind macOS Cmd+A → select-all
     via the existing `editor_input_normalize_super_to_ctrl` path
     and add a Ctrl+letter spelling that's currently unused (e.g.
     no good option — Ctrl+A is the canonical select-all).
   - Punt: ship without select-all in the first cut.
   **Recommend: punt for v1.** Land the model without disturbing
   existing shortcuts; add Cmd/Ctrl+A in a follow-up after the
   anchor mechanics are stable.

2. **Double-click timing seam for tests.** GLUT delivers single
   mouse-down events; we synthesize double-click from a wall-clock
   delta. Tests need a way to drive that deterministically — either
   pass in the click time, expose a `editor_input_set_clock_for_test`
   seam (mirroring `editor_input_set_modifier_provider_for_test`),
   or factor the word-bounds walk into a pure helper that the test
   calls directly and skip the timing logic. The factored-helper
   route is cleanest; it avoids a new test seam.

3. **Snapshot the anchor on the UI side?** Phase E lists two options:
   live read from `editor_state_input()` in `panels.c` versus
   publishing `selection_lo` / `selection_hi` through `UiRenderSnapshot`.
   The snapshot version matches the existing direction
   (`check-ui-no-repl-state-read` baseline); the live version is
   smaller. The active-row render already reads live state for
   `cursor_pos`/`input_len` (see panels.c:256 `cp.cursor_visible`),
   so the precedent for live reads on this exact row exists. Worth
   the small extra plumbing? I'd say yes if `UiRenderSnapshot`
   already carries `cursor_pos`, no if it doesn't.

4. **Selection across the pending-newline overwrite buffer.** When
   `insert_mode == 0` (overwrite mode), input has the additional
   `pending_newline[]` field. Should selection span both? For v1:
   no — selection lives in the main `input[]` only. Document the
   limitation; revisit if users hit it.

5. **Search interaction.** Search-hit highlighting paints its own
   band on the active row. If both are active (user searched, then
   double-clicked a word that happens to be a different match
   instance), which paints on top? Likely the input selection wins
   (most recent user intent), but the rule should be explicit in
   the panel render code.

## Out of Scope

- **Vertical selection** (shift+up/down). The selection model in this
  feature is intentionally single-line, scoped to the active input
  buffer. The line-range clipboard selection covers multi-line use.
- **Block / column selection** (alt+drag). Not a need today.
- **Selection-aware Ctrl+C / Ctrl+X from the input buffer.** Today's
  Ctrl+C/Ctrl+X operate on the line-range clipboard selection (see
  `handle_line_delete_key_route`). Adding "copy/cut the input-buffer
  selection to a separate clipboard slot" is a reasonable follow-up,
  not part of v1. Punt: typing replaces the selection, which covers
  the most common "select word, type replacement" use case.
- **Browser-style cmd-shift-arrow word jumps.** Word-boundary
  navigation is its own feature; this plan only handles word
  *selection* via double-click. Word-jump-by-arrow can layer on later
  using the same shift+_keep_anchor primitive.

## Sequencing Recommendation

Land in this order so each phase is independently reviewable and
testable:

1. **A + B** in one commit: anchor field, accessors, cursor-set
   wrapper split, audit of the 37 call sites. No behavior change yet.
   Verify all existing tests pass.
2. **C** in one commit: selection-aware text mutations. Still no
   user-visible behavior because no UI sets the anchor; tests drive
   it manually.
3. **D-mouse** (single click and drag) in one commit. Now the user
   can drag-to-select inside the input row.
4. **D-keyboard** (shift+arrow / shift+home / shift+end / Esc) in one
   commit.
5. **D-double-click** in one commit.
6. **E** (render band) — could land alongside D-mouse so the drag has
   a visible result, but if it lands later the keyboard cases above
   are still testable via state assertions.
7. **F** focused tests can land alongside each behavior phase; the
   new `test_editor_input_selection.c` should appear with Phase A.

Each commit is small (~50–150 LOC) and individually buildable. The
risk is concentrated in Phase B's 37-site audit — every miss there
leaves a stale anchor. Lean on the test in Phase A that asserts
"after any default cursor_pos_set, anchor is `-1`."
