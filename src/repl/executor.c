/*
 * src/repl/executor.c -- Flat command execution, GLUtesselator resource lifetimes,
 * and execution-time state helpers.
 */
#include "repl/executor.h"
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/state_owners.h"
#include "widgets/replay.h"
#include "widgets/replay_state.h"

/* Camera-distance source for the legacy point-size fallback on
 * platforms missing glPointParameterfv. The executor used to call
 * `glr_camera().dist` directly, but that pulled glr_camera.c into
 * the demo link set even though glr_camera is app-shell state.
 *
 * Step 7e: the controller installs a callback that returns the
 * current camera distance. The demo (and any caller without
 * point-attenuation) leaves the source unset; the fallback then
 * emits `glPointSize(sz)` unchanged. The callback storage is
 * always present (not gated on NO_POINT_PARAMETER) so callers can
 * install unconditionally without #ifdef'ing the install site. */
static ReplExecutorCameraDistanceFn g_camera_distance_source = NULL;

void repl_executor_install_camera_distance_source(ReplExecutorCameraDistanceFn fn) {
    g_camera_distance_source = fn;
}

ReplExecutorCameraDistanceFn repl_executor_camera_distance_source(void) {
    return g_camera_distance_source;
}

#define EXEC_RENDER (repl_state_render_mut())
#define g_lights      (EXEC_RENDER->lights)
#define g_clear_color (EXEC_RENDER->clear_color)

static GLUtesselator *g_tess = NULL;
static TessVertex     g_tess_verts[TESS_VERT_BUF_SIZE];
static int            g_tess_vert_count = 0;

/* Execution context adjusted by replay.c for fade-batch rendering. */
static float g_execute_alpha_scale = 1.0f;

/* Skip expensive geometry-emitting commands (vertices, quadrics, tess) for
 * pc < this value. State-setting commands (transforms, color, enable, var
 * assign, etc.) still run so the GL state at pc == skip_before matches a
 * full walk from 0. Used by the replay fade pass to skip all primitives
 * that sit before the batch's active region; the caller pulls skip_before
 * back to the enclosing CMD_BEGIN / CMD_TESS_BEGIN_POLYGON when vertex-mode
 * batches land mid-primitive, so the full primitive still renders. */
static int g_execute_skip_geom_before_pc = 0;

#ifdef NO_POINT_PARAMETER
/* Approximate glPointParameterfv distance attenuation by scaling every
 * glPointSize call by 2/cam_dist. Defined here so the override only affects
 * the executor's user-facing CMD_POINT_SIZE dispatch.
 *
 * Reads cam_dist from the controller-installed source. With no
 * source installed (the demo, or any embedder without an app-shell
 * camera), cam_dist defaults to 0 and the call passes `sz` through
 * unchanged. */
static void _repl_point_size(GLfloat sz) {
    float cam_dist = g_camera_distance_source ? g_camera_distance_source() : 0.0f;
    glPointSize(cam_dist > 0.0f ? sz * (2.0f / (0.5 * cam_dist)) : sz);
}
#define glPointSize _repl_point_size
#endif

static void repl_render_tess_vtx_begin_cb(GLenum mode) {
    glBegin(mode);
}

static void repl_render_tess_vtx_end_cb(void) {
    glEnd();
}

static void repl_render_tess_vtx_cb(void *vertex_data) {
    TessVertex *v = (TessVertex *)vertex_data;
    glNormal3dv(v->normal);
    glColor4dv(v->color);
    glVertex3dv(v->pos);
}

static void repl_render_tess_comb_cb(GLdouble coords[3],
                                     void *vertex_data[4],
                                     GLfloat weight[4],
                                     void **out_data) {
    if (g_tess_vert_count >= TESS_VERT_BUF_SIZE) {
        *out_data = NULL;
        return;
    }
    TessVertex *v = &g_tess_verts[g_tess_vert_count++];
    v->pos[0] = coords[0];
    v->pos[1] = coords[1];
    v->pos[2] = coords[2];
    for (int c = 0; c < 3; c++)
        v->normal[c] = 0.0;
    for (int c = 0; c < 4; c++)
        v->color[c] = 0.0;
    for (int j = 0; j < 4; j++) {
        if (!vertex_data[j])
            continue;
        TessVertex *src = (TessVertex *)vertex_data[j];
        for (int c = 0; c < 3; c++)
            v->normal[c] += weight[j] * src->normal[c];
        for (int c = 0; c < 4; c++)
            v->color[c] += weight[j] * src->color[c];
    }
    double len = sqrt(v->normal[0] * v->normal[0] +
                      v->normal[1] * v->normal[1] +
                      v->normal[2] * v->normal[2]);
    if (len > 1e-9) {
        v->normal[0] /= len;
        v->normal[1] /= len;
        v->normal[2] /= len;
    }
    *out_data = v;
}

