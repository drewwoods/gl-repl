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

void editor_search_clear_all(void);
int  editor_search_handle_key(unsigned char key);
int  editor_search_handle_special(int key);

/* Step to the next (+1) or previous (-1) match, wrapping at document ends.
 * Same path the Enter / Up / Down keys take; exposed for the find-bar
 * match-navigator buttons. No-op when search is inactive or has no query. */
void editor_search_navigate(int direction);

int  editor_search_row_count(void);
const char *editor_search_row_text(int row_idx);
int  editor_search_row_for_cmd_index(int cmd_idx);
int  editor_search_find_next_in_text(const char *text, const char *query,
                                   int start_pos);
int  editor_search_find_prev_in_text(const char *text, const char *query,
                                   int start_pos);

#endif /* EDITOR_SEARCH_H */