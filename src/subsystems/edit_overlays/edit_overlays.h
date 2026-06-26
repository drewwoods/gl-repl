#ifndef EDIT_OVERLAYS_H
#define EDIT_OVERLAYS_H

#include "render3d/guides/guides_shared.h"  /* Render3dGuideSnapshot */
#include "render3d/view_mode.h"             /* Render3dViewMode */
#include "repl/state_views.h"            /* FlatProgramView, CursorBlockState */

#define OVERLAY_VERTEX_LABEL_LIST(X) \
    X(OFF, "Off")                    \
    X(INDEX, "Index")                \
    X(INDEX_POS, "Index+Pos")        \
    X(INDEX_WORLD, "Index+World")

#define OVERLAY_VERTEX_LABEL_NAME_ENTRY(name, str) [OVERLAY_VERTEX_LABEL_##name] = str,

typedef enum OverlayVertexLabelMode {
#define OVERLAY_VERTEX_LABEL_ENUM_ENTRY(name, str) OVERLAY_VERTEX_LABEL_##name,
    OVERLAY_VERTEX_LABEL_LIST(OVERLAY_VERTEX_LABEL_ENUM_ENTRY)
#undef OVERLAY_VERTEX_LABEL_ENUM_ENTRY
    OVERLAY_VERTEX_LABEL_COUNT
} OverlayVertexLabelMode;


typedef struct OverlayWalkCtx {
    FlatProgramView  program;
    CursorBlockState cursor;
    int              show_vertex_outlines;
    int              highlight_current_poly;
    int              replay_tess_preview;
    int              show_vertex_points;
    int              replay_vertex_points;
    int              replay_anchor_flat_idx; /* -1 or flat idx of current replay vertex */
} OverlayWalkCtx;

typedef struct OverlaySnapshotPack {
    OverlayWalkCtx walk;
    Render3dGuideSnapshot snapshot;
    OverlayVertexLabelMode vertex_label_mode;
    int vertex_label_scope;    /* 0 = one loop instance, 1 = all, 2 = all instances at vertex (no declutter) */
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
