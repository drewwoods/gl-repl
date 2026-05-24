#include "editor/state.h"
#include "app/glr_camera.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "repl/core.h"
#include "editor/input.h"
#include "repl/pipeline.h"
#include "repl/scenes.h"
#include "repl/state.h"
#include "repl/export.h"
#include "repl/executor.h"
#include "ui/app/state.h"

#include "support/test_harness.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define g_render_state_lines    (repl_state_import_export().render_state_lines)
/* Render-config toggles moved to glr_state.render in step 7a. */
#define g_multisample_enabled   (glr_state_render_mut()->multisample_enabled)
#define g_line_smooth_enabled   (glr_state_render_mut()->line_smooth_enabled)
#define g_init_attenuate_points (glr_state_render_mut()->point_attenuation_enabled)

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, expected) do { \
    TEST_ASSERT_INT(&g_harness, label, got, expected); \
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

    for (int i = 0; i < repl_export_init_section_line_count(); i++) {
        repl_export_init_section_line(i, line, sizeof(line));
        if (strcmp(line, needle) == 0)
            return i;
    }
    return -1;
}

static int find_init_line_substr(const char *needle) {
    char line[256];

    for (int i = 0; i < repl_export_init_section_line_count(); i++) {
        repl_export_init_section_line(i, line, sizeof(line));
        if (strstr(line, needle) != NULL)
            return i;
    }
    return -1;
}

