#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

#include "limits.h"
#include "repl/command.h"
#include "ui/editor.h"  /* EditorTransformerList, EditorHighlightList,
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
 * EditorState owns the buffer; src/repl/state_views.h no longer defines or
 * declares anything related to it. */
typedef struct {
    char lines[MAX_COMMANDS][MAX_LINE_LEN];
    int  line_count;
} ReplEditorBuffer;

/* Read-only view over the editor buffer: a const pointer to the
 * lines array plus the active count. Passed by value to REPL
 * consumers that need source text (replay annotations, export,
 * debug dumps, executor display text, flatten reparse helpers,
 * etc.) so they don't reach into editor globals. The view is
 * non-owning and stays valid as long as the editor buffer that
 * produced it. Phase B (Phase A-revised commit 15) introduces
 * the type; Phase B commits 15-16 thread it through call sites. */
typedef struct {
    const char (*lines)[MAX_LINE_LEN];
    int          line_count;
} EditorBufferView;

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
    /* Character-range selection anchor inside input[]. -1 = no
     * selection. When >= 0, the selection is [lo, hi) with
     * lo = min(anchor_pos, cursor_pos) and hi = max(...). An empty
     * selection (anchor_pos == cursor_pos) is not allowed and collapses
     * to -1; see feature/editor-input-selection.md. */
    int  anchor_pos;
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
    int         anchor_pos;
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

/* Editor clipboard. Text-only — paste re-parses each line through the
 * standard commit chain, so the clipboard never holds REPL-shaped
 * data. Same model as file load.
 *
 * Tagged union covering two kinds of payload:
 *   - EDITOR_CLIPBOARD_LINES: source lines (existing line-range copy).
 *     Paste feeds each line through the commit pipeline.
 *   - EDITOR_CLIPBOARD_INPUT_TEXT: a substring lifted from the active
 *     input buffer. Paste inserts text into ReplEditorInputState.input
 *     without going through feed_line.
 * `kind` always matches the populated payload:
 *   EMPTY       -> line_count == 0 && input_text_len == 0
 *   LINES       -> line_count > 0
 *   INPUT_TEXT  -> input_text_len > 0
 * See feature/editor-input-selection.md for the full rules. */
typedef enum {
    EDITOR_CLIPBOARD_EMPTY = 0,
    EDITOR_CLIPBOARD_LINES,
    EDITOR_CLIPBOARD_INPUT_TEXT,
} EditorClipboardKind;

