/*
 * repl_search.c — Case-insensitive incremental search over code-panel rows.
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
 * The module owns g_search_* state; outside code only reads it for
 * rendering (ui_panels.c) and clears via search_clear_all().
 */
#include "sample.h"
#include "repl_core_internal.h"
#include "repl_keys.h"

int  g_search_active = 0;
char g_search_query[MAX_INPUT_LEN] = "";
int  g_search_query_len = 0;
int  g_search_cursor_pos = 0;
int  g_search_hit_line = -1;
int  g_search_hit_char = -1;
int  g_search_hit_ordinal = 0;
int  g_search_match_count = 0;

static int search_row_is_live_input(int row_idx) {
    if (row_idx < 0 || row_idx >= repl_search_row_count())
        return 0;
    return row_idx == repl_state_edit_line();
}

static int search_row_to_nav_line(int row_idx) {
    if (row_idx < 0 || row_idx >= repl_search_row_count())
        return -1;
    if (search_row_is_live_input(row_idx)) {
        if (repl_state_insert_mode())
            return -1;
        return repl_state_edit_line();
    }
    if (repl_state_insert_mode() && row_idx > repl_state_edit_line())
        return row_idx - 1;
    return row_idx;
}

static int search_text_matches_at(const char *text, const char *query, int pos) {
    int text_len;
    int query_len;

    if (!text || !query)
        return 0;

    text_len = (int)strlen(text);
    query_len = (int)strlen(query);
    if (query_len <= 0 || pos < 0 || pos + query_len > text_len)
        return 0;

    for (int i = 0; i < query_len; i++) {
        unsigned char tc = (unsigned char)text[pos + i];
        unsigned char qc = (unsigned char)query[i];
        if (tolower(tc) != tolower(qc))
            return 0;
    }
    return 1;
}

static int search_total_matches(void) {
    int total = 0;

    if (g_search_query_len <= 0)
        return 0;

    for (int row = 0; row < repl_search_row_count(); row++) {
        const char *text = repl_search_row_text(row);
        int pos = repl_search_find_next_in_text(text, g_search_query, 0);
        while (pos >= 0) {
            total++;
            pos = repl_search_find_next_in_text(text, g_search_query, pos + 1);
        }
    }

    return total;
}

static int search_hit_exists(int row_idx, int char_pos) {
    const char *text;

    if (g_search_query_len <= 0)
        return 0;
    if (row_idx < 0 || row_idx >= repl_search_row_count() || char_pos < 0)
        return 0;

    text = repl_search_row_text(row_idx);
    return search_text_matches_at(text, g_search_query, char_pos);
}

static int search_row_occurrence_index(int row_idx, int char_pos) {
    const char *text;
    int occurrence = 0;

    if (!search_hit_exists(row_idx, char_pos))
        return -1;

    text = repl_search_row_text(row_idx);
    for (int pos = repl_search_find_next_in_text(text, g_search_query, 0);
         pos >= 0;
         pos = repl_search_find_next_in_text(text, g_search_query, pos + 1)) {
        if (pos == char_pos)
            return occurrence;
        occurrence++;
    }

    return -1;
}

static int search_char_for_row_occurrence(int row_idx, int occurrence_idx) {
    const char *text;
    int occurrence = 0;

    if (g_search_query_len <= 0 || row_idx < 0 || row_idx >= repl_search_row_count() ||
        occurrence_idx < 0)
        return -1;

    text = repl_search_row_text(row_idx);
    for (int pos = repl_search_find_next_in_text(text, g_search_query, 0);
         pos >= 0;
         pos = repl_search_find_next_in_text(text, g_search_query, pos + 1)) {
        if (occurrence == occurrence_idx)
            return pos;
        occurrence++;
    }

    return -1;
}

