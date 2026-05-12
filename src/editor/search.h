/*
 * editor_search.h - Search overlay query helpers and input routing.
 *
 * Search helpers operate on the shared search overlay state exposed through
 * src/repl/state.h. Query helpers are read-only; the input handlers mutate the
 * overlay state.
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