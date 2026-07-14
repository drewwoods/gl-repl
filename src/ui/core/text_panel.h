/*
 * text_panel.h - Generic wrapped text-panel renderer and hit-test contract.
 *
 * Defines the row model and immutable snapshot the generic text-panel renderer
 * consumes. It has no dependency on the REPL pipeline, editor session, or app
 * controller; feature-specific adapters such as `src/ui/app/repl_code_panel.h`
 * translate their source state into these rows and then delegate to this API.
 *
 * The purity contract is enforced by `check-ui-text-panel-pure`: this header and
 * implementation stay free of higher-level REPL/editor symbols. The file was
 * introduced during the editor-demo split, but its day-to-day purpose is simply
 * “generic text panel, adapter-owned content”.
 */
#ifndef UI_TEXT_PANEL_H
#define UI_TEXT_PANEL_H

#include "ui/core/hit.h"          /* UiHit return type for hit_test */

#define UI_TEXT_PANEL_RIGHT_ACTION_W 12

/* -------------------------------------------------------------------------
 * Colors
 * ---------------------------------------------------------------------- */

/* RGBA color for a text panel element. has_alpha controls whether the alpha
 * channel is applied when rendering; when set, the renderer enables blending
 * for the affected text draw so sub-opaque rows can fade correctly. */
typedef struct {
    float r, g, b, a;
    int   has_alpha;
} UiTextPanelColor;

/* -------------------------------------------------------------------------
 * Row kinds
 * ---------------------------------------------------------------------- */

typedef enum {
    /* Workspace header, render-state, camera, header_pre/post, footer
     * scaffolding — chrome the adapter never edits. */
    UI_TEXT_PANEL_ROW_STATIC = 0,

    /* A committed source row (one per document command in the REPL
     * adapter). */
    UI_TEXT_PANEL_ROW_TEXT,

    /* The active edit row — the renderer draws cursor, selection, and
     * autocomplete ghost/hint here. row->text is ignored; the live input
     * comes from UiTextPanelSnapshot.input. */
    UI_TEXT_PANEL_ROW_INPUT,

    /* Scroll-position-only row (e.g. blank insert-mode preview). Empty-text
     * placeholders render as indentation-only slots; non-empty placeholders
     * fall through to normal wrapped text rendering. */
    UI_TEXT_PANEL_ROW_PLACEHOLDER,

    /* Adapter-supplied row not backed by a source line directly (replay
     * annotations, evaluated-arg previews). ui_text_panel_hit_test()
     * leaves line_idx unresolved for VIRTUAL rows; the adapter rewrites
     * those hits to hit_target_line_idx before controller routing. -1 means
     * the row is not hit-testable (demo-only rows). */
    UI_TEXT_PANEL_ROW_VIRTUAL,
} UiTextPanelRowKind;

/* -------------------------------------------------------------------------
 * Right-edge decoration
 * ---------------------------------------------------------------------- */

/* Right-edge decoration drawn at the trailing end of a logical row.
 * When active the renderer draws a filled color rectangle at the row's
 * right margin. The generic panel treats the color data as opaque display
 * values and does not assign hit semantics to this region. Adapters that
 * use the decoration as an interactive affordance (for example an inline
 * color swatch) own that region test and may rewrite the result to a
 * feature-specific UiHitKind. */
typedef struct {
    int              active;      /* non-zero if a decoration is present */
    UiTextPanelColor color;       /* decoration fill color */
    int              emphasized;  /* non-zero to draw an extra bright outer
                                   * outline. Adapters set this when the
                                   * right-edge decoration is the active
                                   * interactive target (for example the
                                   * swatch bound to a live color picker). */
} UiTextPanelRightAction;

