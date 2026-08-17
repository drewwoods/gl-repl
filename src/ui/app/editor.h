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

#include "repl/command.h"   /* MAX_EDITOR_COMMANDS (line-override cap) */

typedef enum {
    TRANSFORMER_COLOR_PICKER
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
    /* During replay, the funcN(...) call site whose expansion is currently
     * executing. CALL_SITE is the immediate caller; ROOT_CALL_SITE is the
     * outermost caller of a nested chain, pushed only when it differs from
     * the immediate one. Both come from the focused flat command's
     * call_src_cmd_idx / root_call_src_cmd_idx provenance. Kept as the overflow
     * fallback when the frame table is unindexed. */
    HIGHLIGHT_REPLAY_CALL_SITE,
    HIGHLIGHT_REPLAY_ROOT_CALL_SITE,
    /* Multi-entry replay call-chain gutter highlight. When the focused replay
     * command has an interned call frame, one entry is pushed per active ancestor
     * frame on the chain (outermost-first). `aux` carries the 24-bit packed RGB
     * ramp colour for that frame's depth. */
    HIGHLIGHT_REPLAY_CALL_CHAIN,
    HIGHLIGHT_SEARCH_MATCH,
    HIGHLIGHT_SELECTION,
    HIGHLIGHT_TUTORIAL_INSERTION,
    /* Push/pop bracket match: cursor-on-glPopMatrix highlights the
     * matching glPushMatrix line, and cursor-on-glPushMatrix highlights
     * the matching glPopMatrix line. Same gutter color either way. Reused
     * verbatim for the glPushAttrib/glPopAttrib and glBegin/glEnd bracket
     * pairs (the attrib per-bit state highlighting is layered on separately
     * via the two ATTRIB kinds below). */
    HIGHLIGHT_MATCHING_PUSH_MATRIX,
    /* Cursor-on-vertex/glutSolid* highlights every modelview transform
     * (glTranslatef/glScalef/glRotatef) currently in scope, accounting
     * for push/pop matrix and glLoadIdentity. Multiple highlights of
     * this kind may be pushed in a single frame. */
    HIGHLIGHT_AFFECTING_TRANSFORM,
    /* Structurally unbalanced bracket command: a glPushMatrix/glBegin
     * with no matching close, or an orphan glPopMatrix/glEnd. Always-on
     * (not cursor-gated); multiple per frame. The REPL tolerates these,
     * but export auto-balances them, so they are flagged in the gutter. */
    HIGHLIGHT_UNBALANCED,
    /* glPushAttrib/glPopAttrib per-bit highlighting (cursor-gated).
     * ATTRIB_STATE marks a whole setter line the cursor's push saves / pop
     * reverts; `aux` carries the mask of canonical bit *indices* (0..9)
     * whose colors mark it (so the gutter marker can band multiple bits).
     * ATTRIB_BIT_TOKEN records one GL_*_BIT token's source char range on the
     * push line; `aux` is a single bit index. Renderers use the line/index
     * membership to rescan their exact display text. */
    HIGHLIGHT_ATTRIB_STATE,
    HIGHLIGHT_ATTRIB_BIT_TOKEN
} UiHighlightKind;

typedef struct {
    int           line_idx;
    int           char_start;  /* -1 = whole line */
    int           char_end;    /* -1 = whole line */
    UiHighlightKind kind;
    /* Kind-specific payload (0 for every kind that predates it): a bit-index
     * mask for HIGHLIGHT_ATTRIB_STATE, a single bit index for
     * HIGHLIGHT_ATTRIB_BIT_TOKEN. */
    int           aux;
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
    VIRTUAL_STYLE_REPLAY_PATH,
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
    /* Index into UiRenderSnapshot.replay_path. -1 when this is not a
     * PATH row. v1 has one snapshot, so PATH rows carry 0. */
    int              path_idx;
} UiVirtualLine;

#define MAX_VIRTUAL_LINES 512

typedef struct {
    UiVirtualLine items[MAX_VIRTUAL_LINES];
    int               count;
} UiVirtualLineList;

/* Per-line text overrides. The controller may push a replacement
 * text for a source line (e.g., replay's variable-substituted form);
 * editor row-count and render read this slice with a buffer fallback.
 * Sparse - only lines with a real override appear. */
#define MAX_LINE_OVERRIDE_TEXT 256
/* Capped at MAX_EDITOR_COMMANDS so the override list can hold one entry per
 * source command - layout and render both index by source line and
 * must never disagree on whether a given line carries an override.
 * (When the cap was 512, busy replays exceeded it and layout fell
 * back to buffer text while render kept computing live, drifting
 * wrap rows / scroll / hit-testing.) */
#define MAX_LINE_OVERRIDES     MAX_EDITOR_COMMANDS

typedef struct {
    int  line_idx;
    char text[MAX_LINE_OVERRIDE_TEXT];
} UiLineOverride;

typedef struct {
    UiLineOverride items[MAX_LINE_OVERRIDES];
    int                count;
} UiLineOverrideList;

#endif /* UI_EDITOR_H */
