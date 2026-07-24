/*
 * text_search.h - Pure case-insensitive substring search helpers.
 *
 * Shared by generic UI renderers and editor-side search state. These helpers
 * operate on plain NUL-terminated strings and have no dependency on editor,
 * REPL, or controller state.
 *
 * Two matching modes. Plain substring is the default (the three short
 * entry points). The `_opts` twins take a `whole_word` flag that additionally
 * requires identifier boundaries: a match is rejected when the query's first
 * character is an identifier character preceded by one in the text, or its
 * last is followed by one. Queries that begin/end with punctuation are
 * unaffected at that end, matching the usual \b semantics — so whole-word
 * "r" skips the r inside glColor3f but whole-word "(GL" still matches.
 */
#ifndef UI_TEXT_SEARCH_H
#define UI_TEXT_SEARCH_H

int ui_text_matches_at(const char *text, const char *query, int pos);
int ui_text_find_next_in_text(const char *text, const char *query,
                              int start_pos);
int ui_text_find_prev_in_text(const char *text, const char *query,
                              int start_pos);

int ui_text_matches_at_opts(const char *text, const char *query, int pos,
                            int whole_word);
int ui_text_find_next_in_text_opts(const char *text, const char *query,
                                   int start_pos, int whole_word);
int ui_text_find_prev_in_text_opts(const char *text, const char *query,
                                   int start_pos, int whole_word);

/* 1 when `c` may appear inside an identifier ([A-Za-z0-9_]). Exposed
 * because the replace path needs the same boundary rule when it decides
 * whether a query/replacement pair is an identifier rename. */
int ui_text_is_ident_char(int c);

#endif /* UI_TEXT_SEARCH_H */