/* Optional per-character color overrides for a row. The adapter may fill a
 * small ordered list of spans when a row needs color variation across its
 * text (for example tutorial fade or per-kind argument syntax coloring).
 * Any gaps fall back to UiTextPanelRow.color, and the renderer clamps an
 * over-long list to this cap, so callers may emit more spans than fit and
 * the surplus simply degrades to UiTextPanelRow.color (no breakage).
 *
 * 64 covers dense expression rows within MAX_LINE_LEN == 256, including the
 * built-in wave-surface normal math. Truly pathological rows still fall back
 * to the row's class color past the cap.
 */
#define UI_TEXT_PANEL_MAX_COLOR_SEGMENTS 64

/* Drop-shadow opacity for syntax-highlight "On+Shadow" mode: the white
 * shadow copy is drawn at this fraction of the span's own alpha (so faded
 * spans keep their shadow in step). Bump it to make the halo more visible.
 * Used by text_panel_draw_colored_span. */
#define UI_TEXT_PANEL_SHADOW_ALPHA 0.35f

typedef struct {
    int              char_start;
    int              char_count;
    UiTextPanelColor color;
    /* When non-zero the renderer draws this span with a drop shadow: a
     * dark copy offset by (+1, -1) behind the glyphs, then the colored
     * text on top. Driven by the "On + Shadow" syntax-highlight config
     * (GLR_CONFIG_SYNTAX_HIGHLIGHT). */
    int              shadow;
} UiTextPanelColorSegment;

/* -------------------------------------------------------------------------
 * Row descriptor
 * ---------------------------------------------------------------------- */

/* Logical row descriptor. The adapter fills one UiTextPanelRow per source
 * line / static chrome line / virtual annotation / input row and passes the
 * full array to UiTextPanelSnapshot. The generic renderer iterates the array
 * and performs wrap iteration itself — it does not assume one logical row
 * maps to one visual row.
 *
 * Field semantics:
 *   text             - NUL-terminated display text (caller-owned, valid for
 *                      the render/hit-test call duration). May be "" but not
 *                      NULL. INPUT rows ignore this field and render from
 *                      UiTextPanelSnapshot.input.input instead.
 *   kind             - Row classification; drives cursor/generic-hit logic.
 *   left_gutter_label - Line number shown in the leftmost numeric gutter
 *                      column. 0 means no label (static/virtual rows).
 *   left_aux_label   - Optional short string drawn at the idx_x column
 *                      (e.g. "v3", "vn"). Empty string means no label.
 *   right_action     - Right-edge decoration. active==0 means no
 *                      decoration on this row. Any semantic routing for
 *                      this region is adapter-owned.
 *   source_line_idx  - Index into the source document (-1 for rows not
 *                      directly backed by a source command).
 *   hit_target_line_idx - For VIRTUAL rows: the source line index the
 *                      adapter should route clicks to. The generic
 *                      hit-tester leaves line_idx unresolved for these
 *                      rows and the adapter rewrites it before returning
 *                      the hit to callers. -1 for non-hit-testable
 *                      virtual rows.
 *   search_row_idx   - Row index in the adapter search row space, or -1
 *                      for rows outside the search model (static chrome,
 *                      non-searchable virtual rows). Compared with
 *                      UiTextPanelSearch.hit_row for match highlighting.
 *   indent_chars     - Leading indentation in character widths; the renderer
 *                      adds this as additional first_x offset.
 *   background_color - Optional full-row background fill, drawn behind text
 *                      and search highlights (used by adapters such as the
 *                      REPL code panel for replay/selection bands).
 *   background_active - Non-zero to enable the background fill; adapters
 *                      set this when background_color should be drawn.
 *   left_marker_color - Optional narrow accent strip on the panel's left
 *                      edge, repeated on every wrap row (for example the
 *                      REPL adapter's replay/feeding markers).
 *   left_marker_active - Non-zero to enable the left marker; adapters set
 *                      this when left_marker_color should be drawn.
 *   color            - Text fill color for this row.
 *   color_segments   - Optional ordered per-character color spans. When
 *                      color_segment_count == 0 the renderer uses color for
 *                      the whole row. Gaps fall back to color.
 *   hit_eligible     - Non-zero if mouse clicks on this row should generate
 *                      a code-panel hit (UI_HIT_CODE_TEXT etc.). Static
 *                      chrome rows typically set this to 0.
 */
