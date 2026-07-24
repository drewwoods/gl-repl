/*
 * editor/replace.c - Find-bar replace operations.
 *
 * One code path serves both entry points: build the substituted document,
 * hand it to the REPL rebuild, restore the editor's view state. They
 * differ only in which occurrences the substitution accepts — all of
 * them, or the single one the find bar is parked on.
 *
 * Row model. The rewritten document always comes from the committed
 * editor buffer, never from the live input line, so a half-typed line
 * cannot make the whole replace fail. The input buffer gets the same
 * substitution applied to it separately and is restored afterwards, which
 * keeps the row the user is looking at in sync without routing it through
 * a commit.
 */
#include "replace.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "input.h"      /* editor_load_line_to_input */
#include "search.h"
#include "state.h"
#include "undo.h"

#include "repl/eval.h"          /* repl_eval_is_ident_start / _continue */
#include "repl/host_effects.h"  /* repl_set_status / _error */
#include "repl/replace.h"
#include "repl/state_views.h"
#include "ui/core/text_search.h"

/* Substitute matches of `query` in `src`, writing to `dst`.
 *
 * `only_occurrence` selects between the two modes: -1 replaces every
 * match, 0..n replaces just that occurrence within the line (used by
 * Replace-current). Returns the number of substitutions, or -1 when the
 * result would not fit in `dst_sz` — a line that grows past MAX_LINE_LEN
 * fails the whole operation rather than being silently truncated into
 * something that still parses. */
static int substitute_line(const char *src, const char *query,
                           const char *replacement, int whole_word,
                           int only_occurrence, char *dst, int dst_sz) {
    int query_len = (int)strlen(query);
    int repl_len = (int)strlen(replacement);
    int src_pos = 0;
    int out = 0;
    int count = 0;
    int seen = 0;

    if (!src || dst_sz <= 0 || query_len <= 0)
        return -1;

    for (;;) {
        int hit = ui_text_find_next_in_text_opts(src, query, src_pos, whole_word);
        int copy_len;

        if (hit < 0)
            break;
        if (only_occurrence >= 0 && seen != only_occurrence) {
            /* Copy through this match untouched and keep scanning. */
            copy_len = hit + query_len - src_pos;
            if (out + copy_len >= dst_sz)
                return -1;
            memcpy(dst + out, src + src_pos, (size_t)copy_len);
            out += copy_len;
            src_pos = hit + query_len;
            seen++;
            continue;
        }

        copy_len = hit - src_pos;
        if (out + copy_len + repl_len >= dst_sz)
            return -1;
        memcpy(dst + out, src + src_pos, (size_t)copy_len);
        out += copy_len;
        memcpy(dst + out, replacement, (size_t)repl_len);
        out += repl_len;
        src_pos = hit + query_len;
        seen++;
        count++;

        if (only_occurrence >= 0)
            break;
    }

    {
        int tail = (int)strlen(src + src_pos);
        if (out + tail >= dst_sz)
            return -1;
        memcpy(dst + out, src + src_pos, (size_t)tail);
        out += tail;
    }
    dst[out] = '\0';
    return count;
}

static int text_is_identifier(const char *s) {
    if (!s || !s[0])
        return 0;
    if (!repl_eval_is_ident_start((unsigned char)s[0]))
        return 0;
    for (int i = 1; s[i]; i++) {
        if (!repl_eval_is_ident_continue((unsigned char)s[i]))
            return 0;
    }
    return 1;
}

/* Editor view state the REPL rebuild resets (it drives the same input
 * sink an example load does). Captured around the transaction so a
 * replace does not double as "jump to line 0, drop what you were
 * typing". */
typedef struct {
    int  insert_mode;
    int  edit_line;
    int  cursor_pos;
    char input[MAX_INPUT_LEN];
} ReplaceViewState;

static void replace_capture_view(ReplaceViewState *view) {
    view->insert_mode = editor_insert_mode();
    view->edit_line = editor_state_edit_line();
    view->cursor_pos = editor_cursor_pos();
    snprintf(view->input, sizeof(view->input), "%s", editor_state_input().input);
}

static void replace_restore_view(const ReplaceViewState *view,
                                 const char *substituted_input) {
    editor_state_edit_line_set(view->edit_line);
    editor_state_edit_line_clamp();
    editor_insert_mode_set(view->insert_mode);

    if (view->insert_mode) {
        editor_input_set_text(substituted_input);
        editor_cursor_pos_set((int)strlen(substituted_input));
        return;
    }
    /* Overwrite mode: the committed row is the truth after a rebuild, so
     * reload from it rather than re-seating stale input text. */
    editor_load_line_to_input(editor_state_edit_line());
    if (view->cursor_pos <= (int)strlen(editor_state_input().input))
        editor_cursor_pos_set(view->cursor_pos);
}

/* Shared driver. `only_row` / `only_occurrence` are -1 for Replace All,
 * or the document row plus the index of the occurrence within that row
 * for Replace-current. */
