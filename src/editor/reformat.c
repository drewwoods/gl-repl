/*
 * editor/reformat.c - Editor wrapper for the REPL reformat pass.
 *
 * `repl_reformat_program()` (REPL pipeline) walks every command and
 * rewrites the editor-buffer line + GLCmd in canonical form. The
 * wrapper here saves/restores the typed input buffer and refreshes it
 * via editor_load_line_to_input() when the user is not in insert mode, so a
 * Ctrl+\ reformat keeps the editor session intact.
 */
#include <string.h>

#include "reformat.h"

#include "repl/core.h"        /* repl_reformat_program */
#include "repl/state.h"
#include "input.h" /* editor_load_line_to_input */
#include "state.h"

void editor_reformat_commands(void) {
    int saved_edit_line = repl_state_edit_line();
    int saved_inserting = editor_insert_mode();
    char saved_input[MAX_INPUT_LEN];
    int saved_input_len = editor_state_input().input_len;
    int saved_cursor_pos = editor_cursor_pos();
    memcpy(saved_input, editor_state_input().input, sizeof(saved_input));

    repl_reformat_program();

    repl_state_edit_line_set(saved_edit_line);
    repl_state_edit_line_clamp();
    editor_insert_mode_set(saved_inserting);
    if (saved_inserting) {
        EditorInputState *inp = editor_state_input_mut();
        memcpy(inp->input, saved_input, sizeof(saved_input));
        inp->input_len = saved_input_len;
        editor_cursor_pos_set(saved_cursor_pos);
    } else {
        editor_load_line_to_input(repl_state_edit_line());
    }
}
