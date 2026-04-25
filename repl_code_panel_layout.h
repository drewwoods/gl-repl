/*
 * repl_code_panel_layout.h - Pure text layout for code-panel line wrapping.
 *
 * Stateless algorithms for wrapping source lines to a fixed panel width, with
 * support for hanging indentation and optional break points (commas). Used by the
 * code-panel renderer (ui_panels.c) to compute row heights and character positions,
 * and by tests to validate layout behavior without UI dependencies.
 *
 * Wrapping strategy: Lines longer than the panel width are broken at word
 * boundaries or operator commas, with continuation lines indented by a hanging
 * indent amount (computed from the line structure, e.g., position of first
 * argument in a function call). The hanging indent is capped at max_hang_indent_chars
 * to avoid excessive indentation on deeply nested expressions.
 *
 * Example wrapping:
 *   glVertex3f(1.0, 2.0, 3.0, 4.0, 5.0, 6.0)
 *   (breaks at commas if wrap_at_comma is enabled, hangs subsequent arguments)
 *
 * Layout computation: repl_code_panel_layout_make() creates a CodePanelTextLayout
 * descriptor (panel width, first x-coordinate, character width in pixels, wrapping
 * options). This descriptor is passed to all layout queries, allowing flexible
 * testing and UI adaptation without recomputing the descriptor.
 *
 * Query APIs:
 *   - Row count: how many rows does a line occupy?
 *   - Segment lookup: find the text segment for a specific row.
 *   - Cursor position: which row and column is the cursor on?
 *   - Available space: how many characters fit in remaining horizontal space?
 *   - Wrap break detection: where can a line be safely broken?
 *
 * Iterator: CodePanelWrapIter walks a wrapped line segment by segment (one per row),
 * yielding position, length, and x-coordinate for each segment. Used by the renderer
 * to draw each wrapped line segment at the correct position.
 */
#ifndef REPL_CODE_PANEL_LAYOUT_H
#define REPL_CODE_PANEL_LAYOUT_H

/* Layout tuning constants. DEFAULT_RIGHT_PAD_PX reserves pixels on the right to
 * avoid cramming text to the edge. DEFAULT_MAX_HANG_INDENT_CHARS caps hanging
 * indentation to prevent excessive indentation on deeply nested expressions. */
#define CODE_PANEL_LAYOUT_DEFAULT_RIGHT_PAD_PX 4
#define CODE_PANEL_LAYOUT_DEFAULT_MAX_HANG_INDENT_CHARS 12

/* Layout descriptor for text wrapping. panel_w is the pixel width available;
 * first_x is the starting x-coordinate (for initial indentation); char_w is the
 * pixel width of each character (monospace); wrap_at_comma enables breaking at
 * commas instead of only whitespace; right_pad_px reserves right margin space;
 * max_hang_indent_chars caps hanging indentation. Passed to all layout queries. */
typedef struct {
    int panel_w;
    int first_x;
    int char_w;
    int wrap_at_comma;
    int right_pad_px;
    int max_hang_indent_chars;
} CodePanelTextLayout;

/* Iterator state for walking a wrapped line segment by segment. Initialized by
 * repl_code_panel_wrap_iter_init() and advanced by repl_code_panel_wrap_iter_next().
 * Yields position, length, and x-coordinate for each row. Used by the renderer to
 * draw wrapped line segments at correct positions. */
typedef struct {
    const char     *text;
    int             len;
    CodePanelTextLayout layout;
    int             pos;
    int             x;
    int             cont_x;
    int             done;
} CodePanelWrapIter;

/* Create a layout descriptor for text wrapping. panel_w is pixel width; first_x is
 * starting x-coordinate; char_w is character width (monospace); wrap_at_comma enables
 * breaking at commas. Returns a CodePanelTextLayout ready for layout queries. Used by
 * the renderer and tests to configure wrapping behavior. */
CodePanelTextLayout repl_code_panel_layout_make(int panel_w, int first_x,
                                                int char_w, int wrap_at_comma);

/* Compute available characters from position x in the current line, given the layout
 * and right padding. Used to determine how many characters fit before line wrap.
 * Returns character count (rounded down to character boundaries). */
int  repl_code_panel_available_chars(const CodePanelTextLayout *layout, int x);

/* Compute hanging indentation for a continuation line based on the text structure.
 * Scans text for the first argument/token position (e.g., after '(' in a function
 * call) and returns that position as hanging indent, capped at max_hang_indent_chars.
 * Used to visually align function arguments and array indices on continuation lines. */
int  repl_code_panel_cont_indent_chars(const char *text,
                                       int max_hang_indent_chars);

/* Find a safe break point within a text range. Scans from start position to find
 * the nearest whitespace or comma (if wrap_at_comma enabled) within max_chars.
 * Returns the break position; if no break found, returns max_chars. Used by the
 * wrap iterator to identify where to split a line. */
int  repl_code_panel_find_wrap_break(const char *text, int start,
                                     int max_chars, int len);

/* Initialize a wrap iterator for text. Sets up the iterator to walk the text
 * segmented into rows according to the layout. Call repl_code_panel_wrap_iter_next()
 * repeatedly to yield each row's segment. */
void repl_code_panel_wrap_iter_init(CodePanelWrapIter *it, const char *text,
                                    const CodePanelTextLayout *layout);

/* Advance the wrap iterator to the next row. Outputs the segment's start position,
 * length, and x-coordinate for rendering. Returns 1 if a segment was yielded, 0 if
 * at end of text. Used by the renderer to iterate rows. */
int  repl_code_panel_wrap_iter_next(CodePanelWrapIter *it, int *out_start,
                                    int *out_len, int *out_x);

/* Compute the total number of rows (lines) needed to display text with the given
 * layout (accounting for wrapping). Used by the code-panel document model to
 * compute layout heights. Returns 1 for unwrapped single-line text, >1 for wrapped
 * multi-line text. */
int  repl_code_panel_row_count_for_text(const char *text,
                                        const CodePanelTextLayout *layout);

/* Find the text segment for a specific row within wrapped text. want_row is the
 * target row (0 = first row). Outputs the segment's start position (out_start),
 * length (out_len), and x-coordinate for rendering (out_x). Returns 1 on success,
 * 0 if want_row is out of bounds. Used by hit-testing and rendering. */
int  repl_code_panel_segment_for_row(const char *text,
                                     const CodePanelTextLayout *layout,
                                     int want_row, int *out_start,
                                     int *out_len, int *out_x);

/* Find the row and segment position for a cursor within wrapped text. cursor_pos is
 * the character index into text. Outputs the segment start (out_seg_start), length
 * (out_seg_len), and x-coordinate (out_seg_x) for rendering the cursor. Returns 1 on
 * success, 0 if cursor_pos is out of bounds. Used by the cursor renderer and hit-test. */
int  repl_code_panel_cursor_row_for_text(const char *text,
                                         const CodePanelTextLayout *layout,
                                         int cursor_pos,
                                         int *out_seg_start,
                                         int *out_seg_len,
                                         int *out_seg_x);

#endif /* REPL_CODE_PANEL_LAYOUT_H */
