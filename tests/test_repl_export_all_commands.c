#include "app/glr_ctrl.h"
/*
 * test_repl_export_all_commands.c - Comprehensive roundtrip test for all supported GL commands
 *
 * Verifies that every supported GL command survives the export/import cycle
 * with stable, identical code output (like test_repl_core_examples.c).
 */
#include "repl/command_spec.h"  /* cmd_type_name */
#include "repl/core.h"
#include "editor/input.h"
#include "repl/core_internal.h"
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
    CMD_DEPTH_FUNC,
    CMD_POINT_SIZE,
    CMD_LINE_WIDTH,
    CMD_POINT_PARAMETER_FV,
    CMD_BLEND_FUNC,
    CMD_CLEAR_COLOR,
    CMD_DEPTH_MASK,
    CMD_COLOR_MASK,
    CMD_COLOR_MATERIAL,
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
};

static int verify_all_commands_present(void) {
    int expected_count = sizeof(expected_commands) / sizeof(expected_commands[0]);
    const GLCmd *cmds = repl_state_document_cmds();
    int cmd_count = repl_state_document_count();

    /* Mark which commands were found */
    int found[MAX_COMMANDS] = {0};
    for (int i = 0; i < cmd_count; i++) {
        CmdType type = cmds[i].type;
        if (type >= 0 && type < MAX_COMMANDS) {
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

int main(void) {
    const char *path1 = "/tmp/repl_export_all_commands_1.c";
    const char *path2 = "/tmp/repl_export_all_commands_2.c";
    int cmd_count_before = 0;
    CmdType cmd_types_before[MAX_COMMANDS];
    char *code_before = NULL;
    char *code_after = NULL;
    char *code_reexport = NULL;
    int diff_line = 0;

    repl_eval_init_predef_vars();
    glr_app_reset_all();
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

    /* State/capability commands */
    editor_feed_line("glEnable(GL_DEPTH_TEST);");
    editor_feed_line("glDisable(GL_DEPTH_TEST);");
    editor_feed_line("glShadeModel(GL_SMOOTH);");
    editor_feed_line("glFrontFace(GL_CCW);");
    editor_feed_line("glDepthFunc(GL_LEQUAL);");

    /* Attribute commands */
    editor_feed_line("glPointSize(5);");
    editor_feed_line("glLineWidth(2);");
    editor_feed_line("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1, 0, 0.01);");

    /* Blend/depth commands */
    editor_feed_line("glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);");
    editor_feed_line("glClearColor(0.2, 0.2, 0.2, 1);");
    editor_feed_line("glDepthMask(GL_TRUE);");
    editor_feed_line("glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);");

    /* Material/lighting commands */
    editor_feed_line("glColorMaterial(GL_FRONT, GL_DIFFUSE);");
    editor_feed_line("glMaterialf(GL_FRONT, GL_SHININESS, 128);");
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
    for (int i = 0; i < cmd_count_before && i < MAX_COMMANDS; i++) {
        cmd_types_before[i] = repl_state_document_cmds()[i].type;
    }

    /* Capture code before export */
    code_before = dump_code_panel();
    ASSERT_TRUE("code before export captured", code_before != NULL);

    /* First export to file */
    repl_export_save_output(path1, source_document_view(), NULL);

    /* Reimport from file */
    glr_app_reset_all();
    declare_test_vars();
    ASSERT_TRUE("import succeeded", repl_export_load_from_file(path1) == 1);

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
    glr_app_reset_all();
    declare_test_vars();
    ASSERT_TRUE("re-import succeeded", repl_export_load_from_file(path2) == 1);

    code_reexport = dump_code_panel();
    ASSERT_TRUE("code after re-export captured", code_reexport != NULL);

    int stable_match = compare_text(code_after, code_reexport, &diff_line);
    if (!stable_match) {
        printf("Stability mismatch at line %d\nFirst export:\n%s\n\nSecond export:\n%s\n",
               diff_line, code_after, code_reexport);
    }
    ASSERT_TRUE("export stability (idempotent)", stable_match);

    /* Flatten to ensure no parsing errors */
    repl_flatten_commands();
    ASSERT_TRUE("flatten succeeded", repl_state_flat_program_count() > 0);

    /* Clean up */
    free(code_before);
    free(code_after);
    free(code_reexport);
    remove(path1);
    remove(path2);

    return test_harness_report(&g_harness, "test_repl_export_all_commands");
}