int main(void) {
    const char *path = "/tmp/repl_core_roundtrip_output.c";
    const char *scratch_path = "/tmp/repl_core_scratch_output.c";
    const char *func_path = "/tmp/repl_core_func_output.c";
    const char *param_loop_path = "/tmp/repl_core_param_loop_output.c";
    const char *decl_func_path = "/tmp/repl_core_decl_func_output.c";
    const char *decl_func_blank_path = "/tmp/repl_core_decl_func_blank_output.c";
    const char *shape_path = "/tmp/repl_core_shapes_output.c";
    const char *tess_path = "/tmp/repl_core_tess_output.c";
    const char *rand_alias_path = "/tmp/repl_core_rand_alias_output.c";

    repl_eval_init_predef_vars();
    glr_app_reset_all(); declare_test_vars();

    editor_feed_line("x = 1.25;");
    editor_feed_line("glBegin(GL_LINE_STRIP);");
    editor_feed_line("");
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(i + x, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("glEnd();");

    ASSERT_TRUE("pre-save cmds", repl_state_document_count() > 0);
    ASSERT_TRUE("init has host-only ambient line",
                find_init_line("  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);") >= 0);
    ASSERT_TRUE("init omits quadric header line",
                find_init_line("static GLUquadric *g_quadric = NULL;") < 0);
    ASSERT_TRUE("init omits tess init line",
                find_init_line("  g_tess = gluNewTess();") < 0);
    ASSERT_TRUE("init has color material enable bootstrap",
                find_init_line_substr("glEnable(GL_COLOR_MATERIAL);") >= 0);
    ASSERT_TRUE("init has color material mode bootstrap",
                find_init_line_substr("glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);") >= 0);
    ASSERT_TRUE("init has material specular bootstrap",
                find_init_line_substr("glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR") >= 0);
    ASSERT_TRUE("init has material shininess bootstrap",
                find_init_line_substr("glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS") >= 0);
    ASSERT_TRUE("init has two-side bootstrap",
                find_init_line_substr("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);") >= 0);
    ASSERT_TRUE("init has blend enable bootstrap",
                find_init_line_substr("glEnable(GL_BLEND);") >= 0);
    ASSERT_TRUE("init has blend func bootstrap",
                find_init_line_substr("glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);") >= 0);
    /* Default: point parameters supported, so the attenuation
     * bootstrap is present. */
    ASSERT_TRUE("init has point attenuation bootstrap",
                find_init_line_substr("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") >= 0);
    ASSERT_TRUE("init omits tess edge-flag callback",
                find_init_line_substr("GLU_TESS_EDGE_FLAG") < 0);

    int before_n = repl_state_document_count();
    CmdType before_types[MAX_COMMANDS];
    for (int i = 0; i < before_n; i++)
        before_types[i] = repl_state_document_cmds_mut()[i].type;

    g_multisample_enabled = 0;
    g_line_smooth_enabled = 1;
    glr_state_presentation_mut()->show_vertex_outlines = 0;
    glr_state_presentation_mut()->show_vertex_points = 0;
    repl_export_save_output(path, source_document_view(), NULL);
    {
        char buf[16384];
        read_text_file(path, buf, sizeof(buf));
        /* Time advance lives in the tick() timer at a fixed step,
         * matching the live REPL's repl_state_time_advance(0.016).
         * display() must NOT advance t (that would make tDelta
         * frame-rate dependent). */
        ASSERT_TRUE("saved t advances at fixed step in tick()",
                    strstr(buf, "t += 0.016f;") != NULL);
        ASSERT_TRUE("saved tick scheduled via glutTimerFunc",
                    strstr(buf, "glutTimerFunc(16, tick, 0)") != NULL);
        ASSERT_TRUE("saved display does not call glutGet(GLUT_ELAPSED_TIME)",
                    strstr(buf, "glutGet(GLUT_ELAPSED_TIME)") == NULL);
        ASSERT_TRUE("saved no longer uses glutIdleFunc",
                    strstr(buf, "glutIdleFunc") == NULL);
        ASSERT_TRUE("saved multisample header state",
                    strstr(buf, "glDisable(GL_MULTISAMPLE);") != NULL);
        ASSERT_TRUE("saved line smooth header state",
                    strstr(buf, "glEnable(GL_LINE_SMOOTH);") != NULL);
        ASSERT_TRUE("saved geometry helper",
                    strstr(buf, "static void render_repl_geometry(void)") != NULL);
        ASSERT_TRUE("scaffold metadata before includes",
                    appears_before(buf, "// @workspace:",
                                   "#include \"gl_includes.h\""));
        ASSERT_TRUE("scaffold header before globals",
                    appears_before(buf, "#include \"gl_includes.h\"",
                                   "static float t = 0.0f;"));
        /* Predef-var initializer carries the export-time snapshot value
         * (replaces the role reset_repl_vars used to play). */
        ASSERT_TRUE("predef var initializer carries snapshot value",
                    strstr(buf, "static float x = 1.25;") != NULL);
        /* Single-pass exports do not need save/restore helpers. */
        ASSERT_TRUE("single-pass export omits save_repl_vars",
                    strstr(buf, "save_repl_vars") == NULL);
        ASSERT_TRUE("scaffold globals before render helper",
                    appears_before(buf, "static float t = 0.0f;",
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
        ASSERT_TRUE("saved color material line once",
                    count_substr(buf, "glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);") == 1);
        ASSERT_TRUE("saved color material appears inside init()",
                    color_material_line && init_func &&
                    color_material_line > init_func);
        ASSERT_TRUE("saved init light model line once",
                    count_substr(buf, "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);") == 1);
        ASSERT_TRUE("saved init point attenuation line once",
                    count_substr(buf, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") == 1);
    }

    /* @cfg toggle path: neutralized (still emitted today by the
     * toggle-disable branch — unchanged behavior). */
    g_init_attenuate_points = 0;
    ASSERT_TRUE("init hides point attenuation when disabled",
                find_init_line_substr("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") < 0);
    repl_export_save_output(path, source_document_view(), NULL);
    {
        char buf[16384];
        read_text_file(path, buf, sizeof(buf));
        ASSERT_TRUE("saved init omits point attenuation when disabled",
                    strstr(buf, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") == NULL);
    }
    g_init_attenuate_points = 1;

    /* Runtime capability path (replaces the compile-time
     * NO_POINT_PARAMETER #ifndef): when the runtime GL lacks
     * glPointParameterfv the bootstrap entry is skipped entirely —
     * not applied, not emitted — independent of the @cfg toggle. */
    repl_executor_set_point_parameter_supported(0);
    ASSERT_TRUE("init omits point attenuation when unsupported",
                find_init_line_substr("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") < 0);
    repl_export_save_output(path, source_document_view(), NULL);
    {
        char buf[16384];
        read_text_file(path, buf, sizeof(buf));
        ASSERT_TRUE("saved init omits point attenuation when unsupported",
                    strstr(buf, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") == NULL);
    }
    repl_executor_set_point_parameter_supported(1);
    ASSERT_TRUE("init restores point attenuation when supported again",
                find_init_line_substr("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") >= 0);

    glr_app_reset_all(); declare_test_vars();
    ASSERT_TRUE("load saved output", repl_export_load_from_file(path) == 1);
    ASSERT_TRUE("roundtrip cmd count", repl_state_document_count() == before_n);
    for (int i = 0; i < before_n; i++) {
        char label[64];
        snprintf(label, sizeof(label), "roundtrip type %d", i);
        ASSERT_TRUE(label, repl_state_document_cmds_mut()[i].type == before_types[i]);
    }
    ASSERT_TRUE("roundtrip blank line type preserved",
                repl_state_document_cmds_mut()[2].type == CMD_EMPTY);
    ASSERT_TRUE("roundtrip blank line text preserved",
                strcmp(editor_buffer_line(2), "") == 0);

    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("flatten produced cmds", repl_state_flat_program_count() > 0);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("A[0] = 0;");
    editor_feed_line("A[1] = 1;");
    editor_feed_line("A[0] = A[0] + (A[1] - A[0])*0.25;");
    editor_feed_line("glVertex3f(A[0], 0, 0);");
    repl_export_save_output(scratch_path, source_document_view(), NULL);
    {
        char buf[16384];
        read_text_file(scratch_path, buf, sizeof(buf));
        ASSERT_TRUE("scratch global A exported (used)",
                    strstr(buf, "static float A[8] = {0};") != NULL);
        /* B and C are unreferenced in this snippet, so the emit-on-
         * demand gate must skip them. */
        ASSERT_TRUE("scratch global B omitted (unused)",
                    strstr(buf, "static float B[") == NULL);
        ASSERT_TRUE("scratch global C omitted (unused)",
                    strstr(buf, "static float C[") == NULL);
        ASSERT_TRUE("lerp helper omitted",
                strstr(buf, "repl_lerp") == NULL);
        ASSERT_TRUE("scratch blend assignment exported",
            strstr(buf,
                   "A[0] = A[0] + (A[1] - A[0])*0.25;") != NULL);
    }

    /* Cross-array example: A and B used, C unused -> A and B emit, C
     * omitted. Pins the per-letter detection. */
    {
        const char *cross_path = "/tmp/repl_core_scratch_cross.c";
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("A[0] = 1;");
        editor_feed_line("B[0] = 2;");
        editor_feed_line("glVertex3f(A[0], B[0], 0);");
        repl_export_save_output(cross_path, source_document_view(), NULL);
        char buf[8192];
        read_text_file(cross_path, buf, sizeof(buf));
        ASSERT_TRUE("scratch A exported when used",
                    strstr(buf, "static float A[") != NULL);
        ASSERT_TRUE("scratch B exported when used",
                    strstr(buf, "static float B[") != NULL);
        ASSERT_TRUE("scratch C omitted when unused",
                    strstr(buf, "static float C[") == NULL);
        remove(cross_path);
    }

    /* Export helper gate must tolerate already-C spellings that can
     * leak into source text (for example older fixtures or hand-edited
     * snippets) so the standalone file still emits the RNG helpers. */
    {
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(repl_randf(i, 3), repl_rand2f(i, 4), 0);");
        repl_export_save_output(rand_alias_path, source_document_view(), NULL);
        char buf[8192];
        read_text_file(rand_alias_path, buf, sizeof(buf));
        ASSERT_TRUE("rand helper emitted for repl_randf source",
                    strstr(buf, "static float repl_randf(float seed, float iter)") != NULL);
        ASSERT_TRUE("rand2 helper emitted for repl_rand2f source",
                    strstr(buf, "static float repl_rand2f(float seed, float iter)") != NULL);
        ASSERT_TRUE("rand helper emitted once",
                    count_substr(buf, "static float repl_randf(float seed, float iter)") == 1);
        ASSERT_TRUE("rand2 helper emitted once",
                    count_substr(buf, "static float repl_rand2f(float seed, float iter)") == 1);
        remove(rand_alias_path);
    }

    glr_app_reset_all(); declare_test_vars();
    ASSERT_TRUE("load scratch output", repl_export_load_from_file(scratch_path) == 1);
    ASSERT_TRUE("scratch roundtrip command count", repl_state_document_count() == 4);
    ASSERT_TRUE("scratch roundtrip keeps assign type",
                repl_state_document_cmds_mut()[2].type == CMD_SCRATCH_ASSIGN);
    ASSERT_TRUE("scratch roundtrip keeps blend source",
                strstr(editor_buffer_line(2), "A[0] = A[0] + (A[1] - A[0])*0.25;") != NULL);
    ASSERT_TRUE("scratch roundtrip keeps vertex source",
                strstr(editor_buffer_line(3), "glVertex3f(A[0], 0, 0);") != NULL);

    /* Func alias roundtrip: a user-named func decl exports an
     * `// @func 0 = drawCube` workspace directive and reloads with
     * the same alias mapping, so subsequent `drawCube()` calls in
     * the source resolve to the same slot. */
    {
        const char *alias_path = "/tmp/repl_core_func_alias_roundtrip.c";
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("drawCube {");
        editor_feed_line("  glVertex3f(0, 0, 0);");
        editor_feed_line("}");
        editor_feed_line("drawCube();");
        ASSERT_TRUE("alias decl assigned slot 0",
                    repl_func_alias_lookup_slot("drawCube") == 0);
        const char *post_decl = repl_func_alias_get(0);
        ASSERT_TRUE("alias readable post-decl",
                    post_decl && strcmp(post_decl, "drawCube") == 0);
        repl_export_save_output(alias_path, source_document_view(), NULL);
        {
            char buf[16384];
            read_text_file(alias_path, buf, sizeof(buf));
            ASSERT_TRUE("alias workspace directive emitted",
                        strstr(buf, "// @func 0 = drawCube") != NULL);
            ASSERT_TRUE("alias canonical text preserved in display",
                        strstr(buf, "drawCube") != NULL);
        }
        glr_app_reset_all(); declare_test_vars();
        ASSERT_TRUE("alias cleared after reset",
                    repl_func_alias_lookup_slot("drawCube") == -1);
        ASSERT_TRUE("load alias output",
                    repl_export_load_from_file(alias_path) == 1);
        ASSERT_TRUE("alias restored on import",
                    repl_func_alias_lookup_slot("drawCube") == 0);
        const char *post_import = repl_func_alias_get(0);
        ASSERT_TRUE("alias name matches on reload",
                    post_import && strcmp(post_import, "drawCube") == 0);
        remove(alias_path);
    }

    /* Direct file import should reset the alias table before reading
     * @func directives from the new file. Otherwise an aliased import
     * followed by a plain import leaves the old alias live even though
     * the second file never declared it. */
    {
        const char *alias_path = "/tmp/repl_core_direct_import_alias.c";
        const char *plain_path = "/tmp/repl_core_direct_import_plain.c";

        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("drawCube {");
        editor_feed_line("  glVertex3f(0, 0, 0);");
        editor_feed_line("}");
        editor_feed_line("drawCube();");
        repl_export_save_output(alias_path, source_document_view(), NULL);

        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(1, 2, 3);");
        repl_export_save_output(plain_path, source_document_view(), NULL);

        glr_app_reset_all(); declare_test_vars();
        ASSERT_TRUE("direct import aliased file succeeds",
                    repl_export_load_from_file(alias_path) == 1);
        ASSERT_TRUE("alias present after direct aliased import",
                    repl_func_alias_lookup_slot("drawCube") == 0);

        ASSERT_TRUE("direct import plain file succeeds",
                    repl_export_load_from_file(plain_path) == 1);
        ASSERT_TRUE("plain direct import clears stale alias",
                    repl_func_alias_lookup_slot("drawCube") == -1);

        remove(alias_path);
        remove(plain_path);
    }

    /* Aliased func with an expression-bearing call (regression).
     *
     * `parse_and_normalize` keeps the *raw* input text for calls whose
     * args reference a predef var (so animated `t` expressions survive
     * verbatim). The exported standalone C must therefore define the
     * function body under the SAME alias name the call sites use, or
     * the .c won't compile and an export->import->export round-trip
     * drops the alias back to the bare `funcN` slot. The original
     * drawCube case above only exercised a zero-arg call, which takes
     * the canonical-text path and masked this. Distilled from the
     * /tmp import-harness used to chase the examples-rework failure. */
    {
        const char *alias_path = "/tmp/repl_core_func_alias_expr_roundtrip.c";
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("spin(scale) {");
        editor_feed_line("  glVertex3f(scale, t, 0);");
        editor_feed_line("}");
        editor_feed_line("spin(t*0.4);");
        ASSERT_TRUE("expr-alias decl assigned slot 0",
                    repl_func_alias_lookup_slot("spin") == 0);

        repl_export_save_output(alias_path, source_document_view(), NULL);
        {
            char buf[16384];
            read_text_file(alias_path, buf, sizeof(buf));
            /* The C body is defined under the alias, not `funcN`, so
             * the call sites resolve and the file compiles. */
            ASSERT_TRUE("alias func def emitted under alias name",
                        strstr(buf, "static void spin(float scale)") != NULL);
            ASSERT_TRUE("no bare funcN def leaked",
                        strstr(buf, "static void func0(") == NULL);
            ASSERT_TRUE("expr-bearing aliased call preserved verbatim",
                        strstr(buf, "spin(t*0.4);") != NULL);
            ASSERT_TRUE("expr-alias workspace directive emitted",
                        strstr(buf, "// @func 0 = spin") != NULL);
        }

        glr_app_reset_all(); declare_test_vars();
        ASSERT_TRUE("load expr-alias output",
                    repl_export_load_from_file(alias_path) == 1);
        ASSERT_TRUE("expr-alias restored on import",
                    repl_func_alias_lookup_slot("spin") == 0);

        /* Re-export after the round-trip: the call must still carry the
         * alias (the bug regressed it to `func0(t*0.4);` here). */
        const char *alias_path2 = "/tmp/repl_core_func_alias_expr_roundtrip2.c";
        repl_export_save_output(alias_path2, source_document_view(), NULL);
        {
            char buf[16384];
            read_text_file(alias_path2, buf, sizeof(buf));
            ASSERT_TRUE("re-exported aliased call still uses alias",
                        strstr(buf, "spin(t*0.4);") != NULL);
            ASSERT_TRUE("re-exported call did not regress to funcN",
                        strstr(buf, "func0(t*0.4);") == NULL);
        }
        remove(alias_path);
        remove(alias_path2);
    }

    /* `if(...)` does not get hijacked into an alias even when it
     * shares the func-decl shape `IDENT(...) {`. The control-flow
     * keyword must fall through to editor_try_commit_if_block. */
    {
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("if(1) {");
        ASSERT_TRUE("'if' did not register as alias",
                    repl_func_alias_lookup_slot("if") == -1);
    }

    /* Camera state round-trip: non-default eye/center must survive save+load. */
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glEnd();");
    glr_camera_set_orbit(31.523f, 31.4799f);
    glr_camera_set_distance(7.59313f);
    glr_camera_set_pan(1.50f, 0.0f, -2.00f);
    repl_export_save_output(path, source_document_view(), NULL);
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
        float saved_rx   = glr_camera().rx;
        float saved_ry   = glr_camera().ry;
        float saved_dist = glr_camera().dist;
        float saved_tx   = glr_camera().tx;
        float saved_ty   = glr_camera().ty;
        float saved_tz   = glr_camera().tz;
        glr_camera_set_orbit(20.0f, 30.0f);
        glr_camera_set_distance(5.0f);
        glr_camera_set_pan(0.0f, 0.0f, 0.0f);
        glr_app_reset_all(); declare_test_vars();
        ASSERT_TRUE("load camera output", repl_export_load_from_file(path) == 1);
        ASSERT_TRUE("camera rx restored",   fabsf(glr_camera().rx   - saved_rx)   < 1e-2f);
        ASSERT_TRUE("camera ry restored",   fabsf(glr_camera().ry   - saved_ry)   < 1e-2f);
        ASSERT_TRUE("camera dist restored", fabsf(glr_camera().dist - saved_dist) < 1e-2f);
        ASSERT_TRUE("camera tx restored",   fabsf(glr_camera().tx   - saved_tx)   < 1e-2f);
        ASSERT_TRUE("camera ty restored",   fabsf(glr_camera().ty   - saved_ty)   < 1e-2f);
        ASSERT_TRUE("camera tz restored",   fabsf(glr_camera().tz   - saved_tz)   < 1e-2f);
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("x = 1;");
    editor_feed_line("func0(radius, yoff) {");
    editor_feed_line("glVertex3f(radius, yoff, 0);");
    editor_feed_line("}");
    editor_feed_line("func0(1.5, x + 2);");

    int func_n = repl_state_document_count();
    for (int i = 0; i < func_n; i++) before_types[i] = repl_state_document_cmds_mut()[i].type;

    glr_state_presentation_mut()->show_vertex_outlines = 1;
    glr_state_presentation_mut()->show_vertex_points = 1;
    repl_export_save_output(func_path, source_document_view(), NULL);
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

    glr_app_reset_all(); declare_test_vars();
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

    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("param func flatten count", repl_state_flat_program_count() >= 2);
    ASSERT_TRUE("param func flatten vertex type", repl_state_flat_program_cmds_mut()[repl_state_flat_program_count() - 1].type == CMD_VERTEX3F);
    ASSERT_TRUE("param func flatten x",
                fabsf(repl_state_flat_program_cmds_mut()[repl_state_flat_program_count() - 1].args[0] - 1.5f) < 1e-6f);
    ASSERT_TRUE("param func flatten y",
                fabsf(repl_state_flat_program_cmds_mut()[repl_state_flat_program_count() - 1].args[1] - 3.0f) < 1e-6f);

    glr_app_reset_all();
    editor_feed_line("func0(radius, sides, phase) {");
    editor_feed_line("for(i, 0, sides + 1) {");
    editor_feed_line("glVertex3f(cos(i*TAU/sides + phase)*radius, sin(i*TAU/sides + phase)*radius, 0);");
    editor_feed_line("}");
    editor_feed_line("}");
    editor_feed_line("func0(1, 6, 0);");
    repl_export_save_output(param_loop_path, source_document_view(), NULL);
    {
        char buf[32768];
        read_text_file(param_loop_path, buf, sizeof(buf));
        ASSERT_TRUE("saved param loop keeps symbolic C bound",
                    strstr(buf, "for (float i = 0; i < sides + 1; i += 1.0f)") != NULL);
    }

    glr_app_reset_all();
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
    repl_flatten_commands(editor_state_edit_line());
    {
        int vertex_count = 0;
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
                vertex_count++;
        ASSERT_TRUE("loaded param loop iterates through sides plus center close",
                    vertex_count == 7);
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("float r;");
    editor_feed_line("glClearColor(0.1, 0.1, 0.1, 1);");
    editor_feed_line("func0 {");
    editor_feed_line("glVertex3f(r, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("r = 2;");
    editor_feed_line("func0();");
    repl_export_save_output(decl_func_path, source_document_view(), NULL);

    glr_app_reset_all(); declare_test_vars();
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

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("float r;");
    editor_feed_line("");
    editor_feed_line("// func prelude");
    editor_feed_line("func0(radius) {");
    editor_feed_line("glVertex3f(radius, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("func0(2);");
    repl_export_save_output(decl_func_blank_path, source_document_view(), NULL);

    glr_app_reset_all(); declare_test_vars();
    ASSERT_TRUE("load decl blank plus func output",
                repl_export_load_from_file(decl_func_blank_path) == 1);
    ASSERT_TRUE("decl blank plus func cmd count", repl_state_document_count() == 7);
    ASSERT_TRUE("imported decl stays first with blank func prelude",
                repl_state_document_cmds_mut()[0].type == CMD_VAR_DECLARE);
    ASSERT_TRUE("imported blank stays after decl",
                repl_state_document_cmds_mut()[1].type == CMD_EMPTY);
    ASSERT_TRUE("imported comment stays after blank",
                repl_state_document_cmds_mut()[2].type == CMD_COMMENT);
    ASSERT_TRUE("imported func def stays after blank/comment prelude",
                repl_state_document_cmds_mut()[3].type == CMD_FUNC_DEF);
    ASSERT_TRUE("imported func body stays after func def",
                repl_state_document_cmds_mut()[4].type == CMD_VERTEX3F);
    ASSERT_TRUE("imported func end stays after func body",
                repl_state_document_cmds_mut()[5].type == CMD_FUNC_END);
    ASSERT_TRUE("imported call stays after function block",
                repl_state_document_cmds_mut()[6].type == CMD_CALL);
    ASSERT_TRUE("imported blank prelude text preserved",
                strcmp(editor_buffer_line(1), "") == 0);
    ASSERT_TRUE("imported comment prelude text preserved",
                strstr(editor_buffer_line(2) ? editor_buffer_line(2) : "",
                       "func prelude") != NULL);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("x = 0.25;");
    editor_feed_line("glutSolidSphere(x, 16, 12);");
    editor_feed_line("glutSolidCone(0.15, 1.5, 8, 1);");
    editor_feed_line("glutSolidTorus(0.1, 0.35, 12, 4);");
    editor_feed_line("glutSolidTeapot(0.25);");
    editor_feed_line("glutSolidCube(0.5);");
    glr_state_presentation_mut()->show_vertex_outlines = 0;
    glr_state_presentation_mut()->show_vertex_points = 0;
    repl_export_save_output(shape_path, source_document_view(), NULL);
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

    glr_app_reset_all(); declare_test_vars();
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

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0(radius) {");
    editor_feed_line("gluBegin(GLU_POLYGON);");
    editor_feed_line("gluBegin(GLU_CONTOUR);");
    editor_feed_line("for(i, 0, 4) {");
    editor_feed_line("gluColor(0.25 + 0.15*sin(i), 0.3, 0.4);");
    editor_feed_line("gluVertex(radius*cos(i), 0, radius*sin(i));");
    editor_feed_line("}");
    editor_feed_line("gluEnd();");
    editor_feed_line("gluEnd();");
    editor_feed_line("}");
    editor_feed_line("func0(2.0);");
    glr_state_presentation_mut()->show_vertex_outlines = 1;
    glr_state_presentation_mut()->show_vertex_points = 1;
    repl_export_save_output(tess_path, source_document_view(), NULL);
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
        /* Multipass export (vertex_outlines + vertex_points enabled
         * earlier) emits the save/restore helpers, and the tess
         * preamble must come before them so the helpers can reference
         * any tess-related state without forward-decl issues. */
        ASSERT_TRUE("saved tess preamble before save/restore helpers",
                    appears_before(buf, "static GLUtesselator *g_tess = NULL;",
                                   "static void save_repl_vars(void)"));
        ASSERT_TRUE("saved tess export includes tess init",
                    strstr(buf, "g_tess = gluNewTess();") != NULL);
        ASSERT_TRUE("saved tess export includes tess callback",
                    strstr(buf, "GLU_TESS_EDGE_FLAG") != NULL);
        ASSERT_TRUE("saved tess no outline helper variants",
                    strstr(buf, "render_repl_outline_") == NULL);
        ASSERT_TRUE("saved tess no vpoint helper variants",
                    strstr(buf, "render_repl_vpoints_") == NULL);
    }

    /* --- Step 4 [P1] regression: workspace save preserves per-scene cfg ---
     *
     * Pre-fix: repl_save_workspace iterated user-scene slots, called
     * install_scene_into_live(s) for each (which restored cmds/vars but
     * NOT s->scene_cfg), then called repl_export_save_output, which
     * emitted @cfg from currently-live cfg via the bridge. Result:
     * inactive scenes were written with whichever cfg happened to be
     * live when the loop started, not their own saved per-scene cfg.
     *
     * Test: build a workspace with two scene files that have different
     * @cfg wireframe values, load it, force live cfg to a third value,
     * save the workspace to a new dir, and verify each output file
     * still contains its slot's saved cfg (not the live value).
     */
    {
        const char *workspace_in  = "/tmp/repl_core_p1_workspace_in";
        const char *workspace_out = "/tmp/repl_core_p1_workspace_out";

        /* Best-effort cleanup of any prior run. */
        int rm_rc = system("rm -rf /tmp/repl_core_p1_workspace_in /tmp/repl_core_p1_workspace_out");
        (void)rm_rc;
        errno = 0;
        int mk_rc = mkdir(workspace_in, 0755);
        ASSERT_TRUE("p1 workspace_in mkdir",
                    mk_rc == 0 || errno == EEXIST);
        if (mk_rc != 0 && errno != EEXIST) {
            fprintf(stderr, "[p1] mkdir %s failed: errno=%d (%s)\n",
                    workspace_in, errno, strerror(errno));
        }

        /* Scene A — wireframe = 1.  Scene B — wireframe = 0.
         * Body lines must sit inside // Snippet start / end markers
         * (that's how repl_export_load_from_file detects the geometry
         * snippet — see import_try_snippet_start in src/repl/export.c). */
        {
            FILE *f = fopen("/tmp/repl_core_p1_workspace_in/scene_a.c", "w");
            ASSERT_TRUE("p1 scene_a fopen", f != NULL);
            if (f) {
                fprintf(f,
                    "// @scene-name P1 Scene A\n"
                    "// @cfg wireframe = 1\n"
                    "static void render_repl_geometry(void) {\n"
                    "  // Snippet start\n"
                    "  glColor3f(1.0000f, 0.0000f, 0.0000f);\n"
                    "  glBegin(GL_TRIANGLES);\n"
                    "  glVertex3f(0.0000f, 0.0000f, 0.0000f);\n"
                    "  glVertex3f(1.0000f, 0.0000f, 0.0000f);\n"
                    "  glVertex3f(0.0000f, 1.0000f, 0.0000f);\n"
                    "  glEnd();\n"
                    "  // Snippet end\n"
                    "}\n");
                fclose(f);
            }
        }
        {
            FILE *f = fopen("/tmp/repl_core_p1_workspace_in/scene_b.c", "w");
            ASSERT_TRUE("p1 scene_b fopen", f != NULL);
            if (f) {
                fprintf(f,
                    "// @scene-name P1 Scene B\n"
                    "// @cfg wireframe = 0\n"
                    "static void render_repl_geometry(void) {\n"
                    "  // Snippet start\n"
                    "  glColor3f(0.0000f, 1.0000f, 0.0000f);\n"
                    "  glBegin(GL_TRIANGLES);\n"
                    "  glVertex3f(0.0000f, 0.0000f, 0.0000f);\n"
                    "  glVertex3f(1.0000f, 0.0000f, 0.0000f);\n"
                    "  glVertex3f(0.0000f, 1.0000f, 0.0000f);\n"
                    "  glEnd();\n"
                    "  // Snippet end\n"
                    "}\n");
                fclose(f);
            }
        }

        glr_app_reset_all(); declare_test_vars();

        int loaded = repl_load_workspace(workspace_in);
        ASSERT_TRUE("p1 load_workspace returned 2", loaded == 2);

        /* Force a distinctive live cfg between load and save: pick a
         * value that differs from BOTH scenes' saved values for
         * wireframe (e.g. 0 here — only matches scene B).  Pre-fix,
         * scene A would also be exported with wireframe=0. */
        int *wireframe_live = &glr_state_presentation_mut()->wireframe;
        *wireframe_live = 0;

        int saved = repl_save_workspace(workspace_out, NULL);
        ASSERT_TRUE("p1 save_workspace wrote 2 files", saved == 2);

        /* Read each saved file and confirm the @cfg matches the
         * scene's own saved cfg, not the live value at save time. */
        char buf_a[8192], buf_b[8192];
        size_t na = read_text_file("/tmp/repl_core_p1_workspace_out/p1_scene_a.c",
                                   buf_a, sizeof(buf_a));
        size_t nb = read_text_file("/tmp/repl_core_p1_workspace_out/p1_scene_b.c",
                                   buf_b, sizeof(buf_b));
        ASSERT_TRUE("p1 saved scene_a opens", na > 0);
        ASSERT_TRUE("p1 saved scene_b opens", nb > 0);
        ASSERT_TRUE("p1 scene A keeps wireframe=1 across workspace save",
                    strstr(buf_a, "// @cfg wireframe = 1") != NULL);
        ASSERT_TRUE("p1 scene B keeps wireframe=0 across workspace save",
                    strstr(buf_b, "// @cfg wireframe = 0") != NULL);
        ASSERT_TRUE("p1 scene A export does NOT leak the live wireframe=0",
                    strstr(buf_a, "// @cfg wireframe = 0") == NULL);

        /* The post-save live cfg should also match what was set before
         * the save (the loop's restore_live_from_stash now includes
         * cfg). */
        ASSERT_TRUE("p1 live wireframe survives the save",
                    glr_state_presentation().wireframe == 0);
    }

    /* --- Regression: code panel and export must agree on the
     *     display() body's shared dynamic state ---
     *
     * Three variants share state — what the code panel shows, what the
     * REPL executes, and what gets exported. The first two are easy to
     * keep in sync because the panel is decorative; the panel vs export
     * comparison is the one with real drift risk (e.g. a prior bug had
     * the export emit glPushAttrib(GL_LIGHTING_BIT) while the panel
     * showed GL_ALL_ATTRIB_BITS).
     *
     * After the g_display_header split, the export's display() opening
     * is literally `for (g_display_header) fprintf`, so byte-equality
     * is structural. This test pins that contract: every line in
     * g_display_header must appear in the exported file in order and
     * contiguously, and every dynamic line (render_state / cam / light
     * positions) emitted by the export must match the strings the
     * panel reads from the same shared buffers. */
    {
        const char *cmp_path = "/tmp/repl_core_panel_export_agreement.c";

        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glVertex3f(0, 0, 0);");
        editor_feed_line("glEnd();");

        /* Refresh the dynamic-state strings the panel reads. The
         * exporter calls refresh internally, so by mirroring that we
         * read the same byte sequence the panel would have shown. */
        repl_refresh_render_state_strings();
        repl_refresh_camera_lines();
        repl_export_save_output(cmp_path, source_document_view(), NULL);

        char buf[16384];
        size_t nread = read_text_file(cmp_path, buf, sizeof(buf));
        ASSERT_TRUE("panel-export agreement: export readable", nread > 0);

        /* (1) g_display_header lines appear in order and contiguously
         *     in the exported file. */
        const char *cursor = buf;
        int header_ok = 1;
        for (int i = 0; g_display_header[i]; i++) {
            const char *hit = strstr(cursor, g_display_header[i]);
            if (!hit) { header_ok = 0; break; }
            cursor = hit + strlen(g_display_header[i]);
            /* Next line must follow immediately after a newline. */
            if (g_display_header[i + 1] && *cursor != '\n') {
                header_ok = 0;
                break;
            }
        }
        ASSERT_TRUE("g_display_header appears verbatim in export",
                    header_ok);

        /* (2) The opening line of display() in the export must equal
         *     the first line of g_display_header (i.e. no hardcoded
         *     `fprintf(f, "void display()...")` re-introduced). */
        ASSERT_TRUE("first g_display_header line is the display() opener",
                    g_display_header[0] != NULL &&
                    strcmp(g_display_header[0], "void display() {") == 0);

        /* (3) Each render_state / cam line the panel would render must
         *     appear in the exported display() body — same buffers. */
        for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++) {
            char label[64];
            snprintf(label, sizeof(label),
                     "render_state[%d] appears in export", i);
            ASSERT_TRUE(label, strstr(buf, g_render_state_lines[i]) != NULL);
        }

        /* (4) The scratch decoration line is panel-only — must NOT
         *     appear in the export (otherwise it shadows the
         *     file-scope statics emitted by emit_export_scratch_globals_section). */
        ASSERT_TRUE("scratch decoration is panel-only, not in export",
                    strstr(buf, REPL_CODE_PANEL_SCRATCH_DECL_LINE) == NULL);

        /* (5) The export's outer attrib mask in display() must be
         *     GL_ALL_ATTRIB_BITS (not the previous GL_LIGHTING_BIT).
         *     This is what g_display_header guarantees by construction;
         *     pinning it here catches anyone re-introducing a hardcoded
         *     fprintf with the wrong mask. */
        ASSERT_TRUE("display() outer push uses GL_ALL_ATTRIB_BITS",
                    strstr(buf, "  glPushAttrib(GL_ALL_ATTRIB_BITS);") != NULL);
        ASSERT_TRUE("display() does not use GL_LIGHTING_BIT outer",
                    count_substr(buf, "glPushAttrib(GL_LIGHTING_BIT)") == 0);
    }

    /* (E) <math.h> namespace-collision wrappers.
     *
     * A user predef whose name matches a <math.h> identifier would
     * collide with the function symbol at C compile time. The export
     * header brackets `#include <math.h>` with `#define X _X` / `#undef
     * X` pairs that rename the math.h symbols out of the way while
     * leaving the user's bare name intact for the `static float X = …;`
     * declarations that follow.
     *
     * The y0/y1 pair was the original motivator (commit landed long
     * before this audit). After the #7 audit jn / yn / gamma / lgamma
     * / tgamma got the same treatment defensively — all single-or-
     * two-letter math.h symbols that are plausible variable names. */
    {
        const char *math_collision_path = "/tmp/repl_core_math_collisions.c";
        glr_app_reset_all(); declare_test_vars();
        repl_export_save_output(math_collision_path,
                                source_document_view(), NULL);

        char buf[16384];
        read_text_file(math_collision_path, buf, sizeof(buf));

        ASSERT_TRUE("math wrapper: y0 defined before include",
                    appears_before(buf, "#define y0 _y0",
                                   "#include <math.h>"));
        ASSERT_TRUE("math wrapper: y0 undefined after include",
                    appears_before(buf, "#include <math.h>", "#undef y0"));

        ASSERT_TRUE("math wrapper: y1 protected",
                    strstr(buf, "#define y1 _y1") != NULL &&
                    strstr(buf, "#undef y1") != NULL);
        ASSERT_TRUE("math wrapper: yn protected",
                    strstr(buf, "#define yn _yn") != NULL &&
                    strstr(buf, "#undef yn") != NULL);
        ASSERT_TRUE("math wrapper: j0 protected",
                    strstr(buf, "#define j0 _j0") != NULL &&
                    strstr(buf, "#undef j0") != NULL);
        ASSERT_TRUE("math wrapper: j1 protected",
                    strstr(buf, "#define j1 _j1") != NULL &&
                    strstr(buf, "#undef j1") != NULL);
        ASSERT_TRUE("math wrapper: jn protected",
                    strstr(buf, "#define jn _jn") != NULL &&
                    strstr(buf, "#undef jn") != NULL);
        ASSERT_TRUE("math wrapper: gamma protected",
                    strstr(buf, "#define gamma _gamma") != NULL &&
                    strstr(buf, "#undef gamma") != NULL);
        ASSERT_TRUE("math wrapper: lgamma protected",
                    strstr(buf, "#define lgamma _lgamma") != NULL &&
                    strstr(buf, "#undef lgamma") != NULL);
        ASSERT_TRUE("math wrapper: tgamma protected",
                    strstr(buf, "#define tgamma _tgamma") != NULL &&
                    strstr(buf, "#undef tgamma") != NULL);

        remove(math_collision_path);
    }

    /* Float persistence should round-trip float32 values without the
     * 6-digit loss from bare %g. Pin both header metadata and a
     * generated numeric for-loop line. */
    {
        const char *precision_path = "/tmp/repl_core_precision_roundtrip.c";
        const float precise_x = 0.1234567f;
        const float loop_start = 0.1234567f;
        const float loop_end = 0.2234567f;
        const float loop_step = 0.0001234567f;
        char expected_var[128];
        char expected_loop[256];
        char buf[16384];

        glr_app_reset_all(); declare_test_vars();
        int x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("precision test var exists", x_idx >= 0);
        g_predef_vars[x_idx].value = precise_x;
        editor_feed_line("for(i, 0.1234567, 0.2234567, 0.0001234567) {");
        editor_feed_line("glVertex3f(i, 0, 0);");
        editor_feed_line("}");

        repl_export_save_output(precision_path, source_document_view(), NULL);
        read_text_file(precision_path, buf, sizeof(buf));

        snprintf(expected_var, sizeof(expected_var),
                 "// @var x = %.9g", (double)precise_x);
        ASSERT_TRUE("workspace header uses float-safe precision",
                    strstr(buf, expected_var) != NULL);

        snprintf(expected_loop, sizeof(expected_loop),
                 "for (float i = %.9g; i < %.9g; i += %.9gf) {",
                 (double)loop_start, (double)loop_end, (double)loop_step);
        ASSERT_TRUE("generated loop uses float-safe precision",
                    strstr(buf, expected_loop) != NULL);

        glr_app_reset_all(); declare_test_vars();
        ASSERT_TRUE("precision export re-imports",
                    repl_export_load_from_file(precision_path) == 1);
        x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("re-imported header value round-trips exactly",
                    x_idx >= 0 && g_predef_vars[x_idx].value == precise_x);
        ASSERT_TRUE("re-imported loop start round-trips exactly",
                    repl_state_document_cmds_mut()[0].args[0] == loop_start);
        ASSERT_TRUE("re-imported loop end round-trips exactly",
                    repl_state_document_cmds_mut()[0].args[1] == loop_end);
        ASSERT_TRUE("re-imported loop step round-trips exactly",
                    repl_state_document_cmds_mut()[0].args[2] == loop_step);

        remove(precision_path);
    }

    /* A physical line longer than MAX_LINE_LEN-1 must not be split into
     * two logical imports. Pre-fix the overflow tail was parsed as a
     * second line, which could smuggle in a command. */
    {
        const char *trunc_path = "/tmp/repl_core_import_truncated_line.c";
        FILE *f = fopen(trunc_path, "w");
        ASSERT_TRUE("truncated-line fixture fopen", f != NULL);
        if (f) {
            fprintf(f, "static void render_repl_geometry(void) {\n");
            fprintf(f, "  // Snippet start\n");
            for (int i = 0; i < 300; i++)
                fputc(' ', f);
            fprintf(f, "glVertex3f(1, 2, 3);\n");
            fprintf(f, "  // Snippet end\n");
            fprintf(f, "}\n");
            fclose(f);
        }

        glr_app_reset_all(); declare_test_vars();
        ASSERT_TRUE("truncated physical line fails import",
                    repl_export_load_from_file(trunc_path) == 0);
        ASSERT_INT("truncated physical line loads no commands",
                   repl_state_document_count(), 0);
        remove(trunc_path);
    }

    /* Scene/workspace callers must preserve export write failures. Use a
     * regular file as the "workspace dir" so mkdir() sees EEXIST but the
     * subsequent scene export path cannot be opened for writing. */
    {
        const char *bad_workspace = "/tmp/repl_core_workspace_save_failure";
        const char *prev_workspace = "/tmp/repl_core_previous_workspace";
        FILE *f = fopen(bad_workspace, "w");
        ASSERT_TRUE("bad workspace fixture fopen", f != NULL);
        if (f)
            fclose(f);

        glr_app_reset_all(); declare_test_vars();
        repl_set_workspace_dir(prev_workspace);
        repl_load_example(0);
        ASSERT_TRUE("promotion for failing workspace save",
                    repl_promote_example_if_needed() >= 1);

        ASSERT_INT("workspace save surfaces export failure",
                   repl_save_workspace(bad_workspace, NULL), -1);
        UiStatusState status = ui_state_status();
        ASSERT_TRUE("workspace save keeps write error status",
                    strstr(status.text, "cannot write") != NULL);
        ASSERT_TRUE("workspace save does not overwrite with success",
                    strstr(status.text, "Saved ") == NULL);
        ASSERT_TRUE("workspace save preserves previous workspace binding",
                    strcmp(repl_workspace_dir(), prev_workspace) == 0);

        unlink(bad_workspace);
        repl_set_workspace_dir("");
    }

    /* (F) Post-import host cursor publication.
     *
     * Phase 4 of plans/done/edit-line-ownership.md moved cursor
     * storage out of ReplState. repl_export_load_from_file threads
     * cursor through ImportState.edit_line internally; review
     * finding [P1] noted that the final value wasn't being
     * published to the host sink, leaving downstream callers
     * (notably repl_load_scene_as_new_slot, which snapshots via
     * repl_dispatch_edit_line_get right after the import returns)
     * to see 0 instead of the post-import row. Pin the contract
     * here so the next person touching the import flow doesn't
     * silently regress it. */
    {
        const char *import_cursor_path = "/tmp/repl_core_import_cursor.c";

        /* (1) Build a small program and export it. */
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_TRIANGLES);");
        editor_feed_line("glVertex3f(0, 0, 0);");
        editor_feed_line("glVertex3f(1, 0, 0);");
        editor_feed_line("glVertex3f(0, 1, 0);");
        editor_feed_line("glEnd();");
        repl_export_save_output(import_cursor_path,
                                source_document_view(), NULL);

        /* (2) Reset to a clean slate (cursor → 0). */
        glr_app_reset_all(); declare_test_vars();
        ASSERT_INT("pre-import cursor is 0",
                   editor_state_edit_line(), 0);

        /* (3) Re-import. */
        ASSERT_TRUE("import succeeded",
                    repl_export_load_from_file(import_cursor_path) == 1);

        /* (4) The post-import cursor must reflect the import's final
         *     ImportState.edit_line — i.e., the next-line slot above
         *     the last imported command, not the pre-import 0. Without
         *     the [P1] fix the cursor would still be 0 here, causing
         *     a subsequent commit to replace the first imported row
         *     instead of appending. */
        ASSERT_TRUE("post-import cursor advanced past 0",
                    editor_state_edit_line() > 0);
        /* Tighter pin: the importer lands the cursor at the row
         * one past the last imported command (the canonical "next
         * append slot"). The exact value depends on the exported
         * file's prologue (header / @cfg / @declare markers, blank
         * runs), so anchor against the import's own bookkeeping —
         * document_count after import is the unambiguous reference. */
        ASSERT_INT("post-import cursor at document end",
                   editor_state_edit_line(), repl_state_document_count());

        remove(import_cursor_path);
    }

    /* Audit #11/#12: camera export block shape — pin the exact 4-line
     * format and the g_angle preamble so format drift is caught. */
    {
        const char *cam_shape_path = "/tmp/repl_core_cam_shape.c";
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(0,0,0);");
        glr_camera_set_orbit(15.0f, 45.0f);
        glr_camera_set_distance(8.0f);
        glr_camera_set_pan(1.0f, 2.0f, 3.0f);
        repl_export_save_output(cam_shape_path, source_document_view(), NULL);

        char buf[16384];
        read_text_file(cam_shape_path, buf, sizeof(buf));

        ASSERT_TRUE("cam shape: g_angle preamble",
                    strstr(buf, "static float g_angle = 45.0000f;") != NULL);
        ASSERT_TRUE("cam shape: line 0 distance",
                    strstr(buf, "glTranslatef(0.0000f, 0.0000f, -8.0000f);") != NULL);
        ASSERT_TRUE("cam shape: line 1 rx pitch",
                    strstr(buf, "glRotatef(15.0000f, 1.0f, 0.0f, 0.0f);") != NULL);
        ASSERT_TRUE("cam shape: line 2 g_angle yaw (not numeric)",
                    strstr(buf, "glRotatef(g_angle, 0.0f, 1.0f, 0.0f);") != NULL);
        ASSERT_TRUE("cam shape: line 2 NOT numeric ry",
                    strstr(buf, "glRotatef(45.0000f, 0.0f, 1.0f, 0.0f);") == NULL);
        ASSERT_TRUE("cam shape: line 3 pan target",
                    strstr(buf, "glTranslatef(-1.0000f, -2.0000f, -3.0000f);") != NULL);

        remove(cam_shape_path);
    }

    /* Audit #12: import tolerates older numeric-ry format (no g_angle). */
    {
        const char *cam_old_path = "/tmp/repl_core_cam_old_fmt.c";
        FILE *f = fopen(cam_old_path, "w");
        ASSERT_TRUE("create old-fmt cam file", f != NULL);
        if (f) {
            fprintf(f,
                "/* Minimal file with old camera format */\n"
                "#include <GL/gl.h>\n"
                "#include <GL/glut.h>\n"
                "void display(void) {\n"
                "  glTranslatef(0.0000f, 0.0000f, -6.5000f);\n"
                "  glRotatef(25.0000f, 1.0f, 0.0f, 0.0f);\n"
                "  glRotatef(55.0000f, 0.0f, 1.0f, 0.0f);\n"
                "  glTranslatef(-0.5000f, -1.0000f, -1.5000f);\n"
                "  // Snippet start\n"
                "  glBegin(GL_POINTS);\n"
                "  glVertex3f(0, 0, 0);\n"
                "  glEnd();\n"
                "  // Snippet end\n"
                "}\n");
            fclose(f);

            glr_app_reset_all(); declare_test_vars();
            ASSERT_TRUE("old-fmt import succeeds",
                        repl_export_load_from_file(cam_old_path) == 1);
            ASSERT_TRUE("old-fmt rx restored",
                        fabsf(glr_camera().rx - 25.0f) < 1e-2f);
            ASSERT_TRUE("old-fmt ry restored",
                        fabsf(glr_camera().ry - 55.0f) < 1e-2f);
            ASSERT_TRUE("old-fmt dist restored",
                        fabsf(glr_camera().dist - 6.5f) < 1e-2f);
            ASSERT_TRUE("old-fmt tx restored",
                        fabsf(glr_camera().tx - 0.5f) < 1e-2f);
            ASSERT_TRUE("old-fmt ty restored",
                        fabsf(glr_camera().ty - 1.0f) < 1e-2f);
            ASSERT_TRUE("old-fmt tz restored",
                        fabsf(glr_camera().tz - 1.5f) < 1e-2f);

            remove(cam_old_path);
        }
    }

    printf("repl_core_io: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
