/*
 * editor_search.c - Case-insensitive incremental search over code-panel rows.
 *
 * A "row" is one visible line in the code panel. It usually maps 1:1 to a
 * GLCmd in repl_state_document_cmds_mut()[], except while inserting: an extra synthetic row holds
 * the live g_input at repl_state_edit_line(), and real repl_state_document_cmds_mut()[] entries at or beyond
 * that index are shifted down by one row.
 *
 *   row_count = repl_state_document_count()         (overwrite mode)
 *             = repl_state_document_count() + 1     (insert mode, or past-end edit line)
 *
 * A "hit" is (row, char_pos); an "ordinal" is the 1-based hit number among
 * all matches (shown to the user as "3 / 12"). Public helpers preserve
 * ordinals across navigation by remembering the per-row occurrence index
 * and remapping it after a navigation shifts the row mapping.
 *
 * The module owns search behavior; storage lives in src/repl/state.c and is
 * accessed through the typed search facade.
 */
#include "state.h"
#include "input.h"
#include "completion.h"
#include "search.h"
#include "ui/text_search.h"

#include "keys.h"
#include "repl/state_views.h"
#include "ui/state_types.h"

/* ui_state_help_mut is forward-declared here because repl_*.c is not
 * allowed to include ui_state.h per check-controller-boundaries. The
 * search-open path closes the help overlay as a side-effect; the
 * visibility flag lives on UiState while the session-state fields
 * (tab_idx, scroll) live on the editor_help_session peer. */
UiHelpState *ui_state_help_mut(void);
#include "help_session.h"

static int search_row_is_live_input(int row_idx) {
    if (row_idx < 0 || row_idx >= editor_search_row_count())
        return 0;
    return row_idx == repl_state_edit_line();
}

static int search_row_to_nav_line(int row_idx) {
    if (row_idx < 0 || row_idx >= editor_search_row_count())
        return -1;
    if (search_row_is_live_input(row_idx)) {
        if (editor_insert_mode())
            return -1;
        return repl_state_edit_line();
    }
    if (editor_insert_mode() && row_idx > repl_state_edit_line())
        return row_idx - 1;
    return row_idx;
}

int editor_search_find_next_in_text(const char *text, const char *query,
                                  int start_pos) {
    return ui_text_find_next_in_text(text, query, start_pos);
}

int editor_search_find_prev_in_text(const char *text, const char *query,
                                  int start_pos) {
    return ui_text_find_prev_in_text(text, query, start_pos);
}

int editor_search_row_for_cmd_index(int cmd_idx) {
    int edit_line = repl_state_edit_line();

    if (cmd_idx < 0 || cmd_idx >= editor_buffer_count())
        return -1;
    if (editor_insert_mode() && cmd_idx >= edit_line)
        return cmd_idx + 1;
    return cmd_idx;
}

static int search_row_match_count(int row_idx) {
    EditorSearchState srch = editor_state_search();
    const char *text;
    int count = 0;

    if (srch.query_len <= 0)
        return 0;

    text = editor_search_row_text(row_idx);
    for (int pos = editor_search_find_next_in_text(text, srch.query, 0);
         pos >= 0;
         pos = editor_search_find_next_in_text(text, srch.query, pos + 1)) {
        count++;
    }

    return count;
}

static int search_total_matches(void) {
    EditorSearchState srch = editor_state_search();
    int total = 0;

    if (srch.query_len <= 0)
        return 0;

    for (int row = 0; row < editor_search_row_count(); row++) {
        const char *text = editor_search_row_text(row);
        int pos = editor_search_find_next_in_text(text, srch.query, 0);
        while (pos >= 0) {
            total++;
            pos = editor_search_find_next_in_text(text, srch.query, pos + 1);
        }
    }

    return total;
}

static int search_hit_exists(int row_idx, int char_pos) {
    EditorSearchState srch = editor_state_search();
    const char *text;

    if (srch.query_len <= 0)
        return 0;
    if (row_idx < 0 || row_idx >= editor_search_row_count() || char_pos < 0)
        return 0;

    text = editor_search_row_text(row_idx);
    return ui_text_matches_at(text, srch.query, char_pos);
}

static int search_row_occurrence_index(int row_idx, int char_pos) {
    EditorSearchState srch = editor_state_search();
    const char *text;
    int occurrence = 0;

    if (!search_hit_exists(row_idx, char_pos))
        return -1;

    text = editor_search_row_text(row_idx);
    for (int pos = editor_search_find_next_in_text(text, srch.query, 0);
         pos >= 0;
         pos = editor_search_find_next_in_text(text, srch.query, pos + 1)) {
        if (pos == char_pos)
            return occurrence;
        occurrence++;
    }

    return -1;
}

