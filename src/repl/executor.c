/*
 * src/repl/executor.c -- Flat command execution, GLUtesselator resource lifetimes,
 * and execution-time state helpers.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "repl/flatten.h"
#include "repl/attrib_bits.h"   /* repl_attrib_bits_for_type (restore gating) */
#include "repl/executor.h"
#include "repl/state_owners.h"
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
 * scaling every glPointSize call by REF_DIST/cam_dist - a constant
 * footprint on the model, matching the hardware path's pure-quadratic
 * default (see REPL_POINT_SIZE_REF_DIST). Reads cam_dist from the
 * controller-installed source; with no source installed (the demo, or
 * any embedder without an app-shell camera) cam_dist defaults to 0
 * and `sz` passes through unchanged. When supported, emit `sz`
 * directly - CMD_POINT_PARAMETER_FV handles the real attenuation. */
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
    case CMD_MULT_MATRIXF:
        /* Snapshotted by flatten at this point in the stream - see the
         * payload.matrix comment in command.h. */
        glMultMatrixf(cmd->payload.matrix.m);
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

/* Deliberately narrow: the light-enable mask is the only REPL render
 * bookkeeping a state command still carries, because it is the only one with a
 * consumer that cannot see the GL (the light-indicator overlay). This is NOT
 * an observation hook - a pass may call it for a command whose GL it is
 * skipping, so anything routed through here would describe emissions that
 * never happened. Clear observation lives on the cursor, paired with the
 * emission itself (repl_exec_cursor_emit_clear*). */
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
    default:
        break;
    }
}

/* Restore the executor-side state a matching glPushAttrib saved, gated on
 * that push's mask. Bit membership comes from attrib_bits (the light-enable
 * mask is glEnable(GL_LIGHTn) state -> ENABLE|LIGHTING; clear color and color
 * mask are both COLOR_BUFFER), so the executor cannot drift from the
 * analyzer/inspector. Groups the mask did not save are left exactly as the
 * program set them, matching glPopAttrib.
 *
 * The cursor's running background observation is deliberately NOT restored
 * here: a pop rewinds GL state, not pixels that were already written. */
static void repl_exec_restore_attrib_bookkeeping(ReplExecCursor *cursor,
                                                 const ReplAttribSave *save) {
    if (!save)
        return;
    if (save->mask & repl_attrib_bits_for_type(CMD_ENABLE, GL_LIGHT0))
        repl_state_render_set_light_enabled_mask(save->render.light_enabled_mask);
    if (cursor && (save->mask & repl_attrib_bits_for_type(CMD_CLEAR_COLOR, 0)))
        cursor->clear_state = save->clear;
}

/* The channel mask a CMD_COLOR_MASK establishes, derived exactly as the
 * emitted glColorMask reads its own arguments so the tracker and GL cannot
 * disagree about a borderline value. */
static unsigned repl_color_write_mask_from_cmd(const GLCmd *cmd) {
    unsigned mask = 0u;
    int k;

    if (!cmd)
        return REPL_RGBA_ALL;
    for (k = 0; k < 4; k++)
        if ((GLboolean)cmd->args[k])
            mask |= (1u << k);
    return mask;
}

/* glClearColor takes GLclampf: GL clamps every component to [0,1] as it
 * stores them, so the tracked value has to be clamped the same way. The
 * parser caps the upper end of RGB but lets a negative component and any
 * alpha through, and a raw value would then describe a colour the framebuffer
 * cannot hold - harmless where the observation is fed straight back to GL
 * (which re-clamps), wrong where the host computes on it: the Rec. 709
 * luminance behind alpha_scale reads a negative channel as darker than black
 * and saturates the overlay boost. */
static float repl_clamp_unit(float v) {
    if (!(v > 0.0f))   /* also catches NaN */
        return 0.0f;
    return v > 1.0f ? 1.0f : v;
}

