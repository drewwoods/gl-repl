# Scrollable Autocomplete Popup with Extended Match Capacity

Status: **done**

## Context

The autocomplete popup in `gl-repl` currently truncates candidate lists at 10 items. When completing arguments or function names with more than 10 candidates, items after the 10th collected candidate are discarded and unreachable unless the user types further distinguishing characters.

### Collection Mechanics & Truncation
Candidate collection in [`src/app/glr_completion.c`](../../src/app/glr_completion.c) iterates each candidate source table in its definition order, appends any alias candidates, and terminates as soon as `ac->match_count < MAX_AC_MATCHES` is exhausted. Only *then* does `sort_autocomplete_matches()` sort the collected slice alphabetically.

- **`glPushAttrib` bit masks:** There are 12 candidates in total (11 in [`k_attrib_bits[]`](../../src/repl/command_spec.c) plus `GL_ALL_ATTRIB_BITS`). Table order is `COLOR_BUFFER_BIT` ... `POLYGON_BIT`, `STENCIL_BUFFER_BIT` (#10), `TRANSFORM_BIT` (#11), followed by `GL_ALL_ATTRIB_BITS` appended after the loop. The first 10 table entries fill `ac->matches[]`, retaining `STENCIL_BUFFER_BIT` but dropping `TRANSFORM_BIT` and `GL_ALL_ATTRIB_BITS` when completing a broad prefix like `glPushAttrib(` or `glPushAttrib(GL_`. (When the prefix is `GL_ALL`, it matches only the alias and succeeds in isolation).
- **`glEnable` / `glDisable` capabilities:** There are 25 capabilities in [`k_enable_caps[]`](../../src/repl/command_spec.c). Typing `glEnable(GL_` only reveals the first 10 (`GL_BLEND` through `GL_LIGHTING`); the remaining 15 items (`GL_LINE_SMOOTH`, `GL_MULTISAMPLE`, `GL_NORMALIZE`, `GL_POINT_SMOOTH`, `GL_POLYGON_OFFSET_*`, `GL_STENCIL_TEST`, etc.) are dropped.
- **Function names:** Typing `gl` matches ~60 built-in functions in `k_func_completions[]` plus up to 10 user aliases. The first 10 table rows are gathered and sorted; the rest are dropped.

### Root Causes
1. **Hardcoded match ceiling:** [`MAX_AC_MATCHES`](../../src/editor/limits.h) is defined as `10`.
2. **Early loop termination:** Collection loops in [`src/app/glr_completion.c`](../../src/app/glr_completion.c) stop once `ac->match_count == MAX_AC_MATCHES`.
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
4. **Popup mouse wheel handling:** Cache last-rendered cursor anchor (`g_last_ac_cursor_*`) to hit-test wheel events over the popup rect, stepping `selected_idx` + `scroll_top` via `editor_input_autocomplete_scroll_by(delta)` without scrolling the background code panel.
5. **Visual scrollbar indicator:** Render a thin scrollbar track and thumb matching flyout conventions on the right edge of the popup when `match_count > MAX_AC_VISIBLE`.

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
- `EditorAutocompleteState`: 2 pointer arrays (`matches[128]`, `insert_matches[128]`) + `int scroll_top`. Delta from 10 to 128 is `2 * (128 - 10) * 8 + 4 = 1892` bytes on 64-bit platforms.
- `glr_completion.c`: parallel arrays `g_ac_func_matches[128]` (pointer array: `118 * 8 = 944` bytes) and `g_ac_alias_slots[128]` (int array: `118 * 4 = 472` bytes).
- Total runtime static memory increase is ~3.3 KB.

#### [`tools/capacity_matrix.c`](../../tools/capacity_matrix.c)
Update the `MAX_AC_MATCHES` entry:
- Multiplier: `3 * sizeof(char *) + sizeof(int)` (accounts for `matches[]`, `insert_matches[]`, `g_ac_func_matches[]`, and `g_ac_alias_slots[]`).
- Description: Document the combined state and provider-private candidate arrays.

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

### 3. Selection & Scroll Navigation

#### [`src/editor/input.h`](../../src/editor/input.h)
Export wheel scroll entry point:
```c
void editor_input_autocomplete_scroll_by(int delta);
```

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
*Note on keyboard wrap:* Wrapping from top (0) to bottom (`match_count - 1`) triggers `selected_idx >= scroll_top + MAX_AC_VISIBLE`, setting `scroll_top = match_count - MAX_AC_VISIBLE`; wrapping from bottom to top (0) triggers `selected_idx < scroll_top`, resetting `scroll_top = 0`.

Implement `editor_input_autocomplete_scroll_by()`:
```c
void editor_input_autocomplete_scroll_by(int delta) {
    EditorAutocompleteState *ac = editor_state_autocomplete_mut();
    if (ac->match_count <= 0) return;
    int new_idx = ac->selected_idx + delta;
    if (new_idx < 0) new_idx = 0;
    if (new_idx >= ac->match_count) new_idx = ac->match_count - 1;
    ac->selected_idx = new_idx;
    ac_reveal_selected(ac);
    editor_completion_update_selected_preview();
}
```
*Note on wheel clamping:* Unlike keyboard arrow keys, mouse wheel scrolling does **not** wrap around when hitting the ends.

---

### 4. Popup Geometry, Windowed Rendering, & Wheel Routing

#### [`src/ui/app/autocomplete_panel.h`](../../src/ui/app/autocomplete_panel.h) & [`src/ui/app/autocomplete_panel.c`](../../src/ui/app/autocomplete_panel.c)
1. **Geometry calculation:** Provide a single source of truth for bounds and hit-testing in OpenGL bottom-left coordinates:
   ```c
   void ui_autocomplete_panel_rect(const EditorAutocompleteState *ac,
                                   int cursor_px, int cursor_py,
                                   int *out_x, int *out_y, int *out_w, int *out_h);
   int  ui_autocomplete_panel_hit_test(const EditorAutocompleteState *ac,
                                       int cursor_px, int cursor_py,
                                       int gl_x, int gl_y);
   ```
   - Width: Compute `max_w` across all `ac->match_count` candidates (for constant popup width across scrolls), adding 16px padding plus 8px extra when `match_count > MAX_AC_VISIBLE`.
   - Height: `visible_count = min(ac->match_count, MAX_AC_VISIBLE)`, `popup_h = visible_count * LINE_H + 6`.
   - Bottom-left origin: `out_x = popup_x` (clamped against `ui_layout_code_panel_rect`), `out_y = popup_y - popup_h - FONT_H - 4` (covers entry rows and the bottom "Tab to accept" hint strip), `out_w = popup_w`, `out_h = popup_h + FONT_H + 4`.
2. **Defensive clamping in renderer:** `ui_autocomplete_panel_render()` calls `ui_autocomplete_panel_rect()` and defensively clamps `scroll_top` to `[0, max(0, ac.match_count - visible_count)]`.
3. **Scrollbar styling (matching flyouts in `menu_bar.c`):**
   - Track: 3px wide at `popup_x + popup_w - 2`, height `track_h = popup_h - 6`, color `UI_TOK_DIVIDER`.
   - Thumb: 3px wide, color `UI_TOK_TEXT_MUTED`, `thumb_h = max(10, (track_h * visible_count) / ac.match_count)`.
   - Position: `thumb_y = (popup_y - 3 - thumb_h) - (int)((float)(track_h - thumb_h) * ((float)scroll_top / (float)(ac.match_count - visible_count)))`.

#### [`src/app/glr_ctrl.h`](../../src/app/glr_ctrl.h), [`src/app/glr_ctrl.c`](../../src/app/glr_ctrl.c), & [`src/app/glr_ctrl_router.c`](../../src/app/glr_ctrl_router.c)
1. **Cursor anchor caching (Option A):**
   In `glr_ctrl.c`, during the render pass right after `ui_panels_render_code_panel(&ui_snap, &cp_out)`, cache:
   ```c
   g_last_ac_cursor_px    = cp_out.cursor_px;
   g_last_ac_cursor_py    = cp_out.cursor_py;
   g_last_ac_cursor_valid = cp_out.cursor_valid;
   ```
   Reset `g_last_ac_cursor_valid = 0` on session resets / panel closure.
2. **Hit-test wrapper:** Export `int glr_ctrl_autocomplete_popup_hit_test(int mouse_x, int mouse_y)`:
   - Returns 0 if `!g_last_ac_cursor_valid` or `editor_state_autocomplete()->match_count == 0`.
   - Converts GLUT top-left `mouse_y` to OpenGL bottom-left `gl_y = ui_state_viewport().window_h - mouse_y`.
   - Calls `ui_autocomplete_panel_hit_test(editor_state_autocomplete(), g_last_ac_cursor_px, g_last_ac_cursor_py, mouse_x, gl_y)`.
3. **Z-order wheel routing:** In `route_wheel()` in `glr_ctrl_router.c`:
   - Check after menu flyouts and GL-state popup, but *before* assign-plot, console, and code panel:
   ```c
   if (glr_ctrl_autocomplete_popup_hit_test(x, y)) {
       editor_input_autocomplete_scroll_by(delta);
       editor_request_redraw();
       return;
   }
   ```
   - Consumes the wheel unconditionally when over the popup (even if `match_count <= MAX_AC_VISIBLE`), preventing the code panel behind it from scrolling.

---

### 5. Test Suite Additions

#### [`tests/test_repl_autocomplete.c`](../../tests/test_repl_autocomplete.c)
Add test cases:
1. **`glPushAttrib` full match set:** Assert `glPushAttrib(GL_` returns exactly 12 matches, asserting all 12 entries including `GL_ALL_ATTRIB_BITS` and `GL_TRANSFORM_BIT`.
2. **`glEnable` capacity:** Assert `glEnable(GL_` returns exactly 25 matches.
3. **Function prefix capacity:** Assert `"gl"` matches >= 60 built-in functions.
4. **Keyboard navigation & scroll window:** Drive `editor_handle_special(GLUT_KEY_DOWN)` from index 0 through 11 on `glPushAttrib`:
   - Verify `selected_idx` advances.
   - Verify `scroll_top` remains 0 for indices 0..9, and advances to 2 at index 11.
   - Verify wraparound Down from 11 -> 0 resets `scroll_top = 0`.
   - Verify wraparound Up from 0 -> 11 sets `scroll_top = 2`.
5. **Direct wheel mutation:** Test `editor_input_autocomplete_scroll_by()`:
   - Clamping at 0 and `match_count - 1` without wrapping.
   - Scroll window updates and preview sync.
6. **Completion accept:** Accept `GL_TRANSFORM_BIT` at index 11 and verify input buffer becomes `glPushAttrib(GL_TRANSFORM_BIT)`.
7. **Snapshot symmetry:** Verify `scroll_top` survives `editor_state_session_capture` / `editor_state_session_restore`.

#### [`tests/test_ui.c`](../../tests/test_ui.c)
In `test_autocomplete_panel()`:
1. **Windowed text rendering:** For 12 matches with `scroll_top = 0`, assert exactly 10 raster-pos text rows (plus 1 hint) using `ASSERT_INT` on `gl_stub_counts[GL_STUB_glRasterPos2f]`.
2. **Offset text trace:** With `scroll_top = 2`, verify the first raster-drawn text string corresponds to `matches[2]`.
3. **Scrollbar quad emission:** Assert scrollbar quads are emitted when `match_count = 12` (>10), and absent when `match_count = 2` (<=10).

#### [`tests/test_glr_ctrl.c`](../../tests/test_glr_ctrl.c)
Add controller-level wheel routing tests:
1. Wheel inside popup changes `selected_idx` and does not change `editor_scroll()`.
2. Wheel outside popup scrolls the code panel (`editor_scroll()` changes).
3. Wheel over a non-scrollable popup (<=10 items) is consumed without scrolling the code panel.

---

## Verification Plan

### Automated Tests
```bash
# Autocomplete model & navigation tests
make run-test-repl-autocomplete

# UI popup rendering & windowing tests
make run-test-ui

# Controller routing & wheel hit-test tests
make run-test-glr-ctrl

# State ownership check
make check-state-ownership

# Full stubbed test suite
make test
```

### Manual Verification
1. Launch `./gl-repl`.
2. Type `glPushAttrib(GL_`.
3. Verify the popup displays 10 rows with a thin scrollbar on the right.
4. Use Down arrow key to scroll past row 10 to reveal `GL_TRANSFORM_BIT`.
5. Press Tab to accept and verify `glPushAttrib(GL_TRANSFORM_BIT)` is completed.
6. Type `glEnable(GL_` and use mouse wheel over the popup; verify scrolling updates selection and scrollbar without scrolling the editor text behind it.
7. Type `glBegin(GL_` and verify all 10 primitive modes display cleanly without a scrollbar.