static int search_char_for_row_occurrence(int row_idx, int occurrence_idx) {
    EditorSearchState srch = editor_state_search();
    const char *text;
    int occurrence = 0;

    if (srch.query_len <= 0 || row_idx < 0 || row_idx >= editor_search_row_count() ||
        occurrence_idx < 0)
        return -1;

    text = editor_search_row_text(row_idx);
    for (int pos = editor_search_find_next_in_text(text, srch.query, 0);
         pos >= 0;
         pos = editor_search_find_next_in_text(text, srch.query, pos + 1)) {
        if (occurrence == occurrence_idx)
            return pos;
        occurrence++;
    }

    return -1;
}

static int search_ordinal_for_hit(int row_idx, int char_pos) {
    EditorSearchState srch = editor_state_search();
    int ordinal = 1;

    if (!search_hit_exists(row_idx, char_pos))
        return 0;

    for (int row = 0; row < editor_search_row_count(); row++) {
        if (row == row_idx)
            break;
        ordinal += search_row_match_count(row);
    }

    for (int pos = 0; pos < char_pos; ) {
        pos = editor_search_find_next_in_text(editor_search_row_text(row_idx), srch.query, pos);
        if (pos < 0 || pos >= char_pos)
            break;
        ordinal++;
        pos++;
    }

    return ordinal;
}

static void search_clear_matches(void) {
    EditorSearchState *srch = editor_state_search_mut();
    srch->hit_line_idx = -1;
    srch->hit_char_idx = -1;
    srch->hit_ordinal = 0;
    srch->match_count = 0;
}

static void search_store_hit(int row_idx, int char_pos) {
    EditorSearchState *srch = editor_state_search_mut();
    if (!search_hit_exists(row_idx, char_pos)) {
        search_clear_matches();
        return;
    }

    srch->match_count = search_total_matches();
    srch->hit_line_idx = row_idx;
    srch->hit_char_idx = char_pos;
    srch->hit_ordinal = search_ordinal_for_hit(row_idx, char_pos);
}

void editor_search_clear_all(void) {
    EditorSearchState *srch = editor_state_search_mut();
    srch->active = 0;
    srch->query[0] = '\0';
    srch->query_len = 0;
    srch->cursor_pos = 0;
    search_clear_matches();
}

int editor_search_row_count(void) {
    int num_lines = editor_buffer_count();
    if (editor_insert_mode() || repl_state_edit_line() == num_lines)
        return num_lines + 1;
    return num_lines;
}

const char *editor_search_row_text(int row_idx) {
    int edit_line = repl_state_edit_line();

    if (row_idx < 0 || row_idx >= editor_search_row_count())
        return "";
    if (search_row_is_live_input(row_idx))
        return editor_state_input().input;
    if (editor_insert_mode() && row_idx > edit_line)
        row_idx--;
    /* Source text lives in the editor buffer; read it through an
     * EditorBufferView. */
    EditorBufferView text_view = editor_buffer_view();
    const char *text = editor_buffer_view_line(text_view, row_idx);
    if (text)
        return text;
    return "";
}

static int search_find_forward(int start_row, int start_char,
                               int *out_row, int *out_char) {
    EditorSearchState srch = editor_state_search();
    int row_count = editor_search_row_count();

    if (srch.query_len <= 0 || row_count <= 0)
        return 0;

    if (start_row < 0)
        start_row = 0;
    if (start_row >= row_count)
        start_row = row_count - 1;
    if (start_char < 0)
        start_char = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (int row = start_row; row < row_count; row++) {
            int pos = editor_search_find_next_in_text(
                editor_search_row_text(row), srch.query,
                row == start_row ? start_char : 0);
            if (pos >= 0) {
                if (out_row) *out_row = row;
                if (out_char) *out_char = pos;
                return 1;
            }
        }
        start_row = 0;
        start_char = 0;
    }

    return 0;
}

static int search_find_backward(int start_row, int start_char,
                                int *out_row, int *out_char) {
    EditorSearchState srch = editor_state_search();
    int row_count = editor_search_row_count();

    if (srch.query_len <= 0 || row_count <= 0)
        return 0;

    if (start_row < 0)
        start_row = 0;
    if (start_row >= row_count)
        start_row = row_count - 1;

    for (int pass = 0; pass < 2; pass++) {
        for (int row = start_row; row >= 0; row--) {
            const char *text = editor_search_row_text(row);
            int max_start = (int)strlen(text) - srch.query_len;
            int pos;

            if (max_start < 0)
                continue;

            pos = (row == start_row) ? start_char : max_start;
            if (pos > max_start)
                pos = max_start;

            pos = editor_search_find_prev_in_text(text, srch.query, pos);
            if (pos >= 0) {
                if (out_row) *out_row = row;
                if (out_char) *out_char = pos;
                return 1;
            }
        }
        start_row = row_count - 1;
        start_char = MAX_INPUT_LEN;
    }

    return 0;
}

/* Move cursor to the row holding (row, char_pos), then re-resolve the hit
 * in terms of the row's n-th occurrence. Navigating may shift rows (e.g.
 * exiting insert mode collapses the synthetic row), so we remember the
 * occurrence ordinal and look up its new char position afterwards. */
