#include "app/glr_ctrl.h"
/*
 * test_repl_export_all_commands.c - Comprehensive roundtrip test for all supported GL commands
 *
 * Verifies that every supported GL command survives the export/import cycle
 * with stable, identical code output (like test_repl_core_examples.c).
 */
#include "repl/command_spec.h"  /* cmd_type_name */
#include "repl/export.h"
#include "repl/export_internal.h"
#include "source_document.h"
#include "editor/input.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/state.h"
#include "repl/pipeline.h"

#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static int compare_text(const char *text1, const char *text2, int *out_diff_line) {
    int line = 1;
    const char *p1 = text1 ? text1 : "";
    const char *p2 = text2 ? text2 : "";

    while (*p1 && *p2) {
        if (*p1 != *p2) {
            if (out_diff_line) *out_diff_line = line;
            return 0;
        }
        if (*p1 == '\n') line++;
        p1++;
        p2++;
    }
    if (*p1 != *p2) {
        if (out_diff_line) *out_diff_line = line;
        return 0;
    }
    return 1;
}

static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
    repl_eval_declare_predef_var("r", err, sizeof(err));
}

/* List of GL commands that SHOULD be included in the mega example.
 * Excludes control flow (for, if, func, labels), comments, and variable declarations. */
static const CmdType expected_commands[] = {
    CMD_BEGIN,
    CMD_END,
    CMD_VERTEX3F,
    CMD_VERTEX2F,
    CMD_NORMAL3F,
    CMD_COLOR3F,
    CMD_COLOR4F,
    CMD_TRANSLATE3F,
    CMD_SCALEF,
    CMD_ROTATEF,
    CMD_PUSH_MATRIX,
    CMD_POP_MATRIX,
    CMD_LOAD_IDENTITY,
    CMD_ENABLE,
    CMD_DISABLE,
    CMD_SHADE_MODEL,
    CMD_FRONT_FACE,
    CMD_CULL_FACE,
    CMD_DEPTH_FUNC,
    CMD_STENCIL_FUNC,
    CMD_STENCIL_OP,
    CMD_STENCIL_MASK,
    CMD_POINT_SIZE,
    CMD_LINE_WIDTH,
    CMD_LINE_STIPPLE,
    CMD_POINT_PARAMETER_FV,
    CMD_BLEND_FUNC,
    CMD_CLEAR_COLOR,
    CMD_CLEAR_DEPTH,
    CMD_CLEAR_STENCIL,
    CMD_DEPTH_MASK,
    CMD_COLOR_MASK,
    CMD_EDGE_FLAG,
    CMD_COLOR_MATERIAL,
    CMD_MATERIALFV,
    CMD_MATERIALF,
    CMD_LIGHT_MODEL_I,
    CMD_GLUT_SPHERE,
    CMD_GLUT_CUBE,
    CMD_GLUT_CONE,
    CMD_GLUT_TORUS,
    CMD_GLUT_TEAPOT,
    CMD_TESS_BEGIN_POLYGON,
    CMD_TESS_BEGIN_CONTOUR,
    CMD_TESS_NORMAL,
    CMD_TESS_COLOR,
    CMD_TESS_VERTEX,
    CMD_TESS_END,
    CMD_RASTER_POS3F,
    CMD_LABEL,
    CMD_CLIP_PLANE,
    CMD_CLEAR,
    CMD_FOG_I,
    CMD_FOG_F,
    CMD_FOG_FV,
};

static int verify_all_commands_present(void) {
    int expected_count = sizeof(expected_commands) / sizeof(expected_commands[0]);
    const GLCmd *cmds = repl_state_document_cmds();
    int cmd_count = repl_state_document_count();

    /* Mark which commands were found */
    int found[MAX_EDITOR_COMMANDS] = {0};
    for (int i = 0; i < cmd_count; i++) {
        CmdType type = cmds[i].type;
        if (type >= 0 && type < MAX_EDITOR_COMMANDS) {
            found[type] = 1;
        }
    }

    /* Check each expected command is present */
    for (int i = 0; i < expected_count; i++) {
        CmdType expected = expected_commands[i];
        if (!found[expected]) {
            printf("MISSING: %s not found in mega example\n",
                   cmd_type_name(expected));
            return 0;
        }
    }

    return 1;
}


