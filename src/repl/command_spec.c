#include "repl/command_spec.h"

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
    { "GL_COLOR_MATERIAL",  GL_COLOR_MATERIAL },
    { "GL_CULL_FACE",       GL_CULL_FACE },
    { "GL_DEPTH_TEST",      GL_DEPTH_TEST },
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

static const ReplEnumEntry k_bool_vals[] = {
    { "GL_TRUE",  GL_TRUE  },
    { "GL_FALSE", GL_FALSE },
    { NULL, 0 }
};

static const ReplEnumEntry k_point_param_pnames[] = {
    { "GL_POINT_DISTANCE_ATTENUATION", GL_POINT_DISTANCE_ATTENUATION },
    { NULL, 0 }
};

static const ReplEnumEntry k_blend_src_factors[] = {
    { "GL_SRC_ALPHA", GL_SRC_ALPHA },
    { NULL, 0 }
};

static const ReplEnumEntry k_blend_dst_factors[] = {
    { "GL_ONE_MINUS_SRC_ALPHA", GL_ONE_MINUS_SRC_ALPHA },
    { "GL_ONE",                 GL_ONE },
    { NULL, 0 }
};

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
        "GL_BLEND, GL_COLOR_MATERIAL, GL_CULL_FACE, GL_DEPTH_TEST\n"
        "GL_LIGHTING, GL_LIGHT0..GL_LIGHT3, GL_LINE_SMOOTH, GL_LINE_STIPPLE\n"
        "GL_MULTISAMPLE, GL_NORMALIZE, GL_POINT_SMOOTH",
        REPL_HELP_GROUP_STATE },
    { "glDisable(",          "glDisable(cap)",                                           1, { "cap" },
        "Disable a GL capability (same caps as glEnable)", REPL_HELP_GROUP_STATE },
    { "glShadeModel(",       "glShadeModel(mode)",                                       1, { "mode" },
        "GL_SMOOTH, GL_FLAT", REPL_HELP_GROUP_STATE },
    { "glPointSize(",        "glPointSize(size)",                                        1, { "size" },
        "Rasterized point diameter", REPL_HELP_GROUP_STATE },
    { "glLineWidth(",        "glLineWidth(width)",                                       1, { "width" },
        "Rasterized line width", REPL_HELP_GROUP_STATE },
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
        "GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA / GL_ONE", REPL_HELP_GROUP_BLEND },
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
    /* --- Depth & write-mask state --- */
    { "glFrontFace(",        "glFrontFace(mode)",                                        1, { "mode" },
        "GL_CW, GL_CCW", REPL_HELP_GROUP_DEPTH_MASK },
    { "glDepthFunc(",        "glDepthFunc(func)",                                        1, { "func" },
        "GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS", REPL_HELP_GROUP_DEPTH_MASK },
    { "glDepthMask(",        "glDepthMask(flag)",                                        1, { "flag" },
        "GL_TRUE, GL_FALSE (depth-buffer writes)", REPL_HELP_GROUP_DEPTH_MASK },
    { "glColorMask(",        "glColorMask(red, green, blue, alpha)",                     4, { "red", "green", "blue", "alpha" },
        "Per-channel color write enable: GL_TRUE / GL_FALSE", REPL_HELP_GROUP_DEPTH_MASK },
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
    { "goto ",               "goto label",                                               0, { NULL } },
    REPL_FUNC_SLOT_LIST(FUNC_DECL_COMPLETION)
    { "func0(var0) {",       "func0(var0, [var1], ...) {",                               0, { NULL } },
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
    { "floor(",              "floor(x)",                                                 1, { "x" },
        "Largest integer <= x", REPL_HELP_GROUP_MATH },
    { "ceil(",               "ceil(x)",                                                  1, { "x" },
        "Smallest integer >= x", REPL_HELP_GROUP_MATH },
    { "fmod(",               "fmod(x, y)",                                               2, { "x", "y" },
        "Remainder of x/y (truncated toward zero; sign follows x). "
        "Same as the `%` operator.",
        REPL_HELP_GROUP_MATH },
    { "rem(",                "rem(x, y)",                                                2, { "x", "y" },
        "IEEE remainder of x/y (rounded to nearest; |result| <= |y|/2). "
        "Useful for centring an angle in [-PI, PI].",
        REPL_HELP_GROUP_MATH },
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
    { NULL, NULL, 0, { NULL } }
};

#undef FUNC_CALL_COMPLETION
#undef FUNC_DECL_COMPLETION
#undef REPL_FUNC_SLOT_LIST

/* One positional enum-arg slot. Keeps the spec table rows readable now
 * that each enum command carries an explicit args[] array. */
#define ENUM_SLOT(tbl_, usage_, kind_) { (tbl_), (usage_), (kind_) }

/* Strict token-only slot — the behavior-neutral baseline for every
 * non-bool enum slot (and, until the bool-slot policy lands, for
 * glDepthMask). */
#define ENUM_SLOT_TOK(tbl_, usage_) ENUM_SLOT((tbl_), (usage_), REPL_ENUM_SLOT_ENUM_ONLY)

/* Boolean mask slot — token first, else a constant 0/1 (no runtime
 * vars) reverse-mapped to GL_FALSE/GL_TRUE. The bool-slot policy:
 * glDepthMask and glColorMask. */