static int replace_run(int only_row, int only_occurrence) {
    const EditorSearchState *srch = editor_state_search();
    const char *query;
    const char *replacement;
    int line_count = editor_buffer_count();
    char (*lines)[MAX_LINE_LEN] = NULL;
    const char **ptrs = NULL;
    char input_after[MAX_INPUT_LEN];
    ReplaceViewState view;
    ReplDocumentRebuild req;
    ReplDocumentRebuildResult res;
    EditorUndoRingState undo_before;
    int total = 0;
    int rebuilt;

    if (!srch->active || srch->query_len <= 0) {
        repl_set_status_error("Nothing to replace: no search query");
        return 0;
    }

    query = srch->query;
    replacement = srch->replace;
    if (strcmp(query, replacement) == 0) {
        repl_set_status_error("Replace text matches the search text");
        return 0;
    }
    if (srch->match_count <= 0) {
        repl_set_status_error("No matches to replace");
        return 0;
    }

    lines = malloc((size_t)(line_count > 0 ? line_count : 1) * MAX_LINE_LEN);
    ptrs = malloc((size_t)(line_count > 0 ? line_count : 1) * sizeof(*ptrs));
    if (!lines || !ptrs) {
        free(lines);
        free(ptrs);
        repl_set_status_error("Replace failed: out of memory");
        return -1;
    }

    for (int i = 0; i < line_count; i++) {
        const char *src = editor_buffer_line(i);
        int targeted = (only_row < 0) || (i == only_row);
        int at = (only_row < 0) ? -1 : only_occurrence;
        int n;

        if (!src)
            src = "";
        n = !targeted ? 0 : substitute_line(src, query, replacement,
                                            srch->whole_word, at,
                                            lines[i], MAX_LINE_LEN);
        if (n < 0) {
            free(lines);
            free(ptrs);
            {
                char msg[REPL_STATUS_TEXT_MAX];
                snprintf(msg, sizeof(msg),
                         "Replace failed: line %d would exceed %d chars",
                         i + 1, MAX_LINE_LEN - 1);
                repl_set_status_error(msg);
            }
            return -1;
        }
        if (!targeted)
            snprintf(lines[i], MAX_LINE_LEN, "%s", src);
        total += n;
        ptrs[i] = lines[i];
    }

    if (total <= 0) {
        free(lines);
        free(ptrs);
        repl_set_status_error("No matches to replace");
        return 0;
    }

    replace_capture_view(&view);
    /* The live input line is not part of the document; give it the same
     * substitution so the row under the cursor tracks the rewrite. */
    if (substitute_line(view.input, query, replacement, srch->whole_word,
                        -1, input_after, sizeof(input_after)) < 0)
        snprintf(input_after, sizeof(input_after), "%s", view.input);

    editor_undo_ring_state_capture(&undo_before);
    editor_undo_push_snapshot();

    memset(&req, 0, sizeof(req));
    req.lines = ptrs;
    req.count = line_count;
    if (text_is_identifier(query) && text_is_identifier(replacement)) {
        req.rename_from = query;
        req.rename_to = replacement;
    }

    rebuilt = repl_document_rebuild(&req, &res);

    free(lines);
    free(ptrs);

    if (!rebuilt) {
        char msg[REPL_STATUS_TEXT_MAX];

        /* repl_document_rebuild restored the document; drop the undo
         * snapshot too so a rejected replace leaves no trace. */
        editor_undo_ring_state_restore(&undo_before);
        replace_restore_view(&view, view.input);
        if (res.failed_line >= 0)
            snprintf(msg, sizeof(msg), "Replace failed at line %d: %s",
                     res.failed_line + 1, res.err);
        else
            snprintf(msg, sizeof(msg), "Replace failed: %s", res.err);
        repl_set_status_error(msg);
        return -1;
    }

    replace_restore_view(&view, input_after);
    /* Re-scan: with the old text gone the match set has changed, and the
     * count dropping to 0 is the user's confirmation the rename landed. */
    editor_search_rescan();

    {
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof(msg), "Replaced %d occurrence%s",
                 total, total == 1 ? "" : "s");
        repl_set_status(msg);
    }
    return total;
}

int editor_replace_all(void) {
    return replace_run(-1, -1);
}

int editor_replace_current(void) {
    const EditorSearchState *srch = editor_state_search();
    int doc_row;
    int occurrence;

    if (!srch->active || srch->query_len <= 0) {
        repl_set_status_error("Nothing to replace: no search query");
        return 0;
    }
    if (srch->hit_line_idx < 0 || srch->hit_char_idx < 0) {
        repl_set_status_error("No current match to replace");
        return 0;
    }

    occurrence = editor_search_current_hit_occurrence();
    if (occurrence < 0) {
        repl_set_status_error("No current match to replace");
        return 0;
    }

    doc_row = editor_search_row_to_doc_index(srch->hit_line_idx);
    if (doc_row < 0) {
        /* The match sits on the uncommitted input row. That is plain text
         * editing, not a document transaction: rewrite the input buffer
         * and leave the document (and the undo ring) alone. */
        char after[MAX_INPUT_LEN];
        int n = substitute_line(editor_state_input().input, srch->query,
                               srch->replace, srch->whole_word,
                               occurrence, after, sizeof(after));
        if (n <= 0) {
            repl_set_status_error("Replace failed: line too long");
            return -1;
        }
        editor_input_set_text(after);
        editor_cursor_pos_set((int)strlen(after));
        editor_search_rescan();
        repl_set_status("Replaced 1 occurrence");
        return n;
    }

    return replace_run(doc_row, occurrence);
}
