/*
 * src/repl/executor.c -- Flat command execution, GLUtesselator resource lifetimes,
 * and execution-time state helpers.
 */
#include "repl/executor.h"
#include "repl/control_flow.h"
#include "repl/state_owners.h"
#include "repl/text_helpers.h"
#include "support/mesh_ply.h"   /* MESH_PLY_PASS_* feedback normal-encoding markers */

/* Camera-distance source for the point-size fallback used when the
 * runtime GL context lacks glPointParameterfv. The executor used to
 * call `glr_camera().dist` directly, but that pulled glr_camera.c
 * into the demo link set even though glr_camera is app-shell state.
 *
 * The controller installs a callback that returns the current camera
 * distance. The demo (and any caller without point-attenuation)
 * leaves the source unset; the fallback then emits `glPointSize(sz)`
 * unchanged. The callback storage is always present so callers can
 * install unconditionally. */
static ReplExecutorCameraDistanceFn g_camera_distance_source = NULL;

void repl_executor_install_camera_distance_source(ReplExecutorCameraDistanceFn fn) {
    g_camera_distance_source = fn;
}

/* Runtime point-parameter capability. Replaces the old compile-time
 * NO_POINT_PARAMETER macro: glPointParameterfv is core GL 1.4 but
 * absent on some legacy contexts, which is a property of the runtime
 * GL, not the build. The controller detects support post-context
 * (glr_ctrl_init_gl) and sets this; everything else (demo, tests
 * without an explicit set) defaults to supported == today's default
 * build. When unsupported: CMD_POINT_PARAMETER_FV is a no-op and
 * point sizes are scaled by camera distance as a visual stand-in. */
static int g_point_parameter_supported = 1;
/* Runtime-loaded point-parameter entry point. The controller resolves a
 * callable core/ARB/EXT symbol after the GL context is current and
 * installs it here; the executor never calls glPointParameterfv
 * directly on the freeglut/Linux path. */
static ReplExecutorPointParameterProc g_point_parameter_proc = NULL;

void repl_executor_install_point_parameter_proc(ReplExecutorPointParameterProc fn) {
    g_point_parameter_proc = fn;
}

ReplExecutorPointParameterProc repl_executor_point_parameter_proc(void) {
    return g_point_parameter_proc;
}

void repl_executor_set_point_parameter_supported(int supported) {
    g_point_parameter_supported = supported ? 1 : 0;
}

int repl_executor_point_parameter_supported(void) {
    return g_point_parameter_supported;
}

/* Slot index (0..REPL_LIGHT_SLOT_COUNT-1) for a glEnable/glDisable cap, or
 * -1 if `cap` is not one of the tracked GL_LIGHTn ids. The executor records
 * which light slots the program enabled in ReplRenderState;
 * the dimensional light data lives in app state, so the executor never
 * touches positions/colors here. */
static int repl_exec_light_slot_for_cap(GLenum cap) {
    int slot = (int)cap - (int)GL_LIGHT0;
    return (slot >= 0 && slot < REPL_LIGHT_SLOT_COUNT) ? slot : -1;
}

static GLUtesselator *g_tess = NULL;
static TessVertex     g_tess_verts[TESS_VERT_BUF_SIZE];
static int            g_tess_vert_count = 0;



/* User-facing point-size emission. When the runtime lacks
 * glPointParameterfv, approximate its distance attenuation in software by
 * scaling every glPointSize call by REF_DIST/cam_dist — a constant
 * footprint on the model, matching the hardware path's pure-quadratic
 * default (see REPL_POINT_SIZE_REF_DIST). Reads cam_dist from the
 * controller-installed source; with no source installed (the demo, or
 * any embedder without an app-shell camera) cam_dist defaults to 0
 * and `sz` passes through unchanged. When supported, emit `sz`
 * directly — CMD_POINT_PARAMETER_FV handles the real attenuation. */
static void repl_exec_point_size(GLfloat sz) {
    if (!g_point_parameter_supported) {
        float cam_dist = g_camera_distance_source ? g_camera_distance_source() : 0.0f;
        glPointSize(cam_dist > 0.0f ? sz * (REPL_POINT_SIZE_REF_DIST / cam_dist) : sz);
    } else {
        glPointSize(sz);
    }
}

