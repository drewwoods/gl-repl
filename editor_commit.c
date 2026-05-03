/*
 * editor_commit.c -- Editor-side orchestration for compile/apply commits.
 *
 * The orchestration shape is the dual of repl_compile():
 *
 *   editor_commit_apply_compiled_change(change)
 *       repl_apply_predef_ops(change)             // predef-var cascade
 *       editor_buffer_apply_compiled_change(change)  // EditorState only
 *       repl_apply_compiled_change(change)        // ReplState only
 *
 * The undo capture for migrated handlers still rides on
 * repl_undo_push_snapshot() pushed at the dispatch sites
 * (;-key, Enter, feed_line) in repl_editor.c. Phase D's editor_input
 * carve will replace that with a per-commit transaction wrapping
 * this helper.
 */

#include "editor_commit.h"

#include "editor_state.h"
#include "repl_apply.h"
#include "repl_compile.h"

int editor_commit_apply_compiled_change(const struct ReplCompiledChange_s *change) {
    if (!change) return 0;

    /* Predef-var cascade fires first so the cmd-store / buffer
     * apply observe the post-cascade state when they walk the
     * document for any cross-checks. */
    repl_apply_predef_ops(change);

    /* Editor text first so that if the cmd-store call below fails
     * with a capacity error, the buffer state still describes the
     * intended source — the next undo / restore round-trip resyncs
     * cleanly. (This mirrors the existing legacy behaviour: a full
     * cmd-store has historically been treated as terminal.) */
    editor_buffer_apply_compiled_change(change);

    if (!repl_apply_compiled_change(change))
        return 0;

    return 1;
}
