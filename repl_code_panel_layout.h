#ifndef REPL_CODE_PANEL_LAYOUT_H
#define REPL_CODE_PANEL_LAYOUT_H

#define CODE_PANEL_LAYOUT_DEFAULT_RIGHT_PAD_PX 4
#define CODE_PANEL_LAYOUT_DEFAULT_MAX_HANG_INDENT_CHARS 12

typedef struct {
    int panel_w;
    int first_x;
    int char_w;
    int wrap_at_comma;
    int right_pad_px;
    int max_hang_indent_chars;
} CodePanelTextLayout;

typedef struct {
    const char     *text;
    int             len;
    CodePanelTextLayout layout;
    int             pos;
    int             x;
    int             cont_x;
    int             done;
} CodePanelWrapIter;

CodePanelTextLayout repl_code_panel_layout_make(int panel_w, int first_x,
                                                int char_w, int wrap_at_comma);

int  repl_code_panel_available_chars(const CodePanelTextLayout *layout, int x);
int  repl_code_panel_cont_indent_chars(const char *text,
                                       int max_hang_indent_chars);

int  repl_code_panel_find_wrap_break(const char *text, int start,
                                     int max_chars, int len);

void repl_code_panel_wrap_iter_init(CodePanelWrapIter *it, const char *text,
                                    const CodePanelTextLayout *layout);
int  repl_code_panel_wrap_iter_next(CodePanelWrapIter *it, int *out_start,
                                    int *out_len, int *out_x);

int  repl_code_panel_row_count_for_text(const char *text,
                                        const CodePanelTextLayout *layout);
int  repl_code_panel_segment_for_row(const char *text,
                                     const CodePanelTextLayout *layout,
                                     int want_row, int *out_start,
                                     int *out_len, int *out_x);
int  repl_code_panel_cursor_row_for_text(const char *text,
                                         const CodePanelTextLayout *layout,
                                         int cursor_pos,
                                         int *out_seg_start,
                                         int *out_seg_len,
                                         int *out_seg_x);

#endif /* REPL_CODE_PANEL_LAYOUT_H */
