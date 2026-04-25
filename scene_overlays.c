/*
 * scene_overlays.c - scene geometry overlay passes shared by render frame code.
 */
#include "sample.h"
#include "repl_executor.h"
#include "repl_core.h"
#include "repl_state.h"
#include "scene_overlays.h"

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

int scene_overlay_flat_block_matches_cursor(int begin_idx, int is_tess) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *g_flat_cmds = flat_program.cmds;
    int g_num_flat_cmds = flat_program.cmd_count;
    int depth = is_tess ? 1 : 0;

    for (int i = begin_idx; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (repl_flat_cmd_matches_cursor(i))
            return 1;
        if (!is_tess && i > begin_idx && g_flat_cmds[i].type == CMD_END)
            break;
        if (is_tess && i > begin_idx) {
            if (g_flat_cmds[i].type == CMD_TESS_BEGIN_POLYGON) depth++;
            else if (g_flat_cmds[i].type == CMD_TESS_END) {
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
                                void *user) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *g_flat_cmds = flat_program.cmds;
    int g_num_flat_cmds = flat_program.cmd_count;
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
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;

        const GLCmd *cmd = &g_flat_cmds[i];
        if (!state.in_block && is_transform_cmd(cmd->type)) {
            repl_executor_apply_tracked_transform_cmd(cmd, &matrix_depth);
            continue;
        }

        switch (cmd->type) {
        case CMD_BEGIN:
            state.in_block = 1;
            state.block_selected = selected_block_only
                                 ? scene_overlay_flat_block_matches_cursor(i, 0)
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
                                 ? scene_overlay_flat_block_matches_cursor(i, 1)
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
    repl_executor_unwind_tracked_transform_stack(&matrix_depth);
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

void scene_overlays_render_vertex_numbers(void) {
    int g_user_lighting_enabled = repl_state_flat_program_user_lighting_enabled();
    scene_overlay_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glColor3f(1.0f, 1.0f, 0.30f);

    walk_vertex_overlay(1, draw_vertex_number_label, NULL);

    glEnable(GL_DEPTH_TEST);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_overlay_pop_state();
}

void scene_overlays_render_normal_vectors(void) {
    int g_user_lighting_enabled = repl_state_flat_program_user_lighting_enabled();
    scene_overlay_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glColor3f(0.80f, 0.80f, 0.30f);
    float scale = 0.35f;

    walk_vertex_overlay(0, draw_normal_vector_at_vertex, &scale);

    glEnable(GL_DEPTH_TEST);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_overlay_pop_state();
}

void scene_overlays_render_outlines(int show_current_poly,
                                    int replay_tess_preview) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *g_flat_cmds = flat_program.cmds;
    int g_num_flat_cmds = flat_program.cmd_count;
    const ReplRenderState *render = repl_state_render();
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (*render->multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (*render->line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(REPL_OUTLINE_POLYGON_OFFSET_FACTOR,
                    REPL_OUTLINE_POLYGON_OFFSET_UNITS);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    if (*repl_state_presentation()->show_vertex_outlines || show_current_poly) {
        glPushMatrix();
        int in_begin = 0;
        int matrix_depth = 0;
        int block_is_current = 0;
        int tess_in_contour = 0;
        int tess_poly_is_current = 0;

        for (int i = 0; i < g_num_flat_cmds; i++) {
            if (!g_flat_cmds[i].valid) continue;

            if (is_transform_cmd(g_flat_cmds[i].type)) {
                if (!in_begin && !tess_in_contour)
                    repl_executor_apply_tracked_transform_cmd(&g_flat_cmds[i], &matrix_depth);
                continue;
            }

            switch (g_flat_cmds[i].type) {
            case CMD_TESS_BEGIN_CONTOUR:
                if (replay_tess_preview)
                    break;
                if (tess_in_contour) {
                    glEnd();
                    glLineWidth(1.0f);
                }
                if (*repl_state_presentation()->show_vertex_outlines || tess_poly_is_current) {
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
                    glVertex3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                               g_flat_cmds[i].args[2]);
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
                int draw_outline = begin_mode_has_outline_overlay(g_flat_cmds[i].mode);
                if (in_begin) glEnd();
                block_is_current = show_current_poly &&
                                   scene_overlay_flat_block_matches_cursor(i, 0);
                if (block_is_current) {
                    glLineWidth(3.0f);
                    glColor3f(0.0f, 0.9f, 0.9f);
                } else if (*repl_state_presentation()->show_vertex_outlines && draw_outline) {
                    glLineWidth(1.2f);
                    glColor3f(0.0f, 0.0f, 0.0f);
                } else {
                    in_begin = 0;
                    break;
                }
                glBegin(g_flat_cmds[i].mode);
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
                    if (block_is_current || *repl_state_presentation()->show_vertex_outlines)
                        glVertex3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                                   g_flat_cmds[i].args[2]);
                }
                break;
            case CMD_TESS_BEGIN_POLYGON:
                tess_poly_is_current = show_current_poly &&
                                       scene_overlay_flat_block_matches_cursor(i, 1);
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
        repl_executor_unwind_tracked_transform_stack(&matrix_depth);
        glPopMatrix();
    }

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