/* gluTessCallback takes a single callback-pointer type but the GLU
 * callbacks have heterogeneous real signatures; GLU re-dispatches by
 * the `which` enum internally. A prototyped pointer keeps the casts
 * C99-clean (-std=c99 -pedantic-errors rejects the old-style
 * `void (*)()`). Self-owned because the real platform <GL/glu.h> does
 * not expose a portable GLUfuncptr typedef. */
typedef void (*ReplGluCallback)(void);

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
                    (ReplGluCallback)repl_render_tess_vtx_begin_cb);
    gluTessCallback(g_tess, GLU_TESS_END,
                    (ReplGluCallback)repl_render_tess_vtx_end_cb);
    gluTessCallback(g_tess, GLU_TESS_VERTEX,
                    (ReplGluCallback)repl_render_tess_vtx_cb);
    gluTessCallback(g_tess, GLU_TESS_COMBINE,
                    (ReplGluCallback)repl_render_tess_comb_cb);
    gluTessCallback(g_tess, GLU_TESS_ERROR,
                    (ReplGluCallback)repl_render_tess_err_cb);
    gluTessCallback(g_tess, GLU_TESS_EDGE_FLAG, (ReplGluCallback)glEdgeFlag);
}

/* repl_copy_predef_values / repl_restore_predef_values live in eval.c
 * next to the rest of the predef-variable storage helpers. */



