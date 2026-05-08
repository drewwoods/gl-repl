/*
 * ui_editor.h -- Per-frame editor-overlay snapshots pushed by the controller.
 *
 * The controller refills these lists each frame after flatten so UI
 * renderers can draw inline affordances (swatches, sliders), highlights,
 * and virtual annotation lines without walking the document themselves.
 *
 * Currently defines the transformer family (color picker / numeric
 * slider). Steps 5 and 6 of the editor-owns-text redesign will add the
 * sibling highlight and virtual-line families to this header.
 */
#ifndef UI_EDITOR_H
#define UI_EDITOR_H

typedef enum {
    TRANSFORMER_COLOR_PICKER,
    TRANSFORMER_NUMERIC_SLIDER
} TransformerKind;

typedef struct {
    int             line_idx;
    int             char_start;
    int             char_end;
    TransformerKind kind;
    union {
        struct {
            float r;
            float g;
            float b;
            float a;
            int   has_alpha;
            int   is_clear;
        } color;
        struct {
            float min;
            float max;
            float current;
            int   is_log;
        } numeric;
    } state;
} EditorTransformer;

#define MAX_TRANSFORMERS 64

typedef struct {
    EditorTransformer items[MAX_TRANSFORMERS];
    int               count;
} EditorTransformerList;

/* Cross-line highlight kinds. The controller pushes one entry per visible
 * highlight each frame; UI render code iterates the list rather than
 * recomputing feeding-cmd / replay PC / search-match positions inline. */
typedef enum {
    HIGHLIGHT_FEEDING_NORMAL,
    HIGHLIGHT_FEEDING_COLOR,
    HIGHLIGHT_REPLAY_PC,
    HIGHLIGHT_SEARCH_MATCH,
    HIGHLIGHT_SELECTION
} HighlightKind;

typedef struct {
    int           line_idx;
    int           char_start;  /* -1 = whole line */
    int           char_end;    /* -1 = whole line */
    HighlightKind kind;
} EditorHighlight;

#define MAX_HIGHLIGHTS 256

typedef struct {
    EditorHighlight items[MAX_HIGHLIGHTS];
    int             count;
} EditorHighlightList;

/* Virtual lines: editor-overlay rows the controller injects below a real
 * source line (e.g. replay annotations). Layout, scroll, and render all
 * consume this list so a row that affects layout has exactly one source
 * of truth. */
typedef enum {
    VIRTUAL_STYLE_REPLAY_SUBST,
    VIRTUAL_STYLE_REPLAY_EVAL
} VirtualLineStyle;

#define MAX_VIRTUAL_LINE_TEXT 256
#define MAX_VIRTUAL_LINE_AUX  128

typedef struct {
    int              after_line_idx;  /* render below this source line */
    VirtualLineStyle style;
    char             text[MAX_VIRTUAL_LINE_TEXT];
    char             aux[MAX_VIRTUAL_LINE_AUX];  /* trailing comment text */
} EditorVirtualLine;

#define MAX_VIRTUAL_LINES 512

typedef struct {
    EditorVirtualLine items[MAX_VIRTUAL_LINES];
    int               count;
} EditorVirtualLineList;

/* Per-line text overrides. The controller may push a replacement
 * text for a source line (e.g., replay's variable-substituted form);
 * editor row-count and render read this slice with a buffer fallback.
 * Sparse — only lines with a real override appear. */
#define MAX_LINE_OVERRIDE_TEXT 256
#define MAX_LINE_OVERRIDES     512

typedef struct {
    int  line_idx;
    char text[MAX_LINE_OVERRIDE_TEXT];
} EditorLineOverride;

typedef struct {
    EditorLineOverride items[MAX_LINE_OVERRIDES];
    int                count;
} EditorLineOverrideList;

#endif /* UI_EDITOR_H */
