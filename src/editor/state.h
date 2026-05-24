#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

#include "limits.h"
#include "repl/command.h"
#include "ui/app/editor.h"  /* UiTransformerList, UiHighlightList,
                         * UiVirtualLineList, UiLineOverrideList
                         * typedefs (live state) */

/* EditorState owns the editable text-document session: canonical line text,
 * the live input row, selection, clipboard, search, autocomplete, per-frame
 * overlay lists, and scroll. REPL state owns the parsed command/program
 * model; UI state owns chrome and pointer/layout state. The editor is the
 * text owner between those two layers.
 *
 * Capture/restore/reset are editor-local for the same reason. Whole-app reset
 * paths still call through one controller-owned entry point, but the editor's
 * slices no longer piggyback on repl_state_capture/restore. */

/* Per-line canonical text. One slot per source command, indexed by
 * source command index. Text is the user-typed form (no trailing ';',
 * no leading whitespace) — the same shape `editor_load_line_to_input` produces
 * after stripping. The typedef lives in editor_state.h because
 * EditorState owns the buffer; src/repl/state_views.h no longer defines or
 * declares anything related to it. */
typedef struct {
    char lines[MAX_COMMANDS][MAX_LINE_LEN];
    int  line_count;
} EditorBuffer;

/* Read-only view over the editor buffer: a const pointer to the
 * lines array plus the active count. Passed by value to REPL
 * consumers that need source text (replay annotations, export,
 * debug dumps, executor display text, flatten reparse helpers,
 * etc.) so they don't reach into editor globals. The view is
 * non-owning and stays valid as long as the editor buffer that
 * produced it. */
typedef struct {
    const char (*lines)[MAX_LINE_LEN];
    int          line_count;
} EditorBufferView;

/* Live editor-input state: the typing buffer, cursor, insert mode,
 * and pending-newline scratch. The edit-line cursor lives on
 * EditorState.document.edit_line_idx and is read via
 * editor_state_edit_line(); it is not duplicated here. */
typedef struct {
    char input[MAX_INPUT_LEN];
    int  input_capacity;
    int  input_len;
    int  cursor_pos;
    /* Character-range selection anchor inside input[]. -1 = no
     * selection. When >= 0, the selection is [lo, hi) with
     * lo = min(anchor_pos, cursor_pos) and hi = max(...). An empty
    * selection (anchor_pos == cursor_pos) is not allowed and collapses
    * to -1; see done/editor-input-selection.md. */
    int  anchor_pos;
    char pending_newline[MAX_INPUT_LEN];
    int  pending_newline_capacity;
    int  pending_newline_len;
    int  insert_mode;
} EditorInputState;

typedef struct {
    const char *input;
    int         input_len;
    int         cursor_pos;
    int         anchor_pos;
    const char *pending_newline;
    int         pending_newline_len;
    int         insert_mode;
} EditorInputView;

/* Selection anchor pair: which source-line range is selected. The
 * canonical "no selection" state is anchor=-1, end=-1. */
typedef struct {
    int anchor_idx;
    int end_idx;
} EditorSelectionState;

/* Editor clipboard. Text-only — paste re-parses each line through the
 * standard commit chain, so the clipboard never holds REPL-shaped
 * data. Same model as file load.
 *
 * Tagged union covering two kinds of payload:
 *   - EDITOR_CLIPBOARD_LINES: source lines (existing line-range copy).
 *     Paste feeds each line through the commit pipeline.
 *   - EDITOR_CLIPBOARD_INPUT_TEXT: a substring lifted from the active
 *     input buffer. Paste inserts text into EditorInputState.input
 *     without going through editor_feed_line.
 * `kind` always matches the populated payload:
 *   EMPTY       -> line_count == 0 && input_text_len == 0
 *   LINES       -> line_count > 0
 *   INPUT_TEXT  -> input_text_len > 0
 * See done/editor-input-selection.md for the full rules. */
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
} EditorClipboardState;

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
} EditorSearchState;

/* Autocomplete model: the live match list, ghost-text suffix the
 * editor would insert on Tab, and parameter hint string. */
typedef struct {
    const char *matches[MAX_AC_MATCHES];
    const char *insert_matches[MAX_AC_MATCHES];
    int         match_count;
    int         selected_idx;
    char        ghost[MAX_LINE_LEN];
    char        hint[MAX_LINE_LEN];
} EditorAutocompleteState;

typedef struct {
    int cursor_visible;
    int blink_tick;
} EditorCursorBlinkState;


/* Editor scroll position: the doc-line index at the top of the
 * code-panel viewport, plus a flag the editor sets to request the
 * scroll follow cursor moves. The render-only chrome bits
 * (panel_frac, resizing_panel, px / py)
 * remain in UiCodePanelRuntimeState which lives on UiState. The
 * split is intentional: editor owns document scroll, UI owns panel
 * chrome and blink state. */
typedef struct {
    int scroll;
    int scroll_follow_cursor;
} EditorScrollState;

/* Document-cursor state: the active edit-line index over the
 * source-text document. Lives editor-side per the conceptual model
 * (editor owns the cursor over the document text; REPL takes the
 * cursor as explicit input to its parse / flatten / compile / load
 * / export work).
 *
 * The editor accessors editor_state_edit_line / _set / _clamp read
 * and write this field directly; REPL pipeline code receives the
 * cursor as an explicit parameter (parse / compile / flatten / load)
 * or routes through the repl_dispatch_edit_line_get / _set
 * host-effects sink (scene save/restore, the load.c NULL-fallback).
 * REPL files do not link `editor_state_*` symbols (β invariant;
 * storage moved here in Phase 4 of plans/done/edit-line-ownership.md). */