static void repl_executor_apply_non_stack_transform_cmd(const GLCmd *cmd) {
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
        repl_executor_apply_non_stack_transform_cmd(cmd);
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

void repl_executor_draw_glut_solid(const GLCmd *cmd) {
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

void repl_apply_state_bookkeeping(const GLCmd *cmd) {
    if (!cmd)
        return;

    switch (cmd->type) {
    case CMD_ENABLE: {
        int slot = repl_exec_light_slot_for_cap((GLenum)cmd->args[0]);
        if (slot >= 0)
            repl_state_render_set_light_enabled(slot, 1);
        break;
    }
    case CMD_DISABLE: {
        int slot = repl_exec_light_slot_for_cap((GLenum)cmd->args[0]);
        if (slot >= 0)
            repl_state_render_set_light_enabled(slot, 0);
        break;
    }
    case CMD_CLEAR_COLOR: {
        float rgba[4] = {cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]};
        repl_state_render_set_clear_color(rgba);
        break;
    }
    default:
        break;
    }
}

int repl_apply_state_cmd(const GLCmd *cmd, float alpha_scale) {
    if (!cmd)
        return 0;

    /* REPL render-state side effects (light-enable mask, clear color) live
     * here, in one place, so passes that own GL state themselves and skip the
     * GL emission (the hidden-line wireframe pass) stay in sync by calling
     * repl_apply_state_bookkeeping() directly. No-op for commands that carry
     * no bookkeeping. */
    repl_apply_state_bookkeeping(cmd);

    switch (cmd->type) {
    case CMD_ENABLE:
        glEnable((GLenum)cmd->args[0]);
        return 1;
    case CMD_DISABLE:
        glDisable((GLenum)cmd->args[0]);
        return 1;
    case CMD_SHADE_MODEL:
        glShadeModel((GLenum)cmd->args[0]);
        return 1;
    case CMD_COLOR_MATERIAL:
        glColorMaterial((GLenum)cmd->args[0], (GLenum)cmd->args[1]);
        return 1;
    case CMD_MATERIALFV:
        /* args[0]=face, args[1]=pname, args[2..]=value(s). The source
         * always carries the compound-literal form `(GLfloat[]){...}`,
         * but the executor dispatches to the scalar / vector GL entry
         * point that matches the unpacked count, since GL_SHININESS
         * conceptually takes one float and the RGBA pnames take four. */
        if (cmd->num_args == 3) {
            glMaterialf((GLenum)cmd->args[0], (GLenum)cmd->args[1],
                        cmd->args[2]);
        } else if (cmd->num_args == 6) {
            GLfloat mat[4] = {
                cmd->args[2], cmd->args[3], cmd->args[4],
                cmd->args[5] * alpha_scale
            };
            glMaterialfv((GLenum)cmd->args[0], (GLenum)cmd->args[1], mat);
        }
        return 1;
    case CMD_MATERIALF:
        glMaterialf((GLenum)cmd->args[0], (GLenum)cmd->args[1], cmd->args[2]);
        return 1;
    case CMD_LIGHT_MODEL_I:
        glLightModeli((GLenum)cmd->args[0], (GLint)cmd->args[1]);
        return 1;
    case CMD_FRONT_FACE:
        glFrontFace((GLenum)cmd->args[0]);
        return 1;
    case CMD_DEPTH_FUNC:
        glDepthFunc((GLenum)cmd->args[0]);
        return 1;
    case CMD_DEPTH_MASK:
        glDepthMask((GLboolean)cmd->args[0]);
        return 1;
    case CMD_COLOR_MASK:
        glColorMask((GLboolean)cmd->args[0], (GLboolean)cmd->args[1],
                    (GLboolean)cmd->args[2], (GLboolean)cmd->args[3]);
        return 1;
    case CMD_POINT_PARAMETER_FV: {
        /* args[0]=pname, args[1..3]=const/linear/quadratic. Skipped
         * entirely when the runtime GL lacks glPointParameterfv; the
         * repl_exec_point_size camera-distance scaling stands in
         * visually. */
        if (g_point_parameter_supported && g_point_parameter_proc) {
            GLfloat params[3] = { cmd->args[1], cmd->args[2], cmd->args[3] };
           g_point_parameter_proc((GLenum)cmd->args[0], params);
        }
        return 1;
    }
    case CMD_BLEND_FUNC:
        glBlendFunc((GLenum)cmd->args[0], (GLenum)cmd->args[1]);
        return 1;
    case CMD_CLEAR_COLOR:
        /* Render-state clear color recorded by repl_apply_state_bookkeeping. */
        glClearColor(cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
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

static const FlatCmdLocalVars *execution_local_vars_at(FlatProgramView program,
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

/* Transform an object-space normal `n` to world space by the current
 * modelview `m` (column-major), via the inverse-transpose of its upper-left
 * 3x3, then normalize. Used by the .ply export to encode world-space normals
 * into the texcoord channel so they match the world-space vertex positions
 * (a raw texcoord is not modelview-transformed). Correct for rotation,
 * translation, and uniform/non-uniform scale; export-only, so the per-vertex
 * cost is irrelevant. */
static void exec_normal_to_world(const float m[16], const float n[3],
                                 float out[3]) {
    float a = m[0], b = m[4], c = m[8];
    float d = m[1], e = m[5], f = m[9];
    float g = m[2], h = m[6], i = m[10];
    /* Cofactor matrix = inverse-transpose * det. */
    float c00 = e*i - f*h, c01 = -(d*i - f*g), c02 = d*h - e*g;
    float c10 = -(b*i - c*h), c11 = a*i - c*g, c12 = -(a*h - b*g);
    float c20 = b*f - c*e, c21 = -(a*f - c*d), c22 = a*e - b*d;
    float det = a*c00 + b*c01 + c*c02;
    /* (inverse-transpose) * n = (cofactor * n) / det. */
    float ox = c00*n[0] + c01*n[1] + c02*n[2];
    float oy = c10*n[0] + c11*n[1] + c12*n[2];
    float oz = c20*n[0] + c21*n[1] + c22*n[2];
    if (det != 0.0f) { ox /= det; oy /= det; oz /= det; }
    float len = (float)sqrt((double)(ox*ox + oy*oy + oz*oz));
    if (len > 0.0f) { ox /= len; oy /= len; oz /= len; }
    out[0] = ox; out[1] = oy; out[2] = oz;
}

ReplExecCursor repl_exec_cursor_begin(const ReplExecutionOptions *options) {
    ReplExecCursor cursor;
    memset(&cursor, 0, sizeof(cursor));

    if (options)
        cursor.options = *options;
    cursor.program = execution_program_from_options(options);
    cursor.flat_cmd_count = execution_flat_count_from_options(options,
                                                              cursor.program);
    cursor.text = options ? options->text : (SourceTextView){0};
    cursor.encode_normals = options && options->encode_feedback_normals;
    cursor.cur_normal[2] = 1.0f;
    cursor.begin_mv[0] = 1.0f;
    cursor.begin_mv[5] = 1.0f;
    cursor.begin_mv[10] = 1.0f;
    cursor.begin_mv[15] = 1.0f;
    cursor.tess_current_normal[2] = 1.0;
    cursor.tess_current_color[0] = 1.0;
    cursor.tess_current_color[1] = 1.0;
    cursor.tess_current_color[2] = 1.0;
    cursor.alpha_scale = 1.0f;
    if (options && options->has_fade_context) {
        cursor.alpha_scale = options->fade_alpha_scale;
        cursor.skip_geom_before_pc = options->skip_geom_before_pc;
    }
    cursor.tess_current_color[3] = cursor.alpha_scale;

    /* The light-indicator overlay reads light_enabled_mask for its on/off
     * visual. GL's real default — and scene_lights_setup() — is
     * all-lights-disabled; only the program's glEnable(GL_LIGHTn) turns
     * one on. Reset the bookkeeping at the start of every walk so the
     * indicator tracks what the program actually does, instead of the
     * sticky, default-on value that never reflected enable/disable. */
    repl_state_render_clear_light_enabled_mask();

    return cursor;
}

int repl_exec_cursor_done(const ReplExecCursor *cursor) {
    return (!cursor ||
            cursor->pc < 0 ||
            cursor->pc >= cursor->flat_cmd_count ||
            !cursor->program.cmds);
}

const GLCmd *repl_exec_cursor_peek(const ReplExecCursor *cursor) {
    if (repl_exec_cursor_done(cursor))
        return NULL;
    return &cursor->program.cmds[cursor->pc];
}

void repl_exec_cursor_advance(ReplExecCursor *cursor) {
    if (!cursor || cursor->pc >= cursor->flat_cmd_count)
        return;
    cursor->pc++;
}

int repl_exec_cursor_in_begin(const ReplExecCursor *cursor) {
    return cursor ? cursor->in_begin : 0;
}

static void repl_exec_cursor_warn_unhandled_state(const GLCmd *cmd) {
    static int s_warned_unhandled[CMD_TYPE_COUNT];
    int t;

    if (!cmd)
        return;
    t = (int)cmd->type;
    if (t >= 0 && t < CMD_TYPE_COUNT && !s_warned_unhandled[t]) {
        fprintf(stderr,
                "repl_executor: state cmd %d enumerated in "
                "executor switch but not in "
                "repl_apply_state_cmd\n", t);
        s_warned_unhandled[t] = 1;
    }
}

/* Execute the command at cursor->pc and advance the cursor. The old
 * whole-program executor is intentionally just a begin/step/end loop over
 * this API so cursor users get the same command semantics. */
int repl_exec_cursor_step(ReplExecCursor *cursor) {
    const GLCmd *flat_cmds;
    const GLCmd *cmd;

    if (repl_exec_cursor_done(cursor))
        return 0;

    flat_cmds = cursor->program.cmds;
    cmd = &flat_cmds[cursor->pc];
    if (!cmd->valid) {
        cursor->pc++;
        return 1;
    }
    if (repl_cmd_is_transform(cmd->type)) {
        repl_executor_apply_tracked_transform_cmd(cmd, &cursor->matrix_depth);
        cursor->pc++;
        return 1;
    }
    if (cursor->pc < cursor->skip_geom_before_pc) {
        /* Prefix walk: accumulate state but skip the expensive geometry.
         * Structural commands (CMD_BEGIN, CMD_END, CMD_TESS_BEGIN_POLYGON,
         * CMD_TESS_BEGIN_CONTOUR, CMD_TESS_END) are preserved so that in
         * REPLAY_MODE_VERTEX - where old_pc/new_pc may fall inside an open
         * begin/tess block - execute_commands still enters the right scope
         * before emitting the incremental vertices that live at
         * pc >= skip_geom_before_pc. */
        switch (cmd->type) {
        case CMD_VERTEX3F:
        case CMD_VERTEX2F:
        case CMD_GLUT_TORUS:
        case CMD_GLUT_CUBE:
        case CMD_GLUT_SPHERE:
        case CMD_GLUT_TEAPOT:
        case CMD_GLUT_CONE:
        case CMD_TESS_VERTEX:
            cursor->pc++;
            return 1;
        default:
            break;
        }
    }
    switch (cmd->type) {
    case CMD_BEGIN:
        /* Parser rejects nested glBegin via repl_cmd_type_valid_in_begin.
         * Export: mark the primitive's texcoords as authored normals
         * (out of band, before glBegin so it is outside Begin/End). */
        if (cursor->encode_normals) {
            glPassThrough((GLfloat)MESH_PLY_PASS_NORMALS);
            /* The modelview is constant within a begin/end block (the
             * parser rejects transforms inside), and glGet is illegal
             * between glBegin/glEnd — so snapshot it here, once, and reuse
             * it for every vertex's normal transform in the block. */
            glGetFloatv(GL_MODELVIEW_MATRIX, cursor->begin_mv);
        }
        glBegin((GLenum)cmd->args[0]);
        cursor->in_begin = 1;
        break;
    case CMD_END:
        if (cursor->in_begin) {
            glEnd();
            cursor->in_begin = 0;
            /* Export: close the normals scope; subsequent solids / tess /
             * gaps fall back to synthesized normals. */
            if (cursor->encode_normals)
                glPassThrough((GLfloat)MESH_PLY_PASS_NO_NORMALS);
        }
        break;
    case CMD_VERTEX3F:
        if (cursor->in_begin) {
            if (cursor->encode_normals) {
                float nw[3];
                exec_normal_to_world(cursor->begin_mv, cursor->cur_normal, nw);
                glTexCoord3f(nw[0], nw[1], nw[2]);
            }
            glVertex3f(cmd->args[0], cmd->args[1], cmd->args[2]);
        }
        break;
    case CMD_NORMAL3F:
        cursor->cur_normal[0] = cmd->args[0];
        cursor->cur_normal[1] = cmd->args[1];
        cursor->cur_normal[2] = cmd->args[2];
        glNormal3f(cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_COLOR3F:
        glColor4f(cmd->args[0], cmd->args[1], cmd->args[2],
                  cursor->alpha_scale);
        break;
    case CMD_COLOR4F:
        glColor4f(cmd->args[0], cmd->args[1], cmd->args[2],
                  cmd->args[3] * cursor->alpha_scale);
        break;
    case CMD_VERTEX2F:
        if (cursor->in_begin) {
            if (cursor->encode_normals) {
                float nw[3];
                exec_normal_to_world(cursor->begin_mv, cursor->cur_normal, nw);
                glTexCoord3f(nw[0], nw[1], nw[2]);
            }
            glVertex2f(cmd->args[0], cmd->args[1]);
        }
        break;
    /* The CMD_*-not-in-begin block below relies on the parser rejecting
     * these commands inside glBegin/glEnd (repl_cmd_type_valid_in_begin).
     * The executor no longer defensively glEnd()s the active begin block. */
    case CMD_POINT_SIZE:
        repl_exec_point_size(cmd->args[0]);
        break;
    case CMD_LINE_WIDTH:
        glLineWidth(cmd->args[0]);
        break;
    case CMD_GLUT_TORUS:
    case CMD_GLUT_CUBE:
    case CMD_GLUT_SPHERE:
    case CMD_GLUT_TEAPOT:
    case CMD_GLUT_CONE:
        repl_executor_draw_glut_solid(cmd);
        break;
    case CMD_RASTER_POS3F:
        glRasterPos3f(cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_LABEL: {
        /* `label("fmt", a, b, c, d)` — printf-style text emission
         * at the current raster position. Position is set by a preceding
         * glRasterPos3f; this command does not touch it, so the call composes
         * cleanly with whatever modelview/raster state the user has set up.
         *
         * Format substitution walks cmd.payload.label.fmt, expanding %f from
         * args[0..3] and %% to a literal '%'. The flatten pass keeps args[]
         * in sync with current variable values for has_vars commands, so we
         * use them directly. */
        char buf[128];
        int sub_count = cmd->num_args;
        int sub_idx = 0;
        int off = 0;
        const char *fmt = cmd->payload.label.fmt;
        if (sub_count < 0)
            sub_count = 0;
        while (*fmt && off < (int)sizeof(buf) - 1) {
            if (fmt[0] == '%' && fmt[1] == 'f' && sub_idx < sub_count) {
                off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                                "%g", (double)cmd->args[sub_idx]);
                if (off >= (int)sizeof(buf))
                    off = (int)sizeof(buf) - 1;
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
        if (g_tess) {
            g_tess_vert_count = 0;
            gluTessBeginPolygon(g_tess, NULL);
            cursor->tess_depth = 1;
        }
        break;
    case CMD_TESS_BEGIN_CONTOUR:
        if (g_tess && cursor->tess_depth == 1) {
            gluTessBeginContour(g_tess);
            cursor->tess_depth = 2;
        }
        break;
    case CMD_TESS_END:
        if (g_tess && cursor->tess_depth == 2) {
            gluTessEndContour(g_tess);
            cursor->tess_depth = 1;
        } else if (g_tess && cursor->tess_depth == 1) {
            gluTessEndPolygon(g_tess);
            cursor->tess_depth = 0;
        }
        break;
    case CMD_TESS_NORMAL:
        cursor->tess_current_normal[0] = cmd->args[0];
        cursor->tess_current_normal[1] = cmd->args[1];
        cursor->tess_current_normal[2] = cmd->args[2];
        break;
    case CMD_TESS_COLOR:
        cursor->tess_current_color[0] = cmd->args[0];
        cursor->tess_current_color[1] = cmd->args[1];
        cursor->tess_current_color[2] = cmd->args[2];
        cursor->tess_current_color[3] = ((cmd->num_args >= 4)
                                      ? cmd->args[3] : 1.0)
                                     * cursor->alpha_scale;
        break;
    case CMD_TESS_VERTEX:
        if (g_tess && cursor->tess_depth == 2 &&
            g_tess_vert_count < TESS_VERT_BUF_SIZE) {
            TessVertex *v = &g_tess_verts[g_tess_vert_count++];
            v->pos[0] = cmd->args[0];
            v->pos[1] = cmd->args[1];
            v->pos[2] = cmd->args[2];
            memcpy(v->normal, cursor->tess_current_normal, sizeof(v->normal));
            memcpy(v->color, cursor->tess_current_color, sizeof(v->color));
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
        char label_name[REPL_GOTO_LABEL_MAX];
        if (!repl_extract_goto_label(execution_flat_text(cursor->text, cmd),
                                     label_name, sizeof(label_name)))
            break;
        if (cursor->goto_count++ > REPL_GOTO_LOOP_LIMIT) {
            if (cursor->options.status_out &&
                cursor->options.status_out_sz > 0)
                snprintf(cursor->options.status_out,
                         (size_t)cursor->options.status_out_sz,
                         "goto: loop limit reached");
            cursor->pc = cursor->flat_cmd_count;
            return 0;
        }
        for (int label_idx = 0; label_idx < cursor->flat_cmd_count; label_idx++) {
            if (flat_cmds[label_idx].valid &&
                flat_cmds[label_idx].type == CMD_GOTO_LABEL) {
                char target_label[REPL_GOTO_LABEL_MAX];
                if (repl_extract_label_name(execution_flat_text(cursor->text,
                                                                &flat_cmds[label_idx]),
                                            target_label,
                                            sizeof(target_label)) &&
                    strcmp(target_label, label_name) == 0) {
                    cursor->pc = label_idx; /* final advance steps past label */
                    goto goto_done;
                }
            }
        }
        goto_done:;
        break;
    }
    case CMD_IF_BEGIN: {
        /* Re-evaluate the condition at execute time so a goto looping
         * back into the body sees updated vars. The cached args[0] is
         * the flatten-time value, used as the fallback when the line
         * doesn't carry vars (or when the paren-extract fails). The
         * shared kernel with flatten lives in repl_eval_if_condition;
         * see its doc for why both sides evaluate. */
        float cond = cmd->args[0];
        if (cmd->has_vars) {
            const FlatCmdLocalVars *local_vars =
                execution_local_vars_at(cursor->program, cursor->pc);
            const ExprVar *eval_vars = g_predef_vars;
            int eval_num_vars = g_num_predef_vars;
            if (local_vars && local_vars->num_vars > 0) {
                eval_vars = local_vars->vars;
                eval_num_vars = local_vars->num_vars;
            }
            cond = repl_eval_if_condition(execution_flat_text(cursor->text,
                                                              cmd),
                                          eval_vars, eval_num_vars,
                                          cond);
        }
        if (cond == 0.0f) {
            int if_depth = 1;
            while (if_depth > 0 &&
                   ++cursor->pc < cursor->flat_cmd_count) {
                if (flat_cmds[cursor->pc].type == CMD_IF_BEGIN)
                    if_depth++;
                else if (flat_cmds[cursor->pc].type == CMD_IF_END)
                    if_depth--;
            }
            /* pc now points to CMD_IF_END; final advance steps past it. */
        }
        break;
    }
    case CMD_IF_END:
        break; /* body executed; just step past */
    case CMD_VAR_ASSIGN: {
        /* Flatten already computed the RHS against current vars and
         * stored the result in args[0]. Re-evaluating here would
         * double-apply self-referential assignments like
         * `tmp2 = tmp2 + 1;` (once during flatten, once here).
         * Apply args[0] directly so flatten owns the eval semantics. */
        int var_idx = cmd->var_idx;
        float value = cmd->args[0];
        if (var_idx >= 0 && var_idx < g_num_predef_vars)
            g_predef_vars_mut[var_idx].value = value;
        break;
    }
    case CMD_SCRATCH_ASSIGN: {
        /* Same model as CMD_VAR_ASSIGN: flatten owns the eval; we
         * just apply the precomputed value. */
        int array_idx = (int)cmd->args[0];
        int elem_idx = (int)cmd->args[1];
        float value = cmd->args[2];
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
    /* State-mutating cmds: enumerated explicitly so this switch is
     * exhaustive over CmdType. Without enumeration, a `default:`
     * would silently swallow any newly-added CmdType (the prior
     * shape routed the default through repl_apply_state_cmd, whose
     * own default returns 0 — the "you forgot to handle this"
     * signal was lost in both layers). With no default and -Wall
     * (which enables -Wswitch), adding a new CmdType emits a
     * compile-time warning here. Adding a new entry to
     * repl_apply_state_cmd still only requires one new case below;
     * the cluster delegates uniformly. */
    case CMD_ENABLE:
    case CMD_DISABLE:
    case CMD_SHADE_MODEL:
    case CMD_COLOR_MATERIAL:
    case CMD_MATERIALFV:
    case CMD_MATERIALF:
    case CMD_LIGHT_MODEL_I:
    case CMD_FRONT_FACE:
    case CMD_DEPTH_FUNC:
    case CMD_DEPTH_MASK:
    case CMD_COLOR_MASK:
    case CMD_POINT_PARAMETER_FV:
    case CMD_BLEND_FUNC:
    case CMD_CLEAR_COLOR:
        /* Export pass captures raw glColor + all faces. The capture
         * disabled GL_LIGHTING and GL_CULL_FACE, but the program's own
         * glEnable would turn them back on — feedback would then return
         * per-vertex lit colors (not the material color) and drop culled
         * back faces. Suppress those two enables during export (lights /
         * material then no-op on color; both sides are captured). */
        if (cursor->encode_normals && cmd->type == CMD_ENABLE &&
            ((GLenum)cmd->args[0] == GL_LIGHTING ||
             (GLenum)cmd->args[0] == GL_CULL_FACE))
            break;
        /* repl_apply_state_cmd returns 0 if the cmd isn't in its own
         * switch - which would mean the executor enumerated it here but the
         * apply helper hasn't caught up yet. Log loudly once per type so the
         * asymmetry surfaces in dev builds instead of as a silent visual
         * regression. */
        if (!repl_apply_state_cmd(cmd, cursor->alpha_scale))
            repl_exec_cursor_warn_unhandled_state(cmd);
        break;
    }
    cursor->pc++;
    return 1;
}

void repl_exec_cursor_end(ReplExecCursor *cursor) {
    if (!cursor)
        return;
    if (cursor->in_begin) {
        glEnd();
        cursor->in_begin = 0;
    }
    if (!cursor->options.suppress_tess_finalize) {
        if (cursor->tess_depth == 2 && g_tess) {
            gluTessEndContour(g_tess);
            cursor->tess_depth = 1;
        }
        if (cursor->tess_depth == 1 && g_tess)
            gluTessEndPolygon(g_tess);
    }
    cursor->tess_depth = 0;
    repl_executor_unwind_tracked_transform_stack(&cursor->matrix_depth);
    cursor->pc = cursor->flat_cmd_count;
}

/* Walk flat_cmds[0..flat_cmd_count) and issue the corresponding GL
 * calls. Handles vertex submission, state changes, GLU quadrics and
 * tessellator commands, transforms, goto/label control flow, if-block
 * evaluation, and variable assignments.
 *
 * Replay and fade passes provide an explicit limit instead of temporarily
 * mutating repl_state_flat_program_count(). */
void repl_execute_program(const ReplExecutionOptions *options) {
    ReplExecCursor cursor = repl_exec_cursor_begin(options);
    while (repl_exec_cursor_step(&cursor)) {
    }
    repl_exec_cursor_end(&cursor);
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
