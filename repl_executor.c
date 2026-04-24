/*
 * repl_executor.c -- Flat command execution and execution-time state helpers.
 */
#include "sample.h"
#include "repl_core_internal.h"
#include "repl_state.h"

/* Execution context adjusted by repl_replay.c for fade-batch rendering. */
static float g_execute_alpha_scale = 1.0f;

/* Skip expensive geometry-emitting commands (vertices, quadrics, tess) for
 * pc < this value. State-setting commands (transforms, color, enable, var
 * assign, etc.) still run so the GL state at pc == skip_before matches a
 * full walk from 0. Used by the replay fade pass to skip all primitives
 * that sit before the batch's active region; the caller pulls skip_before
 * back to the enclosing CMD_BEGIN / CMD_TESS_BEGIN_POLYGON when vertex-mode
 * batches land mid-primitive, so the full primitive still renders. */
static int g_execute_skip_geom_before_pc = 0;

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
        if (is_transform_cmd(flat_cmds[pc].type)) {
            apply_tracked_transform_cmd(&flat_cmds[pc], &matrix_depth);
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
            case CMD_GLU_SPHERE:
            case CMD_GLU_CYLINDER:
            case CMD_GLU_DISK:
            case CMD_GLU_PARTIAL_DISK:
            case CMD_GLUT_TORUS:
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
        case CMD_GLU_SPHERE:
            if (in_begin) { glEnd(); in_begin = 0; }
            if (g_quadric)
                gluSphere(g_quadric,
                          (double)flat_cmds[pc].args[0],
                          (int)flat_cmds[pc].args[1],
                          (int)flat_cmds[pc].args[2]);
            break;
        case CMD_GLU_CYLINDER:
            if (in_begin) { glEnd(); in_begin = 0; }
            if (g_quadric)
                gluCylinder(g_quadric,
                            (double)flat_cmds[pc].args[0],
                            (double)flat_cmds[pc].args[1],
                            (double)flat_cmds[pc].args[2],
                            (int)flat_cmds[pc].args[3],
                            (int)flat_cmds[pc].args[4]);
            break;
        case CMD_GLU_DISK:
            if (in_begin) { glEnd(); in_begin = 0; }
            if (g_quadric)
                gluDisk(g_quadric,
                        (double)flat_cmds[pc].args[0],
                        (double)flat_cmds[pc].args[1],
                        (int)flat_cmds[pc].args[2],
                        (int)flat_cmds[pc].args[3]);
            break;
        case CMD_GLU_PARTIAL_DISK:
            if (in_begin) { glEnd(); in_begin = 0; }
            if (g_quadric)
                gluPartialDisk(g_quadric,
                               (double)flat_cmds[pc].args[0],
                               (double)flat_cmds[pc].args[1],
                               (int)flat_cmds[pc].args[2],
                               (int)flat_cmds[pc].args[3],
                               (double)flat_cmds[pc].args[4],
                               (double)flat_cmds[pc].args[5]);
            break;
        case CMD_GLUT_TORUS:
            if (in_begin) { glEnd(); in_begin = 0; }
            glutSolidTorus((double)flat_cmds[pc].args[0],
                           (double)flat_cmds[pc].args[1],
                           (int)flat_cmds[pc].args[2],
                           (int)flat_cmds[pc].args[3]);
            break;
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
        case CMD_LABEL:
            break; /* no-op marker */
        case CMD_GOTO: {
            /* Experimental top-level control-flow only.
             * This jumps the flat-command program counter, but it does not
             * rebuild or re-specialize the flat stream, so goto loops are only
             * reliable for control flow and assignments. Variable-driven GL
             * commands still use the args baked into flat_cmds[]. Replay also
             * cannot follow the dynamic jump trace. */
            char label_name[64];
            if (!repl_extract_goto_label(flat_cmds[pc].source,
                                         label_name, sizeof(label_name)))
                break;
            if (goto_count++ > 100000) {
                set_status("goto: loop limit reached");
                goto execute_done;
            }
            for (int label_idx = 0; label_idx < flat_cmd_count; label_idx++) {
                if (flat_cmds[label_idx].valid &&
                    flat_cmds[label_idx].type == CMD_LABEL) {
                    char target_label[64];
                    if (repl_extract_label_name(flat_cmds[label_idx].source,
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
                if (repl_extract_paren_payload(flat_cmds[pc].source,
                                               cond_text, sizeof(cond_text)) &&
                    cond_text[0]) {
                    char repl_cond[MAX_LINE_LEN];
                    c_expr_to_repl(cond_text, repl_cond, sizeof(repl_cond));
                    ExprCtx ctx = { repl_cond, eval_vars, eval_num_vars };
                    cond = eval_expr(&ctx);
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
                if (repl_extract_assignment_parts(flat_cmds[pc].source, NULL, 0,
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
                    c_expr_to_repl(rhs, repl_rhs, sizeof(repl_rhs));
                    ExprCtx ctx = { repl_rhs, eval_vars, eval_num_vars };
                    value = eval_expr(&ctx);
                }
            }
            if (var_idx >= 0 && var_idx < g_num_predef_vars)
                g_predef_vars[var_idx].value = value;
            break;
        }
        /* Transforms handled by is_transform_cmd() early-continue above. */
        case CMD_TRANSLATE3F: case CMD_SCALEF: case CMD_ROTATEF:
        case CMD_PUSH_MATRIX: case CMD_POP_MATRIX:
        /* These are resolved during flatten and should not appear in flat_cmds. */
        case CMD_FOR_BEGIN: case CMD_FOR_END:
        case CMD_FUNC_DEF: case CMD_FUNC_END: case CMD_CALL:
        case CMD_COMMENT:
        case CMD_VAR_DECLARE:
        case CMD_TYPE_COUNT:
            break;
        }
        pc++;
    }
execute_done:
    if (in_begin) glEnd();
    if (!(g_replay_active && g_replay_mode == REPLAY_MODE_VERTEX)) {
        if (tess_depth == 2 && g_tess) { gluTessEndContour(g_tess); tess_depth = 1; }
        if (tess_depth == 1 && g_tess) { gluTessEndPolygon(g_tess); }
    }
    unwind_tracked_transform_stack(&matrix_depth);
}

void execute_commands(void) {
    repl_execute_program(NULL);
}
