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

#endif /* UI_EDITOR_H */
