#ifndef REPL_CODE_PANEL_DOCUMENT_H
#define REPL_CODE_PANEL_DOCUMENT_H

#include "sample.h"
#include "repl_code_panel_layout.h"

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

CodePanelTextLayout repl_code_panel_document_text_layout(int panel_w,
                                                         int first_x);
void repl_code_panel_document_wrap_iter_init(CodePanelWrapIter *it,
                                             const char *text,
                                             int first_x, int panel_w);
int  repl_code_panel_document_wrap_iter_next(CodePanelWrapIter *it,
                                             int *out_start,
                                             int *out_len,
                                             int *out_x);
int  repl_code_panel_document_row_count_for_text(const char *text,
                                                 int first_x, int panel_w);
int  repl_code_panel_document_segment_for_row(const char *text,
                                              int first_x, int panel_w,
                                              int want_row,
                                              int *out_start,
                                              int *out_len,
                                              int *out_x);
int  repl_code_panel_document_cursor_row_for_text(const char *text,
                                                  int first_x, int panel_w,
                                                  int cursor_pos,
                                                  int *out_seg_start,
                                                  int *out_seg_len,
                                                  int *out_seg_x);

int  repl_code_panel_document_active_indent_chars(void);
int  repl_code_panel_document_visible_lines_for_height(int cp_h);
void repl_code_panel_document_build(CodePanelDocumentLayout *layout,
                                    int panel_w, int text_x, int cp_h);
void repl_code_panel_document_apply_follow_scroll(
    const CodePanelDocumentLayout *layout);
int  repl_code_panel_document_target_for_doc_line(
    int doc_line, const CodePanelDocumentLayout *layout,
    int *out_target, int *out_on_insert_line, int *out_row_offset);

#endif /* REPL_CODE_PANEL_DOCUMENT_H */
