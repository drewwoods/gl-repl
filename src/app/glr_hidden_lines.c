#include "app/glr_hidden_lines.h"

#include "gl_includes.h"
#include "config.h"
#include "repl/core_internal.h"
#include "repl/eval.h"
#include "repl/state_owners.h"
#include "repl/transform_utils.h"

#include <stdio.h>
#include <string.h>

#define GLR_HIDDEN_TESS_VERT_BUF_SIZE 256

typedef void (*GlrHiddenLinesGluCallback)(void);

typedef struct {
    GLdouble pos[3];
} GlrHiddenTessVertex;

static GLUtesselator *g_hidden_tess = NULL;
static GlrHiddenTessVertex g_hidden_tess_verts[GLR_HIDDEN_TESS_VERT_BUF_SIZE];
static int g_hidden_tess_vert_count = 0;

static void hidden_tess_begin_cb(GLenum mode) {
    glBegin(mode);
}

static void hidden_tess_end_cb(void) {
    glEnd();
}

static void hidden_tess_vertex_cb(void *vertex_data) {
    GlrHiddenTessVertex *v = (GlrHiddenTessVertex *)vertex_data;
    glVertex3dv(v->pos);
}

static void hidden_tess_combine_cb(GLdouble coords[3],
                                   void *vertex_data[4],
                                   GLfloat weight[4],
                                   void **out_data) {
    GlrHiddenTessVertex *v;
    (void)vertex_data;
    (void)weight;
    if (g_hidden_tess_vert_count >= GLR_HIDDEN_TESS_VERT_BUF_SIZE) {
        *out_data = NULL;
        return;
    }

    v = &g_hidden_tess_verts[g_hidden_tess_vert_count++];
    v->pos[0] = coords[0];
    v->pos[1] = coords[1];
    v->pos[2] = coords[2];
    *out_data = v;
}

static void hidden_tess_error_cb(GLenum err) {
    (void)err;
}

void glr_hidden_lines_destroy_resources(void) {
    if (g_hidden_tess) {
        gluDeleteTess(g_hidden_tess);
        g_hidden_tess = NULL;
    }
    g_hidden_tess_vert_count = 0;
}

void glr_hidden_lines_init_resources(void) {
    glr_hidden_lines_destroy_resources();

    g_hidden_tess = gluNewTess();
    if (!g_hidden_tess)
        return;

    gluTessCallback(g_hidden_tess, GLU_TESS_BEGIN,
                    (GlrHiddenLinesGluCallback)hidden_tess_begin_cb);
    gluTessCallback(g_hidden_tess, GLU_TESS_END,
                    (GlrHiddenLinesGluCallback)hidden_tess_end_cb);
    gluTessCallback(g_hidden_tess, GLU_TESS_VERTEX,
                    (GlrHiddenLinesGluCallback)hidden_tess_vertex_cb);
    gluTessCallback(g_hidden_tess, GLU_TESS_COMBINE,
                    (GlrHiddenLinesGluCallback)hidden_tess_combine_cb);
    gluTessCallback(g_hidden_tess, GLU_TESS_ERROR,
                    (GlrHiddenLinesGluCallback)hidden_tess_error_cb);
    gluTessCallback(g_hidden_tess, GLU_TESS_EDGE_FLAG,
                    (GlrHiddenLinesGluCallback)glEdgeFlag);
}

static int hidden_lines_ensure_tess(void) {
    if (!g_hidden_tess)
        glr_hidden_lines_init_resources();
    return g_hidden_tess != NULL;
}

static int hidden_lines_flat_count(const GlrHiddenLinesRenderContext *ctx) {
    int count;
    if (!ctx || !ctx->program.cmds || ctx->program.cmd_count <= 0)
        return 0;

    count = ctx->flat_cmd_count;
    if (count < 0 || count > ctx->program.cmd_count)
        count = ctx->program.cmd_count;
    return count;
}

static const FlatCmdLocalVars *hidden_lines_local_vars_at(
    FlatProgramView program,
    int flat_cmd_idx) {
    if (!program.local_vars ||
        flat_cmd_idx < 0 ||
        flat_cmd_idx >= program.cmd_count)
        return NULL;
    return &program.local_vars[flat_cmd_idx];
}

static const char *hidden_lines_flat_text(SourceTextView text,
                                          const GLCmd *flat_cmd) {
    if (!flat_cmd)
        return "";
    return source_text_line(text, flat_cmd->src_cmd_idx);
}

static int hidden_lines_light_slot_for_cap(GLenum cap) {
    int slot = (int)cap - (int)GL_LIGHT0;
    return (slot >= 0 && slot < REPL_LIGHT_SLOT_COUNT) ? slot : -1;
}

