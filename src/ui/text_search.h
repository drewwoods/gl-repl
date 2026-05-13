/*
 * text_search.h - Pure case-insensitive substring search helpers.
 *
 * Shared by generic UI renderers and editor-side search state. These helpers
 * operate on plain NUL-terminated strings and have no dependency on editor,
 * REPL, or controller state.
 */
#ifndef UI_TEXT_SEARCH_H
#define UI_TEXT_SEARCH_H

int ui_text_matches_at(const char *text, const char *query, int pos);
int ui_text_find_next_in_text(const char *text, const char *query,
                              int start_pos);
int ui_text_find_prev_in_text(const char *text, const char *query,
                              int start_pos);

#endif /* UI_TEXT_SEARCH_H */

/*
 * text_search.h - Pure case-insensitive substring search helpers.
 *
 * Shared by generic UI renderers and editor-side search state. These helpers
 * operate on plain NUL-terminated strings and have no dependency on editor,
 * REPL, or controller state.
 */
#ifndef UI_TEXT_SEARCH_H
#define UI_TEXT_SEARCH_H

int ui_text_matches_at(const char *text, const char *query, int pos);
int ui_text_find_next_in_text(const char *text, const char *query,
                              int start_pos);
int ui_text_find_prev_in_text(const char *text, const char *query,
                              int start_pos);

#endif /* UI_TEXT_SEARCH_H */
