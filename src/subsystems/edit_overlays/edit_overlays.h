#ifndef EDIT_OVERLAYS_H
#define EDIT_OVERLAYS_H

#include "app/glr_config.h"
#include "scene/guides/guides_shared.h"  /* SceneGuideSnapshot */
#include "repl/state_views.h"           /* GlrVertexLabelMode, FlatProgramView, CursorBlockState */

typedef struct OverlayWalkCtx {
    FlatProgramView  program;
    CursorBlockState cursor;
    int              show_vertex_outlines;
    int              highlight_current_poly;
    int              replay_tess_preview;
    int              show_vertex_points;
    int              replay_vertex_points;
} OverlayWalkCtx;

typedef struct OverlaySnapshotPack {
    OverlayWalkCtx walk;
    SceneGuideSnapshot snapshot;
    int show_vertex_labels;
    int ortho_mode;
    int show_normal_vectors;
    int multisample_enabled;
    int line_smooth_enabled;
} OverlaySnapshotPack;

void edit_overlays_render_outlines(const OverlayWalkCtx *ctx,
                                   int multisample_enabled,
                                   int line_smooth_enabled);

void edit_overlays_render_vertex_points(const OverlayWalkCtx *ctx);

void edit_overlays_render_vertex_numbers(GlrVertexLabelMode mode, int is_ortho);

void edit_overlays_render_normal_vectors(void);

void edit_overlays_render_cursor_guides(const SceneGuideSnapshot *snapshot);

void edit_overlays_post_overlays(void *user_data);

SceneGuideSnapshot cursor_guide_snapshot_with_flat_args(const SceneGuideSnapshot *snapshot,
                                                        const GLCmd *flat,
                                                        int flat_idx);

#endif /* EDIT_OVERLAYS_H */