typedef struct {
    int edit_line_idx;
} EditorDocumentState;

typedef struct {
    int edit_line_idx;
} EditorDocumentView;

typedef struct {
    EditorBuffer      buffer;
    EditorInputState  input;
    EditorSelectionState    selection;
    EditorClipboardState    clipboard;
    EditorSearchState       search;
    EditorAutocompleteState autocomplete;
    UiTransformerList  transformers;
    UiHighlightList    highlights;
    UiVirtualLineList  virtual_lines;
    UiLineOverrideList line_overrides;
    /* variable_drag lives on the variable-panel peer. Callers use
     * variable_panel_drag / variable_panel_drag_mut /
     * variable_panel_handle_drag_* for that state. */
    EditorScrollState      scroll;
    /* Editor-owned document cursor storage. Kept adjacent to the
     * source buffer and scroll state because editor-side callers
     * move through the text document as one unit. */
    EditorDocumentState    document;
    EditorCursorBlinkState cursor_blink;
} EditorState;

/* Capture / restore / reset symmetry with repl_state_*. */
void editor_state_capture(EditorState *snapshot);
void editor_state_restore(const EditorState *snapshot);
void editor_state_reset(void);

/* Struct-level access to the live buffer. Low-level editor-owned call sites
 * that need the whole struct use `_mut`; most readers and writers should use
 * the slice-level API below. */
const EditorBuffer *editor_state_buffer(void);
EditorBuffer       *editor_state_buffer_mut(void);

/* Slice-level editor_buffer API. This is the preferred public surface for
 * line access and mutation; callers pair it with repl_command_store when a
 * source-command change needs both text and GLCmd updates. */
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

/* Slice-level read accessors that take a view explicitly. Prefer these for
 * pure reads that should not reach through editor globals. */
const char *editor_buffer_view_line(EditorBufferView view, int idx);

/* Editor-input slice API. The view variant returns the input row's
 * own state by value; the document cursor is a separate slice
 * (see EditorDocumentState above) and is read via
 * editor_state_edit_line(). */
EditorInputView   editor_state_input(void);
EditorInputState *editor_state_input_mut(void);
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
/* Atomic "start or extend a selection" primitive. If the anchor is
 * currently inactive and the move would actually move the cursor, pin
 * the pre-move cursor as the new anchor *before* moving, so the
 * resulting selection is [old, new). If the anchor is already active,
 * just keep it (selection grows / shrinks like _keep_anchor). Prefer
 * this over `_anchor_set + _keep_anchor` for shift-style handlers —
 * `_anchor_set(cursor_pos)` collapses immediately and would defeat
 * the intended pin. */
void        editor_cursor_pos_extend_selection(int new_pos);
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

/* Document-cursor slice API. The canonical edit-line index over the
 * source-text document.
 *
 * These accessors read and write `EditorState.document.edit_line_idx`
 * directly (no forwarder layer). Editor / app / widget code calls
 * these directly. REPL pipeline functions take edit-line as an
 * explicit parameter (parse / compile / flatten / load entry points)
 * or route through the repl_dispatch_edit_line_get / _set
 * host-effects sink in `src/repl/core.h` (scene save/restore, the
 * load.c NULL-fallback). β invariant: REPL files do not call editor
 * accessors (storage moved off `ReplState.document.edit_line_idx` in
 * plans/done/edit-line-ownership.md, Phase 4). */
int                  editor_state_edit_line(void);
void                 editor_state_edit_line_set(int line);
void                 editor_state_edit_line_clamp(void);

EditorDocumentView   editor_state_document(void);
EditorDocumentState *editor_state_document_mut(void);
void                 editor_state_document_reset(void);

/* Selection slice API. The canonical "no selection" state is
 * anchor=-1, end=-1. */
EditorSelectionState  editor_state_selection(void);
EditorSelectionState *editor_state_selection_mut(void);
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
EditorClipboardState  editor_state_clipboard(void);
EditorClipboardState *editor_state_clipboard_mut(void);
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
EditorSearchState  editor_state_search(void);
EditorSearchState *editor_state_search_mut(void);
void             editor_state_search_clear(void);

/* Autocomplete slice API. The slice is editor-owned; production code outside
 * the registered completion provider should clear it via the provider seam
 * (editor_completion_clear), not by calling
 * editor_state_autocomplete_clear directly. */
EditorAutocompleteState  editor_state_autocomplete(void);
EditorAutocompleteState *editor_state_autocomplete_mut(void);
void                   editor_state_autocomplete_clear(void);

/* Per-frame editor overlay snapshot lists. The controller refills
 * these each frame after flatten so UI renderers and input bridges
 * can iterate them without walking the document. */
const UiTransformerList *editor_state_transformers(void);
void                         editor_state_transformers_clear(void);
int                          editor_state_transformers_append(const UiTransformer *transformer);

const UiHighlightList   *editor_state_highlights(void);
void                         editor_state_highlights_clear(void);
int                          editor_state_highlights_append(int line_idx, int char_start,
                                                            int char_end, UiHighlightKind kind);

const UiVirtualLineList *editor_state_virtual_lines(void);
void                         editor_state_virtual_lines_clear(void);
int                          editor_state_virtual_lines_append(int after_line_idx,
                                                               UiVirtualLineStyle style,
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
const UiLineOverrideList *editor_state_line_overrides(void);
void                          editor_state_line_overrides_clear(void);
int                           editor_state_line_overrides_append(int line_idx,
                                                                 const char *text);
const char                   *editor_state_line_override_for(int line_idx);

/* Editor cursor blink slice. */
EditorCursorBlinkState  editor_state_cursor_blink(void);
EditorCursorBlinkState *editor_state_cursor_blink_mut(void);

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
