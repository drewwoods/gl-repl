/*
 * text_search.c - Pure case-insensitive substring search helpers.
 *
 * Case folding is ASCII-only by design (the editor buffer is ASCII per
 * CLAUDE.md), so the locale-aware tolower() pulls a hazard with no
 * upside - under a Turkish locale, e.g., the 'I'/'i' mapping is wrong.
 * ascii_tolower() keeps the folding deterministic across locales.
 *
 * The `_opts` variants add whole-word (identifier-boundary) matching; the
 * short names forward to them with whole_word = 0 so existing callers keep
 * plain substring behavior. See text_search.h for the boundary rule.
 */
#include "ui/core/text_search.h"

#include <string.h>

static inline int ascii_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int ui_text_is_ident_char(int c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9') || u == '_';
}

/* Identifier-boundary test for a match of `query_len` chars at `pos`.
 * Only the ends where the query itself carries an identifier character
 * are constrained - a query like "(GL" has no left-hand word to bound. */
static int word_boundary_ok(const char *text, int text_len,
                            const char *query, int query_len, int pos) {
    int end = pos + query_len;

    if (pos > 0 && ui_text_is_ident_char(query[0]) &&
        ui_text_is_ident_char(text[pos - 1]))
        return 0;
    if (end < text_len && ui_text_is_ident_char(query[query_len - 1]) &&
        ui_text_is_ident_char(text[end]))
        return 0;
    return 1;
}

int ui_text_matches_at_opts(const char *text, const char *query, int pos,
                            int whole_word) {
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
        if (ascii_tolower(tc) != ascii_tolower(qc))
            return 0;
    }

    if (whole_word && !word_boundary_ok(text, text_len, query, query_len, pos))
        return 0;
    return 1;
}

int ui_text_find_next_in_text_opts(const char *text, const char *query,
                                   int start_pos, int whole_word) {
    int text_len;
    int query_len;

    if (!text || !query)
        return -1;

    text_len = (int)strlen(text);
    query_len = (int)strlen(query);
    if (query_len <= 0 || text_len <= 0)
        return -1;
    if (query_len > text_len)
        return -1;
    if (start_pos < 0)
        start_pos = 0;
    if (start_pos > text_len - query_len)
        return -1;

    for (int pos = start_pos; pos <= text_len - query_len; pos++) {
        if (ui_text_matches_at_opts(text, query, pos, whole_word))
            return pos;
    }

    return -1;
}

int ui_text_find_prev_in_text_opts(const char *text, const char *query,
                                   int start_pos, int whole_word) {
    int text_len;
    int query_len;
    int max_start;

    if (!text || !query)
        return -1;

    text_len = (int)strlen(text);
    query_len = (int)strlen(query);
    if (query_len <= 0 || text_len <= 0)
        return -1;
    if (query_len > text_len)
        return -1;

    max_start = text_len - query_len;
    if (start_pos > max_start)
        start_pos = max_start;
    if (start_pos < 0)
        return -1;

    for (int pos = start_pos; pos >= 0; pos--) {
        if (ui_text_matches_at_opts(text, query, pos, whole_word))
            return pos;
    }

    return -1;
}

int ui_text_matches_at(const char *text, const char *query, int pos) {
    return ui_text_matches_at_opts(text, query, pos, 0);
}

int ui_text_find_next_in_text(const char *text, const char *query,
                              int start_pos) {
    return ui_text_find_next_in_text_opts(text, query, start_pos, 0);
}

int ui_text_find_prev_in_text(const char *text, const char *query,
                              int start_pos) {
    return ui_text_find_prev_in_text_opts(text, query, start_pos, 0);
}
