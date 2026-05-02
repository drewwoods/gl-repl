#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

#include "sample.h"     /* MAX_COMMANDS, MAX_LINE_LEN */
#include "ui_editor.h"  /* EditorTransformerList, EditorHighlightList,
                         * EditorVirtualLineList typedefs (live state) */

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

/* Search session state: the find-text query plus the current match
 * position the navigation has resolved to. */
typedef struct {
    int  active;
    char query[MAX_INPUT_LEN];
    int  query_len;
    int  cursor_pos;
    int  hit_line_idx;
    int  hit_char_idx;
    int  hit_ordinal;
    int  match_count;
} ReplSearchState;

/* Autocomplete model: the live match list, ghost-text suffix the
 * editor would insert on Tab, and parameter hint string. */
typedef struct {
    const char *matches[MAX_AC_MATCHES];
    const char *insert_matches[MAX_AC_MATCHES];
    int         match_count;
    int         selected_idx;
    char        ghost[MAX_LINE_LEN];
    char        hint[MAX_LINE_LEN];
} ReplAutocompleteState;

/* Variable slider drag transaction: which variable is being dragged,
 * the starting value, and the cursor anchor x in window pixels. */
typedef struct {
    int   var_idx;
    int   log_mode;
    float start_value;
    int   start_x;
} ReplVariableDragState;

typedef struct {
    ReplEditorBuffer      buffer;
    ReplEditorInputState  input;
    ReplSelectionState    selection;
    ReplClipboardState    clipboard;
    ReplSearchState       search;
    ReplAutocompleteState autocomplete;
    EditorTransformerList transformers;
    EditorHighlightList   highlights;
    EditorVirtualLineList virtual_lines;
    ReplVariableDragState variable_drag;
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

/* Search slice API. _clear restores the slice to the post-init
 * default (no active query, hits invalidated). */
ReplSearchState  editor_state_search(void);
ReplSearchState *editor_state_search_mut(void);
void             editor_state_search_clear(void);

/* Autocomplete slice API. */
ReplAutocompleteState  editor_state_autocomplete(void);
ReplAutocompleteState *editor_state_autocomplete_mut(void);
void                   editor_state_autocomplete_clear(void);

/* Per-frame editor overlay snapshot lists. The controller refills
 * these each frame after flatten so UI renderers and input bridges
 * can iterate them without walking the document. */
const EditorTransformerList *editor_state_transformers(void);
void                         editor_state_transformers_clear(void);
int                          editor_state_transformers_append(const EditorTransformer *transformer);

const EditorHighlightList   *editor_state_highlights(void);
void                         editor_state_highlights_clear(void);
int                          editor_state_highlights_append(int line_idx, int char_start,
                                                            int char_end, HighlightKind kind);

const EditorVirtualLineList *editor_state_virtual_lines(void);
void                         editor_state_virtual_lines_clear(void);
int                          editor_state_virtual_lines_append(int after_line_idx,
                                                               VirtualLineStyle style,
                                                               const char *text,
                                                               const char *aux);

/* Variable slider drag transaction. */
ReplVariableDragState  editor_state_variable_drag(void);
ReplVariableDragState *editor_state_variable_drag_mut(void);
void                   editor_state_variable_drag_reset(void);

#endif /* EDITOR_STATE_H */
