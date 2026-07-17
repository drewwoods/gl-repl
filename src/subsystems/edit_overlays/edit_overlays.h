#ifndef EDIT_OVERLAYS_H
#define EDIT_OVERLAYS_H

#include "render3d/guides/guides_shared.h"  /* Render3dGuideSnapshot */
#include "render3d/view_mode.h"             /* Render3dViewMode */
#include "repl/state_views.h"            /* FlatProgramView, CursorBlockState */

#define OVERLAY_VERTEX_LABEL_LIST(X) \
    X(OFF, "Off")                    \
    X(INDEX, "Index")                \
    X(INDEX_POS, "Index+Pos")        \
    X(INDEX_WORLD, "Index+World")    \
    X(INDEX_WORLD_FINE, "Index+World Fine")

#define OVERLAY_VERTEX_LABEL_NAME_ENTRY(name, str) [OVERLAY_VERTEX_LABEL_##name] = str,

typedef enum OverlayVertexLabelMode {
#define OVERLAY_VERTEX_LABEL_ENUM_ENTRY(name, str) OVERLAY_VERTEX_LABEL_##name,
    OVERLAY_VERTEX_LABEL_LIST(OVERLAY_VERTEX_LABEL_ENUM_ENTRY)
#undef OVERLAY_VERTEX_LABEL_ENUM_ENTRY
    OVERLAY_VERTEX_LABEL_COUNT
} OverlayVertexLabelMode;

#define OVERLAY_SCOPE_LIST(X) \
    X(FIRST_INSTANCE, "First instance")    \
    X(ALL_INSTANCES, "All instances")      \
    X(AT_VERTEX, "At vertex")              \
    X(VISIBLE, "Visible only")             \
    X(SINGLE_POLYGON, "Single polygon")

#define OVERLAY_SCOPE_NAME_ENTRY(name, str) [OVERLAY_SCOPE_##name] = str,

typedef enum OverlayScope {
#define OVERLAY_SCOPE_ENUM_ENTRY(name, str) OVERLAY_SCOPE_##name,
    OVERLAY_SCOPE_LIST(OVERLAY_SCOPE_ENUM_ENTRY)
#undef OVERLAY_SCOPE_ENUM_ENTRY
    OVERLAY_SCOPE_COUNT
} OverlayScope;

/* Clipped mode re-issues the program's own glClipPlane / clip-cap enables
 * during the outline walk, so the highlight is cut exactly where the user's
 * clip planes cut the shape (see render_outlines_* in edit_overlays.c). */
#define POLY_HIGHLIGHT_LIST(X) \
    X(OFF, "Off")              \
    X(ON, "On")                \
    X(CLIPPED, "Clipped")

#define POLY_HIGHLIGHT_NAME_ENTRY(name, str) [POLY_HIGHLIGHT_##name] = str,

typedef enum PolyHighlightMode {
#define POLY_HIGHLIGHT_ENUM_ENTRY(name, str) POLY_HIGHLIGHT_##name,
    POLY_HIGHLIGHT_LIST(POLY_HIGHLIGHT_ENUM_ENTRY)
#undef POLY_HIGHLIGHT_ENUM_ENTRY
    POLY_HIGHLIGHT_COUNT
} PolyHighlightMode;

#define VERTEX_OUTLINE_STYLE_LIST(X) \
    X(DEFAULT, "Default")            \
    X(BOLD, "Bold")                  \
    X(INVERTED, "Inverted")          \
    X(BOLD_INVERTED, "Bold inverted")

#define VERTEX_OUTLINE_STYLE_NAME_ENTRY(name, str) [VERTEX_OUTLINE_STYLE_##name] = str,

typedef enum VertexOutlineStyle {
#define VERTEX_OUTLINE_STYLE_ENUM_ENTRY(name, str) VERTEX_OUTLINE_STYLE_##name,
    VERTEX_OUTLINE_STYLE_LIST(VERTEX_OUTLINE_STYLE_ENUM_ENTRY)
#undef VERTEX_OUTLINE_STYLE_ENUM_ENTRY
    VERTEX_OUTLINE_STYLE_COUNT
} VertexOutlineStyle;

