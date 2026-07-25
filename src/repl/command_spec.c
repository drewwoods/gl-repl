#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "repl/command_spec.h"
#include "c_compat.h"  /* STATIC_ASSERT */

static const ReplEnumEntry k_begin_modes[] = {
    { "GL_POINTS",         GL_POINTS },
    { "GL_LINES",          GL_LINES },
    { "GL_LINE_STRIP",     GL_LINE_STRIP },
    { "GL_LINE_LOOP",      GL_LINE_LOOP },
    { "GL_TRIANGLES",      GL_TRIANGLES },
    { "GL_TRIANGLE_STRIP", GL_TRIANGLE_STRIP },
    { "GL_TRIANGLE_FAN",   GL_TRIANGLE_FAN },
    { "GL_QUADS",          GL_QUADS },
    { "GL_QUAD_STRIP",     GL_QUAD_STRIP },
    { "GL_POLYGON",        GL_POLYGON },
    { NULL, 0 }
};

static const ReplEnumEntry k_enable_caps[] = {
    { "GL_BLEND",           GL_BLEND },
    { "GL_CLIP_PLANE0",     GL_CLIP_PLANE0 },
    { "GL_CLIP_PLANE1",     GL_CLIP_PLANE1 },
    { "GL_CLIP_PLANE2",     GL_CLIP_PLANE2 },
    { "GL_CLIP_PLANE3",     GL_CLIP_PLANE3 },
    { "GL_CLIP_PLANE4",     GL_CLIP_PLANE4 },
    { "GL_CLIP_PLANE5",     GL_CLIP_PLANE5 },
    { "GL_COLOR_MATERIAL",  GL_COLOR_MATERIAL },
    { "GL_CULL_FACE",       GL_CULL_FACE },
    { "GL_DEPTH_TEST",      GL_DEPTH_TEST },
    { "GL_FOG",             GL_FOG },
    { "GL_LIGHT0",          GL_LIGHT0 },
    { "GL_LIGHT1",          GL_LIGHT1 },
    { "GL_LIGHT2",          GL_LIGHT2 },
    { "GL_LIGHT3",          GL_LIGHT3 },
    { "GL_LIGHTING",        GL_LIGHTING },
    { "GL_LINE_SMOOTH",     GL_LINE_SMOOTH },
    { "GL_LINE_STIPPLE",    GL_LINE_STIPPLE },
    { "GL_MULTISAMPLE",     GL_MULTISAMPLE },
    { "GL_NORMALIZE",       GL_NORMALIZE },
    { "GL_POINT_SMOOTH",    GL_POINT_SMOOTH },
    /* The three glPolygonOffset switches — one per glPolygonMode fill mode.
     * GL_POLYGON_OFFSET_FILL is the one decal passes want; the LINE and
     * POINT variants offset wireframe and point rasterization of polygons. */
    { "GL_POLYGON_OFFSET_FILL",  GL_POLYGON_OFFSET_FILL },
    { "GL_POLYGON_OFFSET_LINE",  GL_POLYGON_OFFSET_LINE },
    { "GL_POLYGON_OFFSET_POINT", GL_POLYGON_OFFSET_POINT },
    { "GL_STENCIL_TEST",         GL_STENCIL_TEST },
    { NULL, 0 }
};

/* glPolygonMode's mode slot. GL also accepts these on glPolygonMode's face
 * slot only as GL_FRONT / GL_BACK / GL_FRONT_AND_BACK, which is k_face_types
 * (shared with glCullFace and the material commands). */
static const ReplEnumEntry k_polygon_modes[] = {
    { "GL_FILL",  GL_FILL },
    { "GL_LINE",  GL_LINE },
    { "GL_POINT", GL_POINT },
    { NULL, 0 }
};

static const ReplEnumEntry k_shade_models[] = {
    { "GL_SMOOTH", GL_SMOOTH },
    { "GL_FLAT",   GL_FLAT },
    { NULL, 0 }
};

static const ReplEnumEntry k_face_types[] = {
    { "GL_FRONT",            GL_FRONT },
    { "GL_BACK",             GL_BACK },
    { "GL_FRONT_AND_BACK",   GL_FRONT_AND_BACK },
    { NULL, 0 }
};

static const ReplEnumEntry k_front_face[] = {
    { "GL_CW",              GL_CW },
    { "GL_CCW",             GL_CCW },
    { NULL, 0 }
};

static const ReplEnumEntry k_depth_funcs[] = {
    { "GL_NEVER",           GL_NEVER },
    { "GL_LESS",            GL_LESS },
    { "GL_EQUAL",           GL_EQUAL },
    { "GL_LEQUAL",          GL_LEQUAL },
    { "GL_GREATER",         GL_GREATER },
    { "GL_NOTEQUAL",        GL_NOTEQUAL },
    { "GL_GEQUAL",          GL_GEQUAL },
    { "GL_ALWAYS",          GL_ALWAYS },
    { NULL, 0 }
};

static const ReplEnumEntry k_stencil_ops[] = {
    { "GL_KEEP",    GL_KEEP },
    { "GL_ZERO",    GL_ZERO },
    { "GL_REPLACE", GL_REPLACE },
    { "GL_INCR",    GL_INCR },
    { "GL_DECR",    GL_DECR },
    { "GL_INVERT",  GL_INVERT },
    { NULL, 0 }
};

static const ReplEnumEntry k_material_params[] = {
    { "GL_AMBIENT",             GL_AMBIENT },
    { "GL_DIFFUSE",             GL_DIFFUSE },
    { "GL_SPECULAR",            GL_SPECULAR },
    { "GL_EMISSION",            GL_EMISSION },
    { "GL_SHININESS",           GL_SHININESS },
    { "GL_AMBIENT_AND_DIFFUSE", GL_AMBIENT_AND_DIFFUSE },
    { NULL, 0 }
};

/* Single-pname pool for glMaterialf's pname slot. The RGBA pnames need
 * 4 floats and live on glMaterialfv; restricting autocomplete here keeps
 * the only-GL_SHININESS rule visible at the spec layer. */
static const ReplEnumEntry k_material_shininess_only[] = {
    { "GL_SHININESS",           GL_SHININESS },
    { NULL, 0 }
};

static const ReplEnumEntry k_color_material_modes[] = {
    { "GL_AMBIENT",             GL_AMBIENT },
    { "GL_DIFFUSE",             GL_DIFFUSE },
    { "GL_SPECULAR",            GL_SPECULAR },
    { "GL_EMISSION",            GL_EMISSION },
    { "GL_AMBIENT_AND_DIFFUSE", GL_AMBIENT_AND_DIFFUSE },
    { NULL, 0 }
};

static const ReplEnumEntry k_light_model_params[] = {
    { "GL_LIGHT_MODEL_LOCAL_VIEWER", GL_LIGHT_MODEL_LOCAL_VIEWER },
    { "GL_LIGHT_MODEL_TWO_SIDE",     GL_LIGHT_MODEL_TWO_SIDE },
    { NULL, 0 }
};

/* The six planes GL guarantees (GL_MAX_CLIP_PLANES >= 6). Slot 0 of the
 * custom-parsed glClipPlane command; also merged into k_enable_caps
 * above so glEnable/glDisable can toggle them. */
static const ReplEnumEntry k_clip_planes[] = {
    { "GL_CLIP_PLANE0", GL_CLIP_PLANE0 },
    { "GL_CLIP_PLANE1", GL_CLIP_PLANE1 },
    { "GL_CLIP_PLANE2", GL_CLIP_PLANE2 },
    { "GL_CLIP_PLANE3", GL_CLIP_PLANE3 },
    { "GL_CLIP_PLANE4", GL_CLIP_PLANE4 },
    { "GL_CLIP_PLANE5", GL_CLIP_PLANE5 },
    { NULL, 0 }
};

/* The buffers glClear() can clear. The REPL owns the frame's own scene
 * clear; GL_ACCUM_BUFFER_BIT remains absent because it would fight the
 * accumulation effects in render.c. Stencil is program-owned too, so its
 * bit is intentionally available alongside color and depth. Table order is
 * the canonical emission order for the ENUM_BITFIELD slot. */