static void hidden_lines_apply_runtime_effect(const GLCmd *cmd) {
    if (!cmd)
        return;

    switch (cmd->type) {
    case CMD_VAR_ASSIGN: {
        int var_idx = cmd->var_idx;
        if (var_idx >= 0 && var_idx < g_num_predef_vars)
            g_predef_vars_mut[var_idx].value = cmd->args[0];
        break;
    }
    case CMD_SCRATCH_ASSIGN:
        repl_eval_scratch_set((int)cmd->args[0],
                              (int)cmd->args[1],
                              cmd->args[2]);
        break;
    case CMD_ENABLE: {
        int slot = hidden_lines_light_slot_for_cap((GLenum)cmd->args[0]);
        if (slot >= 0)
            repl_state_render_set_light_enabled(slot, 1);
        break;
    }
    case CMD_DISABLE: {
        int slot = hidden_lines_light_slot_for_cap((GLenum)cmd->args[0]);
        if (slot >= 0)
            repl_state_render_set_light_enabled(slot, 0);
        break;
    }
    case CMD_CLEAR_COLOR: {
        float rgba[4] = {
            cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]
        };
        repl_state_render_set_clear_color(rgba);
        break;
    }
    default:
        break;
    }
}

static int hidden_lines_begin_mode_writes_fill_depth(GLenum mode) {
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

static void hidden_lines_draw_glut_solid(const GLCmd *cmd) {
    if (!cmd)
        return;

    switch (cmd->type) {
    case CMD_GLUT_TORUS:
        glutSolidTorus((double)cmd->args[0],
                       (double)cmd->args[1],
                       (int)cmd->args[2],
                       (int)cmd->args[3]);
        break;
    case CMD_GLUT_CUBE:
        glutSolidCube((double)cmd->args[0]);
        break;
    case CMD_GLUT_SPHERE:
        glutSolidSphere((double)cmd->args[0],
                        (int)cmd->args[1],
                        (int)cmd->args[2]);
        break;
    case CMD_GLUT_TEAPOT:
        glutSolidTeapot((double)cmd->args[0]);
        break;
    case CMD_GLUT_CONE:
        glutSolidCone((double)cmd->args[0],
                      (double)cmd->args[1],
                      (int)cmd->args[2],
                      (int)cmd->args[3]);
        break;
    default:
        break;
    }
}

static int hidden_lines_is_wireframe_purpose(SceneExecutePurpose purpose) {
    return purpose == SCENE_EXEC_WIREFRAME_HIDDEN_LINES ||
           purpose == SCENE_EXEC_WIREFRAME_DEPTH_FILL ||
           purpose == SCENE_EXEC_WIREFRAME_VISIBLE_LINES;
}

void glr_hidden_lines_execute(const GlrHiddenLinesRenderContext *ctx,
                              SceneExecutePurpose purpose) {
    FlatProgramView program;
    const GLCmd *cmds;
    int cmd_count;
    int pc = 0;
    int goto_count = 0;
    int in_begin = 0; /* 1 = emitting, 2 = skipping non-fill depth primitive */
    int tess_depth = 0;
    int matrix_depth = 0;
    int depth_fill = purpose == SCENE_EXEC_WIREFRAME_DEPTH_FILL;

    if (!ctx || !hidden_lines_is_wireframe_purpose(purpose))
        return;

    program = ctx->program;
    cmds = program.cmds;
    cmd_count = hidden_lines_flat_count(ctx);
    if (!cmds || cmd_count <= 0)
        return;

    repl_state_render_clear_light_enabled_mask();

    glPushMatrix();
    while (pc < cmd_count) {
        if (!cmds[pc].valid) {
            pc++;
            continue;
        }

        if (repl_cmd_is_transform(cmds[pc].type)) {
            if (in_begin != 1 && tess_depth == 0)
                apply_tracked_transform(&cmds[pc], &matrix_depth);
            pc++;
            continue;
        }

        switch (cmds[pc].type) {
        case CMD_BEGIN: {
            GLenum mode = (GLenum)cmds[pc].args[0];
            if (in_begin == 1)
                glEnd();
            if (depth_fill && !hidden_lines_begin_mode_writes_fill_depth(mode)) {
                in_begin = 2;
            } else {
                glBegin(mode);
                in_begin = 1;
            }
            break;
        }
        case CMD_END:
            if (in_begin == 1)
                glEnd();
            in_begin = 0;
            break;
        case CMD_VERTEX3F:
            if (in_begin == 1)
                glVertex3f(cmds[pc].args[0],
                           cmds[pc].args[1],
                           cmds[pc].args[2]);
            break;
        case CMD_VERTEX2F:
            if (in_begin == 1)
                glVertex2f(cmds[pc].args[0], cmds[pc].args[1]);
            break;
        case CMD_GLUT_TORUS:
        case CMD_GLUT_CUBE:
        case CMD_GLUT_SPHERE:
        case CMD_GLUT_TEAPOT:
        case CMD_GLUT_CONE:
            hidden_lines_draw_glut_solid(&cmds[pc]);
            break;
        case CMD_TESS_BEGIN_POLYGON:
            if (hidden_lines_ensure_tess()) {
                g_hidden_tess_vert_count = 0;
                gluTessBeginPolygon(g_hidden_tess, NULL);
                tess_depth = 1;
            }
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (g_hidden_tess && tess_depth == 1) {
                gluTessBeginContour(g_hidden_tess);
                tess_depth = 2;
            }
            break;
        case CMD_TESS_END:
            if (g_hidden_tess && tess_depth == 2) {
                gluTessEndContour(g_hidden_tess);
                tess_depth = 1;
            } else if (g_hidden_tess && tess_depth == 1) {
                gluTessEndPolygon(g_hidden_tess);
                tess_depth = 0;
            }
            break;
        case CMD_TESS_VERTEX:
            if (g_hidden_tess &&
                tess_depth == 2 &&
                g_hidden_tess_vert_count < GLR_HIDDEN_TESS_VERT_BUF_SIZE) {
                GlrHiddenTessVertex *v =
                    &g_hidden_tess_verts[g_hidden_tess_vert_count++];
                v->pos[0] = cmds[pc].args[0];
                v->pos[1] = cmds[pc].args[1];
                v->pos[2] = cmds[pc].args[2];
                gluTessVertex(g_hidden_tess, v->pos, v);
            }
            break;
        case CMD_GOTO_LABEL:
            break;
        case CMD_GOTO: {
            char label_name[REPL_GOTO_LABEL_MAX];
            if (!repl_extract_goto_label(
                    hidden_lines_flat_text(ctx->text, &cmds[pc]),
                    label_name,
                    sizeof(label_name)))
                break;
            if (goto_count++ > REPL_GOTO_LOOP_LIMIT) {
                if (ctx->status_out && ctx->status_out_sz > 0)
                    snprintf(ctx->status_out, (size_t)ctx->status_out_sz,
                             "goto: loop limit reached");
                goto execute_done;
            }
            for (int label_idx = 0; label_idx < cmd_count; label_idx++) {
                if (cmds[label_idx].valid &&
                    cmds[label_idx].type == CMD_GOTO_LABEL) {
                    char target_label[REPL_GOTO_LABEL_MAX];
                    if (repl_extract_label_name(
                            hidden_lines_flat_text(ctx->text,
                                                   &cmds[label_idx]),
                            target_label,
                            sizeof(target_label)) &&
                        strcmp(target_label, label_name) == 0) {
                        pc = label_idx;
                        goto goto_done;
                    }
                }
            }
goto_done:
            break;
        }
        case CMD_IF_BEGIN: {
            float cond = cmds[pc].args[0];
            if (cmds[pc].has_vars) {
                const FlatCmdLocalVars *local_vars =
                    hidden_lines_local_vars_at(program, pc);
                const ExprVar *eval_vars = g_predef_vars;
                int eval_num_vars = g_num_predef_vars;
                if (local_vars && local_vars->num_vars > 0) {
                    eval_vars = local_vars->vars;
                    eval_num_vars = local_vars->num_vars;
                }
                cond = repl_eval_if_condition(
                    hidden_lines_flat_text(ctx->text, &cmds[pc]),
                    eval_vars,
                    eval_num_vars,
                    cond);
            }
            if (cond == 0.0f) {
                int if_depth = 1;
                while (if_depth > 0 && ++pc < cmd_count) {
                    if (cmds[pc].type == CMD_IF_BEGIN)
                        if_depth++;
                    else if (cmds[pc].type == CMD_IF_END)
                        if_depth--;
                }
            }
            break;
        }
        case CMD_IF_END:
            break;
        default:
            hidden_lines_apply_runtime_effect(&cmds[pc]);
            break;
        }
        pc++;
    }

execute_done:
    if (in_begin == 1)
        glEnd();
    if (g_hidden_tess) {
        if (tess_depth == 2) {
            gluTessEndContour(g_hidden_tess);
            tess_depth = 1;
        }
        if (tess_depth == 1)
            gluTessEndPolygon(g_hidden_tess);
    }
    unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}
