/*
 * text_layout.h - Pure text layout for code-panel line wrapping.
 *
 * Stateless algorithms for wrapping source lines to a fixed panel width, with
 * support for hanging indentation and optional break points (commas). Used by
 * code-panel render/layout code and by tests that validate wrapping behavior
 * without any UI or controller dependency.
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
 * Layout computation: code_layout_make() creates a CodeLayout descriptor
 * (panel width, first x-coordinate, character width in pixels, wrapping
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
 * Iterator: CodeWrapIter walks a wrapped line segment by segment (one per row),
 * yielding position, length, and x-coordinate for each segment. Used by the renderer
 * to draw each wrapped line segment at the correct position.
 *
 * This file has no dependency on REPL, editor, or app headers. It was split out
 * of the older editor-local header during the editor-demo refactor.
 *
 * Naming note: all public types and functions use the code_layout_* / CodeLayout
 * prefix rather than text_layout_* to minimise caller churn from the file rename.
 * A follow-up refactor can unify the names independently.
 */
#ifndef UI_TEXT_LAYOUT_H
#define UI_TEXT_LAYOUT_H

/* Layout tuning constants. DEFAULT_RIGHT_PAD_PX reserves pixels on the right to
 * avoid cramming text to the edge. DEFAULT_MAX_HANG_INDENT_CHARS caps hanging
 * indentation to prevent excessive indentation on deeply nested expressions. */
#define CODE_LAYOUT_DEFAULT_RIGHT_PAD_PX 4
#define CODE_LAYOUT_DEFAULT_MAX_HANG_INDENT_CHARS 12

/* Layout descriptor for text wrapping. panel_w is the pixel width available;
 * first_x is the starting x-coordinate relative to the panel's left edge (for
 * initial indentation); char_w is the
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
} CodeLayout;

/* Iterator state for walking a wrapped line segment by segment. Initialized by
 * code_layout_wrap_iter_init() and advanced by code_layout_wrap_iter_next().
 * Yields position, length, and x-coordinate for each row. Used by the renderer to
 * draw wrapped line segments at correct positions. */
typedef struct {
    const char     *text;
    int             len;
    CodeLayout layout;
    int             pos;
    int             x;
    int             cont_x;
    int             done;
} CodeWrapIter;

/* Create a layout descriptor for text wrapping. panel_w is pixel width; first_x is
 * the starting x-coordinate relative to the panel's left edge; char_w is character
 * width (monospace); wrap_at_comma enables breaking at commas. Returns a CodeLayout
 * ready for layout queries. Used by the renderer and tests to configure wrapping
 * behavior. */
CodeLayout code_layout_make(int panel_w, int first_x,
                            int char_w, int wrap_at_comma);

/* Compute available characters from position x in the current line, given the layout
 * and right padding. Used to determine how many characters fit before line wrap.
 * Returns character count (rounded down to character boundaries). */
int  code_layout_available_chars(const CodeLayout *layout, int x);

/* Compute hanging indentation for a continuation line based on the text structure.
 * Scans text for the first argument/token position (e.g., after '(' in a function
 * call) and returns that position as hanging indent, capped at max_hang_indent_chars.
 * Used to visually align function arguments and array indices on continuation lines. */
int  code_layout_cont_indent_chars(const char *text,
                                   int max_hang_indent_chars);

/* Find a safe break point within a text range. Scans from start position to find
 * the nearest whitespace or comma (if wrap_at_comma enabled) within max_chars.
 * Returns the break position; if no break found, returns max_chars. Used by the
 * wrap iterator to identify where to split a line. */
int  code_layout_find_wrap_break(const char *text, int start,
                                 int max_chars, int len);

/* Initialize a wrap iterator for text. Sets up the iterator to walk the text
 * segmented into rows according to the layout. Call code_layout_wrap_iter_next()
 * repeatedly to yield each row's segment. */
void code_layout_wrap_iter_init(CodeWrapIter *it, const char *text,
                                const CodeLayout *layout);

/* Advance the wrap iterator to the next segment. Returns 1 if a segment was
 * yielded (out_start/out_len/out_x are set), 0 if the iterator is exhausted.
 * Used by the renderer to iterate wrapped line segments. */
int  code_layout_wrap_iter_next(CodeWrapIter *it, int *out_start,
                                int *out_len, int *out_x);

/* Compute the total number of rows (lines) needed to display text with the given
 * layout (accounting for wrapping). Used by the code-panel document model to
 * compute layout heights. Returns 1 for unwrapped single-line text, >1 for wrapped
 * multi-line text. */
int  code_layout_row_count_for_text(const char *text,
                                    const CodeLayout *layout);

/* Find the text segment for a specific row within wrapped text. want_row is the
 * target row (0 = first row). Outputs the segment's start position (out_start),
 * length (out_len), and x-coordinate for rendering (out_x). Returns 1 on success,
 * 0 if want_row is out of bounds. Used by hit-testing and rendering. */
int  code_layout_segment_for_row(const char *text,
                                 const CodeLayout *layout,
                                 int want_row, int *out_start,
                                 int *out_len, int *out_x);

/* Find the row and segment position for a cursor within wrapped text. cursor_pos is
 * the character index into text. Outputs the segment start (out_seg_start), length
 * (out_seg_len), and x-coordinate (out_seg_x) for rendering the cursor. Returns 1 on
 * success, 0 if cursor_pos is out of bounds. Used by the cursor renderer and hit-test. */
int  code_layout_cursor_row_for_text(const char *text,
                                     const CodeLayout *layout,
                                     int cursor_pos,
                                     int *out_seg_start,
                                     int *out_seg_len,
                                     int *out_seg_x);

#endif /* UI_TEXT_LAYOUT_H */