static const ReplEnumEntry k_clear_bits[] = {
    { "GL_COLOR_BUFFER_BIT", GL_COLOR_BUFFER_BIT },
    { "GL_DEPTH_BUFFER_BIT", GL_DEPTH_BUFFER_BIT },
    { "GL_STENCIL_BUFFER_BIT", GL_STENCIL_BUFFER_BIT },
    { NULL, 0 }
};

/* The attribute-group bits glPushAttrib()/glPopAttrib() can save/restore,
 * restricted to groups whose state a REPL command can actually set. Table
 * order is the canonical emission order for the ENUM_BITFIELD slot AND the
 * canonical bit-index order shared with the editor's per-bit highlighting.
 * Every value is a single bit < 2^24 so it round-trips through the
 * GLCmd.args[] float storage (see command.h). GL_ALL_ATTRIB_BITS remains out
 * of this atomic table: the glPushAttrib slot exposes it as an alias for the
 * union of these entries, whose value also round-trips exactly. */
static const ReplEnumEntry k_attrib_bits[] = {
    { "GL_CURRENT_BIT",      GL_CURRENT_BIT },
    { "GL_POINT_BIT",        GL_POINT_BIT },
    { "GL_LINE_BIT",         GL_LINE_BIT },
    { "GL_POLYGON_BIT",      GL_POLYGON_BIT },
    { "GL_LIGHTING_BIT",     GL_LIGHTING_BIT },
    { "GL_FOG_BIT",          GL_FOG_BIT },
    { "GL_DEPTH_BUFFER_BIT", GL_DEPTH_BUFFER_BIT },
    { "GL_TRANSFORM_BIT",    GL_TRANSFORM_BIT },
    { "GL_ENABLE_BIT",       GL_ENABLE_BIT },
    { "GL_COLOR_BUFFER_BIT", GL_COLOR_BUFFER_BIT },
    { NULL, 0 }
};

static const ReplEnumEntry k_bool_vals[] = {
    { "GL_TRUE",  GL_TRUE  },
    { "GL_FALSE", GL_FALSE },
    { NULL, 0 }
};

static const ReplEnumEntry k_point_param_pnames[] = {
    { "GL_POINT_DISTANCE_ATTENUATION", GL_POINT_DISTANCE_ATTENUATION },
    { NULL, 0 }
};

/* Fog pnames split by call shape, mirroring the glMaterialf /
 * glMaterialfv precedent: the mode enum lives on glFogi, the scalar
 * pnames on glFogf, and the RGBA color on glFogfv. */
static const ReplEnumEntry k_fog_i_pnames[] = {
    { "GL_FOG_MODE", GL_FOG_MODE },
    { NULL, 0 }
};

static const ReplEnumEntry k_fog_modes[] = {
    { "GL_LINEAR", GL_LINEAR },
    { "GL_EXP",    GL_EXP },
    { "GL_EXP2",   GL_EXP2 },
    { NULL, 0 }
};

static const ReplEnumEntry k_fog_f_pnames[] = {
    { "GL_FOG_DENSITY", GL_FOG_DENSITY },
    { "GL_FOG_START",   GL_FOG_START },
    { "GL_FOG_END",     GL_FOG_END },
    { NULL, 0 }
};

static const ReplEnumEntry k_fog_color_pnames[] = {
    { "GL_FOG_COLOR", GL_FOG_COLOR },
    { NULL, 0 }
};

/* Blend factors are split into the source (sfactor) and destination
 * (dfactor) sets that classic fixed-function GL accepts: GL_SRC_COLOR /
 * GL_ONE_MINUS_SRC_COLOR are dfactor-only, GL_DST_COLOR /
 * GL_ONE_MINUS_DST_COLOR and GL_SRC_ALPHA_SATURATE are sfactor-only, and
 * the rest are shared. Slots are strict token-only, so the asymmetry
 * teaches the GL 1.1 rule at autocomplete time. */
static const ReplEnumEntry k_blend_src_factors[] = {
    { "GL_ZERO",                GL_ZERO },
    { "GL_ONE",                 GL_ONE },
    { "GL_DST_COLOR",           GL_DST_COLOR },
    { "GL_ONE_MINUS_DST_COLOR", GL_ONE_MINUS_DST_COLOR },
    { "GL_SRC_ALPHA",           GL_SRC_ALPHA },
    { "GL_ONE_MINUS_SRC_ALPHA", GL_ONE_MINUS_SRC_ALPHA },
    { "GL_DST_ALPHA",           GL_DST_ALPHA },
    { "GL_ONE_MINUS_DST_ALPHA", GL_ONE_MINUS_DST_ALPHA },
    { "GL_SRC_ALPHA_SATURATE",  GL_SRC_ALPHA_SATURATE },
    { NULL, 0 }
};

static const ReplEnumEntry k_blend_dst_factors[] = {
    { "GL_ZERO",                GL_ZERO },
    { "GL_ONE",                 GL_ONE },
    { "GL_SRC_COLOR",           GL_SRC_COLOR },
    { "GL_ONE_MINUS_SRC_COLOR", GL_ONE_MINUS_SRC_COLOR },
    { "GL_SRC_ALPHA",           GL_SRC_ALPHA },
    { "GL_ONE_MINUS_SRC_ALPHA", GL_ONE_MINUS_SRC_ALPHA },
    { "GL_DST_ALPHA",           GL_DST_ALPHA },
    { "GL_ONE_MINUS_DST_ALPHA", GL_ONE_MINUS_DST_ALPHA },
    { NULL, 0 }
};

/* The list below hand-enumerates the 10 funcN slots. If
 * REPL_FUNC_SLOT_COUNT ever changes, the list must change with it or
 * the autocomplete / help table will silently miss the extra slots. */
STATIC_ASSERT(REPL_FUNC_SLOT_COUNT == 10,
              "REPL_FUNC_SLOT_LIST hand-enumerates slots 0..9; "
              "update the macro if REPL_FUNC_SLOT_COUNT changes");
#define REPL_FUNC_SLOT_LIST(OP) \
    OP(0) \
    OP(1) \
    OP(2) \
    OP(3) \
    OP(4) \
    OP(5) \
    OP(6) \
    OP(7) \
    OP(8) \
    OP(9)

