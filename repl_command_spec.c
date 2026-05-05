#include "repl_command_spec.h"

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

static const ReplEnumEntry k_material_params[] = {
    { "GL_AMBIENT",             GL_AMBIENT },
    { "GL_DIFFUSE",             GL_DIFFUSE },
    { "GL_SPECULAR",            GL_SPECULAR },
    { "GL_EMISSION",            GL_EMISSION },
    { "GL_SHININESS",           GL_SHININESS },
    { "GL_AMBIENT_AND_DIFFUSE", GL_AMBIENT_AND_DIFFUSE },
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

static const ReplFuncCompletion k_func_completions[] = {
    { "glVertex3f(",         "glVertex3f(x, y, z)",                                      3, { "x", "y", "z" } },
    { "glVertex2f(",         "glVertex2f(x, y)",                                         2, { "x", "y" } },
    { "glNormal3f(",         "glNormal3f(nx, ny, nz)",                                   3, { "nx", "ny", "nz" } },
    { "glColor3f(",          "glColor3f(r, g, b)",                                       3, { "r", "g", "b" } },
    { "glColor4f(",          "glColor4f(r, g, b, a)",                                    4, { "r", "g", "b", "a" } },
    { "glClearColor(",       "glClearColor(r, g, b, a)",                                 4, { "r", "g", "b", "a" } },
    { "glBegin(",            "glBegin(mode)",                                            1, { "mode" } },
    { "glEnd()",             "glEnd()",                                                  0, { NULL } },
    { "glEnable(",           "glEnable(cap)",                                            1, { "cap" } },
    { "glDisable(",          "glDisable(cap)",                                           1, { "cap" } },
    { "glShadeModel(",       "glShadeModel(mode)",                                       1, { "mode" } },
    { "glPointSize(",        "glPointSize(size)",                                        1, { "size" } },
    { "glLineWidth(",        "glLineWidth(width)",                                       1, { "width" } },
    { "glPointParameterfv(", "glPointParameterfv(pname, a, b, c)",                       4, { "pname", "a", "b", "c" } },
    { "glBlendFunc(",        "glBlendFunc(sfactor, dfactor)",                            2, { "sfactor", "dfactor" } },
    { "glTranslatef(",       "glTranslatef(x, y, z)",                                    3, { "x", "y", "z" } },
    { "glScalef(",           "glScalef(x, y, z)",                                        3, { "x", "y", "z" } },
    { "glRotatef(",          "glRotatef(angle, x, y, z)",                                4, { "angle", "x", "y", "z" } },
    { "glPushMatrix()",      "glPushMatrix()",                                           0, { NULL } },
    { "glPopMatrix()",       "glPopMatrix()",                                            0, { NULL } },
    { "glColorMaterial(",    "glColorMaterial(face, mode)",                              2, { "face", "mode" } },
    { "glLightModeli(",      "glLightModeli(pname, param)",                              2, { "pname", "param" } },
    { "glFrontFace(",        "glFrontFace(mode)",                                        1, { "mode" } },
    { "glDepthMask(",        "glDepthMask(flag)",                                        1, { "flag" } },
    { "glMaterialf(",        "glMaterialf(face, pname, value[, g, b, a])",               6, { "face", "pname", "value", "g", "b", "a" } },
    { "glutSolidTorus(",     "glutSolidTorus(inner_r, outer_r, nsides, rings)",          4, { "inner_r", "outer_r", "nsides", "rings" } },
    { "glutSolidCube(",      "glutSolidCube(size)",                                      1, { "size" } },
    { "glutSolidSphere(",    "glutSolidSphere(radius, slices, stacks)",                  3, { "radius", "slices", "stacks" } },
    { "glutSolidTeapot(",    "glutSolidTeapot(size)",                                    1, { "size" } },
    { "glutSolidCone(",      "glutSolidCone(base, height, slices, stacks)",              4, { "base", "height", "slices", "stacks" } },
    { "gluBegin(GLU_POLYGON)", "gluBegin(GLU_POLYGON)",                                  0, { NULL } },
    { "gluBegin(GLU_CONTOUR)", "gluBegin(GLU_CONTOUR)",                                  0, { NULL } },
    { "gluEnd()",            "gluEnd()",                                                 0, { NULL } },
    { "gluNormal(",          "gluNormal(nx, ny, nz)",                                    3, { "nx", "ny", "nz" } },
    { "gluColor(",           "gluColor(r, g, b, a)",                                     4, { "r", "g", "b", "a" } },
    { "gluVertex(",          "gluVertex(x, y, z)",                                       3, { "x", "y", "z" } },
    { "float ",              "float name",                                               0, { NULL } },
    { "for(",                "for(var, start, end[, step])",                             4, { "var", "start", "end", "step" } },
    { "if(",                 "if(expr)",                                                 1, { "expr" } },
    { "goto ",               "goto label",                                               0, { NULL } },
    { "func0 {",             "func0 {",                                                  0, { NULL } },
    { "func0(var0) {",       "func0(var0, [var1], ...) {",                               0, { NULL } },
    { "func1 {",             "func1 {",                                                  0, { NULL } },
    { "func2 {",             "func2 {",                                                  0, { NULL } },
    { "func3 {",             "func3 {",                                                  0, { NULL } },
    { "func4 {",             "func4 {",                                                  0, { NULL } },
    { "func5 {",             "func5 {",                                                  0, { NULL } },
    { "func6 {",             "func6 {",                                                  0, { NULL } },
    { "func7 {",             "func7 {",                                                  0, { NULL } },
    { "func8 {",             "func8 {",                                                  0, { NULL } },
    { "func9 {",             "func9 {",                                                  0, { NULL } },
    { "func0()",             "func0()",                                                  0, { NULL } },
    { "func1()",             "func1()",                                                  0, { NULL } },
    { "func2()",             "func2()",                                                  0, { NULL } },
    { "func3()",             "func3()",                                                  0, { NULL } },
    { "func4()",             "func4()",                                                  0, { NULL } },
    { "func5()",             "func5()",                                                  0, { NULL } },
    { "func6()",             "func6()",                                                  0, { NULL } },
    { "func7()",             "func7()",                                                  0, { NULL } },
    { "func8()",             "func8()",                                                  0, { NULL } },
    { "func9()",             "func9()",                                                  0, { NULL } },
    { "x = ",                "x = value",                                                0, { NULL } },
    { "y = ",                "y = value",                                                0, { NULL } },
    { "z = ",                "z = value",                                                0, { NULL } },
    { "i = ",                "i = value",                                                0, { NULL } },
    { "j = ",                "j = value",                                                0, { NULL } },
    { "k = ",                "k = value",                                                0, { NULL } },
    { "n = ",                "n = value",                                                0, { NULL } },
    { "t = ",                "t = value",                                                0, { NULL } },
    { "sin(",                "sin(x)",                                                   1, { "x" } },
    { "cos(",                "cos(x)",                                                   1, { "x" } },
    { "tan(",                "tan(x)",                                                   1, { "x" } },
    { "sqrt(",               "sqrt(x)",                                                  1, { "x" } },
    { "abs(",                "abs(x)",                                                   1, { "x" } },
    { "pow(",                "pow(base, exp)",                                           2, { "base", "exp" } },
    { "min(",                "min(a, b)",                                                2, { "a", "b" } },
    { "max(",                "max(a, b)",                                                2, { "a", "b" } },
    { "floor(",              "floor(x)",                                                 1, { "x" } },
    { "ceil(",               "ceil(x)",                                                  1, { "x" } },
    { "fmod(",               "fmod(x, y)",                                               2, { "x", "y" } },
    { "rand(",               "rand(seed[, iter])",                                       2, { "seed", "iter" } },
    { "PI",                  "PI",                                                       0, { NULL } },
    { "TAU",                 "TAU",                                                      0, { NULL } },
    { NULL, NULL, 0, { NULL } }
};

static const ReplEnumCommandSpec k_enum_command_specs[] = {
    { "glBegin",         CMD_BEGIN,         1, k_begin_modes,        NULL,              "%sglBegin(%s);",             "Unknown mode. Try GL_TRIANGLES, GL_TRIANGLE_STRIP, ...", NULL, 1 },
    { "glEnable",        CMD_ENABLE,        1, k_enable_caps,        NULL,              "%sglEnable(%s);",            "Try GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL", NULL, 0 },
    { "glDisable",       CMD_DISABLE,       1, k_enable_caps,        NULL,              "%sglDisable(%s);",           "Try GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL", NULL, 0 },
    { "glShadeModel",    CMD_SHADE_MODEL,   1, k_shade_models,       NULL,              "%sglShadeModel(%s);",        "Try GL_SMOOTH or GL_FLAT", NULL, 0 },
    { "glFrontFace",     CMD_FRONT_FACE,    1, k_front_face,         NULL,              "%sglFrontFace(%s);",         "Try GL_CW or GL_CCW", NULL, 0 },
    { "glDepthMask",     CMD_DEPTH_MASK,    1, k_bool_vals,          NULL,              "%sglDepthMask(%s);",         "Try GL_TRUE or GL_FALSE", NULL, 0 },
    { "glColorMaterial", CMD_COLOR_MATERIAL,2, k_face_types,         k_color_material_modes, "%sglColorMaterial(%s, %s);", "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK", "mode: GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_EMISSION, GL_AMBIENT_AND_DIFFUSE", 0 },
    { "glMaterialf",     CMD_MATERIALF,    -2, k_face_types,         k_material_params, NULL,                         "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK", "pname: GL_DIFFUSE, GL_AMBIENT, GL_SPECULAR, GL_SHININESS", 0 },
    { "glLightModeli",   CMD_LIGHT_MODEL_I, 2, k_light_model_params, k_bool_vals,       "%sglLightModeli(%s, %s);",   "pname: GL_LIGHT_MODEL_TWO_SIDE, GL_LIGHT_MODEL_LOCAL_VIEWER", "param: GL_TRUE, GL_FALSE, or integer", 0 },
    { "glBlendFunc",     CMD_BLEND_FUNC,    2, k_blend_src_factors,  k_blend_dst_factors, "%sglBlendFunc(%s, %s);",  "sfactor: GL_SRC_ALPHA", "dfactor: GL_ONE_MINUS_SRC_ALPHA, GL_ONE", 0 },
    { NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, 0 }
};

static const ReplStdCommandSpec k_std_command_specs[] = {
    { "glVertex3f",     CMD_VERTEX3F,         3, "glVertex3f(%g, %g, %g);",         "Usage: glVertex3f(x, y, z)", 0 },
    { "glNormal3f",     CMD_NORMAL3F,         3, "glNormal3f(%g, %g, %g);",         "Usage: glNormal3f(nx, ny, nz)", 0 },
    { "glColor3f",      CMD_COLOR3F,          3, "glColor3f(%g, %g, %g);",          "Usage: glColor3f(r, g, b)", 0 },
    { "glColor4f",      CMD_COLOR4F,          4, "glColor4f(%g, %g, %g, %g);",      "Usage: glColor4f(r, g, b, a)", 0 },
    { "glClearColor",   CMD_CLEAR_COLOR,      4, "glClearColor(%g, %g, %g, %g);",   "Usage: glClearColor(r, g, b, a)", 0 },
    { "glTranslatef",   CMD_TRANSLATE3F,      3, "glTranslatef(%g, %g, %g);",       "Usage: glTranslatef(x, y, z)", 0 },
    { "glScalef",       CMD_SCALEF,           3, "glScalef(%g, %g, %g);",           "Usage: glScalef(x, y, z)", 0 },
    { "glRotatef",      CMD_ROTATEF,          4, "glRotatef(%g, %g, %g, %g);",      "Usage: glRotatef(angle, x, y, z)", 0 },
    { "glVertex2f",     CMD_VERTEX2F,         2, "glVertex2f(%g, %g);",             "Usage: glVertex2f(x, y)", 0 },
    { "glutSolidTorus",   CMD_GLUT_TORUS,    4, "glutSolidTorus(%g, %g, %g, %g);",   "Usage: glutSolidTorus(innerR, outerR, nsides, rings)", 0 },
    { "glutSolidCube",    CMD_GLUT_CUBE,     1, "glutSolidCube(%g);",                 "Usage: glutSolidCube(size)", 0 },
    { "glutSolidSphere",  CMD_GLUT_SPHERE,   3, "glutSolidSphere(%g, %g, %g);",       "Usage: glutSolidSphere(radius, slices, stacks)", 0 },
    { "glutSolidTeapot",  CMD_GLUT_TEAPOT,   1, "glutSolidTeapot(%g);",               "Usage: glutSolidTeapot(size)", 0 },
    { "glutSolidCone",    CMD_GLUT_CONE,     4, "glutSolidCone(%g, %g, %g, %g);",     "Usage: glutSolidCone(base, height, slices, stacks)", 0 },
    { "glPointSize",    CMD_POINT_SIZE,       1, "glPointSize(%g);",                "Usage: glPointSize(size)", 0 },
    { "glLineWidth",    CMD_LINE_WIDTH,       1, "glLineWidth(%g);",                "Usage: glLineWidth(width)", 0 },
    { "gluNormal",      CMD_TESS_NORMAL,      3, "gluNormal(%g, %g, %g);",          "Usage: gluNormal(x, y, z)", 1 },
    { "gluVertex",      CMD_TESS_VERTEX,      3, "gluVertex(%g, %g, %g);",          "Usage: gluVertex(x, y, z)", 1 },
    { NULL, 0, 0, NULL, NULL, 0 }
};

#define CMD_TYPE_SPEC(type_, semicolon_, block_indent_, category_) \
    [type_] = { #type_, (semicolon_), (block_indent_), (category_) }

static const ReplCommandTypeSpec g_command_type_specs[CMD_TYPE_COUNT] = {
    CMD_TYPE_SPEC(CMD_BEGIN,                1, 1, CMD_CAT_PRIMITIVE),
    CMD_TYPE_SPEC(CMD_END,                  1, 1, CMD_CAT_PRIMITIVE),
    CMD_TYPE_SPEC(CMD_VERTEX3F,             1, 1, CMD_CAT_VERTEX),
    CMD_TYPE_SPEC(CMD_VERTEX2F,             1, 1, CMD_CAT_VERTEX),
    CMD_TYPE_SPEC(CMD_NORMAL3F,             1, 1, CMD_CAT_NORMAL),
    CMD_TYPE_SPEC(CMD_COLOR3F,              1, 1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_COLOR4F,              1, 1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_ENABLE,               1, 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_DISABLE,              1, 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_SHADE_MODEL,          1, 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_TRANSLATE3F,          1, 1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_SCALEF,               1, 1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_ROTATEF,              1, 1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_PUSH_MATRIX,          1, 1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_POP_MATRIX,           1, 1, CMD_CAT_TRANSFORM),
    CMD_TYPE_SPEC(CMD_COLOR_MATERIAL,       1, 1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_LIGHT_MODEL_I,        1, 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_FRONT_FACE,           1, 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_FOR_BEGIN,            1, 0, CMD_CAT_LOOP),
    CMD_TYPE_SPEC(CMD_FOR_END,              1, 0, CMD_CAT_LOOP),
    CMD_TYPE_SPEC(CMD_FUNC_DEF,             1, 0, CMD_CAT_FUNCTION),
    CMD_TYPE_SPEC(CMD_FUNC_END,             1, 0, CMD_CAT_FUNCTION),
    CMD_TYPE_SPEC(CMD_CALL,                 1, 0, CMD_CAT_FUNCTION),
    CMD_TYPE_SPEC(CMD_IF_BEGIN,             1, 0, CMD_CAT_CONDITIONAL),
    CMD_TYPE_SPEC(CMD_IF_END,               1, 0, CMD_CAT_CONDITIONAL),
    CMD_TYPE_SPEC(CMD_COMMENT,              0, 0, CMD_CAT_COMMENT),
    CMD_TYPE_SPEC(CMD_VAR_ASSIGN,           1, 0, CMD_CAT_VARIABLE),
    CMD_TYPE_SPEC(CMD_VAR_DECLARE,          0, 0, CMD_CAT_VARIABLE),
    CMD_TYPE_SPEC(CMD_LABEL,                0, 0, CMD_CAT_LABEL),
    CMD_TYPE_SPEC(CMD_GOTO,                 1, 0, CMD_CAT_LABEL),
    CMD_TYPE_SPEC(CMD_GLUT_TORUS,           1, 1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC(CMD_GLUT_CUBE,            1, 1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC(CMD_GLUT_SPHERE,          1, 1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC(CMD_GLUT_TEAPOT,          1, 1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC(CMD_GLUT_CONE,            1, 1, CMD_CAT_GLUT_SHAPE),
    CMD_TYPE_SPEC(CMD_TESS_BEGIN_POLYGON,   1, 1, CMD_CAT_TESS_BLOCK),
    CMD_TYPE_SPEC(CMD_TESS_BEGIN_CONTOUR,   1, 1, CMD_CAT_TESS_BLOCK),
    CMD_TYPE_SPEC(CMD_TESS_END,             1, 1, CMD_CAT_TESS_BLOCK),
    CMD_TYPE_SPEC(CMD_TESS_NORMAL,          1, 1, CMD_CAT_NORMAL),
    CMD_TYPE_SPEC(CMD_TESS_COLOR,           1, 1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_TESS_VERTEX,          1, 1, CMD_CAT_VERTEX),
    CMD_TYPE_SPEC(CMD_MATERIALF,            1, 1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_POINT_SIZE,           1, 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_LINE_WIDTH,           1, 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_POINT_PARAMETER_FV,   1, 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_BLEND_FUNC,           1, 1, CMD_CAT_STATE),
    CMD_TYPE_SPEC(CMD_CLEAR_COLOR,          1, 1, CMD_CAT_COLOR),
    CMD_TYPE_SPEC(CMD_DEPTH_MASK,           1, 1, CMD_CAT_STATE),
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

int repl_cmd_type_needs_semicolon(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    return spec ? spec->needs_semicolon : 1;
}

int repl_cmd_type_needs_block_indent(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    return spec ? spec->needs_block_indent : 1;
}

CmdSyntaxCategory repl_cmd_type_category(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    return spec ? spec->category : CMD_CAT_DEFAULT;
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