typedef struct {
    const char           *text;
    UiTextPanelRowKind    kind;
    int                   left_gutter_label;
    char                  left_aux_label[8];
    UiTextPanelRightAction right_action;
    int                   source_line_idx;
    int                   hit_target_line_idx;
    int                   search_row_idx;
    int                   indent_chars;
    UiTextPanelColor      background_color;
    int                   background_active;
    UiTextPanelColor      left_marker_color;
    int                   left_marker_active;
    UiTextPanelColor      color;
    UiTextPanelColorSegment color_segments[UI_TEXT_PANEL_MAX_COLOR_SEGMENTS];
    int                   color_segment_count;
    int                   hit_eligible;
} UiTextPanelRow;

/* -------------------------------------------------------------------------
 * Input state (active edit row)
 * ---------------------------------------------------------------------- */

/* Input buffer state for the active edit row. ghost and hint are
 * pre-resolved NUL-terminated strings from the autocomplete subsystem —
 * the text panel treats them as opaque display strings and does not
 * interpret their grammar. Pass empty strings ("") when not needed
 * (e.g. standalone demo with no grammar to suggest from). */
typedef struct {
    const char *input;          /* input buffer text (NUL-terminated) */
    int         input_len;      /* cached strlen(input) */
    int         cursor;         /* cursor char index within input */
    int         anchor;         /* selection anchor (== cursor when no sel) */
    const char *ghost;          /* autocomplete ghost suffix (may be "") */
    const char *hint;           /* parameter hint string (may be "") */
    int         cursor_visible; /* non-zero while the cursor blink is on */
} UiTextPanelInput;

/* -------------------------------------------------------------------------
 * Search state
 * ---------------------------------------------------------------------- */

/* Search highlight state. hit_row is in the adapter-defined search row space;
 * compared against each row's search_row_idx field when drawing match highlights. */
typedef struct {
    int         active;         /* non-zero when search overlay is visible */
    const char *query;          /* search query string (NUL-terminated) */
    int         query_len;      /* cached strlen(query) */
    int         hit_row;        /* search row index of the current match */
    int         hit_char;       /* char index of match start within that row */
} UiTextPanelSearch;

/* -------------------------------------------------------------------------
 * Snapshot (immutable per-frame input to the renderer)
 * ---------------------------------------------------------------------- */

/* Chrome visibility flags — OR-combination of UI_TEXT_PANEL_CHROME_* bits. */
#define UI_TEXT_PANEL_CHROME_STATUSBAR  (1 << 0)  /* draw statusbar slot */
#define UI_TEXT_PANEL_CHROME_SCROLLBAR  (1 << 1)  /* draw scrollbar */
#define UI_TEXT_PANEL_CHROME_LINE_NUMS  (1 << 2)  /* draw line-number gutter */
#define UI_TEXT_PANEL_CHROME_AUX_COL    (1 << 3)  /* draw left aux-label column */

/* Per-frame snapshot passed to ui_text_panel_render() and
 * ui_text_panel_hit_test(). The adapter builds this once per frame from live
 * state; the generic renderer reads it and does not call any state getters.
 *
 * rows is caller-owned storage valid for the duration of the render/hit-test
 * call. Do not put a fixed UI_TEXT_PANEL_ROW_CAP here; the adapter sizes
 * its own array from live counts.
 *
 * Viewport and panel rect use OpenGL bottom-left coordinates.
 */