#define FUNC_DECL_COMPLETION(slot_) \
    { "func" #slot_ "() {", "func" #slot_ "() {", 0, { NULL } },

#define FUNC_CALL_COMPLETION(slot_) \
    { "func" #slot_ "()", "func" #slot_ "()", 0, { NULL } },

/* Help-overlay / autocomplete entries. Source order is load-bearing: it
 * drives F1 help row order and Tab-completion priority, so entries are
 * grouped by category (NOT alphabetized). Section banners below mark the
 * existing contiguous runs — keep a new command inside the run it belongs
 * to rather than appending at the end. The trailing REPL_HELP_GROUP_* is
 * the separate F1 section bucket. */
static const ReplFuncCompletion k_func_completions[] = {
    /* --- Vertices, normals & color --- */
    { "glVertex3f(",         "glVertex3f(x, y, z)",                                      3, { "x", "y", "z" },
        "Specify a vertex position", REPL_HELP_GROUP_GEOMETRY },
    { "glVertex2f(",         "glVertex2f(x, y)",                                         2, { "x", "y" },
        "Specify a 2D vertex (z=0)", REPL_HELP_GROUP_GEOMETRY },
    { "glNormal3f(",         "glNormal3f(nx, ny, nz)",                                   3, { "nx", "ny", "nz" },
        "Specify a vertex normal", REPL_HELP_GROUP_GEOMETRY },
    { "glColor3f(",          "glColor3f(r, g, b)",                                       3, { "r", "g", "b" },
        "Specify vertex color", REPL_HELP_GROUP_GEOMETRY },
    { "glColor4f(",          "glColor4f(r, g, b, a)",                                    4, { "r", "g", "b", "a" },
        "Specify color with alpha", REPL_HELP_GROUP_GEOMETRY },
    { "glEdgeFlag(",         "glEdgeFlag(flag)",                                         1, { "flag" },
        "Specify whether edges are boundary or nonboundary", REPL_HELP_GROUP_GEOMETRY },
    { "glClearColor(",       "glClearColor(r, g, b, a)",                                 4, { "r", "g", "b", "a" },
        "Set the background clear color", REPL_HELP_GROUP_GEOMETRY },
    /* --- Primitive blocks --- */
    { "glBegin(",            "glBegin(mode)",                                            1, { "mode" },
        "GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_LINE_LOOP, ...", REPL_HELP_GROUP_PRIMITIVE },
    { "glEnd()",             "glEnd()",                                                  0, { NULL },
        "End current primitive block", REPL_HELP_GROUP_PRIMITIVE },
    /* --- Render state --- */
    { "glEnable(",           "glEnable(cap)",                                            1, { "cap" },
        "Enable a GL capability\n"
        "GL_BLEND, GL_CLIP_PLANE0..GL_CLIP_PLANE5, GL_COLOR_MATERIAL\n"
        "GL_CULL_FACE, GL_DEPTH_TEST, GL_FOG, GL_LIGHTING, GL_LIGHT0..GL_LIGHT3\n"
        "GL_LINE_SMOOTH, GL_LINE_STIPPLE, GL_MULTISAMPLE, GL_NORMALIZE, GL_POINT_SMOOTH, GL_STENCIL_TEST",
        REPL_HELP_GROUP_STATE },
    { "glDisable(",          "glDisable(cap)",                                           1, { "cap" },
        "Disable a GL capability (same caps as glEnable)", REPL_HELP_GROUP_STATE },
    { "glShadeModel(",       "glShadeModel(mode)",                                       1, { "mode" },
        "GL_SMOOTH, GL_FLAT", REPL_HELP_GROUP_STATE },
    { "glPointSize(",        "glPointSize(size)",                                        1, { "size" },
        "Rasterized point diameter", REPL_HELP_GROUP_STATE },
    { "glLineWidth(",        "glLineWidth(width)",                                       1, { "width" },
        "Rasterized line width", REPL_HELP_GROUP_STATE },
    { "glLineStipple(",      "glLineStipple(factor, pattern)",                           2, { "factor", "pattern" },
        "Dashed lines: factor stretches the 16-bit pattern bitmask (e.g. 255 = 0x00FF).\n"
        "Enable with glEnable(GL_LINE_STIPPLE).",
        REPL_HELP_GROUP_STATE },
    { "glClipPlane(",        "glClipPlane(plane, (GLdouble[]){a, b, c, d})",             2, { "plane", "(GLdouble[]){a, b, c, d}" },
        "Set a clip plane equation a*x + b*y + c*z + d >= 0 (kept half-space),\n"
        "in the coordinate frame active at the call. plane: GL_CLIP_PLANE0..5.\n"
        "Enable with glEnable(GL_CLIP_PLANEi). Flat shorthand accepted:\n"
        "glClipPlane(plane, a, b, c, d).",
        REPL_HELP_GROUP_STATE },
    { "glFogi(",             "glFogi(GL_FOG_MODE, mode)",                                2, { "GL_FOG_MODE", "mode" },
        "Select the fog equation: GL_LINEAR, GL_EXP, or GL_EXP2.\n"
        "Enable with glEnable(GL_FOG).",
        REPL_HELP_GROUP_STATE },
    { "glFogf(",             "glFogf(pname, value)",                                     2, { "pname", "value" },
        "Scalar fog parameter: GL_FOG_DENSITY (GL_EXP/GL_EXP2 modes) or\n"
        "GL_FOG_START / GL_FOG_END (GL_LINEAR mode).",
        REPL_HELP_GROUP_STATE },
    { "glFogfv(",            "glFogfv(GL_FOG_COLOR, (GLfloat[]){r, g, b, a})",           2, { "GL_FOG_COLOR", "(GLfloat[]){r, g, b, a}" },
        "Set the fog color distant geometry fades toward (usually the clear color).\n"
        "Flat shorthand accepted: glFogfv(GL_FOG_COLOR, r, g, b, a).",
        REPL_HELP_GROUP_STATE },
    { "glStencilFunc(",      "glStencilFunc(func, ref, mask)",                           3, { "func", "ref", "mask" },
        "Choose the stencil comparison. ref is a 0..255 expression; mask is a\n"
        "0..255 decimal or 0xNN literal. Enable GL_STENCIL_TEST to apply it.",
        REPL_HELP_GROUP_STATE },
    { "glStencilOp(",        "glStencilOp(sfail, dpfail, dppass)",                       3, { "sfail", "dpfail", "dppass" },
        "Choose the stencil-buffer update for stencil fail, depth fail, and pass.",
        REPL_HELP_GROUP_STATE },
    { "glStencilMask(",      "glStencilMask(mask)",                                     1, { "mask" },
        "Restrict writable stencil bits with a 0..255 decimal or 0xNN literal.",
        REPL_HELP_GROUP_STATE },
    { "glPushAttrib(",       "glPushAttrib(mask)",                                       1, { "mask" },
        "Save a group of state onto the attribute stack; restore with glPopAttrib.\n"
        "mask: GL_CURRENT_BIT, GL_POINT_BIT, GL_LINE_BIT, GL_POLYGON_BIT, GL_LIGHTING_BIT,\n"
        "GL_FOG_BIT, GL_DEPTH_BUFFER_BIT, GL_TRANSFORM_BIT, GL_ENABLE_BIT, GL_COLOR_BUFFER_BIT,\n"
        "several OR'd with |, or GL_ALL_ATTRIB_BITS for all supported groups.",
        REPL_HELP_GROUP_STATE },
    { "glPopAttrib()",       "glPopAttrib()",                                            0, { NULL },
        "Restore the state saved by the matching glPushAttrib", REPL_HELP_GROUP_STATE },
    /* --- Raster position & bitmap text --- */
    { "glRasterPos3f(",      "glRasterPos3f(x, y, z)",                                   3, { "x", "y", "z" },
        "Set the current raster position (transformed through modelview/projection).\n"
        "Pair with label(...) to draw bitmap text at that point.",
        REPL_HELP_GROUP_RASTER },
    { "label(",              "label(\"fmt\", a, b, c, d)",                               5, { "fmt", "a", "b", "c", "d" },
        "Render bitmap text at the current raster position (set via glRasterPos3f).\n"
        "Format supports %f (substituted from a/b/c/d as %g) and %% (literal '%').\n"
        "Up to 4 substitution args; format string limited to 64 chars.\n"
        "Forbidden in fmt: '//', '(', ')', ',', and any backslash.",
        REPL_HELP_GROUP_RASTER },
    /* --- Point parameters & blending --- */
    { "glPointParameterfv(", "glPointParameterfv(pname, a, b, c)",                       4, { "pname", "a", "b", "c" },
        "Distance attenuation: size *= 1/sqrt(const + linear*d + quadratic*d*d)",
        REPL_HELP_GROUP_BLEND },
    { "glBlendFunc(",        "glBlendFunc(sfactor, dfactor)",                            2, { "sfactor", "dfactor" },
        "Pixel = src*sfactor + dst*dfactor. Common: GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA (alpha)\n"
        "or GL_SRC_ALPHA, GL_ONE (additive glow).\n"
        "sfactor: GL_ZERO/GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA,\n"
        "  GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE\n"
        "dfactor: GL_ZERO/GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,\n"
        "  GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA",
        REPL_HELP_GROUP_BLEND },
    /* --- Matrix transforms --- */
    { "glTranslatef(",       "glTranslatef(x, y, z)",                                    3, { "x", "y", "z" },
        "Translate the modelview matrix", REPL_HELP_GROUP_TRANSFORM },
    { "glScalef(",           "glScalef(x, y, z)",                                        3, { "x", "y", "z" },
        "Scale the modelview matrix", REPL_HELP_GROUP_TRANSFORM },
    { "glRotatef(",          "glRotatef(angle, x, y, z)",                                4, { "angle", "x", "y", "z" },
        "Rotate the modelview matrix (angle in degrees)", REPL_HELP_GROUP_TRANSFORM },
    { "glPushMatrix()",      "glPushMatrix()",                                           0, { NULL },
        "Push current matrix onto stack", REPL_HELP_GROUP_TRANSFORM },
    { "glPopMatrix()",       "glPopMatrix()",                                            0, { NULL },
        "Pop matrix from stack", REPL_HELP_GROUP_TRANSFORM },
    { "glLoadIdentity()",    "glLoadIdentity()",                                         0, { NULL },
        "Reset the current matrix to identity", REPL_HELP_GROUP_TRANSFORM },
    { "glMultMatrixf(",      "glMultMatrixf((GLfloat[]){m0, ..., m15})",                 1, { "(GLfloat[]){m0, ..., m15}" },
        "Post-multiply the current matrix by 16 column-major values — the\n"
        "escape hatch for transforms translate/rotate/scale cannot express,\n"
        "such as a planar shadow projection or a shear.\n"
        "Flat shorthand accepted: glMultMatrixf(m0, ..., m15).\n"
        "glMultMatrixf(A) instead reads a scratch array (A, B, or C), whose\n"
        "cells the A[k] = ... lines above it fill.",
        REPL_HELP_GROUP_TRANSFORM },
    /* --- Depth & write-mask state --- */
    { "glCullFace(",         "glCullFace(mode)",                                         1, { "mode" },
        "GL_BACK, GL_FRONT, GL_FRONT_AND_BACK (which faces glEnable(GL_CULL_FACE) discards)",
        REPL_HELP_GROUP_DEPTH_MASK },
    { "glPolygonMode(",      "glPolygonMode(face, mode)",                                2, { "face", "mode" },
        "GL_FILL, GL_LINE, or GL_POINT rasterization for GL_FRONT / GL_BACK /\n"
        "GL_FRONT_AND_BACK faces — per-face wireframe without rebuilding the\n"
        "geometry as lines.",
        REPL_HELP_GROUP_DEPTH_MASK },
    { "glPolygonOffset(",    "glPolygonOffset(factor, units)",                           2, { "factor", "units" },
        "Nudge polygon depth values before the depth test, so coplanar\n"
        "surfaces stop fighting: draw the decal or outline pass with a small\n"
        "negative offset (-1, -1) to pull it in front. Needs\n"
        "glEnable(GL_POLYGON_OFFSET_FILL) (or the _LINE / _POINT variant).",
        REPL_HELP_GROUP_DEPTH_MASK },
    { "glFrontFace(",        "glFrontFace(mode)",                                        1, { "mode" },
        "GL_CW, GL_CCW", REPL_HELP_GROUP_DEPTH_MASK },
    { "glDepthFunc(",        "glDepthFunc(func)",                                        1, { "func" },
        "GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS", REPL_HELP_GROUP_DEPTH_MASK },
    { "glDepthMask(",        "glDepthMask(flag)",                                        1, { "flag" },
        "GL_TRUE, GL_FALSE (depth-buffer writes)", REPL_HELP_GROUP_DEPTH_MASK },
    { "glColorMask(",        "glColorMask(red, green, blue, alpha)",                     4, { "red", "green", "blue", "alpha" },
        "Per-channel color write enable: GL_TRUE / GL_FALSE", REPL_HELP_GROUP_DEPTH_MASK },
    { "glClear(",            "glClear(mask)",                                            1, { "mask" },
        "Clear buffers mid-scene, from the cursor line down\n"
        "GL_COLOR_BUFFER_BIT, GL_DEPTH_BUFFER_BIT, or both OR'd with |\n"
        "Colour clears to glClearColor and is confined to the 3D viewport",
        REPL_HELP_GROUP_DEPTH_MASK },
    { "glClearDepth(",       "glClearDepth(depth)",                                      1, { "depth" },
        "Depth value glClear(GL_DEPTH_BUFFER_BIT) writes (0..1, default 1)",
        REPL_HELP_GROUP_DEPTH_MASK },
    /* --- Lighting & materials --- */
    { "glColorMaterial(",    "glColorMaterial(face, mode)",                              2, { "face", "mode" },
        "face: GL_FRONT, GL_BACK, or GL_FRONT_AND_BACK\n"
        "mode: GL_AMBIENT / GL_AMBIENT_AND_DIFFUSE / GL_DIFFUSE / GL_SPECULAR / GL_EMISSION",
        REPL_HELP_GROUP_LIGHTING },
    { "glMaterialf(",        "glMaterialf(face, GL_SHININESS, value)",                   3, { "face", "GL_SHININESS", "value" },
        "Scalar material setter, GL_SHININESS only (RGBA pnames need glMaterialfv).",
        REPL_HELP_GROUP_LIGHTING },
    { "glMaterialfv(",       "glMaterialfv(face, pname, (GLfloat[]){r, g, b, a})",       3, { "face", "pname", "(GLfloat[]){r, g, b, a}" },
        "Per-face material: GL_AMBIENT / GL_DIFFUSE / GL_SPECULAR / GL_SHININESS.\n"
        "The compound literal carries 4 RGBA floats, or a single scalar (e.g. (GLfloat[]){30.0}) for GL_SHININESS.",
        REPL_HELP_GROUP_LIGHTING },
    { "glLightModeli(",      "glLightModeli(pname, param)",                              2, { "pname", "param" },
        "pname: GL_LIGHT_MODEL_TWO_SIDE, GL_LIGHT_MODEL_LOCAL_VIEWER",
        REPL_HELP_GROUP_LIGHTING },
    /* --- GLUT solid shapes --- */
    { "glutSolidTorus(",     "glutSolidTorus(inner_r, outer_r, nsides, rings)",          4, { "inner_r", "outer_r", "nsides", "rings" },
        "", REPL_HELP_GROUP_GLUT_SHAPES },
    { "glutSolidCube(",      "glutSolidCube(size)",                                      1, { "size" },
        "", REPL_HELP_GROUP_GLUT_SHAPES },
    { "glutSolidSphere(",    "glutSolidSphere(radius, slices, stacks)",                  3, { "radius", "slices", "stacks" },
        "", REPL_HELP_GROUP_GLUT_SHAPES },
    { "glutSolidTeapot(",    "glutSolidTeapot(size)",                                    1, { "size" },
        "", REPL_HELP_GROUP_GLUT_SHAPES },
    { "glutSolidCone(",      "glutSolidCone(base, height, slices, stacks)",              4, { "base", "height", "slices", "stacks" },
        "", REPL_HELP_GROUP_GLUT_SHAPES },
    /* --- GLU tessellation --- */
    { "gluBegin(GLU_POLYGON)", "gluBegin(GLU_POLYGON)",                                  0, { NULL },
        "Start a tessellated polygon", REPL_HELP_GROUP_GLU_TESS },
    { "gluBegin(GLU_CONTOUR)", "gluBegin(GLU_CONTOUR)",                                  0, { NULL },
        "Start a contour within the polygon", REPL_HELP_GROUP_GLU_TESS },
    { "gluEnd()",            "gluEnd()",                                                 0, { NULL },
        "End contour or polygon", REPL_HELP_GROUP_GLU_TESS },
    { "gluNormal(",          "gluNormal(nx, ny, nz)",                                    3, { "nx", "ny", "nz" },
        "Set per-vertex normal", REPL_HELP_GROUP_GLU_TESS },
    /* gluColor accepts 3 or 4 floats — the parser defaults alpha to
     * 1.0 when omitted. The param-hint walker enumerates everything
     * in `params[]` as mandatory, so we list only the 3 required
     * positions there; the trailing `[, a]` in display_text signals
     * the optional alpha in the popup match list. */
    { "gluColor(",           "gluColor(r, g, b[, a])",                                   3, { "r", "g", "b" },
        "Set per-vertex color (alpha defaults to 1.0 when omitted)",
        REPL_HELP_GROUP_GLU_TESS },
    { "gluVertex(",          "gluVertex(x, y, z)",                                       3, { "x", "y", "z" },
        "Add vertex to current contour", REPL_HELP_GROUP_GLU_TESS },
    /* --- Language constructs (help-skipped: no REPL_HELP_GROUP) --- */
    { "float ",              "float name",                                               0, { NULL } },
    { "for(",                "for(var, start, end[, step])",                             4, { "var", "start", "end", "step" } },
    { "if(",                 "if(expr)",                                                 1, { "expr" } },
    { "} else if(",          "} else if(expr)",                                          1, { "expr" } },
    { "} else {",            "} else {",                                                 0, { NULL } },
    { "goto ",               "goto label",                                               0, { NULL } },
    REPL_FUNC_SLOT_LIST(FUNC_DECL_COMPLETION)
    REPL_FUNC_SLOT_LIST(FUNC_CALL_COMPLETION)
    { "t = ",                "t = value",                                                0, { NULL } },
    { "A[",                  "A[index]",                                                 0, { NULL } },
    { "B[",                  "B[index]",                                                 0, { NULL } },
    { "C[",                  "C[index]",                                                 0, { NULL } },
    /* --- Math functions & constants --- */
    { "sin(",                "sin(x)",                                                   1, { "x" },
        "Sine of x (radians)", REPL_HELP_GROUP_MATH },
    { "cos(",                "cos(x)",                                                   1, { "x" },
        "Cosine of x (radians)", REPL_HELP_GROUP_MATH },
    { "tan(",                "tan(x)",                                                   1, { "x" },
        "Tangent of x (radians)", REPL_HELP_GROUP_MATH },
    { "asin(",               "asin(x)",                                                  1, { "x" },
        "Arc sine of x in radians, [-PI/2, PI/2] (x is clamped to [-1, 1])",
        REPL_HELP_GROUP_MATH },
    { "acos(",               "acos(x)",                                                  1, { "x" },
        "Arc cosine of x in radians, [0, PI] (x is clamped to [-1, 1])",
        REPL_HELP_GROUP_MATH },
    { "atan(",               "atan(x)",                                                  1, { "x" },
        "Arc tangent of x in radians, (-PI/2, PI/2). Takes a slope, so it\n"
        "cannot tell which quadrant the point was in — use atan2 for that.",
        REPL_HELP_GROUP_MATH },
    { "atan2(",              "atan2(y, x)",                                              2, { "y", "x" },
        "Angle in radians from +X to the point (x, y), in [-PI, PI].\n"
        "The way to recover a heading from coordinates: atan2(tz, tx) aims\n"
        "at a target, atan2(y, x) is the polar angle of a vertex.",
        REPL_HELP_GROUP_MATH },
    { "sqrt(",               "sqrt(x)",                                                  1, { "x" },
        "Square root of |x| (negatives are absolute-valued before sqrt)",
        REPL_HELP_GROUP_MATH },
    { "abs(",                "abs(x)",                                                   1, { "x" },
        "Absolute value", REPL_HELP_GROUP_MATH },
    { "pow(",                "pow(base, exp)",                                           2, { "base", "exp" },
        "Raise base to exp (base^exp)", REPL_HELP_GROUP_MATH },
    { "min(",                "min(a, b)",                                                2, { "a", "b" },
        "Smaller of a or b", REPL_HELP_GROUP_MATH },
    { "max(",                "max(a, b)",                                                2, { "a", "b" },
        "Larger of a or b", REPL_HELP_GROUP_MATH },
    { "clamp(",              "clamp(x, lo, hi)",                                         3, { "x", "lo", "hi" },
        "x limited to the range [lo, hi]. Replaces min(max(x, lo), hi).",
        REPL_HELP_GROUP_MATH },
    { "lerp(",               "lerp(a, b, s)",                                            3, { "a", "b", "s" },
        "Blend from a to b by s: a at s=0, b at s=1. Not clamped, so s\n"
        "outside [0, 1] extrapolates past the endpoints (overshoot easings).",
        REPL_HELP_GROUP_MATH },
    { "smoothstep(",         "smoothstep(e0, e1, x)",                                    3, { "e0", "e1", "x" },
        "Eased 0-to-1 ramp as x crosses from e0 to e1: 0 below e0, 1 above\n"
        "e1, and a curve that leaves and arrives at zero slope in between.\n"
        "The fade that doesn't visibly start or stop.",
        REPL_HELP_GROUP_MATH },
    { "sign(",               "sign(x)",                                                  1, { "x" },
        "-1, 0, or 1 by the sign of x (exactly 0 gives 0)", REPL_HELP_GROUP_MATH },
    { "floor(",              "floor(x)",                                                 1, { "x" },
        "Largest integer <= x", REPL_HELP_GROUP_MATH },
    { "ceil(",               "ceil(x)",                                                  1, { "x" },
        "Smallest integer >= x", REPL_HELP_GROUP_MATH },
    { "round(",              "round(x)",                                                 1, { "x" },
        "Nearest integer to x (halfway rounds away from zero)", REPL_HELP_GROUP_MATH },
    { "fmod(",               "fmod(x, y)",                                               2, { "x", "y" },
        "Remainder of x/y (truncated toward zero; sign follows x). "
        "Same as the `%` operator.",
        REPL_HELP_GROUP_MATH },
    { "rem(",                "rem(x, y)",                                                2, { "x", "y" },
        "IEEE remainder of x/y (rounded to nearest; |result| <= |y|/2). "
        "Useful for centring an angle in [-PI, PI].",
        REPL_HELP_GROUP_MATH },
    { "log(",                "log(x)",                                                   1, { "x" },
        "Base-10 logarithm of x", REPL_HELP_GROUP_MATH },
    { "ln(",                 "ln(x)",                                                    1, { "x" },
        "Natural logarithm of x (base e)", REPL_HELP_GROUP_MATH },
    { "rand(",               "rand(seed[, iter])",                                       2, { "seed", "iter" },
        "Deterministic pseudo-random float in [0, 1). "
        "Same (seed, iter) -> same value across frames.",
        REPL_HELP_GROUP_MATH },
    { "rand2(",              "rand2(seed[, iter])",                                      2, { "seed", "iter" },
        "Deterministic pseudo-random float in [-1, 1] (signed variant of rand).\n"
        "Shares the same hash, so rand and rand2 are correlated for the same\n"
        "(seed, iter) pair — useful for centred jitter and signed offsets.",
        REPL_HELP_GROUP_MATH },
    { "PI",                  "PI",                                                       0, { NULL },
        "3.14159265... (half turn in radians)", REPL_HELP_GROUP_MATH },
    { "TAU",                 "TAU",                                                      0, { NULL },
        "6.28318530... (full turn in radians; 2*PI)", REPL_HELP_GROUP_MATH },
    { "e",                   "e",                                                        0, { NULL },
        "2.71828182... (Euler's number; base of natural logarithm)", REPL_HELP_GROUP_MATH },
    { NULL, NULL, 0, { NULL } }
};

#undef FUNC_CALL_COMPLETION
#undef FUNC_DECL_COMPLETION
#undef REPL_FUNC_SLOT_LIST

/* One positional enum-arg slot. Keeps the spec table rows readable now
 * that each enum command carries an explicit args[] array. */
#define ENUM_SLOT(tbl_, usage_, kind_) { (tbl_), (usage_), (kind_), NULL }

/* Strict token-only slot — the behavior-neutral baseline for every
 * non-bool enum slot (and, until the bool-slot policy lands, for
 * glDepthMask). */
#define ENUM_SLOT_TOK(tbl_, usage_) ENUM_SLOT((tbl_), (usage_), REPL_ENUM_SLOT_ENUM_ONLY)

/* Boolean mask slot — token first, else a constant 0/1 (no runtime
 * vars) reverse-mapped to GL_FALSE/GL_TRUE. The bool-slot policy:
 * glDepthMask and glColorMask. */
#define ENUM_SLOT_BOOL(usage_) ENUM_SLOT(k_bool_vals, (usage_), REPL_ENUM_SLOT_ENUM_OR_CONST_VALUE)

/* Bitfield mask slot — one or more of the slot's tokens joined by `|`.
 * The glClear mask policy. */
#define ENUM_SLOT_BITS(tbl_, usage_) ENUM_SLOT((tbl_), (usage_), REPL_ENUM_SLOT_ENUM_BITFIELD)

/* Bitfield slot with one canonical alias for the union of every table bit. */
#define ENUM_SLOT_BITS_ALL(tbl_, usage_, alias_) \
    { (tbl_), (usage_), REPL_ENUM_SLOT_ENUM_BITFIELD, (alias_) }

static const ReplEnumCommandSpec k_enum_command_specs[] = {
    { "glBegin",         CMD_BEGIN,          1, "%sglBegin(%s);",            1,
        .args = { ENUM_SLOT_TOK(k_begin_modes, "Unknown mode. Try GL_TRIANGLES, GL_TRIANGLE_STRIP, ...") } },
    { "glBlendFunc",     CMD_BLEND_FUNC,     2, "%sglBlendFunc(%s, %s);",    0,
        .args = { ENUM_SLOT_TOK(k_blend_src_factors, "sfactor: GL_SRC_ALPHA"),
                  ENUM_SLOT_TOK(k_blend_dst_factors, "dfactor: GL_ONE_MINUS_SRC_ALPHA, GL_ONE") } },
    { "glClear",         CMD_CLEAR,          1, "%sglClear(%s);",            0,
        .args = { ENUM_SLOT_BITS(k_clear_bits,
                                 "mask: GL_COLOR_BUFFER_BIT, GL_DEPTH_BUFFER_BIT, or both OR'd with |") } },
    /* glClipPlane is parsed by a custom branch (num_args -1). args[] is
     * kept only so slot-indexed autocomplete offers the plane tokens;
     * the (GLdouble[]){a, b, c, d} equation is handled by the custom
     * parser, not the generalized enum loop. */
    { "glClipPlane",     CMD_CLIP_PLANE,    -1, NULL,                        0,
        .args = { ENUM_SLOT_TOK(k_clip_planes, "plane: GL_CLIP_PLANE0 .. GL_CLIP_PLANE5") } },
    { "glColorMask",     CMD_COLOR_MASK,     4, "%sglColorMask(%s, %s, %s, %s);", 0,
        .args = { ENUM_SLOT_BOOL("red: GL_TRUE or GL_FALSE"),
                  ENUM_SLOT_BOOL("green: GL_TRUE or GL_FALSE"),
                  ENUM_SLOT_BOOL("blue: GL_TRUE or GL_FALSE"),
                  ENUM_SLOT_BOOL("alpha: GL_TRUE or GL_FALSE") } },
    { "glColorMaterial", CMD_COLOR_MATERIAL, 2, "%sglColorMaterial(%s, %s);", 0,
        .args = { ENUM_SLOT_TOK(k_face_types, "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK"),
                  ENUM_SLOT_TOK(k_color_material_modes, "mode: GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_EMISSION, GL_AMBIENT_AND_DIFFUSE") } },
    { "glCullFace",      CMD_CULL_FACE,      1, "%sglCullFace(%s);",         0,
        .args = { ENUM_SLOT_TOK(k_face_types, "Try GL_BACK, GL_FRONT, or GL_FRONT_AND_BACK") } },
    { "glDepthFunc",     CMD_DEPTH_FUNC,     1, "%sglDepthFunc(%s);",        0,
        .args = { ENUM_SLOT_TOK(k_depth_funcs, "Try GL_LESS, GL_LEQUAL, GL_ALWAYS, ...") } },
    { "glDepthMask",     CMD_DEPTH_MASK,     1, "%sglDepthMask(%s);",        0,
        .args = { ENUM_SLOT_BOOL("Try GL_TRUE or GL_FALSE") } },
    { "glEdgeFlag",      CMD_EDGE_FLAG,      1, "%sglEdgeFlag(%s);",         0,
        .args = { ENUM_SLOT_BOOL("Try GL_TRUE or GL_FALSE") } },
    { "glDisable",       CMD_DISABLE,        1, "%sglDisable(%s);",          0,
        .args = { ENUM_SLOT_TOK(k_enable_caps, "Try GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL") } },
    { "glEnable",        CMD_ENABLE,         1, "%sglEnable(%s);",           0,
        .args = { ENUM_SLOT_TOK(k_enable_caps, "Try GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL") } },
    /* glFogf is parsed by a custom branch (num_args -1). args[] is kept
     * only so slot-indexed autocomplete offers the pname tokens; the
     * trailing scalar value is a full expression handled by the custom
     * parser (GL_FOG_MODE lives on glFogi, GL_FOG_COLOR on glFogfv). */
    { "glFogf",          CMD_FOG_F,         -1, NULL,                        0,
        .args = { ENUM_SLOT_TOK(k_fog_f_pnames, "pname: GL_FOG_DENSITY, GL_FOG_START, GL_FOG_END") } },
    /* glFogfv is parsed by a custom branch (num_args -1): the
     * (GLfloat[]){r, g, b, a} compound literal is handled there, not by
     * the generalized enum loop. */
    { "glFogfv",         CMD_FOG_FV,        -1, NULL,                        0,
        .args = { ENUM_SLOT_TOK(k_fog_color_pnames, "pname: GL_FOG_COLOR") } },
    { "glFogi",          CMD_FOG_I,          2, "%sglFogi(%s, %s);",         0,
        .args = { ENUM_SLOT_TOK(k_fog_i_pnames, "pname: GL_FOG_MODE"),
                  ENUM_SLOT_TOK(k_fog_modes, "mode: GL_LINEAR, GL_EXP, GL_EXP2") } },
    { "glFrontFace",     CMD_FRONT_FACE,     1, "%sglFrontFace(%s);",        0,
        .args = { ENUM_SLOT_TOK(k_front_face, "Try GL_CW or GL_CCW") } },
    { "glLightModeli",   CMD_LIGHT_MODEL_I,  2, "%sglLightModeli(%s, %s);",  0,
        .args = { ENUM_SLOT_TOK(k_light_model_params, "pname: GL_LIGHT_MODEL_TWO_SIDE, GL_LIGHT_MODEL_LOCAL_VIEWER"),
                  ENUM_SLOT(k_bool_vals, "param: GL_TRUE, GL_FALSE, or integer", REPL_ENUM_SLOT_ENUM_OR_EXPR) } },
    /* glMaterialf is parsed by a custom branch (num_args -2). args[] is
     * kept only so slot-indexed autocomplete offers the face/pname
     * tokens; the trailing scalar value is handled by the custom parser.
     * pname slot is GL_SHININESS-only by design (RGBA pnames live on
     * glMaterialfv). */
    { "glMaterialf",     CMD_MATERIALF,     -2, NULL,                        0,
        .args = { ENUM_SLOT_TOK(k_face_types, "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK"),
                  ENUM_SLOT_TOK(k_material_shininess_only, "pname: GL_SHININESS (RGBA pnames need glMaterialfv)") } },
    /* glMaterialfv is parsed by a custom branch (num_args -2). args[] is
     * kept only so slot-indexed autocomplete still offers face/param
     * tokens; abs(num_args) == 2 is the autocomplete slot count. The
     * third arg (the (GLfloat[]){...} compound literal) is handled by
     * the custom parser, not the generalized enum loop. */
    { "glMaterialfv",    CMD_MATERIALFV,    -2, NULL,                        0,
        .args = { ENUM_SLOT_TOK(k_face_types, "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK"),
                  ENUM_SLOT_TOK(k_material_params, "pname: GL_DIFFUSE, GL_AMBIENT, GL_SPECULAR, GL_SHININESS") } },
    { "glPolygonMode",   CMD_POLYGON_MODE,   2, "%sglPolygonMode(%s, %s);",  0,
        .args = { ENUM_SLOT_TOK(k_face_types, "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK"),
                  ENUM_SLOT_TOK(k_polygon_modes, "mode: GL_FILL, GL_LINE, GL_POINT") } },
    { "glPushAttrib",    CMD_PUSH_ATTRIB,    1, "%sglPushAttrib(%s);",       0,
        .args = { ENUM_SLOT_BITS_ALL(k_attrib_bits,
                                     "mask: GL_CURRENT_BIT, GL_ENABLE_BIT, GL_LIGHTING_BIT, "
                                     "GL_COLOR_BUFFER_BIT, GL_DEPTH_BUFFER_BIT, GL_TRANSFORM_BIT, GL_FOG_BIT, "
                                     "GL_POINT_BIT, GL_LINE_BIT, GL_POLYGON_BIT, GL_ALL_ATTRIB_BITS, "
                                     "or several OR'd with |",
                                     "GL_ALL_ATTRIB_BITS") } },
    { "glShadeModel",    CMD_SHADE_MODEL,    1, "%sglShadeModel(%s);",       0,
        .args = { ENUM_SLOT_TOK(k_shade_models, "Try GL_SMOOTH or GL_FLAT") } },
    { "glStencilFunc",   CMD_STENCIL_FUNC,  -1, NULL,                           0,
        .args = { ENUM_SLOT_TOK(k_depth_funcs, "func: GL_NEVER, GL_LESS, GL_EQUAL, ...") } },
    { "glStencilMask",   CMD_STENCIL_MASK,  -1, NULL,                           0,
        .args = { { NULL, "mask: decimal or 0xNN literal in 0..255", REPL_ENUM_SLOT_ENUM_ONLY, NULL } } },
    { "glStencilOp",     CMD_STENCIL_OP,     3, "%sglStencilOp(%s, %s, %s);", 0,
        .args = { ENUM_SLOT_TOK(k_stencil_ops, "sfail: GL_KEEP, GL_ZERO, GL_REPLACE, ..."),
                  ENUM_SLOT_TOK(k_stencil_ops, "dpfail: GL_KEEP, GL_ZERO, GL_REPLACE, ..."),
                  ENUM_SLOT_TOK(k_stencil_ops, "dppass: GL_KEEP, GL_ZERO, GL_REPLACE, ...") } },
    { .name = NULL }
};

static const ReplStdCommandSpec k_std_command_specs[] = {
    { "glClearColor",   CMD_CLEAR_COLOR,      4, "glClearColor(%g, %g, %g, %g);",   "Usage: glClearColor(r, g, b, a)", 0 },
    { "glClearDepth",   CMD_CLEAR_DEPTH,      1, "glClearDepth(%g);",               "Usage: glClearDepth(depth)", 0 },
    { "glColor3f",      CMD_COLOR3F,          3, "glColor3f(%g, %g, %g);",          "Usage: glColor3f(r, g, b)", 0 },
    { "glColor4f",      CMD_COLOR4F,          4, "glColor4f(%g, %g, %g, %g);",      "Usage: glColor4f(r, g, b, a)", 0 },
    { "glLineStipple",  CMD_LINE_STIPPLE,     2, "glLineStipple(%g, %g);",          "Usage: glLineStipple(factor, pattern)", 0 },
    { "glLineWidth",    CMD_LINE_WIDTH,       1, "glLineWidth(%g);",                "Usage: glLineWidth(width)", 0 },
    { "glNormal3f",     CMD_NORMAL3F,         3, "glNormal3f(%g, %g, %g);",         "Usage: glNormal3f(nx, ny, nz)", 0 },
    { "glPointSize",    CMD_POINT_SIZE,       1, "glPointSize(%g);",                "Usage: glPointSize(size)", 0 },
    { "glPolygonOffset", CMD_POLYGON_OFFSET,  2, "glPolygonOffset(%g, %g);",       "Usage: glPolygonOffset(factor, units)", 0 },
    { "glRasterPos3f",  CMD_RASTER_POS3F,     3, "glRasterPos3f(%g, %g, %g);",      "Usage: glRasterPos3f(x, y, z)", 0 },
    { "glRotatef",      CMD_ROTATEF,          4, "glRotatef(%g, %g, %g, %g);",      "Usage: glRotatef(angle, x, y, z)", 0 },
    { "glScalef",       CMD_SCALEF,           3, "glScalef(%g, %g, %g);",           "Usage: glScalef(x, y, z)", 0 },
    { "glTranslatef",   CMD_TRANSLATE3F,      3, "glTranslatef(%g, %g, %g);",       "Usage: glTranslatef(x, y, z)", 0 },
    { "glVertex2f",     CMD_VERTEX2F,         2, "glVertex2f(%g, %g);",             "Usage: glVertex2f(x, y)", 0 },
    { "glVertex3f",     CMD_VERTEX3F,         3, "glVertex3f(%g, %g, %g);",         "Usage: glVertex3f(x, y, z)", 0 },
    { "gluNormal",      CMD_TESS_NORMAL,      3, "gluNormal(%g, %g, %g);",          "Usage: gluNormal(x, y, z)", 1 },
    { "gluVertex",      CMD_TESS_VERTEX,      3, "gluVertex(%g, %g, %g);",          "Usage: gluVertex(x, y, z)", 1 },
    { "glutSolidCone",    CMD_GLUT_CONE,     4, "glutSolidCone(%g, %g, %g, %g);",     "Usage: glutSolidCone(base, height, slices, stacks)", 0 },
    { "glutSolidCube",    CMD_GLUT_CUBE,     1, "glutSolidCube(%g);",                 "Usage: glutSolidCube(size)", 0 },
    { "glutSolidSphere",  CMD_GLUT_SPHERE,   3, "glutSolidSphere(%g, %g, %g);",       "Usage: glutSolidSphere(radius, slices, stacks)", 0 },
    { "glutSolidTeapot",  CMD_GLUT_TEAPOT,   1, "glutSolidTeapot(%g);",               "Usage: glutSolidTeapot(size)", 0 },
    { "glutSolidTorus",   CMD_GLUT_TORUS,    4, "glutSolidTorus(%g, %g, %g, %g);",   "Usage: glutSolidTorus(innerR, outerR, nsides, rings)", 0 },
    { NULL, 0, 0, NULL, NULL, 0 }
};

#define CMD_TYPE_SPEC(type_, semicolon_, category_) \
    [type_] = { #type_, NULL, (semicolon_), (category_), 1 }

/* Same as CMD_TYPE_SPEC but provides a user-facing display name (the GL
 * function name, e.g. "glBegin") used by error messages. The symbolic
 * `#type_` name is still kept on .name for dumps. */
#define CMD_TYPE_SPEC_NAMED(type_, display_, semicolon_, category_) \
    [type_] = { #type_, (display_), (semicolon_), (category_), 1 }

/* Same as CMD_TYPE_SPEC but marks the command as illegal inside a glBegin
 * block. The parser rejects these at commit time; the executor no longer
 * defensively auto-closes the begin block for them. */
#define CMD_TYPE_SPEC_NOT_IN_BEGIN(type_, semicolon_, category_) \
    [type_] = { #type_, NULL, (semicolon_), (category_), 0 }

/* Combined: not-in-begin + display_name. Used by the GL commands whose
 * error message uses the GL name (glPointSize, glRasterPos3f, glutSolid*). */
#define CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(type_, display_, semicolon_, category_) \
    [type_] = { #type_, (display_), (semicolon_), (category_), 0 }

/* Keyed by CmdType via [type_]= designated initializers, so row order is
 * cosmetic (not load-bearing) — they track the CmdType enum order for
 * readability. Adding a command: drop one CMD_TYPE_SPEC row next to its
 * enum neighbours; a missing row is a zero-initialized (invalid) spec. */
static const ReplCommandTypeSpec g_command_type_specs[CMD_TYPE_COUNT] = {
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_BEGIN, "glBegin", 1, CMD_CAT_PRIMITIVE),
    CMD_TYPE_SPEC(CMD_END,                          1, CMD_CAT_PRIMITIVE),
    CMD_TYPE_SPEC(CMD_VERTEX3F,                     1, CMD_CAT_VERTEX),
    CMD_TYPE_SPEC(CMD_VERTEX2F,                     1, CMD_CAT_VERTEX),
    CMD_TYPE_SPEC(CMD_NORMAL3F,                     1, CMD_CAT_NORMAL),
    CMD_TYPE_SPEC(CMD_COLOR3F,                      1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_COLOR4F,                      1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_ENABLE,                       1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_DISABLE,                      1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_SHADE_MODEL,                  1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_TRANSLATE3F,                  1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_SCALEF,                       1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_ROTATEF,                      1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_PUSH_MATRIX,                  1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_POP_MATRIX,                   1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_LOAD_IDENTITY,                1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_MULT_MATRIXF,                 1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_COLOR_MATERIAL,               1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_LIGHT_MODEL_I,                1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_FRONT_FACE,                   1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_CULL_FACE,                    1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_POLYGON_MODE,   "glPolygonMode",   1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_POLYGON_OFFSET, "glPolygonOffset", 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_DEPTH_FUNC,                   1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_STENCIL_FUNC, "glStencilFunc", 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_STENCIL_OP,   "glStencilOp",   1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_STENCIL_MASK, "glStencilMask", 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_FOR_BEGIN,                    1, CMD_CAT_LOOP),
    CMD_TYPE_SPEC(CMD_FOR_END,                      1, CMD_CAT_LOOP),
    CMD_TYPE_SPEC(CMD_FUNC_DEF,                     1, CMD_CAT_FUNCTION),
    CMD_TYPE_SPEC(CMD_FUNC_END,                     1, CMD_CAT_FUNCTION),
    CMD_TYPE_SPEC(CMD_CALL,                         1, CMD_CAT_FUNCTION),
    CMD_TYPE_SPEC(CMD_IF_BEGIN,                     1, CMD_CAT_CONDITIONAL),
    CMD_TYPE_SPEC(CMD_IF_END,                       1, CMD_CAT_CONDITIONAL),
    CMD_TYPE_SPEC(CMD_COMMENT,                      0, CMD_CAT_COMMENT),
    CMD_TYPE_SPEC(CMD_EMPTY,                        0, CMD_CAT_COMMENT),
    CMD_TYPE_SPEC(CMD_VAR_ASSIGN,                   1, CMD_CAT_VARIABLE),
    CMD_TYPE_SPEC(CMD_SCRATCH_ASSIGN,               1, CMD_CAT_VARIABLE),
    CMD_TYPE_SPEC(CMD_VAR_DECLARE,                  0, CMD_CAT_VARIABLE),
    CMD_TYPE_SPEC(CMD_GOTO_LABEL,                   0, CMD_CAT_LABEL),
    CMD_TYPE_SPEC(CMD_GOTO,                         1, CMD_CAT_LABEL),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_GLUT_TORUS,  "glutSolidTorus",  1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_GLUT_CUBE,   "glutSolidCube",   1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_GLUT_SPHERE, "glutSolidSphere", 1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_GLUT_TEAPOT, "glutSolidTeapot", 1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_GLUT_CONE,   "glutSolidCone",   1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_TESS_BEGIN_POLYGON, "gluTessBeginPolygon", 1, CMD_CAT_TESS_BLOCK),
    CMD_TYPE_SPEC(CMD_TESS_BEGIN_CONTOUR,           1, CMD_CAT_TESS_BLOCK),
    CMD_TYPE_SPEC(CMD_TESS_END,                     1, CMD_CAT_TESS_BLOCK),
    CMD_TYPE_SPEC(CMD_TESS_NORMAL,                  1, CMD_CAT_NORMAL),
    CMD_TYPE_SPEC(CMD_TESS_COLOR,                   1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_TESS_VERTEX,                  1, CMD_CAT_VERTEX),
    CMD_TYPE_SPEC(CMD_MATERIALFV,                   1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_MATERIALF,                    1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_POINT_SIZE,         "glPointSize",         1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_LINE_WIDTH,         "glLineWidth",         1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_LINE_STIPPLE,       "glLineStipple",       1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_POINT_PARAMETER_FV, "glPointParameterfv",  1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_BLEND_FUNC,         "glBlendFunc",         1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_CLEAR_COLOR,        "glClearColor",        1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_CLEAR_DEPTH,        "glClearDepth",        1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_DEPTH_MASK,         "glDepthMask",         1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_COLOR_MASK,         "glColorMask",         1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED(CMD_EDGE_FLAG,              "glEdgeFlag",           1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_RASTER_POS3F,       "glRasterPos3f",       1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_LABEL,              "label",               1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC(CMD_ELSE_IF,                    1, CMD_CAT_CONDITIONAL),
    CMD_TYPE_SPEC(CMD_ELSE,                       1, CMD_CAT_CONDITIONAL),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_CLIP_PLANE,         "glClipPlane",         1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_CLEAR,              "glClear",             1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_FOG_I,              "glFogi",              1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_FOG_F,              "glFogf",              1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_FOG_FV,             "glFogfv",             1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_PUSH_ATTRIB,        "glPushAttrib",        1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_POP_ATTRIB,         "glPopAttrib",         1, CMD_CAT_STATE),
};

const ReplCommandTypeSpec *repl_command_type_spec(CmdType type) {
    if (type < 0 || type >= CMD_TYPE_COUNT)
        return NULL;
    if (!g_command_type_specs[type].name)
        return NULL;
    return &g_command_type_specs[type];
}

const char *repl_cmd_type_name(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    return spec ? spec->name : "CMD_UNKNOWN";
}

const char *cmd_type_name(CmdType type) {
    return repl_cmd_type_name(type);
}

const char *repl_cmd_type_display_name(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    if (spec && spec->display_name)
        return spec->display_name;
    return spec ? spec->name : "CMD_UNKNOWN";
}

int repl_cmd_type_needs_semicolon(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    return spec ? spec->needs_semicolon : 1;
}

CmdSyntaxCategory repl_cmd_type_category(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    return spec ? spec->category : CMD_CAT_DEFAULT;
}

int repl_cmd_type_valid_in_begin(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    return spec ? spec->valid_in_begin : 1;
}

const ReplEnumCommandSpec *repl_enum_command_specs(void) {
    return k_enum_command_specs;
}

const ReplStdCommandSpec *repl_std_command_specs(void) {
    return k_std_command_specs;
}

const ReplFuncCompletion *repl_func_completions(void) {
    return k_func_completions;
}

const char *repl_func_signature_for_name(const char *name) {
    size_t name_len;
    int i;

    if (!name || !*name)
        return NULL;
    name_len = strlen(name);
    for (i = 0; k_func_completions[i].display_text; i++) {
        const char *disp = k_func_completions[i].display_text;
        /* Exact match (e.g. "gluBegin(GLU_POLYGON)" or a bare constant),
         * or a `<name>(...)` signature — require the '(' immediately after
         * the name so "glClear" does not match "glClearColor(...)". */
        if (strcmp(disp, name) == 0)
            return disp;
        if (strncmp(disp, name, name_len) == 0 && disp[name_len] == '(')
            return disp;
    }
    return NULL;
}

const ReplEnumEntry *repl_face_type_entries(void) {
    return k_face_types;
}

const ReplEnumEntry *repl_depth_func_entries(void) {
    return k_depth_funcs;
}

const ReplEnumEntry *repl_material_param_entries(void) {
    return k_material_params;
}

const ReplEnumEntry *repl_point_param_pname_entries(void) {
    return k_point_param_pnames;
}

const ReplEnumEntry *repl_fog_f_pname_entries(void) {
    return k_fog_f_pnames;
}

const ReplEnumEntry *repl_fog_color_pname_entries(void) {
    return k_fog_color_pnames;
}

const ReplEnumEntry *repl_fog_mode_entries(void) {
    return k_fog_modes;
}

const ReplEnumEntry *repl_clip_plane_entries(void) {
    return k_clip_planes;
}

const ReplEnumEntry *repl_attrib_bit_entries(void) {
    return k_attrib_bits;
}

const char *repl_begin_mode_name(GLenum mode) {
    for (int i = 0; k_begin_modes[i].name; i++) {
        if (k_begin_modes[i].value == mode)
            return k_begin_modes[i].name;
    }
    return "???";
}
