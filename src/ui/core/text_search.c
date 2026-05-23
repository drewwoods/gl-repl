/*
 * text_search.c - Pure case-insensitive substring search helpers.
 */
#include "ui/core/text_search.h"

#include <ctype.h>
#include <string.h>

int ui_text_matches_at(const char *text, const char *query, int pos) {
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

int ui_text_find_next_in_text(const char *text, const char *query,
                              int start_pos) {
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
        if (ui_text_matches_at(text, query, pos))
            return pos;
    }

    return -1;
}

int ui_text_find_prev_in_text(const char *text, const char *query,
                              int start_pos) {
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
        if (ui_text_matches_at(text, query, pos))
            return pos;
    }

    return -1;
}
