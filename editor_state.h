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

/* Live editor-input state: the typing buffer, cursor, insert mode, and
 * pending-newline scratch. Owned by EditorState (Phase 1 commit 5).
 * `edit_line_idx` exists for view symmetry but is *not* the canonical
 * edit-line cursor — that lives on `document.edit_line_idx`; the view
 * builder copies it in for callers that consume the input view as a
 * single struct. */
typedef struct {
    char input[MAX_INPUT_LEN];
    int  input_capacity;
    int  input_len;
    int  cursor_pos;
    int  edit_line_idx;
    char pending_newline[MAX_INPUT_LEN];
    int  pending_newline_capacity;
    int  pending_newline_len;
    int  insert_mode;
} ReplEditorInputState;

typedef struct {
    const char *input;
    int         input_capacity;
    int         input_len;
    int         cursor_pos;
    int         edit_line_idx;
    const char *pending_newline;
    int         pending_newline_capacity;
    int         pending_newline_len;
    int         insert_mode;
} ReplEditorInputView;

/* Selection anchor pair: which source-line range is selected. The
 * canonical "no selection" state is anchor=-1, end=-1. */
typedef struct {
    int anchor_idx;
    int end_idx;
} ReplSelectionState;

/* Editor clipboard. Holds a parsed-cmd snapshot plus the parallel
 * per-line text the cmds were copied from. The text sidecar is large
 * (1.88 MB) — moving it off ReplState removes that mass from every
 * ReplRuntimeState snapshot. */
typedef struct {
    GLCmd cmds[MAX_COMMANDS];
    char  lines[MAX_COMMANDS][MAX_LINE_LEN];
    int   cmd_count;
} ReplClipboardState;

typedef struct {
    ReplEditorBuffer     buffer;
    ReplEditorInputState input;
    ReplSelectionState   selection;
    ReplClipboardState   clipboard;
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

/* Editor-input slice API. The view variant returns the input by-value
 * with `edit_line_idx` populated from the document cursor (the
 * canonical edit-line index lives on ReplDocumentState, not on the
 * input slice itself). */
ReplEditorInputView   editor_state_input(void);
ReplEditorInputState *editor_state_input_mut(void);
void                  editor_state_input_reset(void);

/* Selection slice API. Field-level getters/setters mirror the
 * pre-migration editor_state_selection_* surface; the canonical
 * "no selection" state is anchor=-1, end=-1. */
ReplSelectionState  editor_state_selection(void);
ReplSelectionState *editor_state_selection_mut(void);
void                editor_state_selection_clear(void);
int                 editor_state_selection_anchor(void);
int                 editor_state_selection_end_idx(void);
void                editor_state_selection_set(int anchor_idx, int end_idx);

/* Clipboard slice API. */
ReplClipboardState  editor_state_clipboard(void);
ReplClipboardState *editor_state_clipboard_mut(void);
void                editor_state_clipboard_clear(void);
GLCmd              *editor_state_clipboard_cmds_mut(void);
int                 editor_state_clipboard_count(void);
void                editor_state_clipboard_count_set(int cmd_count);

#endif /* EDITOR_STATE_H */
