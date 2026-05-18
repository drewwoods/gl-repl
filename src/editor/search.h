/*
 * editor_search.h - Search overlay query helpers and input routing.
 *
 * Owns the search-specific helpers that operate on the editor's search slice:
 * clearing search state, routing search-overlay keystrokes, mapping overlay
 * rows back to source lines, and scanning text forward or backward for the
 * next match. Query helpers are pure; the key handlers mutate the live search
 * session on EditorState.
 */
#ifndef EDITOR_SEARCH_H
#define EDITOR_SEARCH_H

void search_clear_all(void);
int  handle_search_key(unsigned char key);
int  handle_search_special(int key);

int  editor_search_row_count(void);
const char *editor_search_row_text(int row_idx);
int  editor_search_row_for_cmd_index(int cmd_idx);
int  editor_search_find_next_in_text(const char *text, const char *query,
                                   int start_pos);
int  editor_search_find_prev_in_text(const char *text, const char *query,
                                   int start_pos);

#endif /* EDITOR_SEARCH_H */