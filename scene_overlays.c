/*
 * scene_overlays.c - scene geometry overlay passes shared by render frame code.
 */
#include "scene_geometry_guides.h"
#include "scene_overlays.h"
#include "scene_render_types.h"
#include "scene_transform_guides.h"
#include "scene_transform_utils.h"
#include "./include/gl_2d.h"

static int begin_mode_has_outline_overlay(GLenum mode) {
    switch (mode) {
    case GL_POINTS:
    case GL_LINES:
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        return 0;
    default:
        return 1;
    }
}

/* Returns non-zero if the flat command at flat_idx should be highlighted
 * as part of the cursor's current block. Uses only pushed config state. */
static int overlay_flat_cmd_matches_cursor(int flat_idx,
                                           const SceneRenderConfig *cfg) {
    const GLCmd *cmd = &cfg->flat_program.cmds[flat_idx];

    if (flat_idx < 0 || flat_idx >= cfg->flat_program.cmd_count)
        return 0;
    if (!cmd->valid)
        return 0;

    if (cfg->edit_line_idx < 0)
        return 0;

    /* CMD_CALL: highlight commands inlined from the called function */
    if (cmd->call_src_cmd_idx == cfg->edit_line_idx ||
        cmd->root_call_src_cmd_idx == cfg->edit_line_idx)
        return 1;

    /* Function scope: highlight commands sharing the cursor's func scope */
    if (cfg->cursor_func_scope_mask != 0 &&
        (cmd->func_scope_mask & cfg->cursor_func_scope_mask) != 0)
        return 1;

    /* Block range: highlight commands inside the cursor's begin..end block */
    if (cfg->cursor_block_begin_idx >= 0 &&
        flat_idx >= cfg->cursor_block_begin_idx &&
        flat_idx <= cfg->cursor_block_end_idx)
        return 1;

    /* Direct: flat cmd originates from the cursor source line */
    return cmd->src_cmd_idx == cfg->edit_line_idx;
}

int scene_overlay_flat_block_matches_cursor(int begin_idx, int is_tess,
                                            const SceneRenderConfig *cfg) {
    const GLCmd *cmds = cfg->flat_program.cmds;
    int cmd_count = cfg->flat_program.cmd_count;
    int depth = is_tess ? 1 : 0;

    for (int i = begin_idx; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;
        if (overlay_flat_cmd_matches_cursor(i, cfg))
            return 1;
        if (!is_tess && i > begin_idx && cmds[i].type == CMD_END)
            break;
        if (is_tess && i > begin_idx) {
            if (cmds[i].type == CMD_TESS_BEGIN_POLYGON) depth++;
            else if (cmds[i].type == CMD_TESS_END) {
                depth--;
                if (depth == 0) break;
            }
        }
    }

    return 0;
}

typedef struct VertexOverlayState {
    int flat_cmd_idx;
    int vertex_idx;
    int in_block;
    int block_selected;
    int tess_depth;
    float normal[3];
} VertexOverlayState;

typedef void (*VertexOverlayVisitFn)(const GLCmd *cmd,
                                     const VertexOverlayState *state,
                                     void *user);

static void scene_overlay_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void scene_overlay_pop_state(void) {
    glPopAttrib();
}

/* Replays transform commands once while visiting vertex-emitting flat commands.
 * Callers choose whether visits should be restricted to the cursor-selected
 * block; the matrix tracking stays centralized so overlay features cannot
 * drift from each other as flatten/execution rules evolve. */