static void repl_render_tess_err_cb(GLenum err) {
    (void)err;
}

void repl_executor_destroy_resources(void) {
    if (g_tess) {
        gluDeleteTess(g_tess);
        g_tess = NULL;
    }
    g_tess_vert_count = 0;
}

void repl_executor_init_resources(void) {
    repl_executor_destroy_resources();

    g_tess = gluNewTess();
    gluTessCallback(g_tess, GLU_TESS_BEGIN,
                    (void (*)())repl_render_tess_vtx_begin_cb);
    gluTessCallback(g_tess, GLU_TESS_END,
                    (void (*)())repl_render_tess_vtx_end_cb);
    gluTessCallback(g_tess, GLU_TESS_VERTEX,
                    (void (*)())repl_render_tess_vtx_cb);
    gluTessCallback(g_tess, GLU_TESS_COMBINE,
                    (void (*)())repl_render_tess_comb_cb);
    gluTessCallback(g_tess, GLU_TESS_ERROR,
                    (void (*)())repl_render_tess_err_cb);
    gluTessCallback(g_tess, GLU_TESS_EDGE_FLAG, (void (*)())glEdgeFlag);
}

void repl_copy_predef_values(float *dst, int max_vals) {
    int n;

    if (!dst || max_vals <= 0)
        return;

    n = g_num_predef_vars < max_vals ? g_num_predef_vars : max_vals;
    for (int i = 0; i < n; i++)
        dst[i] = g_predef_vars[i].value;
}

void repl_restore_predef_values(const float *src, int max_vals) {
    int n;

    if (!src || max_vals <= 0)
        return;

    n = g_num_predef_vars < max_vals ? g_num_predef_vars : max_vals;
    for (int i = 0; i < n; i++)
        g_predef_vars[i].value = src[i];
}

void repl_execute_set_fade_context(float alpha_scale, int skip_geom_before_pc) {
    g_execute_alpha_scale = alpha_scale;
    g_execute_skip_geom_before_pc = skip_geom_before_pc;
}

