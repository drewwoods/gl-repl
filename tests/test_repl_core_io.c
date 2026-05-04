#include "repl_core.h"
#include "repl_state.h"
#include "repl_export.h"
#include "ui_state.h"

#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define g_render_state_lines    (repl_state_import_export().render_state_lines)
#define g_multisample_enabled   (repl_state_render_mut()->multisample_enabled)
#define g_line_smooth_enabled   (repl_state_render_mut()->line_smooth_enabled)
#define g_init_attenuate_points (repl_state_render_mut()->point_attenuation_enabled)

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
    repl_eval_declare_predef_var("j", err, sizeof(err));
    repl_eval_declare_predef_var("n", err, sizeof(err));
}

static size_t read_text_file(const char *path, char *buf, size_t buf_sz) {
    FILE *f = fopen(path, "r");
    size_t nread = 0;

    if (buf_sz == 0)
        return 0;
    if (f) {
        nread = fread(buf, 1, buf_sz - 1, f);
        fclose(f);
    }
    buf[nread] = '\0';
    return nread;
}

static int count_substr(const char *haystack, const char *needle) {
    int count = 0;
    size_t needle_len;
    const char *p;

    if (!haystack || !needle || !needle[0])
        return 0;

    needle_len = strlen(needle);
    p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

static int appears_before(const char *haystack, const char *first,
                          const char *second) {
    const char *a = strstr(haystack, first);
    const char *b = strstr(haystack, second);
    return a && b && a < b;
}

static int find_init_line(const char *needle) {
    char line[256];

    for (int i = 0; i < init_section_line_count(); i++) {
        init_section_line(i, line, sizeof(line));
        if (strcmp(line, needle) == 0)
            return i;
    }
    return -1;
}

static int find_init_line_substr(const char *needle) {
    char line[256];

    for (int i = 0; i < init_section_line_count(); i++) {
        init_section_line(i, line, sizeof(line));
        if (strstr(line, needle) != NULL)
            return i;
    }
    return -1;
}

int main(void) {
    const char *path = "/tmp/repl_core_roundtrip_output.c";
    const char *func_path = "/tmp/repl_core_func_output.c";
    const char *param_loop_path = "/tmp/repl_core_param_loop_output.c";
    const char *decl_func_path = "/tmp/repl_core_decl_func_output.c";
    const char *shape_path = "/tmp/repl_core_shapes_output.c";
    const char *tess_path = "/tmp/repl_core_tess_output.c";

    repl_eval_init_predef_vars();
    repl_reset_state(); declare_test_vars();

    repl_feed_line_public("x = 1.25;");
    repl_feed_line_public("glBegin(GL_LINE_STRIP);");
    repl_feed_line_public("for(i, 0, 3) {");
    repl_feed_line_public("glVertex3f(i + x, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("glEnd();");

    ASSERT_TRUE("pre-save cmds", repl_state_document_count() > 0);
    ASSERT_TRUE("init has host-only ambient line",
                find_init_line("  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);") >= 0);
    ASSERT_TRUE("init omits quadric header line",
                find_init_line("static GLUquadric *g_quadric = NULL;") < 0);
    ASSERT_TRUE("init omits tess init line",
                find_init_line("  g_tess = gluNewTess();") < 0);
    ASSERT_TRUE("init has color material enable bootstrap",
                find_init_line_substr("glEnable(GL_COLOR_MATERIAL);") >= 0);
    ASSERT_TRUE("display has color material state",
                strcmp(g_render_state_lines[2],
                       "  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);") == 0);
    ASSERT_TRUE("init omits color material mode bootstrap",
                find_init_line_substr("glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);") < 0);
    ASSERT_TRUE("init has two-side bootstrap",
                find_init_line_substr("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);") >= 0);
    ASSERT_TRUE("init has blend enable bootstrap",
                find_init_line_substr("glEnable(GL_BLEND);") >= 0);
    ASSERT_TRUE("init has blend func bootstrap",
                find_init_line_substr("glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);") >= 0);
#ifndef NO_POINT_PARAMETER
    ASSERT_TRUE("init has point attenuation bootstrap",
                find_init_line_substr("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") >= 0);
#endif
    ASSERT_TRUE("init omits tess edge-flag callback",
                find_init_line_substr("GLU_TESS_EDGE_FLAG") < 0);

    int before_n = repl_state_document_count();
    CmdType before_types[MAX_COMMANDS];
    for (int i = 0; i < before_n; i++)
        before_types[i] = repl_state_document_cmds_mut()[i].type;

    g_multisample_enabled = 0;
    g_line_smooth_enabled = 1;
    repl_state_presentation_mut()->show_vertex_outlines = 0;
    repl_state_presentation_mut()->show_vertex_points = 0;
    repl_export_save_output(path, editor_buffer_view());
    {
        char buf[16384];
        read_text_file(path, buf, sizeof(buf));
        ASSERT_TRUE("saved t uses elapsed time",
                    strstr(buf, "t = 0.001f * (float)glutGet(GLUT_ELAPSED_TIME)") != NULL);
        ASSERT_TRUE("saved multisample header state",
                    strstr(buf, "glDisable(GL_MULTISAMPLE);") != NULL);
        ASSERT_TRUE("saved line smooth header state",
                    strstr(buf, "glEnable(GL_LINE_SMOOTH);") != NULL);
        ASSERT_TRUE("saved geometry helper",
                    strstr(buf, "static void render_repl_geometry(void)") != NULL);
        ASSERT_TRUE("scaffold metadata before includes",
                    appears_before(buf, "// @workspace:",
                                   "#include <gl_includes.h>"));
        ASSERT_TRUE("scaffold header before globals",
                    appears_before(buf, "#include <gl_includes.h>",
                                   "static float x = 0.0f;"));
        ASSERT_TRUE("scaffold globals before reset",
                    appears_before(buf, "static float x = 0.0f;",
                                   "static void reset_repl_vars(void)"));
        ASSERT_TRUE("scaffold reset before render helper",
                    appears_before(buf, "static void reset_repl_vars(void)",
                                   "static void render_repl_geometry(void)"));
        ASSERT_TRUE("scaffold render helper before display",
                    appears_before(buf, "static void render_repl_geometry(void)",
                                   "void display()"));
        ASSERT_TRUE("scaffold display before init",
                    appears_before(buf, "void display()", "void init()"));
        ASSERT_TRUE("saved snippet marker start",
                    strstr(buf, "// Snippet start") != NULL);
        ASSERT_TRUE("saved snippet marker end",
                    strstr(buf, "// Snippet end") != NULL);
        ASSERT_TRUE("saved outline helper omitted when disabled",
                    strstr(buf, "render_repl_outline_overlay") == NULL);
        ASSERT_TRUE("saved vertex points helper omitted when disabled",
                    strstr(buf, "render_repl_vertex_points_overlay") == NULL);
        ASSERT_TRUE("saved init ambient model line",
                    strstr(buf, "glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);") != NULL);
        ASSERT_TRUE("saved init omits quadric scaffolding",
                strstr(buf, "g_quadric") == NULL);
        ASSERT_TRUE("saved non-tess export omits tess global",
                    strstr(buf, "static GLUtesselator *g_tess = NULL;") == NULL);
        ASSERT_TRUE("saved non-tess export omits tess setup line",
                    strstr(buf, "g_tess = gluNewTess();") == NULL);
        ASSERT_TRUE("saved non-tess export omits tess callback line",
                    strstr(buf, "GLU_TESS_EDGE_FLAG") == NULL);
        const char *color_material_line =
            strstr(buf, "glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);");
        const char *init_func = strstr(buf, "\nvoid init()");
        ASSERT_TRUE("saved display color material line once",
                    count_substr(buf, "glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);") == 1);
        ASSERT_TRUE("saved color material appears before init",
                    color_material_line && init_func &&
                    color_material_line < init_func);
        ASSERT_TRUE("saved init light model line once",
                    count_substr(buf, "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);") == 1);
#ifndef NO_POINT_PARAMETER
        ASSERT_TRUE("saved init point attenuation line once",
                    count_substr(buf, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") == 1);
#endif
    }

#ifndef NO_POINT_PARAMETER
    g_init_attenuate_points = 0;
    ASSERT_TRUE("init hides point attenuation when disabled",
                find_init_line_substr("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") < 0);
    repl_export_save_output(path, editor_buffer_view());
    {
        char buf[16384];
        read_text_file(path, buf, sizeof(buf));
        ASSERT_TRUE("saved init omits point attenuation when disabled",
                    strstr(buf, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") == NULL);
    }
    g_init_attenuate_points = 1;
#endif

    repl_reset_state(); declare_test_vars();
    ASSERT_TRUE("load saved output", repl_export_load_from_file(path) == 1);
    ASSERT_TRUE("roundtrip cmd count", repl_state_document_count() == before_n);
    for (int i = 0; i < before_n; i++) {
        char label[64];
        snprintf(label, sizeof(label), "roundtrip type %d", i);
        ASSERT_TRUE(label, repl_state_document_cmds_mut()[i].type == before_types[i]);
    }

    repl_flatten_commands();
    ASSERT_TRUE("flatten produced cmds", repl_state_flat_program_count() > 0);

    /* Camera state round-trip: non-default eye/center must survive save+load. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glEnd();");
    ui_state_camera_set_orbit(31.523f, 31.4799f);
    ui_state_camera_set_distance(7.59313f);
    ui_state_camera_set_pan(1.50f, 0.0f, -2.00f);
    repl_export_save_output(path, editor_buffer_view());
    {
        char buf[16384];
        read_text_file(path, buf, sizeof(buf));
        ASSERT_TRUE("camera export seeds g_angle from camera ry",
                    strstr(buf, "static float g_angle = 31.4799f;") != NULL);
        ASSERT_TRUE("camera export omits literal y rotate",
                    strstr(buf, "glRotatef(31.4799f, 0.0f, 1.0f, 0.0f);") == NULL);
        ASSERT_TRUE("camera export keeps one animated y rotate",
                    count_substr(buf, "glRotatef(g_angle, 0.0f, 1.0f, 0.0f);") == 1);
    }
    {
        float saved_rx   = ui_state_camera().rx;
        float saved_ry   = ui_state_camera().ry;
        float saved_dist = ui_state_camera().dist;
        float saved_tx   = ui_state_camera().tx;
        float saved_ty   = ui_state_camera().ty;
        float saved_tz   = ui_state_camera().tz;
        ui_state_camera_set_orbit(20.0f, 30.0f);
        ui_state_camera_set_distance(5.0f);
        ui_state_camera_set_pan(0.0f, 0.0f, 0.0f);
        repl_reset_state(); declare_test_vars();
        ASSERT_TRUE("load camera output", repl_export_load_from_file(path) == 1);
        ASSERT_TRUE("camera rx restored",   fabsf(ui_state_camera().rx   - saved_rx)   < 1e-2f);
        ASSERT_TRUE("camera ry restored",   fabsf(ui_state_camera().ry   - saved_ry)   < 1e-2f);
        ASSERT_TRUE("camera dist restored", fabsf(ui_state_camera().dist - saved_dist) < 1e-2f);
        ASSERT_TRUE("camera tx restored",   fabsf(ui_state_camera().tx   - saved_tx)   < 1e-2f);
        ASSERT_TRUE("camera ty restored",   fabsf(ui_state_camera().ty   - saved_ty)   < 1e-2f);
        ASSERT_TRUE("camera tz restored",   fabsf(ui_state_camera().tz   - saved_tz)   < 1e-2f);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("x = 1;");
    repl_feed_line_public("func0(radius, yoff) {");
    repl_feed_line_public("glVertex3f(radius, yoff, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(1.5, x + 2);");

    int func_n = repl_state_document_count();
    for (int i = 0; i < func_n; i++) before_types[i] = repl_state_document_cmds_mut()[i].type;

    repl_state_presentation_mut()->show_vertex_outlines = 1;
    repl_state_presentation_mut()->show_vertex_points = 1;
    repl_export_save_output(func_path, editor_buffer_view());
    {
        char buf[32768];
        read_text_file(func_path, buf, sizeof(buf));
        ASSERT_TRUE("saved func signature",
                    strstr(buf, "static void func0(float radius, float yoff)") != NULL);
        ASSERT_TRUE("saved func emitted only once",
                    count_substr(buf, "static void func0(float radius, float yoff) {") == 1);
        ASSERT_TRUE("saved func before geometry helper",
                    appears_before(buf, "static void func0(float radius, float yoff) {",
                                   "static void render_repl_geometry(void)"));
        ASSERT_TRUE("saved geometry helper func call",
                    strstr(buf, "func0(1.5, x + 2);") != NULL);
        ASSERT_TRUE("saved geometry helper once",
                    count_substr(buf, "static void render_repl_geometry(void)") == 1);
        ASSERT_TRUE("saved no outline helper variants",
                    strstr(buf, "render_repl_outline_") == NULL);
        ASSERT_TRUE("saved no vpoint helper variants",
                    strstr(buf, "render_repl_vpoints_") == NULL);
        ASSERT_TRUE("saved geometry called thrice in display",
                    count_substr(buf, "render_repl_geometry();") == 3);
        ASSERT_TRUE("saved outline pass polygon mode",
                    strstr(buf, "glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);") != NULL);
        ASSERT_TRUE("saved vpoints pass polygon mode",
                    strstr(buf, "glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);") != NULL);
        ASSERT_TRUE("saved overlay disables color material",
                    strstr(buf, "glDisable(GL_COLOR_MATERIAL);") != NULL);
    }

    repl_reset_state(); declare_test_vars();
    ASSERT_TRUE("load saved param func output", repl_export_load_from_file(func_path) == 1);
    ASSERT_TRUE("param func roundtrip cmd count", repl_state_document_count() == func_n);
    {
        int have_var = 0, have_def = 0, have_body = 0, have_end = 0, have_call = 0;
        for (int i = 0; i < repl_state_document_count(); i++) {
            if (repl_state_document_cmds_mut()[i].type == CMD_VAR_ASSIGN) have_var++;
            if (repl_state_document_cmds_mut()[i].type == CMD_FUNC_DEF) have_def++;
            if (repl_state_document_cmds_mut()[i].type == CMD_VERTEX3F) have_body++;
            if (repl_state_document_cmds_mut()[i].type == CMD_FUNC_END) have_end++;
            if (repl_state_document_cmds_mut()[i].type == CMD_CALL) have_call++;
        }
        ASSERT_TRUE("param func roundtrip has var assign", have_var == 1);
        ASSERT_TRUE("param func roundtrip has func def", have_def == 1);
        ASSERT_TRUE("param func roundtrip has body", have_body == 1);
        ASSERT_TRUE("param func roundtrip has func end", have_end == 1);
        ASSERT_TRUE("param func roundtrip has func call", have_call == 1);
    }

    repl_flatten_commands();
    ASSERT_TRUE("param func flatten count", repl_state_flat_program_count() >= 2);
    ASSERT_TRUE("param func flatten vertex type", repl_state_flat_program_cmds_mut()[repl_state_flat_program_count() - 1].type == CMD_VERTEX3F);
    ASSERT_TRUE("param func flatten x",
                fabsf(repl_state_flat_program_cmds_mut()[repl_state_flat_program_count() - 1].args[0] - 1.5f) < 1e-6f);
    ASSERT_TRUE("param func flatten y",
                fabsf(repl_state_flat_program_cmds_mut()[repl_state_flat_program_count() - 1].args[1] - 3.0f) < 1e-6f);

    repl_reset_state();
    repl_feed_line_public("func0(radius, sides, phase) {");
    repl_feed_line_public("for(i, 0, sides + 1) {");
    repl_feed_line_public("glVertex3f(cos(i*TAU/sides + phase)*radius, sin(i*TAU/sides + phase)*radius, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(1, 6, 0);");
    repl_export_save_output(param_loop_path, editor_buffer_view());
    {
        char buf[32768];
        read_text_file(param_loop_path, buf, sizeof(buf));
        ASSERT_TRUE("saved param loop keeps symbolic C bound",
                    strstr(buf, "for (float i = 0; i < sides + 1; i += 1.0f)") != NULL);
    }

    repl_reset_state();
    ASSERT_TRUE("load saved param loop output", repl_export_load_from_file(param_loop_path) == 1);
    {
        int have_bound = 0;
        for (int i = 0; i < repl_state_document_count(); i++) {
            if (repl_state_document_cmds_mut()[i].type == CMD_FOR_BEGIN &&
                strstr(editor_buffer_line(i) ? editor_buffer_line(i) : "", "sides + 1") != NULL &&
                repl_state_document_cmds_mut()[i].has_vars) {
                have_bound = 1;
            }
        }
        ASSERT_TRUE("loaded param loop keeps function param bound", have_bound == 1);
    }
    repl_flatten_commands();
    {
        int vertex_count = 0;
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
                vertex_count++;
        ASSERT_TRUE("loaded param loop iterates through sides plus center close",
                    vertex_count == 7);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("float r;");
    repl_feed_line_public("glClearColor(0.1, 0.1, 0.1, 1);");
    repl_feed_line_public("func0 {");
    repl_feed_line_public("glVertex3f(r, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("r = 2;");
    repl_feed_line_public("func0();");
    repl_export_save_output(decl_func_path, editor_buffer_view());

    repl_reset_state(); declare_test_vars();
    ASSERT_TRUE("load decl plus promoted func output",
                repl_export_load_from_file(decl_func_path) == 1);
    ASSERT_TRUE("decl plus func cmd count", repl_state_document_count() == 7);
    ASSERT_TRUE("imported decl remains first", repl_state_document_cmds_mut()[0].type == CMD_VAR_DECLARE);
    ASSERT_TRUE("imported func follows decl", repl_state_document_cmds_mut()[1].type == CMD_FUNC_DEF);
    ASSERT_TRUE("imported func body follows header", repl_state_document_cmds_mut()[2].type == CMD_VERTEX3F);
    ASSERT_TRUE("imported func end follows body", repl_state_document_cmds_mut()[3].type == CMD_FUNC_END);
    ASSERT_TRUE("imported prior command follows func block", repl_state_document_cmds_mut()[4].type == CMD_CLEAR_COLOR);
    ASSERT_TRUE("imported var assign follows prior command", repl_state_document_cmds_mut()[5].type == CMD_VAR_ASSIGN);
    ASSERT_TRUE("imported call follows assign", repl_state_document_cmds_mut()[6].type == CMD_CALL);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("x = 0.25;");
    repl_feed_line_public("glutSolidSphere(x, 16, 12);");
    repl_feed_line_public("glutSolidCone(0.15, 1.5, 8, 1);");
    repl_feed_line_public("glutSolidTorus(0.1, 0.35, 12, 4);");
    repl_feed_line_public("glutSolidTeapot(0.25);");
    repl_feed_line_public("glutSolidCube(0.5);");
    repl_state_presentation_mut()->show_vertex_outlines = 0;
    repl_state_presentation_mut()->show_vertex_points = 0;
    repl_export_save_output(shape_path, editor_buffer_view());
    {
        char buf[16384];
        read_text_file(shape_path, buf, sizeof(buf));
        ASSERT_TRUE("saved shape sphere preserves variable expr",
                    strstr(buf, "glutSolidSphere(x, 16, 12);") != NULL);
        ASSERT_TRUE("saved shape cone stays plain",
                    strstr(buf, "glutSolidCone(0.15, 1.5, 8, 1);") != NULL);
        ASSERT_TRUE("saved shape torus stays plain",
                    strstr(buf, "glutSolidTorus(0.1, 0.35, 12, 4);") != NULL);
        ASSERT_TRUE("saved shape teapot stays plain",
                    strstr(buf, "glutSolidTeapot(0.25);") != NULL);
        ASSERT_TRUE("saved shape cube stays plain",
                    strstr(buf, "glutSolidCube(0.5);") != NULL);
        ASSERT_TRUE("saved shape output omits quadric scaffolding",
                    strstr(buf, "g_quadric") == NULL);
    }

    repl_reset_state(); declare_test_vars();
    ASSERT_TRUE("load saved shape output", repl_export_load_from_file(shape_path) == 1);
    ASSERT_TRUE("shape roundtrip cmd count", repl_state_document_count() == 6);
    ASSERT_TRUE("loaded shape assign first",  repl_state_document_cmds_mut()[0].type == CMD_VAR_ASSIGN);
    ASSERT_TRUE("loaded shape sphere second", repl_state_document_cmds_mut()[1].type == CMD_GLUT_SPHERE);
    ASSERT_TRUE("loaded shape cone third",    repl_state_document_cmds_mut()[2].type == CMD_GLUT_CONE);
    ASSERT_TRUE("loaded shape torus fourth",  repl_state_document_cmds_mut()[3].type == CMD_GLUT_TORUS);
    ASSERT_TRUE("loaded shape teapot fifth",  repl_state_document_cmds_mut()[4].type == CMD_GLUT_TEAPOT);
    ASSERT_TRUE("loaded shape cube sixth",    repl_state_document_cmds_mut()[5].type == CMD_GLUT_CUBE);
    ASSERT_TRUE("loaded shape sphere keeps expr source",
                strstr(editor_buffer_line(1) ? editor_buffer_line(1) : "",
                       "glutSolidSphere(x, 16, 12);") != NULL);
    ASSERT_TRUE("loaded shape sphere has_vars set",
                repl_state_document_cmds_mut()[1].has_vars == 1);
    ASSERT_TRUE("loaded shape cone source intact",
                strstr(editor_buffer_line(2) ? editor_buffer_line(2) : "",
                       "glutSolidCone(0.15, 1.5, 8, 1);") != NULL);
    ASSERT_TRUE("loaded shape torus source intact",
                strstr(editor_buffer_line(3) ? editor_buffer_line(3) : "",
                       "glutSolidTorus(0.1, 0.35, 12, 4);") != NULL);
    ASSERT_TRUE("loaded shape teapot source intact",
                strstr(editor_buffer_line(4) ? editor_buffer_line(4) : "",
                       "glutSolidTeapot(0.25);") != NULL);
    ASSERT_TRUE("loaded shape cube source intact",
                strstr(editor_buffer_line(5) ? editor_buffer_line(5) : "",
                       "glutSolidCube(0.5);") != NULL);
    for (int i = 1; i < repl_state_document_count(); i++)
        ASSERT_TRUE("loaded shape source omits g_quadric",
                    strstr(editor_buffer_line(i) ? editor_buffer_line(i) : "",
                           "g_quadric") == NULL);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(radius) {");
    repl_feed_line_public("gluBegin(GLU_POLYGON);");
    repl_feed_line_public("gluBegin(GLU_CONTOUR);");
    repl_feed_line_public("for(i, 0, 4) {");
    repl_feed_line_public("gluColor(0.25 + 0.15*sin(i), 0.3, 0.4);");
    repl_feed_line_public("gluVertex(radius*cos(i), 0, radius*sin(i));");
    repl_feed_line_public("}");
    repl_feed_line_public("gluEnd();");
    repl_feed_line_public("gluEnd();");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(2.0);");
    repl_state_presentation_mut()->show_vertex_outlines = 1;
    repl_state_presentation_mut()->show_vertex_points = 1;
    repl_export_save_output(tess_path, editor_buffer_view());
    {
        char buf[65536];
        read_text_file(tess_path, buf, sizeof(buf));
        ASSERT_TRUE("saved tess color keeps loop expr",
                    strstr(buf, "_tc[0]=0.25 + 0.15*sinf(i);") != NULL);
        ASSERT_TRUE("saved tess vertex keeps param expr",
                    strstr(buf, "_v->pos[0]=radius*cosf(i);") != NULL);
        ASSERT_TRUE("saved tess vertex keeps z expr",
                    strstr(buf, "_v->pos[2]=radius*sinf(i);") != NULL);
        ASSERT_TRUE("saved tess func emitted only once",
                    count_substr(buf, "static void func0(float radius) {") == 1);
        ASSERT_TRUE("saved tess func call once",
                    count_substr(buf, "func0(2.0);") == 1);
        ASSERT_TRUE("saved tess export includes tess global",
                    strstr(buf, "static GLUtesselator *g_tess = NULL;") != NULL);
        ASSERT_TRUE("saved tess preamble before reset",
                    appears_before(buf, "static GLUtesselator *g_tess = NULL;",
                                   "static void reset_repl_vars(void)"));
        ASSERT_TRUE("saved tess export includes tess init",
                    strstr(buf, "g_tess = gluNewTess();") != NULL);
        ASSERT_TRUE("saved tess export includes tess callback",
                    strstr(buf, "GLU_TESS_EDGE_FLAG") != NULL);
        ASSERT_TRUE("saved tess no outline helper variants",
                    strstr(buf, "render_repl_outline_") == NULL);
        ASSERT_TRUE("saved tess no vpoint helper variants",
                    strstr(buf, "render_repl_vpoints_") == NULL);
    }

    printf("repl_core_io: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
