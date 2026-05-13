/*
 * editor_code_panel_document.h - Code-panel document model and scroll layout.
 *
 * Bridges the flat source command array and the wrapped text layout system.
 * Computes full code-panel geometry: header rows (File/Scene/Config menu bar),
 * user code rows (with line wrapping), footer rows (input/status line), and
 * total panel height. Manages scroll state (cursor position, follow target) and
 * provides hit-testing for mouse interaction.
 *
 * Document layout: CodePanelDocumentLayout contains row counts per section
 * (header, code, footer), per-command row counts (main text + replay annotations),
 * visible line count, cursor position, and follow target. Computed once per
 * frame by editor_code_panel_document_build().
 *
 * Wrapping: Uses CodeLayout (from ui/text_layout.h) to wrap
 * individual command text to panel width with hanging indentation. Wrapping
 * accounts for command indentation (depth-based) and optional break points
 * (commas).
 *
 * Scroll following: editor_code_panel_document_apply_follow_scroll() auto-scrolls
 * the view to keep the cursor/target line visible, used during replay and after
 * mutations to keep user focus centered.
 *
 * Hit-testing: editor_code_panel_document_target_for_doc_line() maps a document
 * line index to row scroll target (for Ctrl+K jump-to-line during replay and
 * keyboard navigation).
 */
#ifndef EDITOR_CODE_PANEL_DOCUMENT_H
#define EDITOR_CODE_PANEL_DOCUMENT_H

#include "config.h"
#include "ui/text_layout.h"

/* Full code-panel document layout. Combines header/footer fixed geometry with
 * per-command wrapping information. cmd_main_rows[] stores the row count for
 * each command's source text (accounting for wrapping). replay_extra_rows[]
 * stores extra annotation rows during replay (variable substitution, evaluation).
 * Computed once per frame and used by the renderer for layout and hit-testing. */
typedef struct {
    int panel_w;
    int text_x;
    int cp_h;
    int visible_lines;
    int header_rows;
    int footer_rows;
    int total_lines;
    int cursor_doc_line;
    int follow_doc_line;
    int cmd_main_rows[MAX_COMMANDS];
    int replay_extra_rows[MAX_COMMANDS];
} CodePanelDocumentLayout;

/* Create a CodeLayout descriptor configured for code-panel indentation.
 * panel_w is the panel pixel width; first_x is the x-coordinate offset for
 * the current command's indentation depth. Reads wrap_at_comma from app state.
 * Returns a CodeLayout ready for code_layout_* queries. */
CodeLayout editor_code_panel_document_text_layout(int panel_w, int first_x);


/* Query the current command indentation depth (character count). Used by layout
 * queries to compute hanging indentation for wrapped lines. */
int  editor_code_panel_document_active_indent_chars(void);

/* Compute visible line count from panel height. Delegates to
 * ui_text_panel_visible_lines_for_height() with the REPL code panel's
 * chrome flags. Returns how many lines fit in a code panel of height cp_h
 * (in pixels). */
int  editor_code_panel_document_visible_lines_for_height(int cp_h);

/* Build the full code-panel document layout. Computes header/footer row counts,
 * per-command wrapping (cmd_main_rows[]), replay annotation rows, and total
 * visible line count. Called once per frame before rendering. layout is filled
 * with complete geometry; panel_w is pixel width, text_x is indentation offset,
 * cp_h is panel height. */
void editor_code_panel_document_build(CodePanelDocumentLayout *layout,
                                    int panel_w, int text_x, int cp_h);

/* Auto-scroll to keep the follow target visible. Computes the scroll offset
 * needed to bring follow_doc_line into view, then applies the offset. Called
 * after mutations or during replay to center the user's focus. */
void editor_code_panel_document_apply_follow_scroll(
    const CodePanelDocumentLayout *layout);

/* Compute scroll target for jump-to-line (Ctrl+K during replay, Ctrl+G goto line).
 * Given doc_line (source command index), outputs the scroll target (row offset
 * from top of panel), whether the jump hits the insert line, and row offset
 * within the wrapped segment. Used by keyboard navigation and replay seek. */
int  editor_code_panel_document_target_for_doc_line(
    int doc_line, const CodePanelDocumentLayout *layout,
    int *out_target, int *out_on_insert_line, int *out_row_offset);

#endif /* EDITOR_CODE_PANEL_DOCUMENT_H */