void repl_exec_cursor_emit_clear_color(ReplExecCursor *cursor,
                                       const GLCmd *cmd) {
    int k;

    if (!cursor || !cmd || cmd->type != CMD_CLEAR_COLOR)
        return;
    for (k = 0; k < 4; k++)
        cursor->clear_state.clear_rgba[k] = repl_clamp_unit(cmd->args[k]);
    repl_apply_state_cmd(cmd, cursor->alpha_scale);
}

void repl_exec_cursor_emit_clear(ReplExecCursor *cursor, const GLCmd *cmd,
                                 unsigned write_mask_override) {
    unsigned write_mask;
    int k;

    if (!cursor || !cmd || cmd->type != CMD_CLEAR)
        return;
    write_mask = (write_mask_override == REPL_OBSERVE_TRACKED_MASK)
                     ? cursor->clear_state.color_write_mask
                     : write_mask_override;

    repl_apply_state_cmd(cmd, cursor->alpha_scale);

    if (((GLbitfield)cmd->args[0] & GL_COLOR_BUFFER_BIT) == 0)
        return;
    /* Each channel the mask lets through takes the current clear color and
     * becomes known; a masked-off channel keeps whatever the framebuffer
     * already held, which is exactly its previous observed value and
     * knownness (unknown, unless an earlier clear established it). */
    for (k = 0; k < 4; k++) {
        unsigned bit = 1u << k;
        if (!(write_mask & bit))
            continue;
        cursor->observed_rgba[k] = cursor->clear_state.clear_rgba[k];
        cursor->observed_known_mask |= bit;
    }
}

