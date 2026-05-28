/*
 * ui_editor.h -- Per-frame editor overlay lists pushed by the controller.
 *
 * The controller rebuilds these arrays each frame so UI renderers can draw
 * inline affordances, highlights, virtual rows, and temporary line overrides
 * without re-walking the document or recomputing editor semantics themselves.
 * They are pure snapshot data consumed by `UiRenderSnapshot` and the UI layer.
 */
#ifndef UI_EDITOR_H
#define UI_EDITOR_H

#include "repl/command.h"   /* MAX_COMMANDS (line-override cap) */

typedef enum {
    TRANSFORMER_COLOR_PICKER,
    TRANSFORMER_NUMERIC_SLIDER
} UiTransformerKind;

typedef struct {
    int             line_idx;
    int             char_start;
    int             char_end;
    UiTransformerKind kind;
    union {
        struct {
            float r;
            float g;
            float b;
            float a;
            int   has_alpha;
        } color;
        struct {
            float min;
            float max;
            float current;
            int   is_log;
        } numeric;
    } state;
} UiTransformer;

#define MAX_TRANSFORMERS 64

typedef struct {
    UiTransformer items[MAX_TRANSFORMERS];
    int               count;
} UiTransformerList;

/* Cross-line highlight kinds. The controller pushes one entry per visible
 * highlight each frame; UI render code iterates the list rather than
 * recomputing feeding-cmd / replay PC / search-match positions inline. */
typedef enum {
    HIGHLIGHT_FEEDING_NORMAL,
    HIGHLIGHT_FEEDING_COLOR,
    HIGHLIGHT_REPLAY_PC,
    HIGHLIGHT_SEARCH_MATCH,
    HIGHLIGHT_SELECTION,
    HIGHLIGHT_TUTORIAL_INSERTION,
    /* Cursor-on-glPopMatrix highlights the matching glPushMatrix line. */
    HIGHLIGHT_MATCHING_PUSH_MATRIX,
    /* Cursor-on-vertex/glutSolid* highlights every modelview transform
     * (glTranslatef/glScalef/glRotatef) currently in scope, accounting
     * for push/pop matrix and glLoadIdentity. Multiple highlights of
     * this kind may be pushed in a single frame. */
    HIGHLIGHT_AFFECTING_TRANSFORM
} UiHighlightKind;

typedef struct {
    int           line_idx;
    int           char_start;  /* -1 = whole line */
    int           char_end;    /* -1 = whole line */
    UiHighlightKind kind;
} UiHighlight;

#define MAX_HIGHLIGHTS 256

typedef struct {
    UiHighlight items[MAX_HIGHLIGHTS];
    int             count;
} UiHighlightList;

/* Virtual lines: editor-overlay rows the controller injects below a real
 * source line (e.g. replay annotations). Layout, scroll, and render all
 * consume this list so a row that affects layout has exactly one source
 * of truth. */
typedef enum {
    VIRTUAL_STYLE_REPLAY_SUBST,
    VIRTUAL_STYLE_REPLAY_EVAL
} UiVirtualLineStyle;

#define MAX_VIRTUAL_LINE_TEXT 256
#define MAX_VIRTUAL_LINE_AUX  128

typedef struct {
    int              after_line_idx;  /* render below this source line */
    UiVirtualLineStyle style;
    char             text[MAX_VIRTUAL_LINE_TEXT];
    char             aux[MAX_VIRTUAL_LINE_AUX];  /* trailing comment text */
} UiVirtualLine;

#define MAX_VIRTUAL_LINES 512

typedef struct {
    UiVirtualLine items[MAX_VIRTUAL_LINES];
    int               count;
} UiVirtualLineList;

/* Per-line text overrides. The controller may push a replacement
 * text for a source line (e.g., replay's variable-substituted form);
 * editor row-count and render read this slice with a buffer fallback.
 * Sparse — only lines with a real override appear. */
#define MAX_LINE_OVERRIDE_TEXT 256
/* Capped at MAX_COMMANDS so the override list can hold one entry per
 * source command — layout and render both index by source line and
 * must never disagree on whether a given line carries an override.
 * (When the cap was 512, busy replays exceeded it and layout fell
 * back to buffer text while render kept computing live, drifting
 * wrap rows / scroll / hit-testing.) */
#define MAX_LINE_OVERRIDES     MAX_COMMANDS

typedef struct {
    int  line_idx;
    char text[MAX_LINE_OVERRIDE_TEXT];
} UiLineOverride;

typedef struct {
    UiLineOverride items[MAX_LINE_OVERRIDES];
    int                count;
} UiLineOverrideList;

#endif /* UI_EDITOR_H */