#define VERTEX_OUTLINE_BOLD_SCALE 2.5f
#define VERTEX_OUTLINE_EDGE_WIDTH 1.2f
#define VERTEX_OUTLINE_TESS_WIDTH 1.5f
#define VERTEX_OUTLINE_ACTIVE_WIDTH 3.0f

typedef struct OverlayWalkCtx {
    FlatProgramView  program;
    CursorBlockState cursor;
    /* Mirrors overlay scope for cursor-bound guide/highlight passes. */
    OverlayScope cursor_overlay_scope;
    int              show_vertex_outlines;
    int              vertex_outline_style;
    int              highlight_current_poly;
    /* POLY_HIGHLIGHT_CLIPPED: cut the cursor highlight with the program's own
     * clip planes, like every other overlay. Off (POLY_HIGHLIGHT_ON) draws the
     * highlighted shape as authored — the one overlay that may show geometry
     * the frame doesn't. */
    int              highlight_clipped;
    int              replay_tess_preview;
    int              show_vertex_points;
    int              replay_vertex_points;
    int              replay_vertex_label;
    int              show_normal_vectors;
    int              replay_normal_display; /* ReplayNormalDisplayMode */
    int              replay_anchor_flat_idx; /* -1 or flat idx of current replay vertex */
    int              xform_guide_mode;       /* Render3dXformGuideMode */
    /* SINGLE_POLYGON scope: block-local vertex-ordinal range of the primitive
     * under the cursor, resolved by the controller (repl_flatten_cursor_polygon).
     * cursor_poly_valid == 0 means "no single primitive" (cursor on the glBegin
     * line, GL_POLYGON/GL_LINE_LOOP, tess block, ...) and the whole block is
     * treated as current, matching the other scopes. */
    int              cursor_poly_valid;
    int              cursor_poly_first;      /* first ordinal, inclusive */
    int              cursor_poly_last;       /* last ordinal, inclusive */
    int              cursor_poly_fan_anchor; /* 1 = ordinal 0 (fan center) too */
} OverlayWalkCtx;

typedef struct OverlaySnapshotPack {
    OverlayWalkCtx walk;
    Render3dGuideSnapshot snapshot;
    OverlayVertexLabelMode vertex_label_mode;
    int overlay_scope;    /* 0 = first loop instance, 1 = all, 2 = all at vertex
                            * (no declutter), 3 = all but depth-tested (visible) */
    Render3dViewMode ortho_mode;
    int show_normal_vectors;
    int multisample_enabled;
    int line_smooth_enabled;
} OverlaySnapshotPack;

void edit_overlays_render_outlines(const OverlayWalkCtx *ctx,
                                   int multisample_enabled,
                                   int line_smooth_enabled);

void edit_overlays_render_vertex_points(const OverlayWalkCtx *ctx);

void edit_overlays_render_vertex_numbers(const OverlayWalkCtx *ctx,
                                         OverlayVertexLabelMode mode,
                                         int is_ortho,
                                         int label_options);

void edit_overlays_render_normal_vectors(const OverlayWalkCtx *ctx);

void edit_overlays_render_cursor_guides(const Render3dGuideSnapshot *snapshot,
                                        const OverlayWalkCtx *ctx);

void edit_overlays_post_overlays(void *user_data);

/* Once-per-frame bitmap-text label passes (vertex numbers + replay
 * vertex label), run on the resolved frame via post_resolve_overlays_fn
 * instead of inside the accumulation loop. */
void edit_overlays_post_resolve_overlays(void *user_data);

Render3dGuideSnapshot cursor_guide_snapshot_with_flat_args(const Render3dGuideSnapshot *snapshot,
                                                        const GLCmd *flat,
                                                        int flat_idx);

#endif /* EDIT_OVERLAYS_H */