int repl_apply_state_cmd(const GLCmd *cmd, float alpha_scale) {
    if (!cmd)
        return 0;

    /* The REPL render-state side effect (the light-enable mask) lives here, in
     * one place, so passes that own GL state themselves and skip the GL
     * emission (the hidden-line wireframe pass) stay in sync by calling
     * repl_apply_state_bookkeeping() directly. No-op for commands that carry
     * no bookkeeping. Clear observation is NOT folded in here - it belongs to
     * the cursor's emit-and-observe entry points, because this helper also
     * runs for commands whose GL a pass is skipping. */
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
    case CMD_CLIP_PLANE: {
        /* args[0]=plane, args[1..4]=equation. GL transforms the equation
         * by the modelview current at this call, so user transforms
         * preceding the command position the plane - native semantics. */
        GLdouble eq[4] = {
            (GLdouble)cmd->args[1], (GLdouble)cmd->args[2],
            (GLdouble)cmd->args[3], (GLdouble)cmd->args[4]
        };
        glClipPlane((GLenum)cmd->args[0], eq);
        return 1;
    }
    case CMD_FRONT_FACE:
        glFrontFace((GLenum)cmd->args[0]);
        return 1;
    case CMD_CULL_FACE:
        glCullFace((GLenum)cmd->args[0]);
        return 1;
    case CMD_POLYGON_MODE:
        glPolygonMode((GLenum)cmd->args[0], (GLenum)cmd->args[1]);
        return 1;
    case CMD_POLYGON_OFFSET:
        glPolygonOffset(cmd->args[0], cmd->args[1]);
        return 1;
    case CMD_DEPTH_FUNC:
        glDepthFunc((GLenum)cmd->args[0]);
        return 1;
    case CMD_STENCIL_FUNC:
        glStencilFunc((GLenum)cmd->args[0], (GLint)cmd->args[1],
                      (GLuint)cmd->args[2]);
        return 1;
    case CMD_STENCIL_OP:
        glStencilOp((GLenum)cmd->args[0], (GLenum)cmd->args[1],
                    (GLenum)cmd->args[2]);
        return 1;
    case CMD_STENCIL_MASK:
        glStencilMask((GLuint)cmd->args[0]);
        return 1;
    case CMD_DEPTH_MASK:
        glDepthMask((GLboolean)cmd->args[0]);
        return 1;
    case CMD_COLOR_MASK:
        glColorMask((GLboolean)cmd->args[0], (GLboolean)cmd->args[1],
                    (GLboolean)cmd->args[2], (GLboolean)cmd->args[3]);
        return 1;
    case CMD_EDGE_FLAG:
        glEdgeFlag((GLboolean)cmd->args[0]);
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
        /* The GL emission only. The cursor tracks the value this sets - and
         * scopes it through glPushAttrib - in ReplClearScopedState; drive it
         * through repl_exec_cursor_emit_clear_color() rather than calling
         * here directly, so tracker and GL cannot disagree. */
        glClearColor(cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
        return 1;
    case CMD_CLEAR_DEPTH:
        /* GL clamps the GLclampd to [0,1] itself, so no REPL-side clamp -
         * an out-of-range literal behaves exactly as the exported C does.
         * Unlike the clear color the cursor tracks nothing here: no host
         * consumer needs the program's depth-clear value, and every host-side
         * depth clear sets the value it wants first (see
         * glr_ctrl_clear_chrome). */
        glClearDepth((GLclampd)cmd->args[0]);
        return 1;
    case CMD_CLEAR_STENCIL:
        /* Already an integer in 0..255: the parser rejects an out-of-range
         * literal and the flatten fixup clamps an animated one, so this is
         * the same GLint the exported C computes. */
        glClearStencil((GLint)cmd->args[0]);
        return 1;
    case CMD_CLEAR:
        /* args[0] is the resolved GL_*_BUFFER_BIT mask. This IS the
         * frame's clear for the scene rect - nothing clears it on the
         * program's behalf, so a scene without a glClear line smears
         * (colour) and, with no depth clear, fails the depth test
         * outright. The host scissors this walk to the scene rect, so a
         * colour clear repaints the scene only and leaves the chrome
         * around it (which the host cleared) alone. */
        glClear((GLbitfield)cmd->args[0]);
        return 1;
    case CMD_FOG_I:
        glFogi((GLenum)cmd->args[0], (GLint)cmd->args[1]);
        return 1;
    case CMD_FOG_F:
        glFogf((GLenum)cmd->args[0], cmd->args[1]);
        return 1;
    case CMD_FOG_FV: {
        /* args[0]=pname (GL_FOG_COLOR), args[1..4]=rgba. */
        GLfloat fog[4] = {
            cmd->args[1], cmd->args[2], cmd->args[3], cmd->args[4]
        };
        glFogfv((GLenum)cmd->args[0], fog);
        return 1;
    }
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
    cursor.depth_tint_emitted = -1;
    /* Clear-affecting GL state starts where the caller says the context does:
     * baseline_clear_rgba is what a glClear with no preceding glClearColor
     * would use, and all four channels are writable until a glColorMask says
     * otherwise. Clamped on the way in for the same reason a program's
     * glClearColor is (see repl_clamp_unit): the tracked value must be the one
     * GL would hold, whoever supplied it. The running observation stays zeroed
     * - nothing is known until a clear actually writes a channel. */
    if (options) {
        int b;
        for (b = 0; b < 4; b++)
            cursor.clear_state.clear_rgba[b] =
                repl_clamp_unit(options->baseline_clear_rgba[b]);
    }
    cursor.clear_state.color_write_mask = REPL_RGBA_ALL;
    if (options && options->has_fade_context) {
        cursor.alpha_scale = options->fade_alpha_scale;
        cursor.skip_geom_before_pc = options->skip_geom_before_pc;
    }
    cursor.tess_current_color[3] = cursor.alpha_scale;

    /* The light-indicator overlay reads light_enabled_mask for its on/off
     * visual. GL's real default - and render3d_lights_setup() - is
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
        case CMD_VERTEX4F:
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
    /* Call-depth tint. Placed after the prefix-walk early-outs above, so a
     * skipped vertex does not leave a colour behind for the geometry that
     * actually draws, and keyed on repl_cmd_consumes_current_color() - the
     * existing "this draw reads the current colour" set - so the tint leads
     * exactly the commands it has to and nothing else. Emitting on change
     * rather than per command keeps a 3000-vertex run at one glColor4f.
     *
     * alpha_scale carries the replay fade, the same way the program's own
     * glColor3f does; the tint replaces the hue, not the pass's blending. */
    if (cursor->options.depth_tint_count > 0 &&
        cursor->options.depth_tint_colors &&
        repl_cmd_consumes_current_color(cmd->type)) {
        const float *rgb;
        int d = cmd->call_depth;
        if (d < 0)
            d = 0;
        if (d >= cursor->options.depth_tint_count)
            d = cursor->options.depth_tint_count - 1;
        rgb = cursor->options.depth_tint_colors[d];
        if (cmd->type == CMD_TESS_VERTEX) {
            /* A tess vertex does not read GL's current colour: the value
             * travels in the vertex payload and the combine/vertex callbacks
             * emit it. So the tint lands on the cursor's tess colour instead,
             * and unconditionally rather than on change - a CMD_TESS_COLOR
             * between two same-depth vertices would otherwise reclaim the
             * slot behind the cache's back. No GL call either way. */
            cursor->tess_current_color[0] = rgb[0];
            cursor->tess_current_color[1] = rgb[1];
            cursor->tess_current_color[2] = rgb[2];
            cursor->tess_current_color[3] = cursor->alpha_scale;
        } else if (d != cursor->depth_tint_emitted) {
            glColor4f(rgb[0], rgb[1], rgb[2], cursor->alpha_scale);
            cursor->depth_tint_emitted = d;
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
             * between glBegin/glEnd - so snapshot it here, once, and reuse
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
    case CMD_VERTEX4F:
        if (cursor->in_begin) {
            if (cursor->encode_normals) {
                float nw[3];
                exec_normal_to_world(cursor->begin_mv, cursor->cur_normal, nw);
                glTexCoord3f(nw[0], nw[1], nw[2]);
            }
            glVertex4f(cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
        }
        break;
    case CMD_NORMAL3F:
        cursor->cur_normal[0] = cmd->args[0];
        cursor->cur_normal[1] = cmd->args[1];
        cursor->cur_normal[2] = cmd->args[2];
        glNormal3f(cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_COLOR3F:
        if (cursor->options.state_filter &&
            !cursor->options.state_filter(cmd->type, cmd,
                                          cursor->options.state_filter_ud))
            break;
        glColor4f(cmd->args[0], cmd->args[1], cmd->args[2],
                  cursor->alpha_scale);
        break;
    case CMD_COLOR4F:
        if (cursor->options.state_filter &&
            !cursor->options.state_filter(cmd->type, cmd,
                                          cursor->options.state_filter_ud))
            break;
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
    case CMD_LINE_STIPPLE:
        glLineStipple((GLint)cmd->args[0], (GLushort)cmd->args[1]);
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
        /* `label("fmt", a, b, c, d)` - printf-style text emission
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
    case CMD_LOAD_IDENTITY: case CMD_MULT_MATRIXF:
    /* These are resolved during flatten and should not appear in flat_cmds. */
    case CMD_FOR_BEGIN: case CMD_FOR_END:
    case CMD_BREAK: case CMD_CONTINUE:
    case CMD_FUNC_DEF: case CMD_FUNC_END: case CMD_CALL:
    /* Expanded by flatten into one CMD_SCRATCH_ASSIGN per cell. */
    case CMD_SCRATCH_BLOCK_ASSIGN:
    case CMD_IF_BEGIN: case CMD_ELSE_IF: case CMD_ELSE: case CMD_IF_END:
    case CMD_COMMENT:
    case CMD_EMPTY:
    case CMD_VAR_DECLARE:
    case CMD_TYPE_COUNT:
        break;
    /* State-mutating cmds: enumerated explicitly so this switch is
     * exhaustive over CmdType. Without enumeration, a `default:`
     * would silently swallow any newly-added CmdType (the prior
     * shape routed the default through repl_apply_state_cmd, whose
     * own default returns 0 - the "you forgot to handle this"
     * signal was lost in both layers). With no default and -Wall
     * (which enables -Wswitch), adding a new CmdType emits a
     * compile-time warning here. Adding a new entry to
     * repl_apply_state_cmd still only requires one new case below;
     * the cluster delegates uniformly. */
    /* Attribute stack: scoped like the matrix stack (they carry cursor
     * depth), but dispatched here rather than via the transform early-out
     * because they are not transforms. The real GL stack is only driven while
     * the virtual depth is within REPL_ATTRIB_STACK_CAP; an orphan pop is a
     * silent no-op, which also protects the controller's outer
     * glPushAttrib(GL_ALL_ATTRIB_BITS) bracket from being over-popped by an
     * unbalanced user program. */
    case CMD_PUSH_ATTRIB: {
        unsigned mask = (unsigned)cmd->args[0];
        cursor->attrib_depth++;
        if (cursor->attrib_depth <= REPL_ATTRIB_STACK_CAP) {
            ReplAttribSave *save =
                &cursor->attrib_save[cursor->attrib_depth - 1];
            save->mask = mask;
            save->render = repl_state_render();
            save->clear = cursor->clear_state;
            if (!cursor->options.suppress_attrib_gl) {
                /* GL_ALL_ATTRIB_BITS reaches args[0] as the union of the
                 * supported bits (command_spec.c's k_attrib_bits[]) - GL's
                 * own 0xFFFFFFFF does not survive the float storage. That
                 * union is the REPL's bookkeeping mask, but what real GL
                 * gets must be what the user wrote, which is also what the
                 * exporter writes into the standalone C. */
                GLbitfield gl_mask = (mask == repl_attrib_all_bits_mask())
                                         ? GL_ALL_ATTRIB_BITS
                                         : (GLbitfield)mask;
                glPushAttrib(gl_mask);
            }
        }
        break;
    }
    case CMD_POP_ATTRIB:
        if (cursor->attrib_depth > 0) {
            if (cursor->attrib_depth <= REPL_ATTRIB_STACK_CAP) {
                ReplAttribSave *save =
                    &cursor->attrib_save[cursor->attrib_depth - 1];
                if (!cursor->options.suppress_attrib_gl)
                    glPopAttrib();
                repl_exec_restore_attrib_bookkeeping(cursor, save);
            }
            cursor->attrib_depth--;
        }
        break;
    case CMD_ENABLE:
    case CMD_DISABLE:
    case CMD_SHADE_MODEL:
    case CMD_COLOR_MATERIAL:
    case CMD_MATERIALFV:
    case CMD_MATERIALF:
    case CMD_LIGHT_MODEL_I:
    case CMD_FRONT_FACE:
    case CMD_CULL_FACE:
    case CMD_POLYGON_MODE:
    case CMD_POLYGON_OFFSET:
    case CMD_DEPTH_FUNC:
    case CMD_STENCIL_FUNC:
    case CMD_STENCIL_OP:
    case CMD_STENCIL_MASK:
    case CMD_DEPTH_MASK:
    case CMD_COLOR_MASK:
    case CMD_EDGE_FLAG:
    case CMD_POINT_PARAMETER_FV:
    case CMD_BLEND_FUNC:
    case CMD_CLEAR_COLOR:
    case CMD_CLEAR_DEPTH:
    case CMD_CLEAR_STENCIL:
    case CMD_CLIP_PLANE:
    case CMD_CLEAR:
    case CMD_FOG_I:
    case CMD_FOG_F:
    case CMD_FOG_FV:
        /* Fade-batch replays composite translucent geometry over the
         * frame the fill pass already rendered, and their pre-skip
         * prefix executes state commands - so without this gate every
         * batch would replay the program's leading glClear and wipe
         * that frame (the whole scene then flashes in from black on
         * each replay step). A clear is per-frame setup, not state a
         * fade overlay may re-apply; it has no bookkeeping to run. */
        if (cmd->type == CMD_CLEAR && cursor->options.has_fade_context)
            break;
        /* General state filter: a render pass that owns its own material/
         * lighting/cull state (the winding view) suppresses the program's
         * conflicting state commands here. Bookkeeping (light mask / clear
         * color) still runs so render state stays coherent. */
        if (cursor->options.state_filter &&
            !cursor->options.state_filter(cmd->type, cmd,
                                          cursor->options.state_filter_ud)) {
            repl_apply_state_bookkeeping(cmd);
            break;
        }
        /* Export pass captures raw glColor + all faces. The capture
         * disabled GL_LIGHTING and GL_CULL_FACE, but the program's own
         * glEnable would turn them back on - feedback would then return
         * per-vertex lit colors (not the material color) and drop culled
         * back faces. Suppress those two enables during export (lights /
         * material then no-op on color; both sides are captured). */
        if (cursor->encode_normals && cmd->type == CMD_ENABLE &&
            ((GLenum)cmd->args[0] == GL_LIGHTING ||
             (GLenum)cmd->args[0] == GL_CULL_FACE))
            break;
        /* Past every suppression gate: this command's GL is going out, so the
         * three that decide what the frame's background is emit through the
         * cursor, which pairs each one with the matching observation update.
         * Reaching this point IS the emission decision - a command a filter
         * or the fade gate turned away broke out above and updated neither
         * GL nor the observation. */
        switch (cmd->type) {
        case CMD_CLEAR_COLOR:
            repl_exec_cursor_emit_clear_color(cursor, cmd);
            break;
        case CMD_CLEAR:
            repl_exec_cursor_emit_clear(cursor, cmd,
                                        REPL_OBSERVE_TRACKED_MASK);
            break;
        case CMD_COLOR_MASK:
            /* No public entry point: the glColorMask emission and the
             * tracker update are one case, and no pass overrides it. */
            cursor->clear_state.color_write_mask =
                repl_color_write_mask_from_cmd(cmd);
            repl_apply_state_cmd(cmd, cursor->alpha_scale);
            break;
        default:
            /* repl_apply_state_cmd returns 0 if the cmd isn't in its own
             * switch - which would mean the executor enumerated it here but
             * the apply helper hasn't caught up yet. Log loudly once per type
             * so the asymmetry surfaces in dev builds instead of as a silent
             * visual regression. */
            if (!repl_apply_state_cmd(cmd, cursor->alpha_scale))
                repl_exec_cursor_warn_unhandled_state(cmd);
            break;
        }
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
    /* Unwind any unmatched glPushAttrib (LIFO): pop the real GL stack and
     * restore the bookkeeping mirror for every frame within the cap. Virtual
     * frames above the cap never pushed real GL state, so they only decrement.
     * suppress_attrib_gl cursors restore the mirror but issue no glPopAttrib. */
    while (cursor->attrib_depth > 0) {
        if (cursor->attrib_depth <= REPL_ATTRIB_STACK_CAP) {
            ReplAttribSave *save =
                &cursor->attrib_save[cursor->attrib_depth - 1];
            if (!cursor->options.suppress_attrib_gl)
                glPopAttrib();
            repl_exec_restore_attrib_bookkeeping(cursor, save);
        }
        cursor->attrib_depth--;
    }
    cursor->pc = cursor->flat_cmd_count;

    /* Publish what this walk observed itself clearing. Deliberately after the
     * unwind above: an unmatched push must not leave the *scoped* clear state
     * dangling, while the observation it produced stands regardless. */
    if (cursor->options.observation_out) {
        ReplBackgroundObservation obs;
        memcpy(obs.rgba, cursor->observed_rgba, sizeof(obs.rgba));
        obs.known = (cursor->observed_known_mask == REPL_RGBA_ALL);
        *cursor->options.observation_out = obs;
    }
}

/* Walk flat_cmds[0..flat_cmd_count) and issue the corresponding GL
 * calls. Handles vertex submission, state changes, GLU quadrics and
 * tessellator commands, transforms, if-block evaluation, and variable
 * assignments.
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
    /* Test/legacy entry point: the live flat program, nothing else. */
    ReplExecutionOptions options = {
        .flat_cmd_count = repl_state_flat_program_count(),
        .program        = repl_state_flat_program_view(),
        /* Explicit, not zero-init by omission: this entry point has no frame
         * to speak for, so it must not publish a presentation background. */
        .observation_out = NULL,
    };
    repl_execute_program(&options);
}
