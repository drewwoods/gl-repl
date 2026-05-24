/*
 * src/editor/edit_ops.c - Generic text-editing primitives.
 *
 * See edit_ops.h for the contract. Implementation operates only on
 * the EditorState input slice (input buffer, cursor, selection
 * anchor). REPL-free by construction; locked by
 * scripts/check-edit-ops-pure.sh.
 */

#include "edit_ops.h"

#include "limits.h"   /* MAX_INPUT_LEN */
#include "state.h"

#include <string.h>

/* ---- Buffer-only primitives ------------------------------------- */

int edit_op_buffer_insert_char_at_cursor(char c) {
    EditorInputState *inp = editor_state_input_mut();
    int cur = editor_cursor_pos();

    /* Capacity check matches the live editor's existing rule
     * (input_len < MAX_INPUT_LEN - 2). One slot for the new char,
     * one for the NUL the memmove copies forward. */
    if (inp->input_len >= MAX_INPUT_LEN - 2)
        return 0;

    memmove(&inp->input[cur + 1], &inp->input[cur],
            (size_t)(inp->input_len - cur + 1));
    inp->input[cur] = c;
    inp->input_len++;
    /* Buffer primitive: keep the anchor in place per the contract
     * in edit_ops.h. The default editor_cursor_pos_set clears the
     * anchor; that's the right policy for handlers that want a
     * fresh cursor, but it contradicts the "anchor (if any) is
     * left untouched" promise these buffer-only primitives make.
     * Selection-aware callers go through edit_op_type_char /
     * edit_op_backspace, which consume the selection up front via
     * edit_op_consume_input_selection (whose end-of-function
     * editor_cursor_pos_set clears the anchor as expected,
     * matching the Phase B default cursor-move policy). */
    editor_cursor_pos_set_keep_anchor(cur + 1);
    return 1;
}

int edit_op_buffer_delete_left_of_cursor(void) {
    EditorInputState *inp = editor_state_input_mut();
    int cur = editor_cursor_pos();

    if (cur <= 0 || inp->input_len <= 0)
        return 0;

    memmove(&inp->input[cur - 1], &inp->input[cur],
            (size_t)(inp->input_len - cur + 1));
    inp->input_len--;
    /* Buffer primitive: see comment in
     * edit_op_buffer_insert_char_at_cursor about why this uses the
     * anchor-preserving cursor setter. */
    editor_cursor_pos_set_keep_anchor(cur - 1);
    return 1;
}

int edit_op_buffer_delete_right_of_cursor(void) {
    EditorInputState *inp = editor_state_input_mut();
    int cur = editor_cursor_pos();

    if (cur >= inp->input_len)
        return 0;

    /* Shift input[cur+1..len] (including the NUL) left by one,
     * overwriting input[cur]. Cursor stays put. */
    memmove(&inp->input[cur], &inp->input[cur + 1],
            (size_t)(inp->input_len - cur));
    inp->input_len--;
    return 1;
}

int edit_op_consume_input_selection(void) {
    if (!editor_input_selection_active())
        return 0;

    int lo = editor_input_selection_lo();
    int hi = editor_input_selection_hi();
    EditorInputState *inp = editor_state_input_mut();
    int len = inp->input_len;

    if (lo < 0 || hi <= lo || hi > len) {
        /* Defensive: derived view inconsistent with buffer length.
         * Drop the anchor and bail rather than scribbling on input[]. */
        editor_input_anchor_clear();
        return 0;
    }

    int tail = len - hi;
    memmove(&inp->input[lo], &inp->input[hi], (size_t)tail);
    inp->input_len = len - (hi - lo);
    inp->input[inp->input_len] = '\0';
    /* Default cursor-set clears the anchor as a side effect. */
    editor_cursor_pos_set(lo);
    return 1;
}

/* ---- Selection-aware higher-level operations -------------------- */

int edit_op_type_char(char c) {
    (void)edit_op_consume_input_selection();
    return edit_op_buffer_insert_char_at_cursor(c);
}

int edit_op_backspace(void) {
    if (edit_op_consume_input_selection())
        return 1;
    return edit_op_buffer_delete_left_of_cursor();
}