static void walk_vertex_overlay(int selected_block_only,
                                VertexOverlayVisitFn on_vertex,
                                void *user,
                                const SceneRenderConfig *cfg) {
    const GLCmd *cmds = cfg->flat_program.cmds;
    int cmd_count = cfg->flat_program.cmd_count;
    VertexOverlayState state = {
        .flat_cmd_idx = -1,
        .vertex_idx = 0,
        .in_block = 0,
        .block_selected = selected_block_only ? 0 : 1,
        .tess_depth = 0,
        .normal = {0, 0, 1},
    };
    int matrix_depth = 0;

    glPushMatrix();
    for (int i = 0; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;

        const GLCmd *cmd = &cmds[i];
        if (!state.in_block && repl_cmd_is_transform(cmd->type)) {
            scene_apply_tracked_transform(cmd, &matrix_depth);
            continue;
        }

        switch (cmd->type) {
        case CMD_BEGIN:
            state.in_block = 1;
            state.block_selected = selected_block_only
                                 ? scene_overlay_flat_block_matches_cursor(i, 0, cfg)
                                 : 1;
            state.vertex_idx = 0;
            state.tess_depth = 0;
            state.normal[0] = 0.0f;
            state.normal[1] = 0.0f;
            state.normal[2] = 1.0f;
            break;
        case CMD_END:
            state.in_block = 0;
            state.block_selected = selected_block_only ? 0 : 1;
            state.tess_depth = 0;
            break;
        case CMD_TESS_BEGIN_POLYGON:
            state.in_block = 1;
            state.block_selected = selected_block_only
                                 ? scene_overlay_flat_block_matches_cursor(i, 1, cfg)
                                 : 1;
            state.vertex_idx = 0;
            state.tess_depth = 1;
            state.normal[0] = 0.0f;
            state.normal[1] = 0.0f;
            state.normal[2] = 1.0f;
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (state.tess_depth > 0)
                state.tess_depth++;
            break;
        case CMD_TESS_END:
            if (state.tess_depth > 0) {
                state.tess_depth--;
                if (state.tess_depth == 0) {
                    state.in_block = 0;
                    state.block_selected = selected_block_only ? 0 : 1;
                }
            }
            break;
        case CMD_NORMAL3F:
        case CMD_TESS_NORMAL:
            state.normal[0] = cmd->args[0];
            state.normal[1] = cmd->args[1];
            state.normal[2] = cmd->args[2];
            break;
        case CMD_VERTEX3F:
        case CMD_VERTEX2F:
        case CMD_TESS_VERTEX: {
            int visit = selected_block_only
                      ? (state.in_block && state.block_selected)
                      : 1;
            if (visit && on_vertex) {
                state.flat_cmd_idx = i;
                on_vertex(cmd, &state, user);
            }
            state.vertex_idx++;
            break;
        }
        default:
            break;
        }
    }
    scene_unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}

static void draw_vertex_number_label(const GLCmd *cmd,
                                     const VertexOverlayState *state,
                                     void *user) {
    (void)user;
    char label[16];
    snprintf(label, sizeof(label), " v%d", state->vertex_idx);
    glRasterPos3f(cmd->args[0], cmd->args[1], cmd->args[2]);
    for (const char *c = label; *c; c++)
        glutBitmapCharacter(FONT_MONO, (unsigned char)*c);
}

static void draw_normal_vector_at_vertex(const GLCmd *cmd,
                                         const VertexOverlayState *state,
                                         void *user) {
    float scale = *(const float *)user;
    float vx = cmd->args[0], vy = cmd->args[1], vz = cmd->args[2];
    float nx = state->normal[0], ny = state->normal[1], nz = state->normal[2];

    glBegin(GL_LINES);
    glVertex3f(vx, vy, vz);
    glVertex3f(vx + nx * scale, vy + ny * scale, vz + nz * scale);
    glEnd();
    glPointSize(4.0f);
    glBegin(GL_POINTS);
    glVertex3f(vx + nx * scale, vy + ny * scale, vz + nz * scale);
    glEnd();
    glPointSize(1.0f);
}