static int search_ordinal_for_hit(int row_idx, int char_pos) {
    int ordinal = 0;

    if (!search_hit_exists(row_idx, char_pos))
        return 0;

    for (int row = 0; row < repl_search_row_count(); row++) {
        const char *text = repl_search_row_text(row);
        int pos = repl_search_find_next_in_text(text, g_search_query, 0);
        while (pos >= 0) {
            ordinal++;
            if (row == row_idx && pos == char_pos)
                return ordinal;
            pos = repl_search_find_next_in_text(text, g_search_query, pos + 1);
        }
    }

    return 0;
}

static void search_clear_matches(void) {
    g_search_hit_line = -1;
    g_search_hit_char = -1;
    g_search_hit_ordinal = 0;
    g_search_match_count = 0;
}

static void search_store_hit(int row_idx, int char_pos) {
    if (!search_hit_exists(row_idx, char_pos)) {
        search_clear_matches();
        return;
    }

    g_search_match_count = search_total_matches();
    g_search_hit_line = row_idx;
    g_search_hit_char = char_pos;
    g_search_hit_ordinal = search_ordinal_for_hit(row_idx, char_pos);
}

void search_clear_all(void) {
    g_search_active = 0;
    g_search_query[0] = '\0';
    g_search_query_len = 0;
    g_search_cursor_pos = 0;
    search_clear_matches();
}

int repl_search_row_count(void) {
    int num_cmds = repl_state_document_count();
    if (repl_state_insert_mode() || repl_state_edit_line() == num_cmds)
        return num_cmds + 1;
    return num_cmds;
}

const char *repl_search_row_text(int row_idx) {
    int num_cmds = repl_state_document_count();
    int edit_line = repl_state_edit_line();
    if (row_idx < 0 || row_idx >= repl_search_row_count())
        return "";
    if (search_row_is_live_input(row_idx))
        return repl_state_editor_input()->input;
    if (repl_state_insert_mode() && row_idx > edit_line)
        row_idx--;
    if (row_idx >= 0 && row_idx < num_cmds)
        return repl_state_document_cmds()[row_idx].source;
    return "";
}

int repl_search_row_for_cmd_index(int cmd_idx) {
    int num_cmds = repl_state_document_count();
    if (cmd_idx < 0 || cmd_idx >= num_cmds)
        return -1;
    return (repl_state_insert_mode() && cmd_idx >= repl_state_edit_line()) ? cmd_idx + 1 : cmd_idx;
}

int repl_search_find_next_in_text(const char *text, const char *query,
                                  int start_pos) {
    int text_len;
    int query_len;

    if (!text || !query)
        return -1;

    text_len = (int)strlen(text);
    query_len = (int)strlen(query);
    if (query_len <= 0 || text_len < query_len)
        return -1;
    if (start_pos < 0)
        start_pos = 0;

    for (int pos = start_pos; pos + query_len <= text_len; pos++) {
        if (search_text_matches_at(text, query, pos))
            return pos;
    }
    return -1;
}

int repl_search_find_prev_in_text(const char *text, const char *query,
                                  int start_pos) {
    int text_len;
    int query_len;
    int max_start;

    if (!text || !query)
        return -1;

    text_len = (int)strlen(text);
    query_len = (int)strlen(query);
    if (query_len <= 0 || text_len < query_len)
        return -1;

    max_start = text_len - query_len;
    if (start_pos > max_start)
        start_pos = max_start;

    for (int pos = start_pos; pos >= 0; pos--) {
        if (search_text_matches_at(text, query, pos))
            return pos;
    }
    return -1;
}

