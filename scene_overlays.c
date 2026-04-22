/*
 * scene_overlays.c - scene geometry overlay passes shared by render frame code.
 */
#include "sample.h"
#include "repl_core.h"
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

void scene_overlays_render_outlines(int show_current_poly,
                                    int replay_tess_preview) {
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (g_multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (g_line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(REPL_OUTLINE_POLYGON_OFFSET_FACTOR,
                    REPL_OUTLINE_POLYGON_OFFSET_UNITS);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    if (g_show_outlines || show_current_poly) {
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
                    apply_tracked_transform_cmd(&g_flat_cmds[i], &matrix_depth);
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
                if (g_show_outlines || tess_poly_is_current) {
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
                } else if (g_show_outlines && draw_outline) {
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
                    if (block_is_current || g_show_outlines)
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
        unwind_tracked_transform_stack(&matrix_depth);
        glPopMatrix();
    }

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
