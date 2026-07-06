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

#define OVERLAY_VERTEX_LABEL_SCOPE_LIST(X) \
    X(ONE_INSTANCE, "One instance")        \
    X(ALL_INSTANCES, "All instances")      \
    X(AT_VERTEX, "At vertex")              \
    X(VISIBLE, "Visible only")

#define OVERLAY_VERTEX_LABEL_SCOPE_NAME_ENTRY(name, str) [OVERLAY_VERTEX_LABEL_SCOPE_##name] = str,

typedef enum OverlayVertexLabelScope {
#define OVERLAY_VERTEX_LABEL_SCOPE_ENUM_ENTRY(name, str) OVERLAY_VERTEX_LABEL_SCOPE_##name,
    OVERLAY_VERTEX_LABEL_SCOPE_LIST(OVERLAY_VERTEX_LABEL_SCOPE_ENUM_ENTRY)
#undef OVERLAY_VERTEX_LABEL_SCOPE_ENUM_ENTRY
    OVERLAY_VERTEX_LABEL_SCOPE_COUNT
} OverlayVertexLabelScope;


typedef struct OverlayWalkCtx {
    FlatProgramView  program;
    CursorBlockState cursor;
    /* Mirrors overlay scope for cursor-bound guide/highlight passes. */
    OverlayVertexLabelScope cursor_label_scope;
    int              show_vertex_outlines;
    int              highlight_current_poly;
    int              replay_tess_preview;
    int              show_vertex_points;
    int              replay_vertex_points;
    int              replay_vertex_label;
    int              show_normal_vectors;
    int              replay_normal_display; /* ReplayNormalDisplayMode */
    int              replay_anchor_flat_idx; /* -1 or flat idx of current replay vertex */
    int              xform_guide_mode;       /* Render3dXformGuideMode */
} OverlayWalkCtx;

typedef struct OverlaySnapshotPack {
    OverlayWalkCtx walk;
    Render3dGuideSnapshot snapshot;
    OverlayVertexLabelMode vertex_label_mode;
    int vertex_label_scope;    /* 0 = one loop instance, 1 = all, 2 = all at vertex
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

Render3dGuideSnapshot cursor_guide_snapshot_with_flat_args(const Render3dGuideSnapshot *snapshot,
                                                        const GLCmd *flat,
                                                        int flat_idx);

#endif /* EDIT_OVERLAYS_H */