static void repl_executor_apply_transform_cmd(const GLCmd *cmd) {
    if (!cmd)
        return;

    switch (cmd->type) {
    case CMD_TRANSLATE3F:
        glTranslatef(cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_SCALEF:
        glScalef(cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_ROTATEF:
        glRotatef(cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
        break;
    case CMD_PUSH_MATRIX:
        glPushMatrix();
        break;
    case CMD_POP_MATRIX:
        glPopMatrix();
        break;
    case CMD_LOAD_IDENTITY:
        glLoadIdentity();
        break;
    default:
        break;
    }
}

void repl_executor_apply_tracked_transform_cmd(const GLCmd *cmd, int *matrix_depth) {
    if (!cmd)
        return;

    switch (cmd->type) {
    case CMD_PUSH_MATRIX:
        glPushMatrix();
        if (matrix_depth)
            (*matrix_depth)++;
        break;
    case CMD_POP_MATRIX:
        if (!matrix_depth || *matrix_depth > 0) {
            glPopMatrix();
            if (matrix_depth)
                (*matrix_depth)--;
        }
        break;
    default:
        repl_executor_apply_transform_cmd(cmd);
        break;
    }
}

void repl_executor_unwind_tracked_transform_stack(int *matrix_depth) {
    if (!matrix_depth)
        return;

    while (*matrix_depth > 0) {
        glPopMatrix();
        (*matrix_depth)--;
    }
}

int apply_state_cmd(const GLCmd *cmd, float alpha_scale) {
    if (!cmd)
        return 0;

    switch (cmd->type) {
    case CMD_ENABLE:
        glEnable(cmd->mode);
        for (int light_idx = 0; light_idx < MAX_LIGHTS; light_idx++)
            if (g_lights[light_idx].id == cmd->mode)
                g_lights[light_idx].enabled = 1;
        return 1;
    case CMD_DISABLE:
        glDisable(cmd->mode);
        for (int light_idx = 0; light_idx < MAX_LIGHTS; light_idx++)
            if (g_lights[light_idx].id == cmd->mode)
                g_lights[light_idx].enabled = 0;
        return 1;
    case CMD_SHADE_MODEL:
        glShadeModel(cmd->mode);
        return 1;
    case CMD_COLOR_MATERIAL:
        glColorMaterial(cmd->mode, (GLenum)cmd->args[0]);
        return 1;
    case CMD_MATERIALF:
        if (cmd->num_args == 2) {
            glMaterialf(cmd->mode, (GLenum)cmd->args[0], cmd->args[1]);
        } else if (cmd->num_args == 5) {
            GLfloat mat[4] = {
                cmd->args[1], cmd->args[2], cmd->args[3],
                cmd->args[4] * alpha_scale
            };
            glMaterialfv(cmd->mode, (GLenum)cmd->args[0], mat);
        }
        return 1;
    case CMD_LIGHT_MODEL_I:
        glLightModeli(cmd->mode, (GLint)cmd->args[0]);
        return 1;
    case CMD_FRONT_FACE:
        glFrontFace(cmd->mode);
        return 1;
    case CMD_DEPTH_MASK:
        glDepthMask((GLboolean)cmd->mode);
        return 1;
    case CMD_POINT_PARAMETER_FV: {
        GLfloat params[3] = { cmd->args[0], cmd->args[1], cmd->args[2] };
        glPointParameterfv(cmd->mode, params);
        return 1;
    }
    case CMD_BLEND_FUNC:
        glBlendFunc(cmd->mode, (GLenum)cmd->args[0]);
        return 1;
    default:
        return 0;
    }
}

FlatProgramView repl_flat_program_view_live(void) {
    return repl_state_flat_program_view();
}

static FlatProgramView execution_program_from_options(const ReplExecutionOptions *options) {
    FlatProgramView program = repl_flat_program_view_live();

    if (options && options->program.cmds) {
        program = options->program;
        if (program.cmd_count < 0)
            program.cmd_count = 0;
    }
    if (!program.cmds)
        program.cmd_count = 0;
    return program;
}

static int execution_flat_count_from_options(const ReplExecutionOptions *options,
                                             FlatProgramView program) {
    int flat_cmd_count = options ? options->flat_cmd_count : program.cmd_count;

    if (flat_cmd_count < 0)
        flat_cmd_count = 0;
    if (flat_cmd_count > program.cmd_count)
        flat_cmd_count = program.cmd_count;
    return flat_cmd_count;
}

static FlatCmdLocalVars *execution_local_vars_at(FlatProgramView program,
                                                 int flat_cmd_idx) {
    if (!program.local_vars || flat_cmd_idx < 0 || flat_cmd_idx >= program.cmd_count)
        return NULL;
    return &program.local_vars[flat_cmd_idx];
}

static const char *execution_flat_text(SourceTextView text,
                                       const GLCmd *flat_cmd) {
    int src_cmd_idx;

    if (!flat_cmd)
        return "";

    src_cmd_idx = flat_cmd->src_cmd_idx;
    if (src_cmd_idx < 0 || src_cmd_idx >= repl_state_document_count())
        return "";

    {
        const char *line = source_text_line(text, src_cmd_idx);
        return (line && line[0]) ? line : "";
    }
}

/* Walk flat_cmds[0..flat_cmd_count) and issue the corresponding GL
 * calls. Handles vertex submission, state changes, GLU quadrics and
 * tessellator commands, transforms, goto/label control flow, if-block
 * evaluation, and variable assignments.
 *
 * Replay and fade passes provide an explicit limit instead of temporarily
 * mutating repl_state_flat_program_count(). */
void repl_execute_program(const ReplExecutionOptions *options) {
    FlatProgramView program = execution_program_from_options(options);
    const GLCmd *flat_cmds = program.cmds;
    int flat_cmd_count = execution_flat_count_from_options(options, program);
    SourceTextView text = options ? options->text : (SourceTextView){0};
    int in_begin = 0;
    int tess_depth = 0; /* 0=outside, 1=in polygon, 2=in contour */
    int matrix_depth = 0;
    GLdouble tess_current_normal[3] = {0.0, 0.0, 1.0};
    GLdouble tess_current_color[4]  = {1.0, 1.0, 1.0, 1.0};
    int goto_count = 0; /* safety guard against infinite goto loops */

    tess_current_color[3] = g_execute_alpha_scale;

    int pc = 0;
    while (pc < flat_cmd_count) {
        if (!flat_cmds[pc].valid) { pc++; continue; }
        if (repl_cmd_is_transform(flat_cmds[pc].type)) {
            repl_executor_apply_tracked_transform_cmd(&flat_cmds[pc], &matrix_depth);
            pc++;
            continue;
        }
        if (pc < g_execute_skip_geom_before_pc) {
            /* Prefix walk: accumulate state but skip the expensive geometry.
             * Structural commands (CMD_BEGIN, CMD_END, CMD_TESS_BEGIN_POLYGON,
             * CMD_TESS_BEGIN_CONTOUR, CMD_TESS_END) are preserved so that in
             * REPLAY_MODE_VERTEX - where old_pc/new_pc may fall inside an open
             * begin/tess block - execute_commands still enters the right scope
             * before emitting the incremental vertices that live at
             * pc >= g_execute_skip_geom_before_pc. */
            switch (flat_cmds[pc].type) {
            case CMD_VERTEX3F:
            case CMD_VERTEX2F:
            case CMD_GLUT_TORUS:
            case CMD_GLUT_CUBE:
            case CMD_GLUT_SPHERE:
            case CMD_GLUT_TEAPOT:
            case CMD_GLUT_CONE:
            case CMD_TESS_VERTEX:
                pc++;
                continue;
            default:
                break;
            }
        }
        switch (flat_cmds[pc].type) {
        case CMD_BEGIN:
            if (in_begin) glEnd();
            glBegin(flat_cmds[pc].mode);
            in_begin = 1;
            break;
        case CMD_END:
            if (in_begin) { glEnd(); in_begin = 0; }
            break;
        case CMD_VERTEX3F:
            if (in_begin)
                glVertex3f(flat_cmds[pc].args[0], flat_cmds[pc].args[1],
                           flat_cmds[pc].args[2]);
            break;
        case CMD_NORMAL3F:
            glNormal3f(flat_cmds[pc].args[0], flat_cmds[pc].args[1],
                       flat_cmds[pc].args[2]);
            break;
        case CMD_COLOR3F:
            glColor4f(flat_cmds[pc].args[0], flat_cmds[pc].args[1],
                      flat_cmds[pc].args[2], g_execute_alpha_scale);
            break;
        case CMD_COLOR4F:
            glColor4f(flat_cmds[pc].args[0], flat_cmds[pc].args[1],
                      flat_cmds[pc].args[2],
                      flat_cmds[pc].args[3] * g_execute_alpha_scale);
            break;
        case CMD_ENABLE:
        case CMD_DISABLE:
        case CMD_SHADE_MODEL:
        case CMD_COLOR_MATERIAL:
        case CMD_MATERIALF:
        case CMD_LIGHT_MODEL_I:
            apply_state_cmd(&flat_cmds[pc], g_execute_alpha_scale);
            break;
        case CMD_VERTEX2F:
            if (in_begin)
                glVertex2f(flat_cmds[pc].args[0], flat_cmds[pc].args[1]);
            break;
        case CMD_FRONT_FACE:
        case CMD_DEPTH_MASK:
            apply_state_cmd(&flat_cmds[pc], g_execute_alpha_scale);
            break;
        case CMD_POINT_SIZE:
            if (in_begin) { glEnd(); in_begin = 0; }
            glPointSize(flat_cmds[pc].args[0]);
            break;
        case CMD_LINE_WIDTH:
            if (in_begin) { glEnd(); in_begin = 0; }
            glLineWidth(flat_cmds[pc].args[0]);
            break;
        case CMD_POINT_PARAMETER_FV:
        case CMD_BLEND_FUNC:
            if (in_begin) { glEnd(); in_begin = 0; }
            apply_state_cmd(&flat_cmds[pc], g_execute_alpha_scale);
            break;
        case CMD_CLEAR_COLOR:
            if (in_begin) { glEnd(); in_begin = 0; }
            g_clear_color[0] = flat_cmds[pc].args[0];
            g_clear_color[1] = flat_cmds[pc].args[1];
            g_clear_color[2] = flat_cmds[pc].args[2];
            g_clear_color[3] = flat_cmds[pc].args[3];
            break;
        case CMD_GLUT_TORUS:
            if (in_begin) { glEnd(); in_begin = 0; }
            glutSolidTorus((double)flat_cmds[pc].args[0],
                           (double)flat_cmds[pc].args[1],
                           (int)flat_cmds[pc].args[2],
                           (int)flat_cmds[pc].args[3]);
            break;
        case CMD_GLUT_CUBE:
            if (in_begin) { glEnd(); in_begin = 0; }
            glutSolidCube((double)flat_cmds[pc].args[0]);
            break;
        case CMD_GLUT_SPHERE:
            if (in_begin) { glEnd(); in_begin = 0; }
            glutSolidSphere((double)flat_cmds[pc].args[0],
                            (int)flat_cmds[pc].args[1],
                            (int)flat_cmds[pc].args[2]);
            break;
        case CMD_GLUT_TEAPOT:
            if (in_begin) { glEnd(); in_begin = 0; }
            glutSolidTeapot((double)flat_cmds[pc].args[0]);
            break;
        case CMD_GLUT_CONE:
            if (in_begin) { glEnd(); in_begin = 0; }
            glutSolidCone((double)flat_cmds[pc].args[0],
                          (double)flat_cmds[pc].args[1],
                          (int)flat_cmds[pc].args[2],
                          (int)flat_cmds[pc].args[3]);
            break;
        case CMD_RASTER_POS3F:
            if (in_begin) { glEnd(); in_begin = 0; }
            glRasterPos3f(flat_cmds[pc].args[0],
                          flat_cmds[pc].args[1],
                          flat_cmds[pc].args[2]);
            break;
        case CMD_LABEL: {
            /* `label("fmt", a, b, c, d)` — printf-style text emission
             * at the current raster position. Position is set by a
             * preceding glRasterPos3f; this command does not touch
             * it, so the call composes cleanly with whatever
             * modelview/raster state the user has set up.
             *
             * Format substitution walks cmd.text, expanding %f from
             * args[0..3] and %% to a literal '%'. The flatten pass
             * keeps args[] in sync with current variable values for
             * has_vars commands, so we use them directly. */
            if (in_begin) { glEnd(); in_begin = 0; }
            char buf[128];
            int sub_count = flat_cmds[pc].num_args;
            if (sub_count < 0) sub_count = 0;
            int sub_idx = 0;
            int off = 0;
            const char *fmt = flat_cmds[pc].text;
            while (*fmt && off < (int)sizeof(buf) - 1) {
                if (fmt[0] == '%' && fmt[1] == 'f' && sub_idx < sub_count) {
                    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                                    "%g", (double)flat_cmds[pc].args[sub_idx]);
                    if (off >= (int)sizeof(buf)) off = (int)sizeof(buf) - 1;
                    sub_idx++;
                    fmt += 2;
                } else if (fmt[0] == '%' && fmt[1] == '%') {
                    buf[off++] = '%';
                    fmt += 2;
                } else {
                    buf[off++] = *fmt++;
                }
            }
            buf[off] = '\0';
            for (const char *c = buf; *c; c++)
                glutBitmapCharacter(GLUT_BITMAP_9_BY_15, (unsigned char)*c);
            break;
        }
        case CMD_TESS_BEGIN_POLYGON:
            if (in_begin) { glEnd(); in_begin = 0; }
            if (g_tess) { g_tess_vert_count = 0; gluTessBeginPolygon(g_tess, NULL); tess_depth = 1; }
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (g_tess && tess_depth == 1) { gluTessBeginContour(g_tess); tess_depth = 2; }
            break;
        case CMD_TESS_END:
            if (g_tess && tess_depth == 2) { gluTessEndContour(g_tess); tess_depth = 1; }
            else if (g_tess && tess_depth == 1) { gluTessEndPolygon(g_tess); tess_depth = 0; }
            break;
        case CMD_TESS_NORMAL:
            tess_current_normal[0] = flat_cmds[pc].args[0];
            tess_current_normal[1] = flat_cmds[pc].args[1];
            tess_current_normal[2] = flat_cmds[pc].args[2];
            break;
        case CMD_TESS_COLOR:
            tess_current_color[0] = flat_cmds[pc].args[0];
            tess_current_color[1] = flat_cmds[pc].args[1];
            tess_current_color[2] = flat_cmds[pc].args[2];
            tess_current_color[3] = ((flat_cmds[pc].num_args >= 4)
                                   ? flat_cmds[pc].args[3] : 1.0)
                                  * g_execute_alpha_scale;
            break;
        case CMD_TESS_VERTEX:
            if (g_tess && tess_depth == 2 && g_tess_vert_count < TESS_VERT_BUF_SIZE) {
                TessVertex *v = &g_tess_verts[g_tess_vert_count++];
                v->pos[0] = flat_cmds[pc].args[0];
                v->pos[1] = flat_cmds[pc].args[1];
                v->pos[2] = flat_cmds[pc].args[2];
                memcpy(v->normal, tess_current_normal, sizeof(v->normal));
                memcpy(v->color,  tess_current_color,  sizeof(v->color));
                gluTessVertex(g_tess, v->pos, v);
            }
            break;
        case CMD_GOTO_LABEL:
            break; /* no-op marker */
        case CMD_GOTO: {
            /* Experimental top-level control-flow only.
             * This jumps the flat-command program counter, but it does not
             * rebuild or re-specialize the flat stream, so goto loops are only
             * reliable for control flow and assignments. Variable-driven GL
             * commands still use the args baked into flat_cmds[]. Replay also
             * cannot follow the dynamic jump trace. */
            char label_name[64];
            if (!repl_extract_goto_label(execution_flat_text(text, &flat_cmds[pc]),
                                         label_name, sizeof(label_name)))
                break;
            if (goto_count++ > 100000) {
                repl_set_status("goto: loop limit reached");
                goto execute_done;
            }
            for (int label_idx = 0; label_idx < flat_cmd_count; label_idx++) {
                if (flat_cmds[label_idx].valid &&
                    flat_cmds[label_idx].type == CMD_GOTO_LABEL) {
                    char target_label[64];
                    if (repl_extract_label_name(execution_flat_text(text, &flat_cmds[label_idx]),
                                                target_label,
                                                sizeof(target_label)) &&
                        strcmp(target_label, label_name) == 0) {
                        pc = label_idx; /* outer pc++ steps past the label */
                        goto goto_done;
                    }
                }
            }
            goto_done:;
            break;
        }
        case CMD_IF_BEGIN: {
            /* Evaluate condition at execute time so goto loops see updated vars. */
            float cond = flat_cmds[pc].args[0];
            if (flat_cmds[pc].has_vars) {
                char cond_text[MAX_LINE_LEN] = "";
                FlatCmdLocalVars *local_vars =
                    execution_local_vars_at(program, pc);
                ExprVar *eval_vars = g_predef_vars;
                int eval_num_vars = g_num_predef_vars;
                if (local_vars && local_vars->num_vars > 0) {
                    eval_vars = local_vars->vars;
                    eval_num_vars = local_vars->num_vars;
                }
                if (repl_extract_paren_payload(execution_flat_text(text, &flat_cmds[pc]),
                                               cond_text, sizeof(cond_text)) &&
                    cond_text[0]) {
                    char repl_cond[MAX_LINE_LEN];
                    repl_eval_c_expr_to_repl(cond_text, repl_cond, sizeof(repl_cond));
                    ExprCtx ctx = { repl_cond, eval_vars, eval_num_vars };
                    cond = repl_eval_expr(&ctx);
                }
            }
            if (cond == 0.0f) {
                int if_depth = 1;
                while (if_depth > 0 && ++pc < flat_cmd_count) {
                    if (flat_cmds[pc].type == CMD_IF_BEGIN) if_depth++;
                    else if (flat_cmds[pc].type == CMD_IF_END) if_depth--;
                }
                /* pc now points to CMD_IF_END; outer pc++ steps past it. */
            }
            break;
        }
        case CMD_IF_END:
            break; /* body executed; just step past */
        case CMD_VAR_ASSIGN: {
            /* Re-apply variable assignment so goto loops see updated values. */
            int var_idx = flat_cmds[pc].num_args;
            float value = flat_cmds[pc].args[0];
            if (flat_cmds[pc].has_vars) {
                char rhs[MAX_LINE_LEN] = "";
                if (repl_extract_assignment_parts(execution_flat_text(text, &flat_cmds[pc]), NULL, 0,
                                                  rhs, sizeof(rhs)) && rhs[0]) {
                    FlatCmdLocalVars *local_vars =
                        execution_local_vars_at(program, pc);
                    ExprVar *eval_vars = g_predef_vars;
                    int eval_num_vars = g_num_predef_vars;
                    if (local_vars && local_vars->num_vars > 0) {
                        eval_vars = local_vars->vars;
                        eval_num_vars = local_vars->num_vars;
                    }
                    char repl_rhs[MAX_LINE_LEN];
                    repl_eval_c_expr_to_repl(rhs, repl_rhs, sizeof(repl_rhs));
                    ExprCtx ctx = { repl_rhs, eval_vars, eval_num_vars };
                    value = repl_eval_expr(&ctx);
                }
            }
            if (var_idx >= 0 && var_idx < g_num_predef_vars)
                g_predef_vars[var_idx].value = value;
            break;
        }
        case CMD_SCRATCH_ASSIGN: {
            int array_idx = (int)flat_cmds[pc].args[0];
            int elem_idx = (int)flat_cmds[pc].args[1];
            float value = flat_cmds[pc].args[2];
            if (flat_cmds[pc].has_vars) {
                char name[16] = "";
                char index_expr[MAX_LINE_LEN] = "";
                char rhs[MAX_LINE_LEN] = "";
                if (repl_extract_assignment_target_parts(execution_flat_text(text, &flat_cmds[pc]),
                                                        name, sizeof(name),
                                                        index_expr, sizeof(index_expr),
                                                        rhs, sizeof(rhs)) &&
                    index_expr[0] && rhs[0]) {
                    FlatCmdLocalVars *local_vars =
                        execution_local_vars_at(program, pc);
                    ExprVar *eval_vars = g_predef_vars;
                    int eval_num_vars = g_num_predef_vars;
                    if (local_vars && local_vars->num_vars > 0) {
                        eval_vars = local_vars->vars;
                        eval_num_vars = local_vars->num_vars;
                    }
                    char repl_index[MAX_LINE_LEN];
                    char repl_rhs[MAX_LINE_LEN];
                    repl_eval_c_expr_to_repl(index_expr, repl_index, sizeof(repl_index));
                    repl_eval_c_expr_to_repl(rhs, repl_rhs, sizeof(repl_rhs));
                    ExprCtx index_ctx = { repl_index, eval_vars, eval_num_vars, NULL, 0 };
                    ExprCtx rhs_ctx = { repl_rhs, eval_vars, eval_num_vars, NULL, 0 };
                    elem_idx = (int)repl_eval_expr(&index_ctx);
                    value = repl_eval_expr(&rhs_ctx);
                }
            }
            repl_eval_scratch_set(array_idx, elem_idx, value);
            break;
        }
        /* Transforms handled by repl_cmd_is_transform() early-continue above. */
        case CMD_TRANSLATE3F: case CMD_SCALEF: case CMD_ROTATEF:
        case CMD_PUSH_MATRIX: case CMD_POP_MATRIX:
        case CMD_LOAD_IDENTITY:
        /* These are resolved during flatten and should not appear in flat_cmds. */
        case CMD_FOR_BEGIN: case CMD_FOR_END:
        case CMD_FUNC_DEF: case CMD_FUNC_END: case CMD_CALL:
        case CMD_COMMENT:
        case CMD_EMPTY:
        case CMD_VAR_DECLARE:
        case CMD_TYPE_COUNT:
            break;
        }
        pc++;
    }
execute_done:
    if (in_begin) glEnd();
    if (!(replay_active() && replay_mode() == REPLAY_MODE_VERTEX)) {
        if (tess_depth == 2 && g_tess) { gluTessEndContour(g_tess); tess_depth = 1; }
        if (tess_depth == 1 && g_tess) { gluTessEndPolygon(g_tess); }
    }
    repl_executor_unwind_tracked_transform_stack(&matrix_depth);
}

void repl_execute_commands(void) {
    /* Test/legacy entry point. The executor's goto-label and
     * paren-payload helpers route through the editor-text view; pass
     * the live buffer view so those features work the same as the
     * controller-driven path. */
    ReplExecutionOptions options = {
        .flat_cmd_count = repl_state_flat_program_count(),
        .program        = repl_state_flat_program_view(),
        .text           = source_document_view(),
    };
    repl_execute_program(&options);
}
