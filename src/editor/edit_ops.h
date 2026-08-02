/*
 * src/editor/edit_ops.h - Generic text-editing primitives.
 *
 * The shared text-editor primitive library called by both
 * src/editor/input.c (REPL editor input dispatcher) and
 * tools/editor_demo/input.c (generic editor input dispatcher).
 * Each function operates on EditorState's input buffer (cursor,
 * selection, input text) and is strictly REPL-free.
 *
 * Purity is locked by scripts/check/check-edit-ops-pure.sh: the
 * implementation must not include any repl/ header or reference
 * any repl_* symbol. The guard runs in `make check` and
 * `make check-state-ownership`.
 *
 * Two layers of primitives:
 *
 *   - Buffer-only primitives (edit_op_buffer_*): pure mutations
 *     of the input buffer + cursor. They do not consult or modify
 *     the selection anchor. Useful as building blocks and for
 *     test isolation.
 *
 *   - Selection-aware primitives (edit_op_type_char,
 *     edit_op_backspace): the "what the user expects" entrypoints.
 *     They first consume any active input-buffer selection, then
 *     fall back to the buffer-only primitive. This matches every
 *     mainstream editor's behavior on typing and backspace.
 *
 * Callers handle their own side-effects (autocomplete refresh,
 * cursor blink reset, redraw request). edit_ops never reaches into
 * those higher-level concerns.
 */
#ifndef EDITOR_EDIT_OPS_H
#define EDITOR_EDIT_OPS_H

/* ---- Buffer-only primitives (no selection handling) ------------- */

/* Insert `c` at the current cursor position and advance the cursor
 * by one. Returns 1 on success, 0 if the input buffer cannot fit
 * another character. Pure with respect to selection: the anchor
 * (if any) is left untouched. */
int edit_op_buffer_insert_char_at_cursor(char c);

/* Delete the character immediately before the cursor and move the
 * cursor left by one. Returns 1 if a character was deleted, 0 if
 * the cursor was already at position 0 or the buffer was empty.
 * Pure with respect to selection: the anchor (if any) is left
 * untouched. */
int edit_op_buffer_delete_left_of_cursor(void);

/* Delete the character at the cursor position; the cursor stays in
 * place (the next character shifts under it). Returns 1 if a
 * character was deleted, 0 if the cursor was already at end-of-
 * buffer. Pure with respect to selection: the anchor is untouched. */
int edit_op_buffer_delete_right_of_cursor(void);

/* If a character-range selection is active in the input buffer,
 * delete the selected bytes, move the cursor to the (collapsed)
 * range start, and clear the anchor. Returns 1 if a selection was
 * consumed, 0 if none was active. The buffer-only deletion path -
 * does not touch document state or autocomplete. */
int edit_op_consume_input_selection(void);

/* ---- Selection-aware higher-level operations ------------------- */

/* "Type a character": if an input-buffer selection is active,
 * delete it first, then insert `c` at the (now collapsed) cursor.
 * Returns 1 on success (selection consumed and/or character
 * inserted), 0 if the post-consume buffer cannot fit `c`. */
int edit_op_type_char(char c);

/* "Backspace": if an input-buffer selection is active, delete it.
 * Otherwise, delete the character before the cursor. Returns 1
 * if anything changed (selection consumed OR char deleted), 0 if
 * the cursor was at 0 with no selection. */
int edit_op_backspace(void);

#endif /* EDITOR_EDIT_OPS_H */