typedef struct {
    /* Window / viewport geometry */
    int vp_w;           /* viewport width  (pixels) */
    int vp_h;           /* viewport height (pixels) */

    /* Code-panel rectangle (bottom-left origin) */
    int cp_x;
    int cp_y;
    int cp_w;
    int cp_h;

    /* Text column x-offset (distance from cp_x to where text starts,
     * accounts for gutter width). */
    int text_x;

    /* Wrapping policy for code_layout_make(). The renderer combines this
     * with panel geometry and each row's indent_chars to build per-row
     * CodeLayout values without consulting app state. */
    int wrap_at_comma;

    /* Display-only comment-rule ligature (1 = on). On lines whose first
     * non-whitespace token is "//", the renderer draws any '-' in a run of
     * two or more consecutive '-' using the full-width horizontal-bar glyph
     * (0x12). A lone '-' stays a hyphen, and non-comment lines (including
     * code with a trailing // comment) are left alone. This is a pure 1:1
     * glyph swap at draw time — the underlying row / input text is unchanged,
     * so every column, cursor index, wrap point, and hit-test position is
     * identical to the plain hyphen text and saved files keep the '-'
     * characters. */
    int comment_rule_ligature;

    /* Active-input-row paren aids (1 = on). paren_match tints the
     * caret's matching bracket — '(' / ')' or '{' / '}'; paren_scope
     * draws a faint background band behind the text inside the caret's
     * enclosing '(' / ')' pair (glyph colors untouched). Independent
     * toggles. */
    int paren_match;
    int paren_scope;

    /* Generic top-chrome reserve (pixels) the adapter draws above the
     * code rows, e.g. a scene tab strip. Subtracted from the render
     * origin, hit-test origin, both scrollbar expressions, and the
     * visible-row count so rows/scrollbar/scroll math clear it. Stays
     * REPL-free: just an int (the adapter computes the value). */
    int top_chrome_h;

    /* Height of the status bar along the bottom of the text panel.
     * Replaces the former global statusbar metrics dependency. */
    int statusbar_h;

    /* Logical row array — one entry per source / chrome / virtual / input
     * line. The renderer iterates all rows and wraps each one internally. */
    const UiTextPanelRow *rows;
    int                   row_count;

    /* Scroll position: absolute visual-row index at the top of the viewport. */
    int scroll;

    /* Chrome visibility flags (UI_TEXT_PANEL_CHROME_* bitmask). */
    int chrome_flags;

    /* Input state for the active edit row. */
    UiTextPanelInput  input;

    /* Search state. */
    UiTextPanelSearch search;
} UiTextPanelSnapshot;

/* -------------------------------------------------------------------------
 * Output (render-discovered state returned to the adapter)
 * ---------------------------------------------------------------------- */

/* Simple integer rectangle (bottom-left origin, OpenGL coordinates). */
typedef struct {
    int x, y, w, h;
} UiTextPanelRect;

/* Render-discovered state written by ui_text_panel_render(). The adapter
 * may use these to overlay REPL-specific chrome (status strip, etc.) without
 * recomputing layout.
 *
 *   cursor_px / cursor_py  - pixel position of the text cursor (bottom-left).
 *   cursor_valid           - non-zero if cursor coordinates are meaningful.
 *   total_rows             - total wrapped visual row count across all logical
 *                            rows (used to size the scrollbar).
 *   visible_rows           - how many visual rows fit in the panel height.
 *   text_area              - pixel rect of the text content area (excludes
 *                            gutters and statusbar slot).
 *   statusbar_slot         - pixel rect reserved for the statusbar strip at
 *                            the bottom of the panel (zero-size when
 *                            UI_TEXT_PANEL_CHROME_STATUSBAR is not set).
 */
typedef struct {
    int            cursor_px;
    int            cursor_py;
    int            cursor_valid;
    int            total_rows;
    int            visible_rows;
    UiTextPanelRect text_area;
    UiTextPanelRect statusbar_slot;
} UiTextPanelOutput;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/* Compute how many visual rows fit in a panel of height panel_h pixels for
 * the supplied statusbar height. Pure geometry — does not read
 * any global state. */
int  ui_text_panel_visible_lines_for_height(int panel_h, int statusbar_h,
                                             int top_chrome_h);