static char *slurp_stream(FILE *f) {
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    int c;

    if (!f) return NULL;
    rewind(f);

    while ((c = fgetc(f)) != EOF) {
        if (len + 1 >= cap) {
            cap = cap ? cap * 2 : 4096;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)c;
    }
    if (buf) {
        if (len >= cap) {
            char *tmp = realloc(buf, len + 1);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
        buf[len] = '\0';
    } else {
        buf = strdup("");
    }
    return buf;
}

static char *dump_code_panel(void) {
    /* Capture the code panel text like test_repl_core_examples does */
    FILE *tmp = tmpfile();
    char *buf;

    if (!tmp)
        return strdup("");
    repl_dump_code_panel_text(tmp, source_document_view());
    buf = slurp_stream(tmp);
    fclose(tmp);
    return buf ? buf : strdup("");
}

static char *slurp_path(const char *path) {
    FILE *f = fopen(path, "rb");
    char *buf;

    if (!f)
        return NULL;
    buf = slurp_stream(f);
    fclose(f);
    return buf;
}

static void test_export_prologue_direct(void) {
    char err_buf[128];

    /* Initialize and register a persistent variable */
    repl_eval_init_predef_vars();
    repl_eval_declare_predef_var("persistent_var", err_buf, sizeof(err_buf));
    repl_eval_declare_predef_var("t", err_buf, sizeof(err_buf)); /* "t" is non-persistent */

    /* 1. export_has_persistent_predef_vars */
    ASSERT_TRUE("export_has_persistent_predef_vars is true", export_has_persistent_predef_vars());

    /* 2. write_predef_var_globals */
    FILE *f1 = tmpfile();
    if (f1) {
        write_predef_var_globals(f1);
        char *str = slurp_stream(f1);
        ASSERT_TRUE("globals contains persistent_var", str && strstr(str, "static float persistent_var") != NULL);
        ASSERT_TRUE("globals contains t", str && strstr(str, "static float t") != NULL);
        free(str);
        fclose(f1);
    }

    /* 3. write_save_restore_helpers */
    FILE *f2 = tmpfile();
    if (f2) {
        write_save_restore_helpers(f2);
        char *str = slurp_stream(f2);
        ASSERT_TRUE("save/restore contains persistent_var", str && strstr(str, "_saved_persistent_var") != NULL);
        ASSERT_TRUE("save/restore does not contain t", str && strstr(str, "_saved_t") == NULL);
        free(str);
        fclose(f2);
    }

    /* 4. write_rand_helper */
    FILE *f3 = tmpfile();
    if (f3) {
        write_rand_helper(f3);
        char *str = slurp_stream(f3);
        ASSERT_TRUE("rand helper written", str && strstr(str, "repl_randf") != NULL);
        free(str);
        fclose(f3);
    }

    /* 5. write_glfloat_vector_helpers */
    FILE *f4 = tmpfile();
    if (f4) {
        write_glfloat_vector_helpers(f4);
        char *str = slurp_stream(f4);
        ASSERT_TRUE("glfloat helpers written", str && strstr(str, "repl_glfloat4_buf") != NULL);
        free(str);
        fclose(f4);
    }

    /* 6. write_label_helper */
    FILE *f5 = tmpfile();
    if (f5) {
        write_label_helper(f5);
        char *str = slurp_stream(f5);
        ASSERT_TRUE("label helper written", str && strstr(str, "void label(const char *fmt, ...)") != NULL);
        free(str);
        fclose(f5);
    }

    /* 7. write_tess_preamble */
    FILE *f6 = tmpfile();
    if (f6) {
        write_tess_preamble(f6);
        char *str = slurp_stream(f6);
        ASSERT_TRUE("tess preamble written", str && strstr(str, "GLUtesselator") != NULL);
        free(str);
        fclose(f6);
    }

    /* 8. write_render_helper_as_c */
    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glEnd();");
    export_set_source_text_view(source_document_view());
    FILE *f7 = tmpfile();
    if (f7) {
        write_render_helper_as_c(f7, "my_draw_scene");
        char *str = slurp_stream(f7);
        ASSERT_TRUE("render helper name", str && strstr(str, "static void my_draw_scene(void)") != NULL);
        ASSERT_TRUE("render helper body", str && strstr(str, "glVertex3f(0, 0, 0);") != NULL);
        free(str);
        fclose(f7);
    }

    /* Unbalanced Begin/End rendering */
    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    export_set_source_text_view(source_document_view());
    FILE *f7b = tmpfile();
    if (f7b) {
        write_render_helper_as_c(f7b, "my_draw_scene_unbalanced");
        char *str = slurp_stream(f7b);
        ASSERT_TRUE("unbalanced render helper ends", str && strstr(str, "glEnd();") != NULL);
        free(str);
        fclose(f7b);
    }

    /* 9. write_func_defs_as_c */
    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line("func0() {");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("}");
    export_set_source_text_view(source_document_view());
    FILE *f8 = tmpfile();
    if (f8) {
        write_func_defs_as_c(f8);
        char *str = slurp_stream(f8);
        ASSERT_TRUE("func def written", str && strstr(str, "static void func0(void) {") != NULL);
        free(str);
        fclose(f8);
    }

    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line("func1(x, i) {");
    editor_feed_line("glVertex3f(x, i, 0);");
    editor_feed_line("}");
    export_set_source_text_view(source_document_view());
    FILE *f9 = tmpfile();
    if (f9) {
        write_func_defs_as_c(f9);
        char *str = slurp_stream(f9);
        ASSERT_TRUE("func def with params written", str && strstr(str, "static void func1(float x, float i) {") != NULL);
        free(str);
        fclose(f9);
    }
}

/* Count document rows carrying is_auto, split by whether the pass generated
 * them. A hand-written normal must never come back marked. */
static int count_auto_normals(void) {
    int n = 0;
    for (int i = 0; i < repl_state_document_count(); i++) {
        const GLCmd *cmd = &repl_state_document_cmds()[i];
        if (!cmd->valid)
            continue;
        if ((cmd->type == CMD_NORMAL3F || cmd->type == CMD_TESS_NORMAL) &&
            cmd->is_auto)
            n++;
    }
    return n;
}

/* is_auto has to survive export/import or a reloaded scene's generated
 * normals read as hand-written, and the autonormal pass then refuses to
 * update them (a hand-written normal owns its block) — the normals freeze
 * at whatever values were exported. The marker rides the command's own
 * line, so this also pins that it never leaks into the REPL row text. */
static void test_auto_normal_marker_roundtrip(void) {
    const char *path = "/tmp/repl_export_auto_normal.c";
    char *export_text;
    char *code_before;
    char *code_after;
    int auto_before, auto_after;
    int manual_row = -1;
    int diff_line = 0;
    FILE *f;

    repl_eval_init_predef_vars();
    glr_ctrl_reset_all();

    /* First block keeps a hand-written normal (the pass leaves it alone),
     * second gets a generated one, third exercises the tess contour path
     * whose exported form is lowered C rather than REPL source text. */
    editor_feed_line("glBegin(GL_TRIANGLES);");
    /* A normal no face of this document generates, so the assertions
     * below cannot match a generated line by coincidence. */
    editor_feed_line("glNormal3f(0.25, 0.5, 0.75);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 2);");
    editor_feed_line("glVertex3f(1, 0, 2);");
    editor_feed_line("glVertex3f(0, 1, 2);");
    editor_feed_line("glEnd();");
    editor_feed_line("gluBegin(GLU_POLYGON);");
    editor_feed_line("gluBegin(GLU_CONTOUR);");
    editor_feed_line("gluVertex(0, 0, 4);");
    editor_feed_line("gluVertex(1, 0, 4);");
    editor_feed_line("gluVertex(1, 1, 4);");
    editor_feed_line("gluEnd();");
    editor_feed_line("gluEnd();");

    repl_recompute_autonormals(REPL_AUTONORMAL_FACE, NULL);

    for (int i = 0; i < repl_state_document_count(); i++) {
        const GLCmd *cmd = &repl_state_document_cmds()[i];
        if (cmd->valid && cmd->type == CMD_NORMAL3F && !cmd->is_auto) {
            manual_row = i;
            break;
        }
    }
    ASSERT_TRUE("hand-written normal survives the autonormal pass",
                manual_row >= 0);

    auto_before = count_auto_normals();
    ASSERT_TRUE("autonormal pass generated both an immediate and a tess normal",
                auto_before >= 2);

    code_before = dump_code_panel();
    repl_export_save_output(path, source_document_view(), NULL);

    f = fopen(path, "r");
    ASSERT_TRUE("auto-normal export opened", f != NULL);
    export_text = f ? slurp_stream(f) : NULL;
    if (f) fclose(f);
    ASSERT_TRUE("auto-normal export read", export_text != NULL);

    if (export_text) {
        ASSERT_TRUE("generated normal exports with the auto marker",
                    strstr(export_text, "/* @auto */") != NULL);
        ASSERT_TRUE("hand-written normal exports untagged",
                    strstr(export_text,
                           "glNormal3f(0.25, 0.5, 0.75); /* @auto */") == NULL);
        ASSERT_TRUE("hand-written normal still exports",
                    strstr(export_text, "glNormal3f(0.25, 0.5, 0.75);") != NULL);
        ASSERT_TRUE("tess normal carries the marker on its lowered form",
                    strstr(export_text, "_tn[2] = 1; } /* @auto */") != NULL ||
                    strstr(export_text, "_tn[2] = -1; } /* @auto */") != NULL);
    }

    glr_ctrl_reset_all();
    ASSERT_TRUE("auto-normal import succeeded",
                repl_export_load_from_file(path, NULL) == 1);

    auto_after = count_auto_normals();
    ASSERT_TRUE("is_auto restored on import", auto_after == auto_before);

    {
        int manual_after = -1;
        for (int i = 0; i < repl_state_document_count(); i++) {
            const GLCmd *cmd = &repl_state_document_cmds()[i];
            if (cmd->valid && cmd->type == CMD_NORMAL3F && !cmd->is_auto) {
                manual_after = i;
                break;
            }
        }
        ASSERT_TRUE("hand-written normal stays unmarked across the roundtrip",
                    manual_after >= 0);
    }

    /* The marker is transport, not source: no row may come back carrying it. */
    code_after = dump_code_panel();
    ASSERT_TRUE("no @auto marker leaks into the document text",
                code_after && strstr(code_after, "@auto") == NULL);
    ASSERT_TRUE("auto-normal roundtrip is text-exact",
                compare_text(code_before, code_after, &diff_line));
    if (code_before && code_after &&
        !compare_text(code_before, code_after, &diff_line)) {
        printf("Auto-normal roundtrip mismatch at line %d\nBefore:\n%s\n\nAfter:\n%s\n",
               diff_line, code_before, code_after);
    }

    free(export_text);
    free(code_before);
    free(code_after);
    remove(path);
}

int main(void) {
    test_export_prologue_direct();
    test_auto_normal_marker_roundtrip();
    const char *path1 = "/tmp/repl_export_all_commands_1.c";
    const char *path2 = "/tmp/repl_export_all_commands_2.c";
    int cmd_count_before = 0;
    CmdType cmd_types_before[MAX_EDITOR_COMMANDS];
    char *code_before = NULL;
    char *code_after = NULL;
    char *code_reexport = NULL;
    char *export_text = NULL;
    int diff_line = 0;
    int dynamic_stencil_ref = -1;

    repl_eval_init_predef_vars();
    glr_ctrl_reset_all();
    declare_test_vars();

    /* Add one example of every supported GL command to verify roundtrip */

    /* Vertex/geometry commands */
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex2f(1, 1);");
    editor_feed_line("glNormal3f(0, 0, 1);");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glColor4f(0, 1, 0, 0.5);");
    editor_feed_line("glEnd();");

    /* Transform commands */
    editor_feed_line("glTranslatef(1, 2, 3);");
    editor_feed_line("glScalef(0.5, 0.5, 0.5);");
    editor_feed_line("glRotatef(45, 0, 0, 1);");
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glPopMatrix();");
    editor_feed_line("glLoadIdentity();");
    /* glMultMatrixf names a scratch array; the cells feeding it have to
     * exist in the document too, or the roundtrip has nothing to carry. */
    editor_feed_line("A[0] = 1;");
    editor_feed_line("A[5] = 1;");
    editor_feed_line("A[10] = 1;");
    editor_feed_line("A[15] = 1;");
    editor_feed_line("glMultMatrixf(A);");
    /* The other glMultMatrixf form: 16 inline cells, which export writes
     * through the repl_glfloat16 C89 helper and import reads back. */
    editor_feed_line("glMultMatrixf((GLfloat[]){1, 0, 0, 0, 0, 1, 0, 0, "
                     "0, 0, 1, 0, 0.5, 0, 0, 1});");

    /* State/capability commands */
    editor_feed_line("glEnable(GL_DEPTH_TEST);");
    editor_feed_line("glDisable(GL_DEPTH_TEST);");
    editor_feed_line("glShadeModel(GL_SMOOTH);");
    editor_feed_line("glFrontFace(GL_CCW);");
    editor_feed_line("glCullFace(GL_BACK);");
    editor_feed_line("glDepthFunc(GL_LEQUAL);");
    editor_feed_line("glStencilFunc(GL_EQUAL, 1, 0xFF);");
    editor_feed_line("glStencilFunc(GL_EQUAL, x + 0.9, 0xFF);");
    editor_feed_line("glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);");
    editor_feed_line("glStencilMask(0x7F);");
    editor_feed_line("glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);");
    editor_feed_line("glPolygonOffset(-1, -1);");

    /* Attribute commands */
    editor_feed_line("glPointSize(5);");
    editor_feed_line("glLineWidth(2);");
    editor_feed_line("glLineStipple(2, 61680);");
    editor_feed_line("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1, 0, 0.01);");

    /* Blend/depth commands */
    editor_feed_line("glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);");
    editor_feed_line("glClipPlane(GL_CLIP_PLANE0, (GLdouble[]){0, 1, 0, 0.5});");
    editor_feed_line("glFogi(GL_FOG_MODE, GL_EXP2);");
    editor_feed_line("glFogf(GL_FOG_DENSITY, 0.08);");
    editor_feed_line("glFogfv(GL_FOG_COLOR, (GLfloat[]){0.05, 0.06, 0.08, 1});");
    editor_feed_line("glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);");
    editor_feed_line("glClearColor(0.2, 0.2, 0.2, 1);");
    editor_feed_line("glClearDepth(0.9);");
    editor_feed_line("glClearStencil(0x2A);");
    editor_feed_line("glDepthMask(GL_TRUE);");
    editor_feed_line("glEdgeFlag(GL_TRUE);");
    editor_feed_line("glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);");

    /* Material/lighting commands */
    editor_feed_line("glColorMaterial(GL_FRONT, GL_DIFFUSE);");
    editor_feed_line("glMaterialfv(GL_FRONT, GL_SHININESS, 128);");
    editor_feed_line("glMaterialf(GL_BACK, GL_SHININESS, 64);");
    editor_feed_line("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);");

    /* GLUT solid shapes */
    editor_feed_line("glutSolidSphere(1, 16, 12);");
    editor_feed_line("glutSolidCube(0.5);");
    editor_feed_line("glutSolidCone(0.5, 1, 8, 1);");
    editor_feed_line("glutSolidTorus(0.1, 0.5, 8, 4);");
    editor_feed_line("glutSolidTeapot(0.5);");

    /* glRasterPos3f sets the raster position; label emits text at
     * that position. Together they exercise the round-trip for both
     * the new state primitive and the format-string + substitution
     * arg list. */
    editor_feed_line("glRasterPos3f(0, 1.5, 0);");
    editor_feed_line("label(\"x = %f\", x);");

    /* GLU tessellation commands - polygon */
    editor_feed_line("gluBegin(GLU_POLYGON);");
    editor_feed_line("gluNormal(0, 0, 1);");
    editor_feed_line("gluColor(1, 0, 0, 1);");
    editor_feed_line("gluVertex(0.5, 0.5, 0);");
    editor_feed_line("gluEnd();");

    /* GLU tessellation commands - contour */
    editor_feed_line("gluBegin(GLU_CONTOUR);");
    editor_feed_line("gluVertex(1, 1, 0);");
    editor_feed_line("gluEnd();");

    cmd_count_before = repl_state_document_count();
    ASSERT_TRUE("commands loaded", cmd_count_before > 0);

    /* Verify all supported commands are included in mega example */
    ASSERT_TRUE("all supported commands present", verify_all_commands_present());

    /* Save command types for verification */
    for (int i = 0; i < cmd_count_before && i < MAX_EDITOR_COMMANDS; i++) {
        cmd_types_before[i] = repl_state_document_cmds()[i].type;
    }

    /* The live flat command truncates the dynamic reference exactly as C's
     * GLint parameter conversion does, while export retains the expression
     * for the standalone C program to evaluate at runtime. */
    {
        int x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("dynamic stencil ref predef exists", x_idx >= 0);
        if (x_idx >= 0)
            g_predef_vars_mut[x_idx].value = 3.9f;
        repl_flatten_commands(editor_state_edit_line());
        for (int i = 0; i < repl_state_flat_program_count(); i++) {
            const GLCmd *cmd = &repl_state_flat_program_cmds()[i];
            if (cmd->type == CMD_STENCIL_FUNC && cmd->has_vars)
                dynamic_stencil_ref = (int)cmd->args[1];
        }
        ASSERT_TRUE("dynamic stencil ref reaches the flat program",
                    dynamic_stencil_ref >= 0);
        ASSERT_TRUE("dynamic stencil ref matches C GLint truncation",
                    dynamic_stencil_ref == (GLint)(3.9f + 0.9f));
    }

    /* Capture code before export */
    code_before = dump_code_panel();
    ASSERT_TRUE("code before export captured", code_before != NULL);

    /* First export to file */
    repl_export_save_output(path1, source_document_view(), NULL);
    export_text = slurp_path(path1);
    ASSERT_TRUE("export file readable", export_text != NULL);
    ASSERT_TRUE("export preserves dynamic stencil expression for C conversion",
                export_text &&
                strstr(export_text, "glStencilFunc(GL_EQUAL, x + 0.9") != NULL);
    ASSERT_TRUE("export uses C89 block comments",
                strstr(export_text, "//") == NULL);
    ASSERT_TRUE("export avoids C99 compound GLfloat literals",
                strstr(export_text, "(GLfloat[]){") == NULL);
    ASSERT_TRUE("export emits C89 GLfloat vector helpers",
                strstr(export_text, "static GLfloat *repl_glfloat4") != NULL);
    ASSERT_TRUE("export avoids C99 compound GLdouble literals",
                strstr(export_text, "(GLdouble[]){") == NULL);
    ASSERT_TRUE("export emits C89 GLdouble vector helper",
                strstr(export_text, "static GLdouble *repl_gldouble4") != NULL);
    ASSERT_TRUE("export uses prototyped display",
                strstr(export_text, "void display(void)") != NULL &&
                strstr(export_text, "void display()") == NULL);
    ASSERT_TRUE("export uses prototyped init",
                strstr(export_text, "void init(void)") != NULL &&
                strstr(export_text, "void init()") == NULL);
    ASSERT_TRUE("label helper hoists char pointer",
                strstr(export_text, "const char *ch;\n  char text[128];") != NULL);
    ASSERT_TRUE("label helper loop reuses declared pointer",
                strstr(export_text, "for (ch = text; *ch; ch++)") != NULL);
    ASSERT_TRUE("tess helper hoists C89 locals",
                strstr(export_text,
                       "TessVertex *v;\n  TessVertex *src;\n  double len;\n  int c;\n  int j;") != NULL);
    ASSERT_TRUE("tess helper loop reuses declared counters",
                strstr(export_text, "for (c = 0; c < 3; c++)") != NULL);
    ASSERT_TRUE("no exported helper uses C99 for-loop declarations",
                strstr(export_text, "for (const char *") == NULL &&
                strstr(export_text, "for (int c =") == NULL);
    /* glMultMatrixf's two forms export differently: the array form is
     * already C against the scratch global, the literal form has to route
     * its C99 compound literal through the C89 helper. */
    ASSERT_TRUE("glMultMatrixf array form exports verbatim",
                strstr(export_text, "glMultMatrixf(A);") != NULL);
    ASSERT_TRUE("glMultMatrixf literal form exports via the C89 helper",
                strstr(export_text,
                       "glMultMatrixf(repl_glfloat16(1, 0, 0, 0, 0, 1, 0, 0, "
                       "0, 0, 1, 0, 0.5, 0, 0, 1));") != NULL);
    ASSERT_TRUE("no exported line uses a C99 compound literal",
                strstr(export_text, "(GLfloat[]){") == NULL &&
                strstr(export_text, "(GLdouble[]){") == NULL);

    /* Reimport from file */
    glr_ctrl_reset_all();
    declare_test_vars();
    ASSERT_TRUE("import succeeded", repl_export_load_from_file(path1, NULL) == 1);

    /* Verify command count preserved */
    int cmd_count_after = repl_state_document_count();
    ASSERT_TRUE("roundtrip cmd count", cmd_count_after == cmd_count_before);

    /* Verify command types preserved */
    for (int i = 0; i < cmd_count_after && i < cmd_count_before; i++) {
        CmdType type_after = repl_state_document_cmds()[i].type;
        if (type_after != cmd_types_before[i]) {
            char label[128];
            snprintf(label, sizeof(label), "cmd %d type preserved", i);
            ASSERT_TRUE(label, 0);
            break;
        }
    }

    /* Capture code after import for exact match comparison */
    code_after = dump_code_panel();
    ASSERT_TRUE("code after import captured", code_after != NULL);

    /* Verify exact roundtrip: code before == code after */
    int exact_match = compare_text(code_before, code_after, &diff_line);
    if (!exact_match) {
        printf("Code mismatch at line %d\nBefore:\n%s\n\nAfter:\n%s\n",
               diff_line, code_before, code_after);
    }
    ASSERT_TRUE("exact export/import roundtrip", exact_match);

    /* Verify stability: re-export should produce the same output */
    repl_export_save_output(path2, source_document_view(), NULL);
    glr_ctrl_reset_all();
    declare_test_vars();
    ASSERT_TRUE("re-import succeeded", repl_export_load_from_file(path2, NULL) == 1);

    code_reexport = dump_code_panel();
    ASSERT_TRUE("code after re-export captured", code_reexport != NULL);

    int stable_match = compare_text(code_after, code_reexport, &diff_line);
    if (!stable_match) {
        printf("Stability mismatch at line %d\nFirst export:\n%s\n\nSecond export:\n%s\n",
               diff_line, code_after, code_reexport);
    }
    ASSERT_TRUE("export stability (idempotent)", stable_match);

    /* Flatten to ensure no parsing errors */
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("flatten succeeded", repl_state_flat_program_count() > 0);

    /* Clean up */
    free(code_before);
    free(code_after);
    free(code_reexport);
    free(export_text);
    remove(path1);
    remove(path2);

    return test_harness_report(&g_harness, "test_repl_export_all_commands");
}
