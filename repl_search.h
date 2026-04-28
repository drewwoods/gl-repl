/*
 * repl_search.h - Search overlay query helpers and input routing.
 *
 * Search state is owned by repl_search.c and surfaced through repl_state.h.
 * Query helpers are read-only; the input handlers mutate the owned search
 * overlay state.
 */
#ifndef REPL_SEARCH_H
#define REPL_SEARCH_H

void search_clear_all(void);
int  handle_search_key(unsigned char key);
int  handle_search_special(int key);

int  repl_search_row_count(void);
const char *repl_search_row_text(int row_idx);
int  repl_search_row_for_cmd_index(int cmd_idx);
int  repl_search_find_next_in_text(const char *text, const char *query,
                                   int start_pos);
int  repl_search_find_prev_in_text(const char *text, const char *query,
                                   int start_pos);

#endif /* REPL_SEARCH_H */