# Scrollable Autocomplete Popup with Extended Match Capacity

Status: **in-review**

## Context

The autocomplete popup in `gl-repl` currently truncates match lists at 10 items. When completing arguments or function names with more than 10 candidates, items after the 10th alphabetical match are discarded and unreachable unless the user types further distinguishing characters.

### Specific Examples
- **`glPushAttrib` bit masks:** There are 12 candidate matches in total (the 11 bit groups in [`k_attrib_bits[]`](../../src/repl/command_spec.c) plus the `GL_ALL_ATTRIB_BITS` alias). Table collection order is `COLOR_BUFFER_BIT` ... `POLYGON_BIT`, `STENCIL_BUFFER_BIT` (#10), `TRANSFORM_BIT` (#11), followed by `GL_ALL_ATTRIB_BITS` appended after the table scan. Because collection terminates at 10 items before sorting, both `GL_TRANSFORM_BIT` and `GL_ALL_ATTRIB_BITS` are dropped when typing a broad prefix like `glPushAttrib(` or `glPushAttrib(GL_`.
- **`glEnable` / `glDisable` capabilities:** There are 25 capabilities in [`k_enable_caps[]`](../../src/repl/command_spec.c). Typing `glEnable(GL_` only reveals the first 10 (`GL_BLEND` through `GL_LIGHTING`); the remaining 15 items (`GL_LINE_SMOOTH`, `GL_MULTISAMPLE`, `GL_NORMALIZE`, `GL_POINT_SMOOTH`, `GL_POLYGON_OFFSET_*`, `GL_STENCIL_TEST`, etc.) are truncated.
- **Function names:** Typing `gl` matches ~60 built-in functions (plus up to 10 user aliases), but only the first 10 appear in the dropdown.

### Root Causes
1. **Hardcoded match ceiling:** [`MAX_AC_MATCHES`](../../src/editor/limits.h) is defined as `10`.
2. **Early loop termination:** In [`src/app/glr_completion.c`](../../src/app/glr_completion.c), collection loops stop adding candidates once `ac->match_count < MAX_AC_MATCHES` is reached.
3. **No windowing/scrolling in renderer:** [`src/ui/app/autocomplete_panel.c`](../../src/ui/app/autocomplete_panel.c) computes `popup_h = ac.match_count * LINE_H + 6` and renders all `ac.match_count` items directly without viewport clipping or scroll offset.
4. **Wrap-around selection cycling:** [`src/editor/input.c`](../../src/editor/input.c) cycles `selected_idx` mod `match_count` without updating a scroll window.
5. **Wheel passthrough:** Mouse wheel events over the autocomplete popup currently pass through to scroll the underlying code panel.

---

## Design

Because the full vocabulary of commands and enums in `gl-repl` is small (~70 built-in functions, 10 user aliases, max enum table of 25 entries), all matches for any prefix easily fit in memory and can be filtered and sorted in under a microsecond without asynchronous streaming or pagination.

We solve this with:
1. **Expanded static capacity:** Raise `MAX_AC_MATCHES` to `128` (providing ample headroom for all built-ins, aliases, and future commands).
2. **Windowed viewport rendering:** Keep the visible row ceiling at `MAX_AC_VISIBLE = 10` so lists that fit today (such as `glBegin`'s 10 modes) do not sprout unnecessary scrollbars.
3. **Selection-following scroll:** Maintain `scroll_top` in `EditorAutocompleteState` via a keep-in-view helper (`ac_reveal_selected`) during Up/Down arrow navigation and mouse wheel scrolling.
4. **Popup mouse wheel handling:** Consume wheel events over the autocomplete popup rect to step `selected_idx` and `scroll_top`, preventing unwanted code panel scrolling behind the popup.
5. **Visual scrollbar indicator:** Render a thin scrollbar track and thumb on the right edge of the popup when `match_count > MAX_AC_VISIBLE`.

---

## Proposed Changes

### 1. Capacity & Limits

#### [`src/editor/limits.h`](../../src/editor/limits.h)
- Change `MAX_AC_MATCHES` from `10` to `128`.
- Define `MAX_AC_VISIBLE 10`.

```c
#define MAX_INPUT_LEN   1024
#define MAX_AC_MATCHES  128
#define MAX_AC_VISIBLE  10
```

*Memory Impact:*
- `EditorAutocompleteState` contains two pointer arrays (`matches[MAX_AC_MATCHES]` and `insert_matches[MAX_AC_MATCHES]`), plus `int scroll_top`. The delta from 10 to 128 is `2 * (128 - 10) * 8 + 4 = 1892` bytes on 64-bit platforms.
- `glr_completion.c` has parallel arrays `g_ac_func_matches[MAX_AC_MATCHES]` (pointer array: `118 * 8 = 944` bytes) and `g_ac_alias_slots[MAX_AC_MATCHES]` (int array: `118 * 4 = 472` bytes).
- Total runtime static memory increase is ~3.3 KB.

---

### 2. Autocomplete Model & State Management

#### [`src/editor/state.h`](../../src/editor/state.h)
Add `scroll_top` to `EditorAutocompleteState`:
```c
typedef struct {
    const char *matches[MAX_AC_MATCHES];
    const char *insert_matches[MAX_AC_MATCHES];
    int         match_count;
    int         selected_idx;
    int         scroll_top;
    char        ghost[MAX_LINE_LEN];
    char        hint[MAX_LINE_LEN];
} EditorAutocompleteState;
```

#### State Reset & Ownership Invariant
- `editor_state_autocomplete_clear()` in `src/editor/state.c` reassigns `EditorAutocompleteState` from BSS-zero defaults, resetting `scroll_top = 0` automatically.
- `reset_ac_statics()` in `src/app/glr_completion.c` remains provider-private (`g_ac_mode`, `g_ac_func_matches[]`, etc.) and does **not** touch editor-owned `scroll_top`.
- Navigation routes call `editor_completion_update_selected_preview()` rather than a full autocomplete rebuild, preserving `scroll_top` across keystrokes.

---

### 3. Keep-In-View Selection Navigation

#### [`src/editor/input.c`](../../src/editor/input.c)
Extract a single `ac_reveal_selected()` helper to maintain the scroll window whenever `selected_idx` changes:

```c
static void ac_reveal_selected(EditorAutocompleteState *ac) {
    if (ac->match_count <= MAX_AC_VISIBLE) {
        ac->scroll_top = 0;
        return;
    }
    int max_scroll = ac->match_count - MAX_AC_VISIBLE;
    if (ac->selected_idx < ac->scroll_top) {
        ac->scroll_top = ac->selected_idx;
    } else if (ac->selected_idx >= ac->scroll_top + MAX_AC_VISIBLE) {
        ac->scroll_top = ac->selected_idx - MAX_AC_VISIBLE + 1;
    }
    if (ac->scroll_top > max_scroll) ac->scroll_top = max_scroll;
    if (ac->scroll_top < 0) ac->scroll_top = 0;
}
```

In `handle_vertical_special_key_route()`:
```c
case GLUT_KEY_UP:
    if (ac->match_count > 1) {
        ac->selected_idx = (ac->selected_idx - 1 + ac->match_count) % ac->match_count;
        ac_reveal_selected(ac);
        editor_completion_update_selected_preview();
    }
    ...
case GLUT_KEY_DOWN:
    if (ac->match_count > 1) {
        ac->selected_idx = (ac->selected_idx + 1) % ac->match_count;
        ac_reveal_selected(ac);
        editor_completion_update_selected_preview();
    }
    ...
```
*Note:* Wraparound behavior is naturally handled: wrapping from top (0) to bottom (`match_count - 1`) triggers the `selected_idx >= scroll_top + MAX_AC_VISIBLE` branch setting `scroll_top = match_count - MAX_AC_VISIBLE`; wrapping from bottom to top (0) triggers `selected_idx < scroll_top` resetting `scroll_top = 0`.

Add public entry point for mouse wheel scrolling:
```c
void editor_input_autocomplete_scroll_by(int delta);
```

---

### 4. Popup Geometry, Mouse Wheel, & Windowed Rendering

#### [`src/ui/app/autocomplete_panel.h`](../../src/ui/app/autocomplete_panel.h) & [`src/ui/app/autocomplete_panel.c`](../../src/ui/app/autocomplete_panel.c)
1. **Geometry helper:** Export a pure geometry calculation so rendering and hit-testing share identical bounds:
   ```c
   void ui_autocomplete_panel_rect(const EditorAutocompleteState *ac,
                                   int cursor_px, int cursor_py,
                                   int *out_x, int *out_y, int *out_w, int *out_h);
   int  ui_autocomplete_panel_hit_test(const EditorAutocompleteState *ac,
                                       int cursor_px, int cursor_py,
                                       int x, int y);
   ```
2. **Width calculation:** Measure `max_w` over all `ac.match_count` strings (not just visible ones) so the popup width stays constant during scrolling. When `ac.match_count > MAX_AC_VISIBLE`, reserve an extra 8px of padding for the scrollbar.
3. **Defensive clamping in renderer:** Clamp `scroll_top` locally to `[0, max(0, ac.match_count - visible_count)]`.
4. **Row rendering:** Render `visible_count = min(ac.match_count, MAX_AC_VISIBLE)` items starting at `ac.scroll_top`.
5. **Scrollbar formula:** When `ac.match_count > MAX_AC_VISIBLE`, draw a 3px vertical track and thumb:
   - Track: `(popup_x + popup_w - 5, popup_y - popup_h + 3)`, height `track_h = popup_h - 6`, color `UI_TOK_BORDER` / `UI_TOK_DIVIDER`.
   - Thumb: height `thumb_h = max(8, (track_h * visible_count) / ac.match_count)`.
   - Thumb offset: `thumb_y = (popup_y - 3 - thumb_h) - (int)((float)(track_h - thumb_h) * ((float)scroll_top / (float)(ac.match_count - visible_count)))`.

#### [`src/app/glr_ctrl.c`](../../src/app/glr_ctrl.c) & [`src/app/glr_ctrl_router.c`](../../src/app/glr_ctrl_router.c)
- In `route_wheel()`, check if the autocomplete popup is active (`ac->match_count > 0`) and `(x, y)` hits the popup rect via `glr_ctrl_autocomplete_popup_hit_test(x, y)`.
- If hit: consume the wheel event, step `selected_idx` by delta clamped to `[0, match_count - 1]`, call `ac_reveal_selected()`, refresh preview, and request redraw. (Consumes the event even if `match_count <= MAX_AC_VISIBLE` so the code panel behind it is never scrolled).

---

### 5. Diagnostics & Capacity Matrix

#### [`tools/capacity_matrix.c`](../../tools/capacity_matrix.c)
Update the `MAX_AC_MATCHES` capacity matrix entry:
- Capacity value: `MAX_AC_MATCHES` (128).
- Multiplier / description: Document `EditorAutocompleteState.matches[]` + `insert_matches[]` plus parallel provider arrays `g_ac_func_matches[]` and `g_ac_alias_slots[]`.

---

### 6. Test Suite Additions

#### [`tests/test_repl_autocomplete.c`](../../tests/test_repl_autocomplete.c)
Add test cases:
1. **`glPushAttrib` full match set:** Assert `glPushAttrib(GL_` returns exactly 12 matches (assert all 12 entries, verifying `GL_ALL_ATTRIB_BITS` and `GL_TRANSFORM_BIT`).
2. **`glEnable` capacity:** Assert `glEnable(GL_` returns exactly 25 matches.
3. **Function prefix capacity:** Assert `"gl"` matches >= 60 built-in functions.
4. **Navigation & scroll window:** Drive `editor_handle_special(GLUT_KEY_DOWN)` from index 0 through 11 on `glPushAttrib`:
   - Verify `selected_idx` advances.
   - Verify `scroll_top` remains 0 for indices 0..9, and advances to 2 at index 11.
   - Verify wraparound Down from 11 -> 0 resets `scroll_top = 0`.
   - Verify wraparound Up from 0 -> 11 sets `scroll_top = 2`.
5. **Completion accept:** Accept `GL_TRANSFORM_BIT` at index 11 and verify input buffer becomes `glPushAttrib(GL_TRANSFORM_BIT)`.
6. **Snapshot symmetry:** Verify `scroll_top` survives `editor_state_session_capture` / `editor_state_session_restore`.

#### [`tests/test_ui.c`](../../tests/test_ui.c)
Add test cases in `test_autocomplete_panel()`:
1. **Windowed text rendering:** For 12 matches with `scroll_top = 0`, assert exactly 10 raster-pos text rows (plus 1 hint) are rendered, not 12.
2. **Offset text rendering:** With `scroll_top = 2`, verify the first raster-drawn text matches `matches[2]`.
3. **Scrollbar presence:** Assert scrollbar quads are emitted when `match_count = 12` (>10), and absent when `match_count = 2` (<=10).

---

## Verification Plan

### Automated Tests
```bash
# Run autocomplete model & navigation tests
make run-test-repl-autocomplete

# Run UI popup rendering & windowing tests
make run-test-ui

# Run state ownership verification
make check-state-ownership

# Run full stubbed test suite
make test
```

### Manual Verification
1. Launch `build/release/gl-repl`.
2. Type `glPushAttrib(`.
3. Verify the popup displays 10 rows with a thin scrollbar on the right.
4. Use Down arrow key to scroll past row 10 to reveal `GL_TRANSFORM_BIT`.
5. Press Tab to accept and verify `glPushAttrib(GL_TRANSFORM_BIT)` is completed.
6. Type `glEnable(GL_` and use mouse wheel over the popup; verify scrolling updates selection and scrollbar without scrolling the editor text behind it.
7. Type `glBegin(` and verify all 10 primitive modes display cleanly without a scrollbar.
