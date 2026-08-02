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

/* Where a label sits, which is a property of the label and not of the scope
 * that chose which vertices get one - the two are orthogonal, so every label
 * mode is reachable at either placement.
 *
 * DECLUTTERED runs the 2D layout pass: labels are pushed off each other along
 * Y and yield to the fixed-priority cursor-guide text, so a crowded block
 * stays readable at the cost of leader lines. AT_VERTEX bypasses that
 * entirely and pins each label to its own projected vertex, additively
 * blended - exact position, overlaps and all. It also stops labels being
 * registered as guide-label obstacles, since they no longer claim a row. */
#define OVERLAY_LABEL_PLACEMENT_LIST(X) \
    X(DECLUTTERED, "Decluttered")       \
    X(AT_VERTEX, "At vertex")

#define OVERLAY_LABEL_PLACEMENT_NAME_ENTRY(name, str) [OVERLAY_LABEL_PLACEMENT_##name] = str,

typedef enum OverlayLabelPlacement {
#define OVERLAY_LABEL_PLACEMENT_ENUM_ENTRY(name, str) OVERLAY_LABEL_PLACEMENT_##name,
    OVERLAY_LABEL_PLACEMENT_LIST(OVERLAY_LABEL_PLACEMENT_ENUM_ENTRY)
#undef OVERLAY_LABEL_PLACEMENT_ENUM_ENTRY
    OVERLAY_LABEL_PLACEMENT_COUNT
} OverlayLabelPlacement;

/* LAST_INSTANCE picks the final unrolled copy of the cursor's block. Flatten
 * threads assignments forward, so it is the copy whose loop-body variable
 * values the variable panel still reports, and the last one a replay draws -
 * highlights, labels and cursor guides then agree with the rest of the UI.
 * (The `for` counter itself is loop-scoped and restored, so it reads as its
 * declared value either way.)
 *
 * Every scope but WHOLE_SCENE is anchored to the block under the cursor -
 * hence the shared name prefix. WHOLE_SCENE drops the cursor anchor for
 * *labels* only: it numbers every vertex the program emits. The highlight
 * stays cursor-bound there, because a highlight that covers everything
 * distinguishes nothing.
 *
 * A scope answers "which vertices get a label", nothing else. Two things that
 * once lived in this list and do not belong to it: occlusion, which applies
 * whenever the GL context supports depth readback (a label you can see for a
 * vertex you cannot is a lie about where the geometry is; WebGL cannot read
 * the default framebuffer's depth, so it falls back to showing all in-scope
 * labels), and at-vertex placement, which is OverlayLabelPlacement above. The
 * polygon highlight is deliberately exempt from occlusion and still draws
 * through hidden geometry so the cursor's shape stays findable. */
#define OVERLAY_SCOPE_LIST(X) \
    X(LAST_INSTANCE, "Cursor block, last instance")  \
    X(ALL_INSTANCES, "Cursor block, all instances")  \
    X(WHOLE_SCENE, "Whole scene")                    \
    X(SINGLE_POLYGON, "Cursor block, single polygon")

#define OVERLAY_SCOPE_NAME_ENTRY(name, str) [OVERLAY_SCOPE_##name] = str,

typedef enum OverlayScope {
#define OVERLAY_SCOPE_ENUM_ENTRY(name, str) OVERLAY_SCOPE_##name,
    OVERLAY_SCOPE_LIST(OVERLAY_SCOPE_ENUM_ENTRY)
#undef OVERLAY_SCOPE_ENUM_ENTRY
    OVERLAY_SCOPE_COUNT
} OverlayScope;

/* Clipped & culled draws the cursor highlight through the program's own clip
 * planes and cull state, like every other overlay; On draws the shape as
 * authored instead (see the overlay_gl_* helpers in edit_overlays.c). */
#define POLY_HIGHLIGHT_LIST(X)     \
    X(OFF, "Off")                  \
    X(ON, "On")                    \
    X(CLIPPED_CULLED, "Clipped & culled")

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

/* Vertex-point overlay styling, in pixels: line-mode vertices get the
 * smaller dot so a dense polyline stays readable. */
#define EDIT_OVERLAY_VERTEX_POINT_SIZE 6.0f
#define EDIT_OVERLAY_VERTEX_LINE_POINT_SIZE 3.0f

typedef struct OverlayWalkCtx {
    FlatProgramView  program;
    CursorBlockState cursor;
    /* Mirrors overlay scope for cursor-bound guide/highlight passes. */
    OverlayScope cursor_overlay_scope;
    int              show_vertex_outlines;
    int              vertex_outline_style;
    int              highlight_current_poly;
    /* POLY_HIGHLIGHT_CLIPPED_CULLED: draw the cursor highlight through the
     * program's own clip planes and cull state, like every other overlay. Off
     * (POLY_HIGHLIGHT_ON) draws the highlighted shape as authored - the one
     * overlay that may show geometry the frame doesn't. */
    int              highlight_clipped_culled;
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
     * The ordinals are block-local, so they address the same primitive in
     * whichever unrolled copy LAST_INSTANCE selection lands on.
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
    int overlay_scope;          /* OverlayScope           */
    int vertex_label_placement; /* OverlayLabelPlacement  */
    int depth_readback_supported;
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
                                         int scope,
                                         int placement,
                                         int depth_readback_supported);

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
