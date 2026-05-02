#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

#include "sample.h"  /* MAX_COMMANDS, MAX_LINE_LEN */

/* EditorState owns editable text, cursor, selection, navigation, undo
 * transactions, and the rest of the code-editor session per the
 * three-layer ownership contract in MODULES.md and
 * feature/editor-owns-text-completion.md.
 *
 * Phase 1 progress:
 *   commit 3: scaffold (placeholder struct).
 *   commit 4: editor_buffer slice (this commit).
 *
 * Slices still pending migration:
 *   commit 5: editor_input / cursor / edit_line / insert_mode / pending_newline
 *   commit 6: selection + clipboard + search + autocomplete
 *
 * `repl_state_capture/restore` no longer covers editor-owned slices; the
 * editor session has its own `editor_state_capture/restore` pair.
 * `repl_state_reset_all` drains EditorState for tests via
 * `editor_state_reset` so a single reset call still clears all three
 * structs.
 */

/* Per-line canonical text. One slot per source command, indexed by
 * source command index. Text is the user-typed form (no trailing ';',
 * no leading whitespace) — the same shape `load_line_to_input` produces
 * after stripping. The typedef lives in editor_state.h because
 * EditorState owns the buffer; repl_state_views.h no longer defines or
 * declares anything related to it. */
typedef struct {
    char lines[MAX_COMMANDS][MAX_LINE_LEN];
    int  line_count;
} ReplEditorBuffer;

typedef struct {
    ReplEditorBuffer buffer;
} EditorState;

/* Capture / restore / reset symmetry with repl_state_*. */
void editor_state_capture(EditorState *snapshot);
void editor_state_restore(const EditorState *snapshot);
void editor_state_reset(void);

/* Struct-level access to the live buffer. Callers that walk the buffer
 * directly (repl_command_store internals during the Phase 2 transition)
 * use `_mut`; readers that need the whole struct as `const` use the
 * non-mut form. Everyone else uses the slice-level API below. */
const ReplEditorBuffer *editor_state_buffer(void);
ReplEditorBuffer       *editor_state_buffer_mut(void);

/* Slice-level editor_buffer API. This is the long-term surface; the
 * Phase 2 commit 8 mutation primitives (insert / replace / delete /
 * load) extend the same namespace. */
const char *editor_buffer_line(int idx);
void        editor_buffer_set_line(int idx, const char *text);
int         editor_buffer_count(void);
void        editor_buffer_set_count(int count);

#endif /* EDITOR_STATE_H */