#define ENUM_SLOT_BOOL(usage_) ENUM_SLOT(k_bool_vals, (usage_), REPL_ENUM_SLOT_ENUM_OR_CONST_VALUE)

static const ReplEnumCommandSpec k_enum_command_specs[] = {
    { "glBegin",         CMD_BEGIN,          1, "%sglBegin(%s);",            1,
        .args = { ENUM_SLOT_TOK(k_begin_modes, "Unknown mode. Try GL_TRIANGLES, GL_TRIANGLE_STRIP, ...") } },
    { "glBlendFunc",     CMD_BLEND_FUNC,     2, "%sglBlendFunc(%s, %s);",    0,
        .args = { ENUM_SLOT_TOK(k_blend_src_factors, "sfactor: GL_SRC_ALPHA"),
                  ENUM_SLOT_TOK(k_blend_dst_factors, "dfactor: GL_ONE_MINUS_SRC_ALPHA, GL_ONE") } },
    { "glColorMask",     CMD_COLOR_MASK,     4, "%sglColorMask(%s, %s, %s, %s);", 0,
        .args = { ENUM_SLOT_BOOL("red: GL_TRUE or GL_FALSE"),
                  ENUM_SLOT_BOOL("green: GL_TRUE or GL_FALSE"),
                  ENUM_SLOT_BOOL("blue: GL_TRUE or GL_FALSE"),
                  ENUM_SLOT_BOOL("alpha: GL_TRUE or GL_FALSE") } },
    { "glColorMaterial", CMD_COLOR_MATERIAL, 2, "%sglColorMaterial(%s, %s);", 0,
        .args = { ENUM_SLOT_TOK(k_face_types, "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK"),
                  ENUM_SLOT_TOK(k_color_material_modes, "mode: GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_EMISSION, GL_AMBIENT_AND_DIFFUSE") } },
    { "glDepthFunc",     CMD_DEPTH_FUNC,     1, "%sglDepthFunc(%s);",        0,
        .args = { ENUM_SLOT_TOK(k_depth_funcs, "Try GL_LESS, GL_LEQUAL, GL_ALWAYS, ...") } },
    { "glDepthMask",     CMD_DEPTH_MASK,     1, "%sglDepthMask(%s);",        0,
        .args = { ENUM_SLOT_BOOL("Try GL_TRUE or GL_FALSE") } },
    { "glDisable",       CMD_DISABLE,        1, "%sglDisable(%s);",          0,
        .args = { ENUM_SLOT_TOK(k_enable_caps, "Try GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL") } },
    { "glEnable",        CMD_ENABLE,         1, "%sglEnable(%s);",           0,
        .args = { ENUM_SLOT_TOK(k_enable_caps, "Try GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL") } },
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
    { "glShadeModel",    CMD_SHADE_MODEL,    1, "%sglShadeModel(%s);",       0,
        .args = { ENUM_SLOT_TOK(k_shade_models, "Try GL_SMOOTH or GL_FLAT") } },
    { .name = NULL }
};

static const ReplStdCommandSpec k_std_command_specs[] = {
    { "glClearColor",   CMD_CLEAR_COLOR,      4, "glClearColor(%g, %g, %g, %g);",   "Usage: glClearColor(r, g, b, a)", 0 },
    { "glColor3f",      CMD_COLOR3F,          3, "glColor3f(%g, %g, %g);",          "Usage: glColor3f(r, g, b)", 0 },
    { "glColor4f",      CMD_COLOR4F,          4, "glColor4f(%g, %g, %g, %g);",      "Usage: glColor4f(r, g, b, a)", 0 },
    { "glLineWidth",    CMD_LINE_WIDTH,       1, "glLineWidth(%g);",                "Usage: glLineWidth(width)", 0 },
    { "glNormal3f",     CMD_NORMAL3F,         3, "glNormal3f(%g, %g, %g);",         "Usage: glNormal3f(nx, ny, nz)", 0 },
    { "glPointSize",    CMD_POINT_SIZE,       1, "glPointSize(%g);",                "Usage: glPointSize(size)", 0 },
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
    CMD_TYPE_SPEC(CMD_COLOR_MATERIAL,               1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_LIGHT_MODEL_I,                1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_FRONT_FACE,                   1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_DEPTH_FUNC,                   1, CMD_CAT_STATE),
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
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_POINT_PARAMETER_FV, "glPointParameterfv",  1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_BLEND_FUNC,         "glBlendFunc",         1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_CLEAR_COLOR,        "glClearColor",        1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_DEPTH_MASK,         "glDepthMask",         1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_COLOR_MASK,         "glColorMask",         1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_RASTER_POS3F,       "glRasterPos3f",       1, CMD_CAT_STATE),
    CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(CMD_LABEL,              "label",               1, CMD_CAT_GLUT_SHAPE),
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

const ReplEnumEntry *repl_face_type_entries(void) {
    return k_face_types;
}

const ReplEnumEntry *repl_material_param_entries(void) {
    return k_material_params;
}

const ReplEnumEntry *repl_point_param_pname_entries(void) {
    return k_point_param_pnames;
}

const char *repl_begin_mode_name(GLenum mode) {
    for (int i = 0; k_begin_modes[i].name; i++) {
        if (k_begin_modes[i].value == mode)
            return k_begin_modes[i].name;
    }
    return "???";
}