void scene_overlays_render_vertex_numbers(const FrameRenderContext *frame_ctx) {
    scene_overlay_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glColor3f(1.0f, 1.0f, 0.30f);

    walk_vertex_overlay(1, draw_vertex_number_label, NULL, &frame_ctx->config);

    glEnable(GL_DEPTH_TEST);
    if (frame_ctx->config.user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_overlay_pop_state();
}

void scene_overlays_render_normal_vectors(const FrameRenderContext *frame_ctx) {
    scene_overlay_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glColor3f(0.80f, 0.80f, 0.30f);
    float scale = 0.35f;

    walk_vertex_overlay(0, draw_normal_vector_at_vertex, &scale, &frame_ctx->config);

    glEnable(GL_DEPTH_TEST);
    if (frame_ctx->config.user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_overlay_pop_state();
}

void scene_overlays_render_outlines(const FrameRenderContext *frame_ctx,
                                    int show_current_poly,
                                    int replay_tess_preview) {
    const SceneRenderConfig *cfg = &frame_ctx->config;
    const GLCmd *cmds = cfg->flat_program.cmds;
    int cmd_count = cfg->flat_program.cmd_count;

    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (cfg->multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (cfg->line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(REPL_OUTLINE_POLYGON_OFFSET_FACTOR,
                    REPL_OUTLINE_POLYGON_OFFSET_UNITS);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    if (cfg->show_vertex_outlines || show_current_poly) {
        glPushMatrix();
        int in_begin = 0;
        int matrix_depth = 0;
        int block_is_current = 0;
        int tess_in_contour = 0;
        int tess_poly_is_current = 0;

        for (int i = 0; i < cmd_count; i++) {
            if (!cmds[i].valid) continue;

            if (repl_cmd_is_transform(cmds[i].type)) {
                if (!in_begin && !tess_in_contour)
                    scene_apply_tracked_transform(&cmds[i], &matrix_depth);
                continue;
            }

            switch (cmds[i].type) {
            case CMD_TESS_BEGIN_CONTOUR:
                if (replay_tess_preview)
                    break;
                if (tess_in_contour) {
                    glEnd();
                    glLineWidth(1.0f);
                }
                if (cfg->show_vertex_outlines || tess_poly_is_current) {
                    glLineWidth(1.5f);
                    if (tess_poly_is_current)
                        glColor3f(0.0f, 0.9f, 0.9f);
                    else
                        glColor3f(0.55f, 0.20f, 0.70f);
                    glBegin(GL_LINE_LOOP);
                    tess_in_contour = 1;
                }
                break;
            case CMD_TESS_VERTEX:
                if (replay_tess_preview)
                    break;
                if (tess_in_contour)
                    glVertex3f(cmds[i].args[0], cmds[i].args[1],
                               cmds[i].args[2]);
                break;
            case CMD_TESS_END:
                if (replay_tess_preview) {
                    tess_poly_is_current = 0;
                    break;
                }
                if (tess_in_contour) {
                    glEnd();
                    glLineWidth(1.0f);
                    tess_in_contour = 0;
                }
                if (!tess_in_contour)
                    tess_poly_is_current = 0;
                break;
            case CMD_BEGIN: {
                int draw_outline = begin_mode_has_outline_overlay(cmds[i].mode);
                if (in_begin) glEnd();
                block_is_current = show_current_poly &&
                                   scene_overlay_flat_block_matches_cursor(i, 0, cfg);
                if (block_is_current) {
                    glLineWidth(3.0f);
                    glColor3f(0.0f, 0.9f, 0.9f);
                } else if (cfg->show_vertex_outlines && draw_outline) {
                    glLineWidth(1.2f);
                    glColor3f(0.0f, 0.0f, 0.0f);
                } else {
                    in_begin = 0;
                    break;
                }
                glBegin(cmds[i].mode);
                in_begin = 1;
                break;
            }
            case CMD_END:
                if (in_begin) {
                    glEnd();
                    in_begin = 0;
                    glLineWidth(1.0f);
                    glColor3f(0.0f, 0.0f, 0.0f);
                }
                block_is_current = 0;
                break;
            case CMD_VERTEX3F:
                if (in_begin) {
                    if (block_is_current || cfg->show_vertex_outlines)
                        glVertex3f(cmds[i].args[0], cmds[i].args[1],
                                   cmds[i].args[2]);
                }
                break;
            case CMD_VERTEX2F:
                if (in_begin) {
                    if (block_is_current || cfg->show_vertex_outlines)
                        glVertex2f(cmds[i].args[0], cmds[i].args[1]);
                }
                break;
            case CMD_TESS_BEGIN_POLYGON:
                tess_poly_is_current = show_current_poly &&
                                       scene_overlay_flat_block_matches_cursor(i, 1, cfg);
                break;
            default:
                break;
            }
        }

        if (in_begin) {
            glEnd();
            glLineWidth(1.0f);
        }
        if (tess_in_contour) {
            glEnd();
            glLineWidth(1.0f);
        }
        scene_unwind_transform_stack(&matrix_depth);
        glPopMatrix();
    }

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void scene_overlays_render_vertex_points(const FrameRenderContext *frame_ctx) {
    const SceneRenderConfig *cfg = &frame_ctx->config;
    if (!cfg->show_vpoints && !cfg->replay_vertex_points)
        return;

    /* Snapshot the pure camera-view matrix (before any user transforms are
     * applied below) so the transform guide can render in world axes at an
     * anchor point, independent of pre-cursor rotations. */
    const SceneGuideSnapshot *guide_snapshot = &cfg->guide_snapshot;
    SceneTransformGuidePlan xform_guide_plan;

    scene_overlay_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);

    if (cfg->replay_vertex_points)
        glColor4f(1.0f, 0.88f, 0.20f, 0.75f);
    else
        glColor4f(0.05f, 0.05f, 0.10f, 0.80f);

    glPushMatrix();
    float tg_cam_view[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, tg_cam_view);
    scene_transform_guides_prepare(guide_snapshot, &xform_guide_plan);

    {
        const GLCmd *flat_cmds = cfg->flat_program.cmds;
        int flat_cmd_count = cfg->flat_program.cmd_count;
        int matrix_depth = 0;
        GLenum primitive_mode = 0;

        for (int i = 0; i < flat_cmd_count; i++) {
            if (!flat_cmds[i].valid) continue;
            int is_cursor = (flat_cmds[i].src_cmd_idx == guide_snapshot->edit_line_idx);


            /* Draw vertex guides and normals if this vertex is at the cursor
             * position. Do this inline so that the current model matrix is
             * applied to the guides, ensuring they are positioned correctly
             * even when transforms separate blocks. */
            if (is_cursor && !cfg->replaying) {
                scene_geometry_guides_render_for_cursor(guide_snapshot);
            }

            scene_transform_guides_render_if_due(guide_snapshot,
                                                 &xform_guide_plan,
                                                 i, tg_cam_view);

            if (repl_cmd_is_transform(flat_cmds[i].type)) {
                scene_apply_tracked_transform(&flat_cmds[i], &matrix_depth);
            } else if (flat_cmds[i].type == CMD_BEGIN) {
                primitive_mode = flat_cmds[i].mode;
            } else if (flat_cmds[i].type == CMD_END) {
                primitive_mode = 0;
            } else if (flat_cmds[i].type == CMD_VERTEX3F ||
                       flat_cmds[i].type == CMD_VERTEX2F ||
                       flat_cmds[i].type == CMD_TESS_VERTEX) {
                int is_line = (primitive_mode == GL_LINES ||
                               primitive_mode == GL_LINE_STRIP ||
                               primitive_mode == GL_LINE_LOOP);
                // Lines tend to be higher tesselated than polygons and points can obscure the line.
                glPointSize(is_line ? 2.0f : 7.0f);
                glBegin(GL_POINTS);
                glVertex3f(flat_cmds[i].args[0], flat_cmds[i].args[1],
                           flat_cmds[i].args[2]);
                glEnd();
            }
        }
        glPointSize(1.0f);
        scene_unwind_transform_stack(&matrix_depth);
    }

    glPopMatrix();
    scene_overlay_pop_state();
}
