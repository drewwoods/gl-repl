/*
 * src/repl/replace.h - Atomic whole-document rebuild from source text.
 *
 * The find-bar's replace operations need something the incremental commit
 * path cannot give them: a rewrite that is only ever validated as a whole.
 * Renaming a variable or a funcN alias is invalid at every intermediate
 * step — the declaration cannot be renamed while call sites still use the
 * old name, and no call site can be renamed before the declaration — so
 * the editor substitutes text across every row and hands the finished
 * document here.
 *
 * The rebuild replays the substituted lines through repl_load_apply_line,
 * the same loader that reads saved files and built-in examples, so any
 * document that would load from disk is a legal result. It is all-or-
 * nothing: a SceneSnapshot taken before the reset is applied back if any
 * line fails, leaving commands, text, variables, scratch arrays, aliases,
 * cfg, and camera exactly as they were.
 *
 * State that survives a successful rebuild, because the loader would
 * otherwise reset it:
 *   - predefined-variable VALUES, matched to the new table by name;
 *     `rename_from` / `rename_to` carry the value of a variable the
 *     substitution renamed,
 *   - the is_auto flag on auto-generated glNormal3f rows, restored by row
 *     position (a text substitution never changes the row count), so
 *     auto-normals keep tracking their geometry instead of freezing into
 *     user normals.
 *
 * Layering: text matching and substitution belong to the editor's search
 * session (src/editor/replace.c) — this module takes finished lines and
 * owns only the document transaction.
 */
#ifndef REPL_REPLACE_H
#define REPL_REPLACE_H

#include "config.h"

typedef struct ReplDocumentRebuild {
    /* Substituted document text, one entry per row, in document order. */
    const char *const *lines;
    int count;

    /* Optional identifier rename, used only to carry a predefined
     * variable's live value across the rename. NULL/empty when the
     * replacement is not a rename. */
    const char *rename_from;
    const char *rename_to;
} ReplDocumentRebuild;

typedef struct ReplDocumentRebuildResult {
    int  failed_line;   /* 0-based row that refused to load, else -1 */
    char err[REPL_DIAG_TEXT_MAX];
} ReplDocumentRebuildResult;

/* Rebuild the live document from `req->lines`. Returns 1 on success; on
 * failure returns 0, restores the pre-call document wholesale, and fills
 * `out` with the offending row and the loader's diagnostic. `out` may be
 * NULL. The caller is responsible for the undo snapshot and for reloading
 * the editor input line afterwards. */
int repl_document_rebuild(const ReplDocumentRebuild *req,
                          ReplDocumentRebuildResult *out);

#endif /* REPL_REPLACE_H */