static int search_find_forward(int start_row, int start_char,
                               int *out_row, int *out_char) {
    int row_count = repl_search_row_count();

    if (g_search_query_len <= 0 || row_count <= 0)
        return 0;

    if (start_row < 0)
        start_row = 0;
    if (start_row >= row_count)
        start_row = row_count - 1;
    if (start_char < 0)
        start_char = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (int row = start_row; row < row_count; row++) {
            int pos = repl_search_find_next_in_text(
                repl_search_row_text(row), g_search_query,
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
    int row_count = repl_search_row_count();

    if (g_search_query_len <= 0 || row_count <= 0)
        return 0;

    if (start_row < 0)
        start_row = 0;
    if (start_row >= row_count)
        start_row = row_count - 1;

    for (int pass = 0; pass < 2; pass++) {
        for (int row = start_row; row >= 0; row--) {
            const char *text = repl_search_row_text(row);
            int max_start = (int)strlen(text) - g_search_query_len;
            int pos;

            if (max_start < 0)
                continue;

            pos = (row == start_row) ? start_char : max_start;
            if (pos > max_start)
                pos = max_start;

            pos = repl_search_find_prev_in_text(text, g_search_query, pos);
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
        g_scroll_follow_cursor = 1;
        navigate_to_line(nav_line);
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
    int row;
    int char_pos;

    if (!g_search_active)
        return;

    g_search_query_len = (int)strlen(g_search_query);
    if (g_search_cursor_pos > g_search_query_len)
        g_search_cursor_pos = g_search_query_len;
    if (g_search_query_len <= 0) {
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
    int row;
    int char_pos;
    int found;
    int have_hit = (g_search_hit_line >= 0 && g_search_hit_char >= 0);

    if (!g_search_active || g_search_query_len <= 0)
        return;

    if (direction < 0) {
        int start_row  = have_hit ? g_search_hit_line      : repl_state_edit_line();
        int start_char = have_hit ? g_search_hit_char - 1  : MAX_INPUT_LEN;
        found = search_find_backward(start_row, start_char, &row, &char_pos);
    } else {
        int start_row  = have_hit ? g_search_hit_line      : repl_state_edit_line();
        int start_char = have_hit ? g_search_hit_char + 1  : 0;
        found = search_find_forward(start_row, start_char, &row, &char_pos);
    }

    if (!found) {
        search_clear_matches();
        return;
    }

    search_apply_hit(row, char_pos);
}

static void search_open(void) {
    if (g_search_active)
        return;

    g_search_active = 1;
    g_search_cursor_pos = g_search_query_len;
    g_show_help = 0;
    g_help_tab = 0;
    g_help_scroll = 0;
    clear_autocomplete_state();
}

int handle_search_key(unsigned char key) {
    if (key == KEY_CTRL_F) {
        search_open();
        return 1;
    }
    if (!g_search_active)
        return 0;

    if (key == KEY_ESC) {
        search_clear_all();
        return 1;
    }

    if (key == '\r' || key == '\n') {
        search_navigate(+1);
        return 1;
    }

    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (g_search_cursor_pos > 0 && g_search_query_len > 0) {
            memmove(&g_search_query[g_search_cursor_pos - 1],
                    &g_search_query[g_search_cursor_pos],
                    (size_t)(g_search_query_len - g_search_cursor_pos + 1));
            g_search_query_len--;
            g_search_cursor_pos--;
            search_refresh_query();
        }
        return 1;
    }

    if (key >= 32 && key < 127 && g_search_query_len < MAX_INPUT_LEN - 2) {
        memmove(&g_search_query[g_search_cursor_pos + 1],
                &g_search_query[g_search_cursor_pos],
                (size_t)(g_search_query_len - g_search_cursor_pos + 1));
        g_search_query[g_search_cursor_pos] = (char)key;
        g_search_query_len++;
        g_search_cursor_pos++;
        search_refresh_query();
        return 1;
    }

    return 1;
}

int handle_search_special(int key) {
    if (!g_search_active)
        return 0;

    switch (key) {
    case GLUT_KEY_LEFT:
        if (g_search_cursor_pos > 0)
            g_search_cursor_pos--;
        break;
    case GLUT_KEY_RIGHT:
        if (g_search_cursor_pos < g_search_query_len)
            g_search_cursor_pos++;
        break;
    case GLUT_KEY_HOME:
        g_search_cursor_pos = 0;
        break;
    case GLUT_KEY_END:
        g_search_cursor_pos = g_search_query_len;
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