typedef struct {
    EditorClipboardKind kind;

    char  lines[MAX_COMMANDS][MAX_LINE_LEN];
    int   line_count;

    char  input_text[MAX_INPUT_LEN];
    int   input_text_len;
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
 * the variable name captured at drag-begin, the starting value, the
 * cursor anchor x in window pixels, and whether the controller has
 * already captured the coalesced undo snapshot for this drag. */
typedef struct {
    int   var_idx;
    int   log_mode;
    float start_value;
    int   start_x;
    char  name[16];
    int   undo_snapshot_pushed;
} ReplVariableDragState;

/* Editor scroll position: the doc-line index at the top of the
 * code-panel viewport, plus a flag the editor sets to request the
 * scroll follow cursor moves. The render-only chrome bits
 * (panel_frac, resizing_panel, cursor_visible / blink / px / py)
 * remain in ReplCodePanelRuntimeState which lives on UiState. The
 * split happened in Phase 1 commit 11. */
typedef struct {
    int scroll;
    int scroll_follow_cursor;
} EditorScrollState;

typedef struct {
    ReplEditorBuffer      buffer;
    ReplEditorInputState  input;
    ReplSelectionState    selection;
    ReplClipboardState    clipboard;
    ReplSearchState       search;
    ReplAutocompleteState autocomplete;
    EditorTransformerList  transformers;
    EditorHighlightList    highlights;
    EditorVirtualLineList  virtual_lines;
    EditorLineOverrideList line_overrides;
    /* variable_drag lives on the variable_panel peer (Phase F
     * commit 31). Callers use variable_panel_drag /
     * variable_panel_drag_mut / variable_panel_handle_drag_*. */
    EditorScrollState      scroll;
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
 * Phase B commit 17 mutation primitives (insert / replace / delete /
 * load) extend the same namespace and replace the previous text-aware
 * `repl_command_store_*_with_line[s]` overloads. */
const char *editor_buffer_line(int idx);
void        editor_buffer_set_line(int idx, const char *text);
int         editor_buffer_count(void);
void        editor_buffer_set_count(int count);

/* Mutation primitives. Each operates only on the editor buffer
 * (lines + line_count); command-array mutations stay in
 * repl_command_store. Callers pair the two — `repl_command_store_*`
 * for the GLCmd shift, then `editor_buffer_*` for the parallel text
 * write — typically inside one undo transaction.
 *
 * Semantics:
 *   insert_lines(pos, lines, count)
 *       Shift lines[pos..line_count) to the right by count, then
 *       fill lines[pos..pos+count) from `lines[0..count)` (or "" if
 *       lines is NULL or lines[i] is NULL). line_count grows by
 *       count, clamped to MAX_COMMANDS.
 *   insert_line(pos, line)
 *       Convenience wrapper for inserting a single line.
 *   replace_line(pos, line)
 *       Overwrite the slot at pos with line (or ""). line_count is
 *       extended to (pos+1) if it was below.
 *   delete_range(start, count)
 *       Shift lines[start+count..line_count) left by count. Clears
 *       trailing slots.
 *   load_lines(lines, count)
 *       Bulk replace: clear, then write lines[0..count) (or "" if
 *       lines is NULL or lines[i] is NULL). line_count becomes
 *       count.
 *   clear()
 *       line_count = 0. Trailing slots are not zeroed (next mutation
 *       will overwrite them).
 *
 * Returns 1 on success, 0 on bounds/capacity error. */
int  editor_buffer_insert_lines(int pos, const char *const *lines, int count);
int  editor_buffer_insert_line(int pos, const char *line);
int  editor_buffer_replace_line(int pos, const char *line);
int  editor_buffer_delete_range(int start, int count);
int  editor_buffer_load_lines(const char *const *lines, int count);
void editor_buffer_clear(void);

/* Build a read-only EditorBufferView over the live editor buffer.
 * Cheap (one struct copy of pointer + int). REPL consumers should
 * accept the view as a parameter rather than calling
 * `editor_buffer_*` reads through globals. */
EditorBufferView editor_buffer_view(void);

/* Forward decl for apply boundary. The full type lives in
 * src/repl/compile.h; the editor commit orchestration drives it
 * alongside repl_apply_compiled_change inside one undo
 * transaction. */
struct ReplCompiledChange_s;

/* Apply the editor-text portion of a compiled change. Mutates
 * EditorState text only — does not touch ReplState, status, or
 * undo. Returns 1 on success, 0 on capacity error.
 *
 * The dual of repl_apply_compiled_change(). The editor commit
 * orchestration drives both inside one undo transaction. */
int editor_buffer_apply_compiled_change(const struct ReplCompiledChange_s *change);

/* Slice-level read accessors that take a view explicitly. The view
 * variants are the long-term API; the global-state variants above
 * remain during the migration and will be removed once every reader
 * has converted (Phase B commit 18 closes that down with a hard
 * guard). */
const char *editor_buffer_view_line(EditorBufferView view, int idx);

/* Editor-input slice API. The view variant returns the input by-value
 * with `edit_line_idx` populated from the document cursor (the
 * canonical edit-line index lives on ReplDocumentState, not on the
 * input slice itself). */
ReplEditorInputView   editor_state_input(void);
ReplEditorInputState *editor_state_input_mut(void);
void                  editor_state_input_reset(void);

/* Editor-input convenience getters/setters. Implementations live in
 * editor_state.c since commit 5 (storage moved); the names finally
 * match the namespace they live in (commit 10). */
const char *editor_input_text(void);
char       *editor_input_buffer_mut(void);
int         editor_input_len(void);
void        editor_input_len_set(int input_len);
void        editor_input_set_text(const char *text);
void        editor_input_clear(void);
int         editor_cursor_pos(void);
/* Default cursor move: clears the input-selection anchor. Every
 * existing cursor-set call site uses this and inherits auto-clear,
 * which keeps stale highlights from outliving the move that produced
 * them. */
void        editor_cursor_pos_set(int cursor_pos);
/* Shift-extended cursor move: leaves the anchor in place so the
 * selection grows or shrinks. Reserved for handlers that explicitly
 * want to extend an active selection (shift+arrow, shift+home/end,
 * mouse drag). If the move collapses the selection to empty
 * (anchor == cursor) the anchor still drops to -1 so "has selection"
 * remains a single test. */
void        editor_cursor_pos_set_keep_anchor(int cursor_pos);
/* Input-buffer character-range selection anchor. -1 means "no
 * selection." `_set` clamps to [0, input_len]; if the new value equals
 * the current cursor, the anchor collapses to -1 (empty selections are
 * not allowed). `_clear` is shorthand for `_set(-1)`. */
int         editor_input_anchor(void);
void        editor_input_anchor_set(int pos);
void        editor_input_anchor_clear(void);
int         editor_input_selection_active(void);
int         editor_input_selection_lo(void);   /* -1 if inactive */
int         editor_input_selection_hi(void);   /* -1 if inactive */
int         editor_insert_mode(void);
void        editor_insert_mode_set(int insert_mode);
char       *editor_pending_newline_buffer_mut(void);
int         editor_pending_newline_len(void);
void        editor_pending_newline_len_set(int newline_len);
void        editor_pending_newline_set_text(const char *text);
void        editor_pending_newline_clear(void);

/* Selection slice API. Field-level getters/setters mirror the
 * pre-migration editor_state_selection_* surface; the canonical
 * "no selection" state is anchor=-1, end=-1. */
ReplSelectionState  editor_state_selection(void);
ReplSelectionState *editor_state_selection_mut(void);
void                editor_state_selection_clear(void);
int                 editor_state_selection_anchor(void);
int                 editor_state_selection_end_idx(void);
void                editor_state_selection_set(int anchor_idx, int end_idx);

/* Clipboard slice API.
 *
 * `_clear` resets to EDITOR_CLIPBOARD_EMPTY (line_count = 0,
 * input_text_len = 0). `_count_set(n>0)` sets kind = LINES and clears
 * any stale input_text; `_count_set(0)` resets to EMPTY. The
 * input-text helpers below set kind = INPUT_TEXT and clear the line
 * payload symmetrically. */
ReplClipboardState  editor_state_clipboard(void);
ReplClipboardState *editor_state_clipboard_mut(void);
void                editor_state_clipboard_clear(void);
int                 editor_state_clipboard_count(void);
void                editor_state_clipboard_count_set(int line_count);

/* Input-text clipboard payload. The selected substring is naturally a
 * `[lo, hi)` slice inside `input[]`, not a standalone NUL-terminated
 * string, so the setter takes an explicit length and copies that many
 * bytes before NUL-terminating internally. */
void        editor_clipboard_set_input_text(const char *text, int len);
int         editor_clipboard_has_input_text(void);
const char *editor_clipboard_input_text(void);
int         editor_clipboard_input_text_len(void);

/* Search slice API. _clear restores the slice to the post-init
 * default (no active query, hits invalidated). */
ReplSearchState  editor_state_search(void);
ReplSearchState *editor_state_search_mut(void);
void             editor_state_search_clear(void);

/* Autocomplete slice API. The slice is editor-owned; production code
 * outside the registered EditorCompletionProvider (today
 * repl_autocomplete.c) should clear it via the provider seam
 * (editor_completion_clear), not by calling
 * editor_state_autocomplete_clear directly. */
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
/* Count virtual rows anchored after the given source line. Used by
 * code-panel layout to extend row-height accounting; the editor stays
 * agnostic to which feature pushed the rows. */
int                          editor_state_virtual_lines_count_for(int after_line_idx);

/* Per-line text override slice. Controller pushes overrides each
 * frame for source lines whose displayed text should differ from the
 * buffer text (e.g., replay's expand_args annotations). Editor reads
 * via _for() with a buffer fallback. */
const EditorLineOverrideList *editor_state_line_overrides(void);
void                          editor_state_line_overrides_clear(void);
int                           editor_state_line_overrides_append(int line_idx,
                                                                 const char *text);
const char                   *editor_state_line_override_for(int line_idx);

/* Editor scroll slice. */
EditorScrollState  editor_state_scroll(void);
EditorScrollState *editor_state_scroll_mut(void);
int                editor_scroll(void);
void               editor_scroll_set(int scroll);
int                editor_scroll_follow_cursor(void);
void               editor_scroll_follow_cursor_set(int follow);

/* Line-comment prefix configuration.
 *
 * Generic editor feature: the comment-toggle key reads / writes lines
 * using the configured prefix (e.g., "// " for C-like, "# " for shell).
 * The editor itself has no default — the controller registers a prefix
 * at startup. While unset (NULL or empty), the comment-toggle key is a
 * no-op.
 *
 * The setter takes ownership of nothing; the supplied pointer must
 * stay valid for the lifetime of the editor (use a string literal or
 * static buffer). */
void        editor_set_line_comment_prefix(const char *prefix);
const char *editor_line_comment_prefix(void);

#endif /* EDITOR_STATE_H */
