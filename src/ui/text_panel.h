/*
 * text_panel.h - Generic REPL-free text panel contract.
 *
 * Defines the data types and API for a generic text-panel renderer and
 * hit-tester that has no dependency on the REPL pipeline, editor state,
 * or app controller. The REPL-specific adapter (src/ui/repl_code_panel.h,
 * added in Phase 3) builds UiTextPanelSnapshot/Row values from REPL state
 * and delegates to these functions.
 *
 * Constraints (enforced by make check-ui-text-panel-pure, Phase 7):
 *   - This file must not include higher-level REPL/editor headers.
 *   - This file must not mention command-model symbols.
 *
 * Row model:
 *   The adapter ships *logical* rows — one entry per source line, virtual
 *   annotation row, static chrome row, or input row — covering the entire
 *   document. The generic panel owns wrap iteration and visible-row clipping.
 *   The adapter does not pre-clip; it lets the renderer walk the full set.
 *
 * Added in Phase 1 of the editor-demo SRP split (feature/editor-demo.md).
 */
#ifndef UI_TEXT_PANEL_H
#define UI_TEXT_PANEL_H

#include "ui/hit.h"          /* UiHit return type for hit_test */

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
 * 16 comfortably covers real REPL lines (MAX_LINE_LEN == 256); denser lines
 * fall back to the row's class color past the cap.
 */
#define UI_TEXT_PANEL_MAX_COLOR_SEGMENTS 16

typedef struct {
    int              char_start;
    int              char_count;
    UiTextPanelColor color;
    /* EXPERIMENT: when non-zero the renderer draws this span "fake bold"
     * (a solid centre pass plus 4 additive 1px-shifted passes). Temporary
     * hook for tuning weight; remove with the experiment. */
    int              bold;
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
 *   search_row_idx   - Row index in the editor search row space, or -1
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
 * (e.g. editor demo with no grammar to suggest from). */
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

/* Search highlight state. hit_row is in the editor search row space
 * (see editor_search_row_for_cmd_index); compared against each row's
 * search_row_idx field when drawing match highlights. */
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
 * the supplied UI_TEXT_PANEL_CHROME_* bitmask. Pure geometry — does not read
 * any global state. */
int  ui_text_panel_visible_lines_for_height(int panel_h, int chrome_flags);

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

#endif /* UI_TEXT_PANEL_H */