static void search_apply_hit(int row, int char_pos) {
    int row_occurrence = search_row_occurrence_index(row, char_pos);
    int nav_line = search_row_to_nav_line(row);
    if (nav_line >= 0) {
        editor_scroll_follow_cursor_set(1);
        editor_navigate_to_line(nav_line);
        row = repl_state_edit_line();
        if (row_occurrence >= 0) {
            int remapped_char = search_char_for_row_occurrence(row, row_occurrence);
            if (remapped_char >= 0)
                char_pos = remapped_char;
        }
    }
    search_store_hit(row, char_pos);
}

/* Re-seed the search after the query text changed. Always anchors to
 * repl_state_edit_line() rather than the previous hit, so typing another character
 * jumps to the nearest match from the cursor instead of chaining from
 * wherever the last match landed. */
static void search_refresh_query(void) {
    EditorSearchState *srch = editor_state_search_mut();
    int row;
    int char_pos;

    if (!srch->active)
        return;

    srch->query_len = (int)strlen(srch->query);
    if (srch->cursor_pos > srch->query_len)
        srch->cursor_pos = srch->query_len;
    if (srch->query_len <= 0) {
        search_clear_matches();
        return;
    }

    if (!search_find_forward(repl_state_edit_line(), 0, &row, &char_pos)) {
        search_clear_matches();
        return;
    }
    search_apply_hit(row, char_pos);
}

/* Jump to the next (+1) or previous (-1) match, wrapping at document ends.
 * If a hit is already active, we step one char past/before it; otherwise we
 * anchor the scan at repl_state_edit_line(). */
static void search_navigate(int direction) {
    EditorSearchState srch = editor_state_search();
    int row;
    int char_pos;
    int found;
    int have_hit = (srch.hit_line_idx >= 0 && srch.hit_char_idx >= 0);

    if (!srch.active || srch.query_len <= 0)
        return;

    if (direction < 0) {
        int start_row  = have_hit ? srch.hit_line_idx      : repl_state_edit_line();
        int start_char = have_hit ? srch.hit_char_idx - 1  : MAX_INPUT_LEN;
        found = search_find_backward(start_row, start_char, &row, &char_pos);
    } else {
        int start_row  = have_hit ? srch.hit_line_idx      : repl_state_edit_line();
        int start_char = have_hit ? srch.hit_char_idx + 1  : 0;
        found = search_find_forward(start_row, start_char, &row, &char_pos);
    }

    if (!found) {
        search_clear_matches();
        return;
    }

    search_apply_hit(row, char_pos);
}

static void search_open(void) {
    EditorSearchState *srch = editor_state_search_mut();
    if (srch->active)
        return;

    srch->active = 1;
    srch->cursor_pos = srch->query_len;
    ui_state_help_mut()->visible = 0;
    editor_help_session_set_tab(0);
    editor_help_session_set_scroll(0);
    editor_completion_clear();
}

int editor_search_handle_key(unsigned char key) {
    EditorSearchState *srch = editor_state_search_mut();
    if (key == KEY_CTRL_F) {
        search_open();
        return 1;
    }
    if (!srch->active)
        return 0;

    if (key == KEY_ESC) {
        editor_search_clear_all();
        return 1;
    }

    if (key == '\r' || key == '\n') {
        search_navigate(+1);
        return 1;
    }

    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (srch->cursor_pos > 0 && srch->query_len > 0) {
            memmove(&srch->query[srch->cursor_pos - 1],
                &srch->query[srch->cursor_pos],
                (size_t)(srch->query_len - srch->cursor_pos + 1));
            srch->query_len--;
            srch->cursor_pos--;
            search_refresh_query();
        }
        return 1;
    }

    if (key_is_printable_ascii(key) && srch->query_len < MAX_INPUT_LEN - 2) {
        memmove(&srch->query[srch->cursor_pos + 1],
                &srch->query[srch->cursor_pos],
                (size_t)(srch->query_len - srch->cursor_pos + 1));
        srch->query[srch->cursor_pos] = (char)key;
        srch->query_len++;
        srch->cursor_pos++;
        search_refresh_query();
        return 1;
    }

    return 1;
}

int editor_search_handle_special(int key) {
    EditorSearchState *srch = editor_state_search_mut();
    if (!srch->active)
        return 0;

    switch (key) {
    case GLUT_KEY_LEFT:
        if (srch->cursor_pos > 0)
            srch->cursor_pos--;
        break;
    case GLUT_KEY_RIGHT:
        if (srch->cursor_pos < srch->query_len)
            srch->cursor_pos++;
        break;
    case GLUT_KEY_HOME:
        srch->cursor_pos = 0;
        break;
    case GLUT_KEY_END:
        srch->cursor_pos = srch->query_len;
        break;
    case GLUT_KEY_UP:
        search_navigate(-1);
        break;
    case GLUT_KEY_DOWN:
        search_navigate(+1);
        break;
    default:
        break;
    }

    return 1;
}