/* Return the pixel baseline y-coordinate of the logical row at
 * input_row_idx (index into snap->rows). Accounts for wrap, scroll,
 * top chrome. Writes *out_py and returns 1 on success; returns 0 if
 * the row is scrolled off-screen or the index is invalid. Pure. */
int  ui_text_panel_input_row_y(const UiTextPanelSnapshot *snap,
                                int input_row_idx,
                                int *out_py);

/* Render the text panel using the provided snapshot. Writes render-discovered
 * state into *out (must not be NULL). The snapshot and all pointer fields it
 * contains must remain valid for the duration of the call. */
void ui_text_panel_render(const UiTextPanelSnapshot *snap,
                          UiTextPanelOutput         *out);

/* Hit-test the text panel at window coordinates (mx, my). Returns a UiHit
 * describing the region struck. Only generic text-panel hit kinds are
 * returned: UI_HIT_CODE_TEXT, UI_HIT_CODE_INSERT_LINE,
 * UI_HIT_CODE_GUTTER, UI_HIT_PANEL_DIVIDER, UI_HIT_NONE. Adapters that
 * expose feature-specific right-edge actions own that routing and may
 * rewrite to UI_HIT_INLINE_COLOR_SWATCH or another feature-specific kind.
 * For UI_TEXT_PANEL_ROW_VIRTUAL rows, the generic hit-test may leave
 * line_idx = -1; the adapter is responsible for rewriting it to the
 * owning source line before controller routing. */
UiHit ui_text_panel_hit_test(const UiTextPanelSnapshot *snap,
                              int mx, int my);

int  ui_text_panel_right_action_rect(const UiTextPanelSnapshot *snap,
                                    int line_y,
                                    int *out_sx, int *out_sy, int *out_sw);

/* Bracket matching for the active input row. If the character "in
 * front of" the caret (s[cursor], i.e. the one immediately to the right
 * of the insertion point) is a '(' / ')' or '{' / '}', writes that
 * character's index to *self and its balanced partner's index to *match,
 * then returns 1. The two bracket kinds are matched independently (depth
 * counts only the active pair's own brackets). Returns 0 when s[cursor]
 * is not a bracket or the pair is unbalanced (no partner within
 * [0, len)). Pure — no GL, no global state. The renderer uses it to tint
 * the matching pair on the active row. */
int  ui_text_panel_match_paren(const char *s, int len, int cursor,
                               int *self, int *match);

/* Multi-row curly-brace matching. Given the caret's bracket at
 * (self_row, self_char) — where the row's text holds a '{' or '}' at
 * self_char — scans the editable rows (UI_TEXT_PANEL_ROW_TEXT /
 * _ROW_INPUT, skipping chrome / virtual rows) in document order: forward
 * for '{', backward for '}', counting brace depth. On the balancing
 * bracket writes its row index to *match_row and char index to
 * *match_char and returns 1. Returns 0 when the caret is not on a curly
 * brace ('(' / ')' are single-row — use ui_text_panel_match_paren) or the
 * brace is unbalanced across the document. Pure — reads only the snapshot
 * rows + input text, no GL or global state. Lets the renderer tint the
 * partner brace of an if / for block on its own line. */
int  ui_text_panel_match_bracket_multiline(const UiTextPanelSnapshot *snap,
                                           int self_row, int self_char,
                                           int *match_row, int *match_char);

/* Innermost parenthesis pair *enclosing* the caret. Scans left from
 * `cursor` for the nearest unbalanced '(' then forward for its partner.
 * On success writes the open/close indices to *open / *close (the pair's
 * own parens are part of the enclosed span) and returns 1. Returns 0 when
 * the caret is not inside any pair, or the enclosing '(' has no close.
 * Pure — no GL, no global state. The renderer draws a faint background
 * band behind [open, close] so the active paren scope stands out. */
int  ui_text_panel_enclosing_parens(const char *s, int len, int cursor,
                                     int *open, int *close);

#endif /* UI_TEXT_PANEL_H */
