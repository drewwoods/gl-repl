#include "editor/state.h"
#include "app/glr_camera.h"
#include "app/glr_state.h"
#include "app/glr_mesh_export.h"
#include "app/glr_ctrl.h"
#include "repl/command_store.h"
#include "repl/example_loader.h"
#include "repl/reformat.h"
#include "repl/state_notify.h"
#include "source_document.h"
#include "repl/text_helpers.h"  /* repl_format_source_float, REPL_SOURCE_FLOAT_TEXT_MAX */
#include "editor/input.h"
#include "repl/pipeline.h"
#include "repl/scenes.h"
#include "repl/state_owners.h"
#include "repl/export.h"
#include "repl/executor.h"
#include "ui/app/state.h"
#include "subsystems/edit_overlays/edit_overlays.h"

#include "support/test_harness.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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

#define ASSERT_STR(label, got, expected) do { \
    TEST_ASSERT_STR(&g_harness, label, got, expected); \
} while (0)

static void test_import_robustness(void);
static void test_config_variants_export(void);
static void test_workspace_header_budget_worst_case(void);
#ifdef GL_STUBS
static void test_mesh_export_feedback(void);
#endif
static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
    repl_eval_declare_predef_var("j", err, sizeof(err));
    repl_eval_declare_predef_var("n", err, sizeof(err));
}

static size_t read_text_file(const char *path, char *buf, size_t buf_sz) {
    if (buf_sz == 0)
        return 0;

    FILE *f = fopen(path, "r");
    size_t nread = 0;

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

/* Write `contents` to a temp .c file, import it with fd 2 (stderr)
 * redirected into `out`, and return repl_export_load_from_file's value.
 * Mirrors the dup2 capture used by the #8 load_err regression — used to
 * assert the importer warns (rather than silently dropping) on overflowed
 * @func / @declare names. */
static int import_with_captured_stderr(const char *contents,
                                       char *out, size_t out_sz) {
    const char *src_path = "/tmp/repl_core_io_warn_src.c";
    const char *err_path = "/tmp/repl_core_io_warn_stderr.txt";

    FILE *f = fopen(src_path, "w");
    if (f) { fputs(contents, f); fclose(f); }

    fflush(stderr);
    int cap_fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int saved_fd = dup(STDERR_FILENO);
    if (cap_fd >= 0) dup2(cap_fd, STDERR_FILENO);

    int rc = repl_export_load_from_file(src_path, NULL);
    fflush(stderr);

    if (saved_fd >= 0) { dup2(saved_fd, STDERR_FILENO); close(saved_fd); }
    if (cap_fd >= 0) close(cap_fd);

    out[0] = '\0';
    if (out_sz > 0) {
        FILE *rd = fopen(err_path, "r");
        if (rd) {
            size_t n = fread(out, 1, out_sz - 1, rd);
            out[n] = '\0';
            fclose(rd);
        }
    }
    remove(src_path);
    remove(err_path);
    return rc;
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
    glr_ctrl_reset_all(); declare_test_vars();

    editor_feed_line("x = 1.25;");
    editor_feed_line("glBegin(GL_LINE_STRIP);");
    editor_feed_line("");
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(i + x, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("glEnd();");

    ASSERT_TRUE("pre-save cmds", repl_state_document_count() > 0);
    ASSERT_TRUE("init has host-only ambient line",
                find_init_line("  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, repl_glfloat4(0.15f, 0.15f, 0.20f, 1.0f));") >= 0);
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
         * matching the live REPL's repl_state_time_advance(0.01667).
         * display() must NOT advance t (that would make tDelta
         * frame-rate dependent). */
        ASSERT_TRUE("saved t advances at fixed step in tick()",
                    strstr(buf, "t += 0.01667f;") != NULL);
        ASSERT_TRUE("saved tick computes paced delay",
                    strstr(buf, "delay = timer_delay_ms(&next_deadline_ms);") != NULL);
        ASSERT_TRUE("saved tick reschedules via computed delay",
                    strstr(buf, "glutTimerFunc((unsigned int)delay, tick, 0)") != NULL);
        ASSERT_TRUE("saved main starts tick timer",
                    strstr(buf, "glutTimerFunc(16, tick, 0)") != NULL);
        ASSERT_TRUE("saved display does not derive t from elapsed time",
                    strstr(buf, "t = (float)glutGet(GLUT_ELAPSED_TIME)") == NULL);
        ASSERT_TRUE("saved no longer uses glutIdleFunc",
                    strstr(buf, "glutIdleFunc") == NULL);
        ASSERT_TRUE("saved multisample header state",
                    strstr(buf, "glDisable(GL_MULTISAMPLE);") != NULL);
        ASSERT_TRUE("saved line smooth header state",
                    strstr(buf, "glEnable(GL_LINE_SMOOTH);") != NULL);
        ASSERT_TRUE("saved geometry helper",
                    strstr(buf, "static void draw_scene(void)") != NULL);
        ASSERT_TRUE("generated banner before metadata",
                    appears_before(buf, "Generated by gl-repl",
                                   "/* @workspace:"));
        ASSERT_TRUE("export does not include repo-local gl_includes",
                    strstr(buf, "#include \"gl_includes.h\"") == NULL);
        ASSERT_TRUE("flattened macOS GLUT include present",
                    strstr(buf, "#include <GLUT/glut.h>") != NULL);
        ASSERT_TRUE("flattened Linux freeglut include present",
                    strstr(buf, "#include <GL/freeglut.h>") != NULL);
        ASSERT_TRUE("scaffold metadata before includes",
                    appears_before(buf, "/* @workspace:",
                                   "#include <stddef.h>"));
        ASSERT_TRUE("scaffold header before globals",
                    appears_before(buf, "#include <GL/freeglut.h>",
                                   "static float t = "));
        /* Predef-var initializer carries the export-time snapshot value
         * (replaces the role reset_repl_vars used to play). */
        ASSERT_TRUE("predef var initializer carries snapshot value",
                    strstr(buf, "static float x = 1.25;") != NULL);
        /* Single-pass exports do not need save/restore helpers. */
        ASSERT_TRUE("single-pass export omits save_repl_vars",
                    strstr(buf, "save_repl_vars") == NULL);
        ASSERT_TRUE("single-pass export omits restore_repl_vars",
                    strstr(buf, "restore_repl_vars") == NULL);
        ASSERT_TRUE("scaffold globals before render helper",
                    appears_before(buf, "static float t = ",
                                   "static void draw_scene(void)"));
        ASSERT_TRUE("scaffold render helper before display",
                    appears_before(buf, "static void draw_scene(void)",
                                   "void display(void)"));
        ASSERT_TRUE("scaffold display before init",
                    appears_before(buf, "void display(void)", "void init(void)"));
        ASSERT_TRUE("saved snippet marker start",
                    strstr(buf, "/* Snippet start */") != NULL);
        ASSERT_TRUE("saved snippet marker end",
                    strstr(buf, "/* Snippet end */") != NULL);
        ASSERT_TRUE("saved outline helper omitted when disabled",
                    strstr(buf, "render_repl_outline_overlay") == NULL);
        ASSERT_TRUE("saved vertex points helper omitted when disabled",
                    strstr(buf, "render_repl_vertex_points_overlay") == NULL);
        ASSERT_TRUE("saved init ambient model line",
                    strstr(buf, "glLightModelfv(GL_LIGHT_MODEL_AMBIENT, repl_glfloat4(0.15f, 0.15f, 0.20f, 1.0f));") != NULL);
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
        const char *init_func = strstr(buf, "\nvoid init(void)");
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

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load saved output", repl_export_load_from_file(path, NULL) == 1);
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

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("A[0] = 0;");
    editor_feed_line("A[1] = 1;");
    editor_feed_line("A[0] = A[0] + (A[1] - A[0])*0.25;");
    editor_feed_line("glVertex3f(A[0], 0, 0);");
    repl_export_save_output(scratch_path, source_document_view(), NULL);
    {
        char buf[16384];
        read_text_file(scratch_path, buf, sizeof(buf));
        ASSERT_TRUE("scratch global A exported (used)",
                    strstr(buf, "static float A[16] = {0};") != NULL);
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
        glr_ctrl_reset_all(); declare_test_vars();
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
        glr_ctrl_reset_all(); declare_test_vars();
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

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load scratch output", repl_export_load_from_file(scratch_path, NULL) == 1);
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
        glr_ctrl_reset_all(); declare_test_vars();
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
                        strstr(buf, "/* @func 0 = drawCube */") != NULL);
            ASSERT_TRUE("alias canonical text preserved in display",
                        strstr(buf, "drawCube") != NULL);
        }
        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("alias cleared after reset",
                    repl_func_alias_lookup_slot("drawCube") == -1);
        ASSERT_TRUE("load alias output",
                    repl_export_load_from_file(alias_path, NULL) == 1);
        ASSERT_TRUE("alias restored on import",
                    repl_func_alias_lookup_slot("drawCube") == 0);
        const char *post_import = repl_func_alias_get(0);
        ASSERT_TRUE("alias name matches on reload",
                    post_import && strcmp(post_import, "drawCube") == 0);
        remove(alias_path);
    }

    /* Long func-alias roundtrip (regression): a 21-char alias name must
     * survive export -> import without truncation. With REPL_FUNC_NAME_MAX
     * at its old value of 16 the stored alias was clipped to 15 chars and
     * the matching `static void <name>(...)` definition failed to map back
     * to its slot, so the whole function was dropped on reload. */
    {
        const char *long_path = "/tmp/repl_core_func_alias_long.c";
        const char *long_name = "computeVerticalMotion"; /* 21 chars */
        ASSERT_TRUE("test name within current func-name limit",
                    (int)strlen(long_name) < REPL_FUNC_NAME_MAX);
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("computeVerticalMotion {");
        editor_feed_line("  glVertex3f(0, 0, 0);");
        editor_feed_line("}");
        editor_feed_line("computeVerticalMotion();");
        ASSERT_TRUE("long alias decl assigned slot 0",
                    repl_func_alias_lookup_slot(long_name) == 0);
        const char *long_post_decl = repl_func_alias_get(0);
        ASSERT_TRUE("long alias stored untruncated post-decl",
                    long_post_decl && strcmp(long_post_decl, long_name) == 0);
        repl_export_save_output(long_path, source_document_view(), NULL);
        {
            char buf[16384];
            read_text_file(long_path, buf, sizeof(buf));
            ASSERT_TRUE("long alias workspace directive emitted",
                        strstr(buf, "/* @func 0 = computeVerticalMotion */") != NULL);
            ASSERT_TRUE("long alias used as C function name",
                        strstr(buf, "static void computeVerticalMotion(") != NULL);
        }
        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("long alias cleared after reset",
                    repl_func_alias_lookup_slot(long_name) == -1);
        ASSERT_TRUE("load long alias output",
                    repl_export_load_from_file(long_path, NULL) == 1);
        ASSERT_TRUE("long alias restored on import (not dropped)",
                    repl_func_alias_lookup_slot(long_name) == 0);
        const char *long_post_import = repl_func_alias_get(0);
        ASSERT_TRUE("long alias name matches untruncated on reload",
                    long_post_import && strcmp(long_post_import, long_name) == 0);
        remove(long_path);
    }

    /* Truncation boundary (invariant): a name that exactly fills the alias
     * buffer (REPL_FUNC_NAME_MAX - 1 chars) is a valid func alias, but one
     * char longer overflows and is rejected. This is the cap that import's
     * parse_func_alias truncates against (and now warns about) — the reason
     * the long alias above was dropped before REPL_FUNC_NAME_MAX was bumped. */
    {
        char at_limit[REPL_FUNC_NAME_MAX];        /* MAX-1 'a's + NUL */
        char over_limit[REPL_FUNC_NAME_MAX + 1];  /* MAX   'a's + NUL */
        memset(at_limit,   'a', sizeof(at_limit)   - 1);
        memset(over_limit, 'a', sizeof(over_limit) - 1);
        at_limit[sizeof(at_limit)     - 1] = '\0';
        over_limit[sizeof(over_limit) - 1] = '\0';
        ASSERT_TRUE("func name filling the buffer is accepted",
                    (int)strlen(at_limit) == REPL_FUNC_NAME_MAX - 1 &&
                    repl_func_alias_name_is_valid(at_limit) == 1);
        ASSERT_TRUE("func name one char over the limit is rejected",
                    (int)strlen(over_limit) == REPL_FUNC_NAME_MAX &&
                    repl_func_alias_name_is_valid(over_limit) == 0);
    }

    /* else is reserved and cannot be a function alias name */
    {
        ASSERT_TRUE("else is rejected as a function alias",
                    repl_func_alias_name_is_valid("else") == 0);
        ASSERT_TRUE("elsewhere is valid as a function alias",
                    repl_func_alias_name_is_valid("elsewhere") == 1);
    }

    /* Direct file import should reset the alias table before reading
     * @func directives from the new file. Otherwise an aliased import
     * followed by a plain import leaves the old alias live even though
     * the second file never declared it. */
    {
        const char *alias_path = "/tmp/repl_core_direct_import_alias.c";
        const char *plain_path = "/tmp/repl_core_direct_import_plain.c";

        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("drawCube {");
        editor_feed_line("  glVertex3f(0, 0, 0);");
        editor_feed_line("}");
        editor_feed_line("drawCube();");
        repl_export_save_output(alias_path, source_document_view(), NULL);

        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(1, 2, 3);");
        repl_export_save_output(plain_path, source_document_view(), NULL);

        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("direct import aliased file succeeds",
                    repl_export_load_from_file(alias_path, NULL) == 1);
        ASSERT_TRUE("alias present after direct aliased import",
                    repl_func_alias_lookup_slot("drawCube") == 0);

        ASSERT_TRUE("direct import plain file succeeds",
                    repl_export_load_from_file(plain_path, NULL) == 1);
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
        glr_ctrl_reset_all(); declare_test_vars();
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
                        strstr(buf, "/* @func 0 = spin */") != NULL);
        }

        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("load expr-alias output",
                    repl_export_load_from_file(alias_path, NULL) == 1);
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
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("if(1) {");
        ASSERT_TRUE("'if' did not register as alias",
                    repl_func_alias_lookup_slot("if") == -1);
    }

    /* If-chain (else / else-if) export -> import round-trip.
     *
     * test_repl_core_format.c covers the import-only direction (loading a
     * hand-written else file) and test_repl_core_commit.c covers commit +
     * flatten arm selection. This block closes the loop: build a
     * first-class if / else-if / else chain in the REPL, EXPORT it, and
     * verify the separators are emitted as canonical `} else if(...) {` /
     * `} else {` C and reconstruct as CMD_ELSE_IF / CMD_ELSE source
     * commands on reload (not lowered to nested ifs), with flatten still
     * selecting the first true arm and a byte-stable re-export. */
    {
        const char *else_path  = "/tmp/repl_core_else_roundtrip.c";
        const char *else_path2 = "/tmp/repl_core_else_roundtrip2.c";

        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("n = 2;");
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("if(n < 1) {");
        editor_feed_line("glVertex3f(1, 0, 0);");
        editor_feed_line("} else if(n < 3) {");
        editor_feed_line("glVertex3f(2, 0, 0);");
        editor_feed_line("} else {");
        editor_feed_line("glVertex3f(9, 0, 0);");
        editor_feed_line("}");
        editor_feed_line("glEnd();");

        int else_n = repl_state_document_count();
        CmdType else_types[MAX_COMMANDS];
        int built_elif = 0, built_else = 0;
        for (int i = 0; i < else_n; i++) {
            else_types[i] = repl_state_document_cmds_mut()[i].type;
            if (else_types[i] == CMD_ELSE_IF) built_elif++;
            if (else_types[i] == CMD_ELSE)    built_else++;
        }
        ASSERT_INT("else chain built with one else-if", built_elif, 1);
        ASSERT_INT("else chain built with one else", built_else, 1);

        repl_export_save_output(else_path, source_document_view(), NULL);
        {
            char buf[16384];
            read_text_file(else_path, buf, sizeof(buf));
            ASSERT_TRUE("export emits canonical else-if separator",
                        strstr(buf, "} else if(n < 3) {") != NULL);
            ASSERT_TRUE("export emits canonical else separator",
                        strstr(buf, "} else {") != NULL);
        }

        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("load else round-trip output",
                    repl_export_load_from_file(else_path, NULL) == 1);
        ASSERT_INT("else round-trip cmd count",
                   repl_state_document_count(), else_n);
        for (int i = 0; i < else_n; i++) {
            char label[64];
            snprintf(label, sizeof(label), "else round-trip type %d", i);
            ASSERT_TRUE(label,
                        repl_state_document_cmds_mut()[i].type == else_types[i]);
        }
        {
            int rt_elif = 0, rt_else = 0;
            for (int i = 0; i < repl_state_document_count(); i++) {
                if (repl_state_document_cmds_mut()[i].type == CMD_ELSE_IF) rt_elif++;
                if (repl_state_document_cmds_mut()[i].type == CMD_ELSE)    rt_else++;
            }
            ASSERT_INT("else round-trip keeps first-class else-if", rt_elif, 1);
            ASSERT_INT("else round-trip keeps first-class else", rt_else, 1);
        }

        /* Arm selection survives the round-trip: n == 2 skips the false
         * `n < 1` then takes the `n < 3` else-if arm -> glVertex3f(2,...). */
        repl_flatten_commands(editor_state_edit_line());
        {
            int vtx_count = 0;
            float vtx_x = -1.0f;
            for (int i = 0; i < repl_state_flat_program_count(); i++) {
                if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F) {
                    vtx_count++;
                    vtx_x = repl_state_flat_program_cmds_mut()[i].args[0];
                }
            }
            ASSERT_INT("else round-trip flatten emits exactly one arm",
                       vtx_count, 1);
            ASSERT_TRUE("else round-trip selects else-if arm (n=2 -> x=2)",
                        fabsf(vtx_x - 2.0f) < 1e-6f);
        }

        /* Stability: re-export of the reloaded program is byte-identical
         * to the original export (lossless canonical round-trip). */
        repl_export_save_output(else_path2, source_document_view(), NULL);
        {
            char buf1[16384];
            char buf2[16384];
            read_text_file(else_path, buf1, sizeof(buf1));
            read_text_file(else_path2, buf2, sizeof(buf2));
            ASSERT_TRUE("else round-trip re-export byte-stable",
                        strcmp(buf1, buf2) == 0);
        }

        remove(else_path);
        remove(else_path2);
    }

    /* Camera state round-trip: non-default eye/center must survive save+load. */
    glr_ctrl_reset_all(); declare_test_vars();
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
        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("load camera output", repl_export_load_from_file(path, NULL) == 1);
        ASSERT_TRUE("camera rx restored",   fabsf(glr_camera().rx   - saved_rx)   < 1e-2f);
        ASSERT_TRUE("camera ry restored",   fabsf(glr_camera().ry   - saved_ry)   < 1e-2f);
        ASSERT_TRUE("camera dist restored", fabsf(glr_camera().dist - saved_dist) < 1e-2f);
        ASSERT_TRUE("camera tx restored",   fabsf(glr_camera().tx   - saved_tx)   < 1e-2f);
        ASSERT_TRUE("camera ty restored",   fabsf(glr_camera().ty   - saved_ty)   < 1e-2f);
        ASSERT_TRUE("camera tz restored",   fabsf(glr_camera().tz   - saved_tz)   < 1e-2f);
    }

    /* Camera round-trip with a user function that contains its own
     * glTranslatef/glRotatef calls. The exporter writes that function
     * (a `static void funcN(...)` body) before display(), so on import
     * the camera state machine sees those transforms first. It must not
     * consume them as camera lines — doing so corrupted both the function
     * and the real camera block in display() (camera distance regressed
     * to 0). Regression for the workspace/new_scene.c report. */
    {
        const char *cam_func_path = "/tmp/repl_core_cam_func_roundtrip.c";
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("func0() {");
        editor_feed_line("glTranslatef(-0.65, 0, 0);");
        editor_feed_line("glRotatef(15, 0, 1, 0);");
        editor_feed_line("glVertex3f(0, 0, 0);");
        editor_feed_line("}");
        editor_feed_line("func0();");
        glr_camera_set_orbit(22.5235f, -49.0020f);
        glr_camera_set_distance(6.3524f);
        glr_camera_set_pan(0.0f, 0.0f, 0.0f);
        repl_export_save_output(cam_func_path, source_document_view(), NULL);

        glr_camera_set_orbit(10.0f, 10.0f);
        glr_camera_set_distance(99.0f);
        glr_camera_set_pan(7.0f, 7.0f, 7.0f);
        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("load camera-with-func output",
                    repl_export_load_from_file(cam_func_path, NULL) == 1);
        ASSERT_TRUE("camera dist not eaten by func glTranslatef",
                    fabsf(glr_camera().dist - 6.3524f) < 1e-2f);
        ASSERT_TRUE("camera rx restored past func transforms",
                    fabsf(glr_camera().rx - 22.5235f) < 1e-2f);
        ASSERT_TRUE("camera ry restored past func transforms",
                    fabsf(glr_camera().ry - (-49.0020f)) < 1e-2f);
        remove(cam_func_path);
    }

    glr_ctrl_reset_all(); declare_test_vars();
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
                                   "static void draw_scene(void)"));
        ASSERT_TRUE("saved geometry helper func call",
                    strstr(buf, "func0(1.5, x + 2);") != NULL);
        ASSERT_TRUE("saved geometry helper once",
                    count_substr(buf, "static void draw_scene(void)") == 1);
        ASSERT_TRUE("saved no outline helper variants",
                    strstr(buf, "render_repl_outline_") == NULL);
        ASSERT_TRUE("saved no vpoint helper variants",
                    strstr(buf, "render_repl_vpoints_") == NULL);
        ASSERT_TRUE("saved geometry called once in display",
                    count_substr(buf, "draw_scene();") == 1);
        /* Outline/point pass specs are intentionally disabled in export
         * (see export_build_display_passes in src/repl/export_display.c), even
         * when the cfg flags are on. Multipass save/restore follows the
         * pass specs rather than the cfg flags. */
        ASSERT_TRUE("saved overlay-disabled export omits save/restore",
                    strstr(buf, "save_repl_vars") == NULL);
        ASSERT_TRUE("saved outline pass omits line polygon mode",
                    strstr(buf, "glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);") == NULL);
        ASSERT_TRUE("saved vpoints pass omits point polygon mode",
                    strstr(buf, "glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);") == NULL);
        ASSERT_TRUE("saved overlay omits color material disable",
                    strstr(buf, "glDisable(GL_COLOR_MATERIAL);") == NULL);
    }

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load saved param func output", repl_export_load_from_file(func_path, NULL) == 1);
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

    glr_ctrl_reset_all();
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
        ASSERT_TRUE("saved param loop hoists loop var decl with marker",
                    strstr(buf, "float i; /* repl-export-c89-loop-var */") != NULL);
        ASSERT_TRUE("saved param loop keeps symbolic C bound",
                    strstr(buf, "for (i = 0; i < sides + 1; i += 1.0f)") != NULL);
    }

    glr_ctrl_reset_all();
    ASSERT_TRUE("load saved param loop output", repl_export_load_from_file(param_loop_path, NULL) == 1);
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

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("float r;");
    editor_feed_line("glClearColor(0.1, 0.1, 0.1, 1);");
    editor_feed_line("func0 {");
    editor_feed_line("glVertex3f(r, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("r = 2;");
    editor_feed_line("func0();");
    repl_export_save_output(decl_func_path, source_document_view(), NULL);

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load decl plus promoted func output",
                repl_export_load_from_file(decl_func_path, NULL) == 1);
    ASSERT_TRUE("decl plus func cmd count", repl_state_document_count() == 7);
    ASSERT_TRUE("imported decl remains first", repl_state_document_cmds_mut()[0].type == CMD_VAR_DECLARE);
    ASSERT_TRUE("imported func follows decl", repl_state_document_cmds_mut()[1].type == CMD_FUNC_DEF);
    ASSERT_TRUE("imported func body follows header", repl_state_document_cmds_mut()[2].type == CMD_VERTEX3F);
    ASSERT_TRUE("imported func end follows body", repl_state_document_cmds_mut()[3].type == CMD_FUNC_END);
    ASSERT_TRUE("imported prior command follows func block", repl_state_document_cmds_mut()[4].type == CMD_CLEAR_COLOR);
    ASSERT_TRUE("imported var assign follows prior command", repl_state_document_cmds_mut()[5].type == CMD_VAR_ASSIGN);
    ASSERT_TRUE("imported call follows assign", repl_state_document_cmds_mut()[6].type == CMD_CALL);

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("float r;");
    editor_feed_line("");
    editor_feed_line("// func prelude");
    editor_feed_line("func0(radius) {");
    editor_feed_line("glVertex3f(radius, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("func0(2);");
    repl_export_save_output(decl_func_blank_path, source_document_view(), NULL);

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load decl blank plus func output",
                repl_export_load_from_file(decl_func_blank_path, NULL) == 1);
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

    glr_ctrl_reset_all(); declare_test_vars();
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

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load saved shape output", repl_export_load_from_file(shape_path, NULL) == 1);
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

    glr_ctrl_reset_all(); declare_test_vars();
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
                    strstr(buf, "_tc[0] = 0.25 + 0.15*sinf(i);") != NULL);
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
        ASSERT_TRUE("saved tess export omits save/restore when pass specs disabled",
                    strstr(buf, "save_repl_vars") == NULL);
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
         * snippet — see import_try_snippet_start in src/repl/import.c). */
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

        glr_ctrl_reset_all(); declare_test_vars();

        int loaded = repl_load_workspace(workspace_in);
        ASSERT_TRUE("p1 load_workspace returned 2", loaded == 2);

        /* Force a distinctive live cfg between load and save: pick a
         * value that differs from BOTH scenes' saved values for
         * wireframe (e.g. 0 here — only matches scene B).  Pre-fix,
         * scene A would also be exported with wireframe=0. */
        glr_state_presentation_mut()->wireframe = WIREFRAME_OFF;

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
                    strstr(buf_a, "/* @cfg wireframe = WIREFRAME_PLAIN */") != NULL);
        ASSERT_TRUE("p1 scene B keeps wireframe=0 across workspace save",
                    strstr(buf_b, "/* @cfg wireframe = WIREFRAME_OFF */") != NULL);
        ASSERT_TRUE("p1 scene A export does NOT leak the live wireframe=0",
                    strstr(buf_a, "/* @cfg wireframe = WIREFRAME_OFF */") == NULL);

        /* The post-save live cfg should also match what was set before
         * the save (the loop's restore_live_from_stash now includes
         * cfg). */
        ASSERT_TRUE("p1 live wireframe survives the save",
                    glr_state_presentation().wireframe == WIREFRAME_OFF);
    }

    /* --- Regression: workspace save preserves per-scene camera ---
     *
     * Pre-fix mirror of the cfg regression above. Symptoms (verbatim
     * from the bug report):
     *
     *   "When you save the entire workspace, it also updates other
     *    scene's in the workspaces' camera (not expected). … the
     *    workspace in the current repo will load the whale.c scene,
     *    but not with the whale's camera in the file."
     *
     * Root cause: repl_save_workspace's iteration called
     * install_scene_into_live(s) for each slot, but
     * install_scene_into_live didn't restore the slot's saved camera.
     * The export bridge then read the LIVE camera (= the active
     * scene's camera, captured at the top of the iteration) and wrote
     * the same camera into every output file. After the fix, each
     * slot carries its own ReplExportCameraBlock and the iteration
     * snap-applies it before export. Load-side: the camera bridge's
     * import path wrote to live; each file's camera ended up in the
     * last-loaded slot's slot, not its own. After the fix,
     * load_scene_file_into_slot's save_scene_to_slot captures the
     * (just-loaded) camera into the new slot.
     */
    {
        const char *workspace_in  = "/tmp/repl_core_cam_workspace_in";
        const char *workspace_out = "/tmp/repl_core_cam_workspace_out";

        int rm_rc = system("rm -rf /tmp/repl_core_cam_workspace_in /tmp/repl_core_cam_workspace_out");
        (void)rm_rc;
        errno = 0;
        int mk_rc = mkdir(workspace_in, 0755);
        ASSERT_TRUE("cam workspace_in mkdir",
                    mk_rc == 0 || errno == EEXIST);

        /* Scene A has a "close, high pitch" camera; scene B has a
         * "far, low pitch" one. Format mirrors what the exporter
         * writes (4-line block plus the static float g_angle preamble
         * — both halves are part of the save format the bridge
         * understands on import). */
        {
            FILE *f = fopen("/tmp/repl_core_cam_workspace_in/scene_a.c", "w");
            ASSERT_TRUE("cam scene_a fopen", f != NULL);
            if (f) {
                fprintf(f,
                    "// @scene-name Cam Scene A\n"
                    "static float g_angle = 11.0000f;\n"
                    "static void render_repl_geometry(void) {\n"
                    "  // camera\n"
                    "  glTranslatef(0.0000f, 0.0000f, -3.5000f);\n"
                    "  glRotatef(45.0000f, 1.0f, 0.0f, 0.0f);\n"
                    "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);\n"
                    "  glTranslatef(-1.0000f, -2.0000f, -3.0000f);\n"
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
            FILE *f = fopen("/tmp/repl_core_cam_workspace_in/scene_b.c", "w");
            ASSERT_TRUE("cam scene_b fopen", f != NULL);
            if (f) {
                fprintf(f,
                    "// @scene-name Cam Scene B\n"
                    "static float g_angle = 99.0000f;\n"
                    "static void render_repl_geometry(void) {\n"
                    "  // camera\n"
                    "  glTranslatef(0.0000f, 0.0000f, -25.5000f);\n"
                    "  glRotatef(5.0000f, 1.0f, 0.0f, 0.0f);\n"
                    "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);\n"
                    "  glTranslatef(-4.0000f, -5.0000f, -6.0000f);\n"
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

        glr_ctrl_reset_all(); declare_test_vars();

        int loaded = repl_load_workspace(workspace_in);
        ASSERT_TRUE("cam load_workspace returned 2", loaded == 2);

        /* Force a distinctive live camera between load and save —
         * different from either scene's saved pose. Pre-fix, both
         * output files would have ended up with this camera. */
        glr_camera_set_distance(50.0f);
        glr_camera_set_orbit(80.0f, 200.0f);
        glr_camera_set_pan(-7.0f, -8.0f, -9.0f);

        int saved = repl_save_workspace(workspace_out, NULL);
        ASSERT_TRUE("cam save_workspace wrote 2 files", saved == 2);

        char buf_a[8192], buf_b[8192];
        size_t na = read_text_file("/tmp/repl_core_cam_workspace_out/cam_scene_a.c",
                                   buf_a, sizeof(buf_a));
        size_t nb = read_text_file("/tmp/repl_core_cam_workspace_out/cam_scene_b.c",
                                   buf_b, sizeof(buf_b));
        ASSERT_TRUE("cam saved scene_a opens", na > 0);
        ASSERT_TRUE("cam saved scene_b opens", nb > 0);

        /* Each output file keeps ITS slot's camera. Two key markers
         * per scene: the dist line (line 0 of the block) and the
         * static float g_angle preamble (which encodes ry). */
        ASSERT_TRUE("cam scene_a keeps own dist=-3.5000",
                    strstr(buf_a, "glTranslatef(0.0000f, 0.0000f, -3.5000f);") != NULL);
        ASSERT_TRUE("cam scene_a keeps own rx=45.0000",
                    strstr(buf_a, "glRotatef(45.0000f, 1.0f, 0.0f, 0.0f);") != NULL);
        ASSERT_TRUE("cam scene_a keeps own g_angle=11.0000 (ry)",
                    strstr(buf_a, "static float g_angle = 11.0000f;") != NULL);
        /* Camera-block line 4 is `glTranslatef(-tx, -ty, -tz)`, so the
         * round-trip preserves the original "-1, -2, -3" text. */
        ASSERT_TRUE("cam scene_a keeps own pan -1,-2,-3",
                    strstr(buf_a, "glTranslatef(-1.0000f, -2.0000f, -3.0000f);") != NULL);
        ASSERT_TRUE("cam scene_b keeps own dist=-25.5000",
                    strstr(buf_b, "glTranslatef(0.0000f, 0.0000f, -25.5000f);") != NULL);
        ASSERT_TRUE("cam scene_b keeps own rx=5.0000",
                    strstr(buf_b, "glRotatef(5.0000f, 1.0f, 0.0f, 0.0f);") != NULL);
        ASSERT_TRUE("cam scene_b keeps own g_angle=99.0000 (ry)",
                    strstr(buf_b, "static float g_angle = 99.0000f;") != NULL);
        ASSERT_TRUE("cam scene_b keeps own pan -4,-5,-6",
                    strstr(buf_b, "glTranslatef(-4.0000f, -5.0000f, -6.0000f);") != NULL);

        /* Cross-check: neither file leaks the live camera that was
         * set between load and save (dist 50, rx 80). */
        ASSERT_TRUE("cam scene_a does NOT leak live dist=50",
                    strstr(buf_a, "glTranslatef(0.0000f, 0.0000f, -50.0000f);") == NULL);
        ASSERT_TRUE("cam scene_b does NOT leak live dist=50",
                    strstr(buf_b, "glTranslatef(0.0000f, 0.0000f, -50.0000f);") == NULL);
        ASSERT_TRUE("cam scene_a does NOT leak live rx=80",
                    strstr(buf_a, "glRotatef(80.0000f, 1.0f, 0.0f, 0.0f);") == NULL);

        /* Live camera survives the workspace save (matches the cfg
         * regression's symmetry — stash/restore brackets the loop). */
        ASSERT_TRUE("cam live dist survives the save",
                    glr_camera().dist == 50.0f);
        ASSERT_TRUE("cam live rx survives the save",
                    glr_camera().rx == 80.0f);
    }

    /* --- Regression: tab switch eases to the slot's saved camera ---
     *
     * Same root-cause family as the workspace-save regression above,
     * but on the load/switch side. When the user clicks (or F12s)
     * to a different tab, the live camera should snap toward that
     * slot's saved pose; pre-fix the slot carried no camera at all,
     * so tab-switching kept whatever camera the previous tab had
     * set. We snap rather than ease in the test so the assertion
     * doesn't have to tick the easing forward; the runtime path
     * (load_scene_from_slot) calls apply_example_block (ease), but
     * either path proves the slot is carrying the right camera —
     * which is what the bug was about. */
    {
        const char *workspace_in  = "/tmp/repl_core_cam_switch_workspace_in";

        int rm_rc = system("rm -rf /tmp/repl_core_cam_switch_workspace_in");
        (void)rm_rc;
        errno = 0;
        int mk_rc = mkdir(workspace_in, 0755);
        ASSERT_TRUE("cam-switch workspace_in mkdir",
                    mk_rc == 0 || errno == EEXIST);
        {
            FILE *f = fopen("/tmp/repl_core_cam_switch_workspace_in/scene_a.c", "w");
            if (f) {
                fprintf(f,
                    "// @scene-name SW Scene A\n"
                    "static float g_angle = 17.0000f;\n"
                    "static void render_repl_geometry(void) {\n"
                    "  // camera\n"
                    "  glTranslatef(0.0000f, 0.0000f, -4.5000f);\n"
                    "  glRotatef(13.0000f, 1.0f, 0.0f, 0.0f);\n"
                    "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);\n"
                    "  glTranslatef(0.0000f, 0.0000f, 0.0000f);\n"
                    "  // Snippet start\n"
                    "  glColor3f(1.0000f, 0.0000f, 0.0000f);\n"
                    "  // Snippet end\n"
                    "}\n");
                fclose(f);
            }
        }
        {
            FILE *f = fopen("/tmp/repl_core_cam_switch_workspace_in/scene_b.c", "w");
            if (f) {
                fprintf(f,
                    "// @scene-name SW Scene B\n"
                    "static float g_angle = 71.0000f;\n"
                    "static void render_repl_geometry(void) {\n"
                    "  // camera\n"
                    "  glTranslatef(0.0000f, 0.0000f, -22.0000f);\n"
                    "  glRotatef(33.0000f, 1.0f, 0.0f, 0.0f);\n"
                    "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);\n"
                    "  glTranslatef(0.0000f, 0.0000f, 0.0000f);\n"
                    "  // Snippet start\n"
                    "  glColor3f(0.0000f, 1.0000f, 0.0000f);\n"
                    "  // Snippet end\n"
                    "}\n");
                fclose(f);
            }
        }

        glr_ctrl_reset_all(); declare_test_vars();

        ASSERT_TRUE("cam-switch load returned 2",
                    repl_load_workspace(workspace_in) == 2);

        /* Find each slot by name so we don't depend on readdir order. */
        int slot_a = -1, slot_b = -1;
        for (int s = 0; s < 8 /* MAX_USER_SCENES */; s++) {
            if (!repl_user_scene_slot_used(s)) continue;
            const char *name = repl_user_scene_name(s);
            if (name && strcmp(name, "SW Scene A") == 0) slot_a = s;
            if (name && strcmp(name, "SW Scene B") == 0) slot_b = s;
        }
        ASSERT_TRUE("cam-switch slot A found", slot_a >= 0);
        ASSERT_TRUE("cam-switch slot B found", slot_b >= 0);

        /* Activate B then A. Each load_scene_from_slot eases via
         * apply_example_block; force the next tick to snap by
         * overriding the per-ease decay to 0, then drive one tick.
         * After that the live camera == the slot's saved camera and
         * we can assert per-slot. */
        repl_load_user_scene_idx(slot_b);
        ASSERT_TRUE("cam-switch ease target armed after activating B",
                    glr_camera_target_active());
        glr_camera_set_target_decay(0.0f);
        glr_camera_tick();
        ASSERT_TRUE("cam-switch live dist matches B (~22.0)",
                    glr_camera().dist > 21.5f && glr_camera().dist < 22.5f);
        ASSERT_TRUE("cam-switch live rx matches B (~33.0)",
                    glr_camera().rx > 32.5f && glr_camera().rx < 33.5f);

        repl_load_user_scene_idx(slot_a);
        ASSERT_TRUE("cam-switch ease target armed after activating A",
                    glr_camera_target_active());
        glr_camera_set_target_decay(0.0f);
        glr_camera_tick();
        ASSERT_TRUE("cam-switch live dist matches A (~4.5)",
                    glr_camera().dist > 4.0f && glr_camera().dist < 5.0f);
        ASSERT_TRUE("cam-switch live rx matches A (~13.0)",
                    glr_camera().rx > 12.5f && glr_camera().rx < 13.5f);
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

        glr_ctrl_reset_all(); declare_test_vars();
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
                    strcmp(g_display_header[0], "void display(void) {") == 0);

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
        glr_ctrl_reset_all(); declare_test_vars();
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

    /* Float persistence should round-trip float32 values exactly while
     * emitting the *shortest* exact decimal (repl_format_source_float),
     * not bare %g's 6-digit loss nor %.9g's redundant padding (0.1234567f
     * exports as "0.1234567", not "0.123456702"). Pin both header
     * metadata and a generated numeric for-loop line. */
    {
        const char *precision_path = "/tmp/repl_core_precision_roundtrip.c";
        const float precise_x = 0.1234567f;
        const float loop_start = 0.1234567f;
        const float loop_end = 0.2234567f;
        const float loop_step = 0.0001234567f;
        char start_s[REPL_SOURCE_FLOAT_TEXT_MAX], end_s[REPL_SOURCE_FLOAT_TEXT_MAX];
        char step_s[REPL_SOURCE_FLOAT_TEXT_MAX], x_s[REPL_SOURCE_FLOAT_TEXT_MAX];
        char expected_var[128];
        char expected_loop[256];
        char buf[16384];

        glr_ctrl_reset_all(); declare_test_vars();
        int x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("precision test var exists", x_idx >= 0);
        g_predef_vars_mut[x_idx].value = precise_x;
        editor_feed_line("for(i, 0.1234567, 0.2234567, 0.0001234567) {");
        editor_feed_line("glVertex3f(i, 0, 0);");
        editor_feed_line("}");

        repl_export_save_output(precision_path, source_document_view(), NULL);
        read_text_file(precision_path, buf, sizeof(buf));

        repl_format_source_float(x_s, sizeof(x_s), precise_x);
        repl_format_source_float(start_s, sizeof(start_s), loop_start);
        repl_format_source_float(end_s, sizeof(end_s), loop_end);
        repl_format_source_float(step_s, sizeof(step_s), loop_step);

        char expected_decl[64];
        snprintf(expected_decl, sizeof(expected_decl), "static float x = %s;", x_s);
        ASSERT_TRUE("static float uses shortest exact float",
                    strstr(buf, expected_decl) != NULL);
        /* Guard against a regression to bare %g (lossy 6-digit). */
        ASSERT_TRUE("static float is not %g-truncated",
                    strstr(buf, "0.123457") == NULL);
        ASSERT_TRUE("no @var lines appear in exported buffer",
                    strstr(buf, "/* @var") == NULL);

        snprintf(expected_loop, sizeof(expected_loop),
                 "for (i = %s; i < %s; i += %sf) {",
                 start_s, end_s, step_s);
        ASSERT_TRUE("generated loop hoists loop var decl",
                    strstr(buf, "float i; /* repl-export-c89-loop-var */") != NULL);
        ASSERT_TRUE("generated loop uses shortest exact float",
                    strstr(buf, expected_loop) != NULL);

        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("precision export re-imports",
                    repl_export_load_from_file(precision_path, NULL) == 1);
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

    /* Non-finite predef values must export as valid C99 constants rather
     * than bare printf spellings like `nan`, and import must preserve
     * them through the round-trip. */
    {
        const char *nonfinite_path = "/tmp/repl_core_nonfinite_roundtrip.c";
        char buf[16384];
        int x_idx;
        int n_idx;

        glr_ctrl_reset_all(); declare_test_vars();
        x_idx = repl_eval_find_predef_var_idx("x");
        n_idx = repl_eval_find_predef_var_idx("n");
        ASSERT_TRUE("nonfinite test vars exist", x_idx >= 0 && n_idx >= 0);
        g_predef_vars_mut[x_idx].value = NAN;
        g_predef_vars_mut[n_idx].value = INFINITY;
        editor_feed_line("glPointSize(1);");

        repl_export_save_output(nonfinite_path, source_document_view(), NULL);
        read_text_file(nonfinite_path, buf, sizeof(buf));

        ASSERT_TRUE("nonfinite export has no @var lines",
                    strstr(buf, "/* @var") == NULL);
        ASSERT_TRUE("nonfinite globals use NAN",
                    strstr(buf, "static float x = NAN;") != NULL);
        ASSERT_TRUE("nonfinite globals use INFINITY",
                    strstr(buf, "static float n = INFINITY;") != NULL);

        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("nonfinite export re-imports",
                    repl_export_load_from_file(nonfinite_path, NULL) == 1);
        x_idx = repl_eval_find_predef_var_idx("x");
        n_idx = repl_eval_find_predef_var_idx("n");
        ASSERT_TRUE("re-imported NAN survives",
                    x_idx >= 0 && isnan(g_predef_vars[x_idx].value));
        ASSERT_TRUE("re-imported INFINITY survives",
                    n_idx >= 0 &&
                    isinf(g_predef_vars[n_idx].value) &&
                    g_predef_vars[n_idx].value > 0.0f);

        remove(nonfinite_path);
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

        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("truncated physical line fails import",
                    repl_export_load_from_file(trunc_path, NULL) == 0);
        ASSERT_INT("truncated physical line loads no commands",
                   repl_state_document_count(), 0);
        remove(trunc_path);
    }

    /* Scene/workspace callers must preserve save failures. Use a regular
     * file as the "workspace dir" so recursive directory creation rejects
     * the path before any success status can mask the error. */
    {
        const char *bad_workspace = "/tmp/repl_core_workspace_save_failure";
        const char *prev_workspace = "/tmp/repl_core_previous_workspace";
        FILE *f = fopen(bad_workspace, "w");
        ASSERT_TRUE("bad workspace fixture fopen", f != NULL);
        if (f)
            fclose(f);

        glr_ctrl_reset_all(); declare_test_vars();
        repl_set_workspace_dir(prev_workspace);
        repl_load_example(0);
        ASSERT_TRUE("promotion for failing workspace save",
                    repl_promote_example_if_needed() >= 0);

        ASSERT_INT("workspace save surfaces export failure",
                   repl_save_workspace(bad_workspace, NULL), -1);
        UiStatusState status = ui_state_status();
        ASSERT_TRUE("workspace save keeps create error status",
                    strstr(status.text, "cannot create") != NULL);
        ASSERT_TRUE("workspace save does not overwrite with success",
                    strstr(status.text, "Saved ") == NULL);
        ASSERT_TRUE("workspace save preserves previous workspace binding",
                    strcmp(repl_workspace_dir(), prev_workspace) == 0);

        unlink(bad_workspace);
        repl_set_workspace_dir("");
    }

    /* (F) Post-import host cursor publication.
     *
     * Phase 4 of docs/plans/done/edit-line-ownership.md moved cursor
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
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_TRIANGLES);");
        editor_feed_line("glVertex3f(0, 0, 0);");
        editor_feed_line("glVertex3f(1, 0, 0);");
        editor_feed_line("glVertex3f(0, 1, 0);");
        editor_feed_line("glEnd();");
        repl_export_save_output(import_cursor_path,
                                source_document_view(), NULL);

        /* (2) Reset to a clean slate (cursor → 0). */
        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_INT("pre-import cursor is 0",
                   editor_state_edit_line(), 0);

        /* (3) Re-import. */
        ASSERT_TRUE("import succeeded",
                    repl_export_load_from_file(import_cursor_path, NULL) == 1);

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
        glr_ctrl_reset_all(); declare_test_vars();
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

            glr_ctrl_reset_all(); declare_test_vars();
            ASSERT_TRUE("old-fmt import succeeds",
                        repl_export_load_from_file(cam_old_path, NULL) == 1);
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

    /* Audit #11/#12: numeric-ry camera blocks advance directly to the target
     * translate step. A later g_angle line is not part of the camera block and
     * must not be consumed by the camera parser. */
    {
        const ReplExportCameraBridge *bridge;

        glr_ctrl_reset_all(); declare_test_vars();
        bridge = repl_export_camera_bridge();
        ASSERT_TRUE("cam parser bridge installed", bridge != NULL);
        ASSERT_TRUE("cam parser bridge has reset",
                    bridge && bridge->reset_import != NULL);
        ASSERT_TRUE("cam parser bridge has consumer",
                    bridge && bridge->try_consume_import_line != NULL);

        if (bridge && bridge->reset_import && bridge->try_consume_import_line) {
            bridge->reset_import();

            ASSERT_INT("cam parser consumes dist line",
                       bridge->try_consume_import_line(
                           "glTranslatef(0.0000f, 0.0000f, -6.5000f);"),
                       1);
            ASSERT_INT("cam parser consumes rx line",
                       bridge->try_consume_import_line(
                           "glRotatef(25.0000f, 1.0f, 0.0f, 0.0f);"),
                       1);
            ASSERT_INT("cam parser consumes numeric ry line",
                       bridge->try_consume_import_line(
                           "glRotatef(55.0000f, 0.0f, 1.0f, 0.0f);"),
                       1);

            ASSERT_INT("cam parser does not consume g_angle after numeric ry",
                       bridge->try_consume_import_line(
                           "glRotatef(g_angle, 0.0f, 1.0f, 0.0f);"),
                       0);

            ASSERT_INT("cam parser still consumes target translate",
                       bridge->try_consume_import_line(
                           "glTranslatef(-0.5000f, -1.0000f, -1.5000f);"),
                       1);

            ASSERT_TRUE("cam parser keeps rx from parsed block",
                        fabsf(glr_camera().rx - 25.0f) < 1e-2f);
            ASSERT_TRUE("cam parser keeps ry from parsed block",
                        fabsf(glr_camera().ry - 55.0f) < 1e-2f);
            ASSERT_TRUE("cam parser keeps tx from parsed block",
                        fabsf(glr_camera().tx - 0.5f) < 1e-2f);
            ASSERT_TRUE("cam parser keeps ty from parsed block",
                        fabsf(glr_camera().ty - 1.0f) < 1e-2f);
            ASSERT_TRUE("cam parser keeps tz from parsed block",
                        fabsf(glr_camera().tz - 1.5f) < 1e-2f);

            bridge->reset_import();
        }
    }

    /* Audit #11/#12 prep: save -> load -> save idempotency. Audit
     * proposes removing the legacy state-3 path and the synthetic
     * g_angle line in cam_consume_example_block_now. Round-trip pose
     * tests above pin "load restores the camera"; an idempotency check
     * pins that the SAVE side hasn't drifted either — two consecutive
     * exports of the loaded state produce identical camera blocks.
     * A regression on either side (e.g., the audit's parser unification
     * subtly altering ry interpretation, or the formatter switching
     * between g_angle and numeric ry mid-refactor) yields a diff.
     *
     * Compare ONLY the camera region of the file (the rest depends on
     * derived state like timestamps / cfg defaults that can legitimately
     * change between runs; the camera block is the audit's surface). */
    {
        const char *path_a = "/tmp/repl_core_cam_idem_a.c";
        const char *path_b = "/tmp/repl_core_cam_idem_b.c";

        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(0,0,0);");
        glr_camera_set_orbit(12.5f, 67.5f);
        glr_camera_set_distance(9.25f);
        glr_camera_set_pan(0.5f, -1.5f, 2.25f);
        repl_export_save_output(path_a, source_document_view(), NULL);

        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("cam idem: load A succeeds",
                    repl_export_load_from_file(path_a, NULL) == 1);
        repl_export_save_output(path_b, source_document_view(), NULL);

        char buf_a[16384], buf_b[16384];
        read_text_file(path_a, buf_a, sizeof(buf_a));
        read_text_file(path_b, buf_b, sizeof(buf_b));

        /* Extract camera region: "void display(void) {\n" up to the
         * subsequent blank line. Both files share the same prologue
         * generator, so the boundary is stable. */
        const char *marker = "void display(void) {";
        const char *start_a = strstr(buf_a, marker);
        const char *start_b = strstr(buf_b, marker);
        ASSERT_TRUE("cam idem: A has display marker", start_a != NULL);
        ASSERT_TRUE("cam idem: B has display marker", start_b != NULL);
        if (start_a && start_b) {
            /* g_angle preamble must match — it's the seed for the
             * subsequent glRotatef(g_angle, ...) line. */
            const char *angle_a = strstr(buf_a, "static float g_angle = ");
            const char *angle_b = strstr(buf_b, "static float g_angle = ");
            ASSERT_TRUE("cam idem: A has g_angle preamble", angle_a != NULL);
            ASSERT_TRUE("cam idem: B has g_angle preamble", angle_b != NULL);
            if (angle_a && angle_b) {
                const char *eol_a = strchr(angle_a, '\n');
                const char *eol_b = strchr(angle_b, '\n');
                ASSERT_INT("cam idem: g_angle preamble length match",
                           (int)(eol_a - angle_a), (int)(eol_b - angle_b));
                ASSERT_INT("cam idem: g_angle preamble bytes match",
                           memcmp(angle_a, angle_b, (size_t)(eol_a - angle_a)),
                           0);
            }
            /* The 4-line camera block follows immediately after the
             * display brace. Compare a 256-byte window covering all
             * four lines. */
            ASSERT_INT("cam idem: 4-line camera block bytes match",
                       memcmp(start_a, start_b, 256), 0);
        }

        remove(path_a);
        remove(path_b);
    }

    /* Regression for #9: load-side ferror/fclose handling. Pointing
     * repl_export_load_from_file at a directory triggers a real fgets
     * read error (errno=EISDIR on macOS/Linux) — fopen succeeds, the
     * first fgets returns NULL with ferror set. Before the fix, the
     * read loop treated this as EOF and the function returned 1 as if
     * the load had succeeded. Post-fix the function reports failure. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        int rc = repl_export_load_from_file("/tmp", NULL);
        ASSERT_INT("load from a directory returns failure", rc, 0);

        /* Sibling sanity check: loading a missing file still fails too
         * (this path predates #9 — fopen returns NULL — but pin it so
         * a refactor doesn't silently flip the error contract). */
        rc = repl_export_load_from_file("/tmp/repl_core_io_nope_NOT_A_FILE.c", NULL);
        ASSERT_INT("load of missing file returns failure", rc, 0);
    }

    /* Regression for #9 review follow-up: a failed load must clear the
     * @cfg accumulator (and the pending scene-name / workspace-dir +
     * deferred-var list) before returning. parse_cfg populates the
     * accumulator per-line as it reads; if the load then aborts on a
     * read/close error or a truncated line, leaving the accumulator
     * populated lets a subsequent repl_export_apply_pending_cfg()
     * (called by example_loader.c on every example load) re-apply the
     * stale slugs. The /tmp directory test above doesn't catch this
     * because it errors before any line is parsed. */
    {
        const char *leak_path = "/tmp/repl_core_io_cfg_leak.c";

        /* Build a file: one valid @cfg line, then a too-long line that
         * fires the truncated_line bailout. MAX_LINE_LEN=256 — pad to
         * 600 chars so fgets fills its buffer without a newline. */
        FILE *f = fopen(leak_path, "w");
        ASSERT_TRUE("leak fixture opens for write", f != NULL);
        if (f) {
            fprintf(f, "// @cfg wireframe = 1\n");
            for (int i = 0; i < 600; i++) fputc('x', f);
            fputc('\n', f);
            fclose(f);
        }

        /* Force live wireframe to 0 first. File import now keeps @cfg in
         * ImportState until the load succeeds, so the partial read must not
         * mutate live config either. */
        glr_ctrl_reset_all(); declare_test_vars();
        glr_state_presentation_mut()->wireframe = WIREFRAME_OFF;

        int rc = repl_export_load_from_file(leak_path, NULL);
        ASSERT_INT("leak fixture import fails (truncated line)", rc, 0);
        ASSERT_INT("failed load did not apply @cfg before success",
                   glr_state_presentation().wireframe, WIREFRAME_OFF);

        /* Reset live wireframe back to 0 so a leaked accumulator
         * re-apply through repl_export_apply_pending_cfg becomes a
         * visible mutation we can assert against. */
        glr_state_presentation_mut()->wireframe = WIREFRAME_OFF;
        repl_export_apply_pending_cfg();
        ASSERT_INT("failed load did not leak @cfg into pending accumulator",
                   glr_state_presentation().wireframe, WIREFRAME_OFF);

        remove(leak_path);
    }

    /* Regression for #8: import_feed_one_line must surface
     * repl_load_apply_line's per-line load_err through stderr. Force
     * the cmd-store to capacity, then drive a load of a one-line
     * geometry file; pre-fix the user saw "Warning: could not parse
     * line: ..." with no clue why. Post-fix the line ends with
     * "(command store at capacity (max N))". */
    {
        const char *fail_path = "/tmp/repl_core_io_load_err.c";
        const char *stderr_path = "/tmp/repl_core_io_stderr.txt";

        FILE *f = fopen(fail_path, "w");
        ASSERT_TRUE("load_err fixture opens for write", f != NULL);
        if (f) {
            /* Snippet markers gate the geometry-line dispatch path
             * (import_process_line drops non-snippet body lines).
             * One line is enough to exercise the import_feed_one_line
             * warning emit. */
            fprintf(f, "// Snippet start\n");
            fprintf(f, "glVertex3f(1, 2, 3);\n");
            fprintf(f, "// Snippet end\n");
            fclose(f);
        }

        glr_ctrl_reset_all(); declare_test_vars();

        /* Force cmd-store at capacity so any insert fails. (Mirrors
         * the existing [P1 loader] test in test_repl_compile.c.) */
        ReplCommandStore store = repl_command_store_live();
        int capacity = repl_command_store_capacity(&store);
        int saved_count = *store.count;
        *store.count = capacity;

        /* Redirect fd 2 (stderr) to a temp file so we can inspect
         * the Warning the importer emits. Using dup2 on the bare fd
         * rather than freopen avoids FILE*-level buffering surprises
         * — fprintf(stderr, ...) inside the importer writes to fd 2
         * which we've now pointed at the capture file. */
        fflush(stderr);
        int capture_fd = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ASSERT_TRUE("stderr capture file opens", capture_fd >= 0);
        int saved_stderr_fd = dup(STDERR_FILENO);
        ASSERT_TRUE("stderr fd saved", saved_stderr_fd >= 0);
        if (capture_fd >= 0)
            dup2(capture_fd, STDERR_FILENO);

        (void)repl_export_load_from_file(fail_path, NULL);
        fflush(stderr);

        /* Restore fd 2 and close the capture descriptor. */
        if (saved_stderr_fd >= 0) {
            dup2(saved_stderr_fd, STDERR_FILENO);
            close(saved_stderr_fd);
        }
        if (capture_fd >= 0) close(capture_fd);
        *store.count = saved_count;

        /* The return value depends on whether anything loaded (here
         * nothing did because every insert failed capacity preflight);
         * the contract under test is that the per-line warning DID
         * fire and DID include the load_err detail. Check the
         * captured stderr text. */
        char captured[4096] = "";
        FILE *rd = fopen(stderr_path, "r");
        if (rd) {
            size_t nread = fread(captured, 1, sizeof(captured) - 1, rd);
            captured[nread] = '\0';
            fclose(rd);
        }
        ASSERT_TRUE("stderr captured Warning line",
                    strstr(captured, "Warning:") != NULL &&
                    strstr(captured, "could not parse line") != NULL);
        ASSERT_TRUE("stderr captured the load_err detail",
                    strstr(captured, "(command store at capacity") != NULL);

        remove(fail_path);
        remove(stderr_path);
    }

    /* Silent-drop coverage: an overflowed @declare name must WARN on import
     * rather than vanishing without a trace. Companion to the @func long-name
     * warning on this branch. Each case captures fd 2 and checks the
     * importer named the offending directive; (b) is a negative control
     * proving the checks aren't passing on stray stderr noise. */
    {
        char cap[4096];

        /* (a) @declare snippet marker with an over-long name. Pre-fix the
         * `len >= MAX` break dropped it (and any names after it) silently
         * without even bumping the warning counter. */
        glr_ctrl_reset_all();
        int rc_a = import_with_captured_stderr(
            "// Snippet start\n"
            "// @declare thisDeclNameTooLong\n"
            "glVertex3f(0, 0, 0);\n"
            "// Snippet end\n",
            cap, sizeof(cap));
        ASSERT_TRUE("long @declare import still succeeds", rc_a == 1);
        ASSERT_TRUE("long @declare name warns instead of dropping silently",
                    strstr(cap, "@declare name") != NULL &&
                    strstr(cap, "exceeds") != NULL);

        /* (b) negative control: a clean file emits no Warning lines. */
        glr_ctrl_reset_all();
        int rc_b = import_with_captured_stderr(
            "static float n = 5.0f;\n"
            "// Snippet start\n"
            "// @declare n\n"
            "glVertex3f(n, 0, 0);\n"
            "// Snippet end\n",
            cap, sizeof(cap));
        ASSERT_TRUE("clean import succeeds", rc_b == 1);
        ASSERT_TRUE("clean import emits no Warning",
                    strstr(cap, "Warning:") == NULL);
        ASSERT_TRUE("clean import summary shows no warning count",
                    strstr(cap, "warnings)") == NULL);
    }

    /* Regression for #83 in docs/plans/done/src-repl-code-smell-audit-2.md:
     * parse_snippet_declare used to use a `< 0` check on
     * repl_eval_declare_predef_var (which actually returns 1/0, not
     * idx-or-(-1)). On a reserved or otherwise-invalid name the
     * failure was silently swallowed: the bad name still landed in
     * the reconstructed CMD_VAR_DECLARE.payload.decl.names[] and the
     * canonical decl_line, leaving source/cmd-state out of sync
     * with the predef table. Post-fix the `!` check catches it and
     * the directive is dropped. */
    {
        const char *p83_path = "/tmp/repl_core_declare_reserved_name.c";
        FILE *f = fopen(p83_path, "w");
        ASSERT_TRUE("p83 fixture fopen", f != NULL);
        if (f) {
            fprintf(f, "static void render_repl_geometry(void) {\n");
            fprintf(f, "  // Snippet start\n");
            /* `PI` is in k_reserved_identifiers, so the underlying
             * _declare_predef_var_with_value returns -1 and the thin
             * wrapper returns 0. */
            fprintf(f, "  // @declare PI\n");
            fprintf(f, "  glVertex3f(1, 2, 3);\n");
            fprintf(f, "  // Snippet end\n");
            fprintf(f, "}\n");
            fclose(f);
        }

        glr_ctrl_reset_all();
        ASSERT_TRUE("p83 load completes",
                    repl_export_load_from_file(p83_path, NULL) == 1);
        ASSERT_INT("p83 reserved name is not registered as predef",
                   repl_eval_find_predef_var_idx("PI"), -1);
        int decl_with_pi = 0;
        for (int i = 0; i < repl_state_document_count(); i++) {
            const GLCmd *c = &repl_state_document_cmds()[i];
            if (!c->valid || c->type != CMD_VAR_DECLARE) continue;
            for (int n = 0; n < c->payload.decl.count; n++) {
                if (strcmp(c->payload.decl.names[n], "PI") == 0)
                    decl_with_pi++;
            }
        }
        ASSERT_INT("p83 reserved name dropped from CMD_VAR_DECLARE",
                   decl_with_pi, 0);
        remove(p83_path);
    }

    /* --- Trailing inline comments on commands round-trip --------------- */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        const char *cmt_path = "/tmp/repl_core_trailing_comment_output.c";

        editor_feed_line("glBegin(GL_LINES); // open the batch");
        editor_feed_line("glVertex3f(0, t, 0); // top, uses max and PI");
        editor_feed_line("glEnd(); // done");

        SourceTextView doc = source_document_view();
        /* The trailing comment survives commit, and the ';' sits BEFORE
         * it (the has-vars path used to emit `... // c;`). */
        ASSERT_TRUE("no-vars command keeps trailing comment on commit",
                    strstr(source_text_line(doc, 0), "glBegin(GL_LINES); // open the batch") != NULL);
        ASSERT_TRUE("has-vars command keeps trailing comment, ';' before it",
                    strstr(source_text_line(doc, 1), "); // top, uses max and PI") != NULL);
        ASSERT_TRUE("has-vars command has no ';' after the comment",
                    strstr(source_text_line(doc, 1), "PI;") == NULL);

        /* Reformat (Ctrl+R) preserves them. */
        repl_reformat_program();
        doc = source_document_view();
        ASSERT_TRUE("reformat keeps no-vars comment",
                    strstr(source_text_line(doc, 0), "// open the batch") != NULL);
        ASSERT_TRUE("reformat keeps has-vars comment, ';' before it",
                    strstr(source_text_line(doc, 1), "); // top, uses max and PI") != NULL);

        /* Export -> import round-trips them, and a builtin-looking word
         * inside the comment (`max`, `PI`) is NOT expr-translated. */
        repl_export_save_output(cmt_path, source_document_view(), NULL);
        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("trailing-comment file loads",
                    repl_export_load_from_file(cmt_path, NULL) == 1);
        doc = source_document_view();
        int begin_ok = 0, vertex_ok = 0;
        for (int i = 0; i < repl_state_document_count(); i++) {
            const char *ln = source_text_line(doc, i);
            if (!ln) continue;
            if (strstr(ln, "glBegin(GL_LINES)") && strstr(ln, "// open the batch"))
                begin_ok = 1;
            /* comment text intact: not rewritten to fmaxf / M_PI */
            if (strstr(ln, "glVertex3f") && strstr(ln, "// top, uses max and PI"))
                vertex_ok = 1;
        }
        ASSERT_TRUE("export/import preserves no-vars trailing comment", begin_ok);
        ASSERT_TRUE("export/import preserves has-vars comment verbatim", vertex_ok);
        remove(cmt_path);
    }

    /* --- C89 loop marker tokens in user comments must not be dropped --- */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        const char *marker_comment_path = "/tmp/repl_core_marker_comment_output.c";

        editor_feed_line("// repl-export-c89-loop-scope is user-authored text");
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glVertex3f(0, 0, 0); // repl-export-c89-loop-var is not generated here");
        editor_feed_line("glEnd();");

        repl_export_save_output(marker_comment_path, source_document_view(), NULL);
        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("marker-comment file loads",
                    repl_export_load_from_file(marker_comment_path, NULL) == 1);

        SourceTextView doc = source_document_view();
        int standalone_ok = 0;
        int trailing_ok = 0;
        for (int i = 0; i < repl_state_document_count(); i++) {
            const char *ln = source_text_line(doc, i);
            if (!ln) continue;
            if (strstr(ln, "// repl-export-c89-loop-scope is user-authored text"))
                standalone_ok = 1;
            if (strstr(ln, "glVertex3f") &&
                strstr(ln, "// repl-export-c89-loop-var is not generated here"))
                trailing_ok = 1;
        }
        ASSERT_TRUE("standalone marker-token comment survives import", standalone_ok);
        ASSERT_TRUE("trailing marker-token comment survives import", trailing_ok);
        remove(marker_comment_path);
    }

    /* --- Trailing garbage after ')' is rejected (regression) ---------- */
    {
        /* `glutSolidSphere(1,10,10)fafa` used to silently drop the `fafa`
         * and commit the command. Only whitespace, an optional ';', and a
         * `// ...` comment may follow a command's close paren. Covers
         * float-arg, enum, and no-arg `(...)` commands. */
        const char *garbage[] = {
            "glutSolidSphere(1, 10, 10)fafa", "glVertex3f(0, 0, 0)fafa",
            "glBegin(GL_LINES)xyz", "glPushMatrix()junk",
            "glColor3f(1, 0, 0) oops", "glRotatef(45, 0, 1, 0)1",
            NULL };
        for (int i = 0; garbage[i]; i++) {
            glr_ctrl_reset_all(); declare_test_vars();
            editor_input_set_text(garbage[i]);
            editor_handle_key(';', 0, 0);
            char msg[160];
            snprintf(msg, sizeof msg, "trailing garbage rejected: %s", garbage[i]);
            ASSERT_INT(msg, repl_state_document_count(), 0);
        }

        /* Valid trailing (nothing / ';' / `// ...` comment) still commits. */
        const char *ok[] = {
            "glVertex3f(0, 0, 0)", "glVertex3f(0, 0, 0) // note",
            "glPushMatrix() // save", "glutSolidSphere(1, 10, 10) // ball",
            NULL };
        for (int i = 0; ok[i]; i++) {
            glr_ctrl_reset_all(); declare_test_vars();
            editor_input_set_text(ok[i]);
            editor_handle_key(';', 0, 0);
            char msg[160];
            snprintf(msg, sizeof msg, "valid command commits: %s", ok[i]);
            ASSERT_INT(msg, repl_state_document_count(), 1);
        }
    }

    /* repl_active_scene_export_path: mirrors Save Scene's naming for any
     * extension (the .ply export uses it). Single source of truth shared with
     * repl_save_active_scene, so this also pins the no-drift invariant. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        const char *ws = "/tmp/repl_core_ply_ws";

        /* (1) No active named scene -> output.<ext>. */
        repl_set_workspace_dir(NULL);
        ASSERT_INT("export-path no-scene .ply",
                   strcmp(repl_active_scene_export_path("ply"), "output.ply"), 0);
        ASSERT_INT("export-path no-scene .c",
                   strcmp(repl_active_scene_export_path("c"), "output.c"), 0);

        /* Named active user scene (promote an example, then rename). */
        repl_load_example(0);
        int slot = repl_promote_example_if_needed();
        ASSERT_INT("promoted to active user scene", repl_active_user_scene(), slot);
        repl_user_scene_rename(slot, "My PLY Scene");   /* slug -> my_ply_scene */

        /* (2) Named, no workspace dir -> <slug>.<ext> in the cwd. */
        repl_set_workspace_dir(NULL);
        ASSERT_INT("export-path named no-ws .ply",
                   strcmp(repl_active_scene_export_path("ply"), "my_ply_scene.ply"), 0);
        ASSERT_INT("export-path named no-ws .c",
                   strcmp(repl_active_scene_export_path("c"), "my_ply_scene.c"), 0);

        /* (3) Named + workspace dir bound -> <dir>/<slug>.<ext>. */
        repl_set_workspace_dir(ws);
        char want_ply[256];
        snprintf(want_ply, sizeof want_ply, "%s/my_ply_scene.ply", ws);
        ASSERT_INT("export-path named + workspace .ply",
                   strcmp(repl_active_scene_export_path("ply"), want_ply), 0);

        /* (4) No-drift parity: the .c path the helper returns is exactly the
         *     file repl_save_active_scene writes. */
        char savedc[256];
        snprintf(savedc, sizeof savedc, "%s", repl_active_scene_export_path("c"));
        char want_c[256];
        snprintf(want_c, sizeof want_c, "%s/my_ply_scene.c", ws);
        ASSERT_INT("export-path .c matches workspace path",
                   strcmp(savedc, want_c), 0);
        unlink(savedc);
        repl_save_active_scene(NULL);
        ASSERT_INT("Save Scene wrote the helper's .c path",
                   access(savedc, F_OK), 0);
        unlink(savedc);
    }

    test_config_variants_export();
    test_workspace_header_budget_worst_case();
    test_import_robustness();
#ifdef GL_STUBS
    test_mesh_export_feedback();
#endif

    printf("repl_core_io: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}

static void test_import_robustness(void) {
    printf("--- import system robustness ---\n");

    const char *test_path = "/tmp/repl_import_robustness.c";
    FILE *f;

    /* 1. Coords fallback in eval_tess_brace_floats */
    f = fopen(test_path, "w");
    fprintf(f, "// Snippet start\n");
    fprintf(f, "  { _tn[0] = 0.5; _tn[1] = 0.6; _tn[3] = 0.7; }\n"); // fails parse_tess_brace_block because of _tn[3]
    fprintf(f, "  { _tc[0] = 0.1; _tc[1] = 0.2; _tc[2] = 0.3; _tc[4] = 0.4; }\n"); // fails parse_tess_brace_block because of _tc[4]
    fprintf(f, "  gluTessVertex(tess, _v->pos[0] = 1.1; _v->pos[1] = 2.2; _v->pos[3] = 3.3;, _v); // TessVertex\n");
    fprintf(f, "// Snippet end\n");
    fclose(f);

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load tess fallback file", repl_export_load_from_file(test_path, NULL) == 1);
    ASSERT_INT("loaded tess fallback count", repl_state_document_count(), 3);
    ASSERT_TRUE("normal loaded with fallback values",
                strstr(editor_buffer_line(0), "gluNormal(0.5, 0.6, 0.7);") != NULL);
    ASSERT_TRUE("color loaded with fallback values",
                strstr(editor_buffer_line(1), "gluColor(0.1, 0.2, 0.3, 0.4);") != NULL);
    ASSERT_TRUE("vertex loaded with fallback values",
                strstr(editor_buffer_line(2), "gluVertex(1.1, 2.2, 3.3);") != NULL);

    /* 2. Truncated Input: EOF mid-block */
    f = fopen(test_path, "w");
    fprintf(f, "// Snippet start\n");
    fprintf(f, "for(i, 0, 10) {\n");
    fprintf(f, "  glVertex3f(1, 0, 0);\n");
    // EOF without closing }
    fclose(f);

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load truncated file", repl_export_load_from_file(test_path, NULL) == 1);
    ASSERT_INT("truncated file count", repl_state_document_count(), 2);
    ASSERT_INT("indent cache updated safely", repl_state_document_cmds_mut()[1].type, CMD_VERTEX3F);

    /* 3. Mixed Line Endings (CRLF) */
    f = fopen(test_path, "wb");
    fprintf(f, "// Snippet start\r\n");
    fprintf(f, "glVertex3f(1, 2, 3);\r\n");
    fprintf(f, "glVertex3f(4, 5, 6);\r\n");
    fprintf(f, "// Snippet end\r\n");
    fclose(f);

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load CRLF file", repl_export_load_from_file(test_path, NULL) == 1);
    ASSERT_INT("CRLF count", repl_state_document_count(), 2);
    ASSERT_STR("CRLF line 0", editor_buffer_line(0), "  glVertex3f(1, 2, 3);");
    ASSERT_STR("CRLF line 1", editor_buffer_line(1), "  glVertex3f(4, 5, 6);");

    /* 4. Extremely Long Line (>1000 chars) */
    f = fopen(test_path, "w");
    fprintf(f, "// Snippet start\n");
    fprintf(f, "// ");
    for (int i = 0; i < 1100; i++) fprintf(f, "a");
    fprintf(f, "\n");
    fprintf(f, "glVertex3f(1, 2, 3);\n");
    fprintf(f, "// Snippet end\n");
    fclose(f);

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("long line fails gracefully", repl_export_load_from_file(test_path, NULL) == 0);

    /* 5. Embedded NUL byte in comment */
    f = fopen(test_path, "wb");
    fprintf(f, "// Snippet start\n");
    fprintf(f, "glVertex3f(1, 2, 3); // comment ");
    fputc('\0', f);
    fprintf(f, " with nul\n");
    fprintf(f, "// Snippet end\n");
    fclose(f);

    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load file with NUL fails gracefully", repl_export_load_from_file(test_path, NULL) == 0);
    ASSERT_INT("NUL file count", repl_state_document_count(), 0);

    /* 6. Multi-line C statements reassemble into one logical command.
     * The importer accumulates physical lines until brackets are
     * balanced and a terminator is seen, so hand-written / pasted C
     * whose calls span several lines still imports. */

    /* 6a. A plain call split across lines. */
    f = fopen(test_path, "w");
    fprintf(f, "// Snippet start\n");
    fprintf(f, "glBegin(GL_TRIANGLES);\n");
    fprintf(f, "glVertex3f(\n    1.0,\n    2.0,\n    3.0);\n");
    fprintf(f, "glEnd();\n");
    fprintf(f, "// Snippet end\n");
    fclose(f);
    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load split-call file", repl_export_load_from_file(test_path, NULL) == 1);
    ASSERT_INT("split-call count", repl_state_document_count(), 3);
    ASSERT_TRUE("split call reassembled",
                strstr(editor_buffer_line(1), "glVertex3f(1, 2, 3);") != NULL);

    /* 6b. Trailing inline comments on continuation lines are dropped,
     * but the comment on the terminating line is preserved. */
    f = fopen(test_path, "w");
    fprintf(f, "// Snippet start\n");
    fprintf(f, "glVertex3f(\n    1.0,  // x\n    2.0,  // y\n    3.0); // keep me\n");
    fprintf(f, "// Snippet end\n");
    fclose(f);
    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load commented-continuation file", repl_export_load_from_file(test_path, NULL) == 1);
    ASSERT_INT("commented-continuation count", repl_state_document_count(), 1);
    ASSERT_TRUE("continuation comments dropped, vertex intact",
                strstr(editor_buffer_line(0), "glVertex3f(1, 2, 3);") != NULL);
    ASSERT_TRUE("terminating-line comment preserved",
                strstr(editor_buffer_line(0), "// keep me") != NULL);
    ASSERT_TRUE("mid-statement comment text not bled in",
                strstr(editor_buffer_line(0), "// x") == NULL);

    /* 6c. A blank line inside an open statement does not split it. */
    f = fopen(test_path, "w");
    fprintf(f, "// Snippet start\n");
    fprintf(f, "glVertex3f(\n    1.0,\n\n    2.0, 3.0);\n");
    fprintf(f, "// Snippet end\n");
    fclose(f);
    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load blank-in-statement file", repl_export_load_from_file(test_path, NULL) == 1);
    ASSERT_INT("blank-in-statement count (no split, no stray blank)",
               repl_state_document_count(), 1);
    ASSERT_TRUE("blank-in-statement reassembled",
                strstr(editor_buffer_line(0), "glVertex3f(1, 2, 3);") != NULL);

    /* 6d. A split compound literal: the inner `{` is not a false
     * statement terminator while a paren is still open. */
    f = fopen(test_path, "w");
    fprintf(f, "// Snippet start\n");
    fprintf(f, "glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){\n    1, 0, 0, 1});\n");
    fprintf(f, "// Snippet end\n");
    fclose(f);
    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load split-compound-literal file", repl_export_load_from_file(test_path, NULL) == 1);
    ASSERT_INT("split-compound-literal count", repl_state_document_count(), 1);
    ASSERT_TRUE("split compound literal reassembled",
                strstr(editor_buffer_line(0), "(GLfloat[]){1, 0, 0, 1}") != NULL);

    /* 6e. Single-line statements still stay separate (regression guard
     * for the over-accumulation that the depth/terminator gate prevents). */
    f = fopen(test_path, "w");
    fprintf(f, "// Snippet start\n");
    fprintf(f, "glColor3f(1, 0, 0); // red\n");
    fprintf(f, "glVertex3f(0, 0, 0); // origin\n");
    fprintf(f, "// Snippet end\n");
    fclose(f);
    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load single-line file", repl_export_load_from_file(test_path, NULL) == 1);
    ASSERT_INT("single-line count stays 2", repl_state_document_count(), 2);
    ASSERT_TRUE("single line 0 keeps its own trailing comment",
                strstr(editor_buffer_line(0), "glColor3f(1, 0, 0); // red") != NULL);
    ASSERT_TRUE("single line 1 keeps its own trailing comment",
                strstr(editor_buffer_line(1), "glVertex3f(0, 0, 0); // origin") != NULL);

    /* 7. A brace inside a // comment in a function body must not move
     * function-nesting depth. Pre-fix, the stray `{` left func_depth
     * stuck above 0, so the next function definition was swallowed as
     * a body line (loading 4 commands with warnings instead of 6). */
    f = fopen(test_path, "w");
    fprintf(f, "static void func0(void) {\n");
    fprintf(f, "  glVertex3f(1, 1, 1); // brace { in comment\n");
    fprintf(f, "}\n");
    fprintf(f, "static void func1(void) {\n");
    fprintf(f, "  glVertex3f(2, 2, 2);\n");
    fprintf(f, "}\n");
    fclose(f);
    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load brace-in-comment funcbody file",
                repl_export_load_from_file(test_path, NULL) == 1);
    ASSERT_INT("comment brace did not swallow the next function",
               repl_state_document_count(), 6);
    ASSERT_TRUE("func0 defined", strstr(editor_buffer_line(0), "func0") != NULL);
    ASSERT_TRUE("func1 still defined (not swallowed)",
                strstr(editor_buffer_line(3), "func1") != NULL);

    /* 8. Exported files write user function definitions before draw_scene()
     * for C, then leave comment-only @func-body markers inside the snippet so
     * import can reconstruct the original source order. A user command before
     * the function must stay before that function after import. */
    f = fopen(test_path, "w");
    fprintf(f, "/* @workspace: REPL state (auto-saved) */\n");
    fprintf(f, "/* @func 0 = gear */\n");
    fprintf(f, "static void gear(void) {\n");
    fprintf(f, "  glVertex3f(1, 1, 1);\n");
    fprintf(f, "}\n");
    fprintf(f, "static void draw_scene(void) {\n");
    fprintf(f, "  // Snippet start\n");
    fprintf(f, "  glColor3f(0.1, 0.2, 0.3);\n");
    fprintf(f, "  /* @func-body 0 */\n");
    fprintf(f, "  glColor3f(0.4, 0.5, 0.6);\n");
    fprintf(f, "  // Snippet end\n");
    fprintf(f, "}\n");
    fclose(f);
    glr_ctrl_reset_all(); declare_test_vars();
    ASSERT_TRUE("load func-body marker file",
                repl_export_load_from_file(test_path, NULL) == 1);
    ASSERT_INT("func-body marker order count", repl_state_document_count(), 5);
    ASSERT_TRUE("pre-function command remains first",
                strstr(editor_buffer_line(0), "glColor3f(0.1, 0.2, 0.3);") != NULL);
    ASSERT_TRUE("function placed at marker",
                strstr(editor_buffer_line(1), "gear") != NULL);
    ASSERT_TRUE("post-function command follows function",
                strstr(editor_buffer_line(4), "glColor3f(0.4, 0.5, 0.6);") != NULL);

    remove(test_path);
}

void glr_actions_install_export_cfg_bridge(void);

static void test_config_variants_export(void) {
    printf("--- config-variants export tests ---\n");

    /* Install config bridge so config changes map to the export layer */
    glr_actions_install_export_cfg_bridge();

    const char *export_path = "/tmp/repl_config_variants_export.c";
    char buf[16384];

    /* We must have some document lines to trigger geometry rendering if we want to check vertex outline/point export */
    glr_ctrl_reset_all(); declare_test_vars();

    /* Test variant 1: MSAA enabled, Line smooth enabled, outlines enabled, points enabled, point attenuation enabled */
    glr_config_set(GLR_CONFIG_MSAA, 1);
    glr_config_set(GLR_CONFIG_LINE_SMOOTH, 1);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINE_STYLE, VERTEX_OUTLINE_STYLE_BOLD_INVERTED);
    glr_config_set(GLR_CONFIG_VERTEX_POINTS, 1);
    glr_config_set(GLR_CONFIG_POINT_ATTENUATION, 1);

    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(1, 2, 3);");
    editor_feed_line("glEnd();");

    /* Refresh and save */
    repl_refresh_render_state_strings();
    repl_export_save_output(export_path, source_document_view(), NULL);
    read_text_file(export_path, buf, sizeof(buf));

    ASSERT_TRUE("Variant 1: saved MSAA enabled line exists", strstr(buf, "glEnable(GL_MULTISAMPLE);") != NULL);
    ASSERT_TRUE("Variant 1: saved LINE_SMOOTH enabled line exists", strstr(buf, "glEnable(GL_LINE_SMOOTH);") != NULL);
    ASSERT_TRUE("Variant 1: saved point_attenuation enabled line exists", strstr(buf, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") != NULL);
    ASSERT_TRUE("Variant 1: saved vertex outlines pass is omitted while disabled in export", strstr(buf, "/* Vertex Outline Pass */") == NULL);
    ASSERT_TRUE("Variant 1: saved vertex points pass is omitted while disabled in export", strstr(buf, "/* Vertex Point Pass */") == NULL);
    ASSERT_TRUE("Variant 1: saved save/restore helpers omitted for single enabled pass", strstr(buf, "save_repl_vars") == NULL);

    /* Test variant 2: MSAA disabled, Line smooth disabled, outlines disabled, points disabled, point attenuation disabled */
    glr_config_set(GLR_CONFIG_MSAA, 0);
    glr_config_set(GLR_CONFIG_LINE_SMOOTH, 0);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 0);
    glr_config_set(GLR_CONFIG_VERTEX_POINTS, 0);
    glr_config_set(GLR_CONFIG_POINT_ATTENUATION, 0);

    /* Refresh and save */
    repl_refresh_render_state_strings();
    repl_export_save_output(export_path, source_document_view(), NULL);
    read_text_file(export_path, buf, sizeof(buf));

    ASSERT_TRUE("Variant 2: saved MSAA disabled line exists", strstr(buf, "glDisable(GL_MULTISAMPLE);") != NULL);
    ASSERT_TRUE("Variant 2: saved LINE_SMOOTH disabled line exists", strstr(buf, "glDisable(GL_LINE_SMOOTH);") != NULL);
    ASSERT_TRUE("Variant 2: saved point_attenuation disabled line is omitted", strstr(buf, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") == NULL);
    ASSERT_TRUE("Variant 2: saved vertex outlines pass is omitted", strstr(buf, "/* Vertex Outline Pass */") == NULL);
    ASSERT_TRUE("Variant 2: saved vertex points pass is omitted", strstr(buf, "/* Vertex Point Pass */") == NULL);

    remove(export_path);
}

/* Worst case for the shared workspace-header line budget: fill the
 * predefined-variable table to its user-declarable max, then confirm
 * every trailing @cfg line (vertex_outlines among them) still round-trips.
 * With the old 48-line budget the metadata lines crowded the late @cfg
 * lines off the end and toggles like Vertex outlines silently failed to save. */
static void test_workspace_header_budget_worst_case(void) {
    printf("--- workspace header budget (worst case: max vars + all cfg) ---\n");

    glr_actions_install_export_cfg_bridge();

    const char *export_path = "/tmp/repl_header_budget_worst_case.c";
    static char buf[32768];

    glr_ctrl_reset_all();

    /* Saturate the predef-var table (t is reserved, leaving
     * MAX_PREDEF_VARS - 1 user slots). */
    char err[128];
    int declared = 0;
    for (int i = 0; i < MAX_PREDEF_VARS + 2; i++) {
        char name[16];
        snprintf(name, sizeof(name), "v%d", i);
        if (repl_eval_declare_predef_var(name, err, sizeof(err)))
            declared++;
    }
    ASSERT_INT("filled the full user var table", declared, MAX_PREDEF_VARS - 1);

    /* Turn on toggles that live late in the @cfg emit order — these are
     * the first to be dropped when the budget is too small. */
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINE_STYLE, VERTEX_OUTLINE_STYLE_BOLD_INVERTED);
    glr_config_set(GLR_CONFIG_VERTEX_POINTS, 1);
    glr_config_set(GLR_CONFIG_POLY_HIGHLIGHT, 1);

    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(1, 2, 3);");
    editor_feed_line("glEnd();");

    repl_refresh_render_state_strings();
    repl_export_save_output(export_path, source_document_view(), NULL);
    read_text_file(export_path, buf, sizeof(buf));

    /* Every late @cfg line must survive even with the var table full. */
    ASSERT_TRUE("worst case: @cfg vertex_outlines present",
                strstr(buf, "/* @cfg vertex_outlines = 1 */") != NULL);
    ASSERT_TRUE("worst case: @cfg vertex_outline_style present",
                strstr(buf, "/* @cfg vertex_outline_style = "
                            "VERTEX_OUTLINE_STYLE_BOLD_INVERTED */") != NULL);
    ASSERT_TRUE("worst case: @cfg vertex_points present",
                strstr(buf, "/* @cfg vertex_points = 1 */") != NULL);
    ASSERT_TRUE("worst case: @cfg poly_highlight present",
                strstr(buf, "/* @cfg poly_highlight = 1 */") != NULL);
    /* "Audio" is the very last CFG_ITEM in emit order — its presence is
     * the strongest "nothing fell off the end" check. If this fails after
     * adding a config toggle, the emitted @cfg count outgrew a capacity:
     * raise REPL_CFG_MAX_ITEMS (cfg_baseline.h — the cfg bag silently
     * drops past it) AND MAX_WORKSPACE_HEADER_LINES (export_state.h, sized
     * from it; its STATIC_ASSERT in export_setup.c will also flag the shortfall). */
    ASSERT_TRUE("worst case: last @cfg (audio) present — if failing after a "
                "new config toggle, raise REPL_CFG_MAX_ITEMS (cfg_baseline.h) "
                "and MAX_WORKSPACE_HEADER_LINES (export_state.h)",
                strstr(buf, "/* @cfg audio = ") != NULL);

    /* Round-trip: a fresh state must come back with the toggle restored. */
    glr_ctrl_reset_all();
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 0);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINE_STYLE, VERTEX_OUTLINE_STYLE_DEFAULT);
    ASSERT_INT("worst case: outlines cleared before load",
               glr_config_get(GLR_CONFIG_VERTEX_OUTLINES), 0);
    ASSERT_INT("worst case: vertex outline style cleared before load",
               glr_config_get(GLR_CONFIG_VERTEX_OUTLINE_STYLE),
               VERTEX_OUTLINE_STYLE_DEFAULT);
    repl_export_load_from_file(export_path, NULL);
    ASSERT_INT("worst case: vertex_outlines restored on load",
               glr_config_get(GLR_CONFIG_VERTEX_OUTLINES), 1);
    ASSERT_INT("worst case: vertex_outline_style restored on load",
               glr_config_get(GLR_CONFIG_VERTEX_OUTLINE_STYLE),
               VERTEX_OUTLINE_STYLE_BOLD_INVERTED);

    remove(export_path);
}

#ifdef GL_STUBS
static void test_mesh_export_feedback(void) {
    printf("--- PLY feedback mesh exporter ---\n");

    /* 1. Test invalid arguments / empty program */
    glr_ctrl_reset_all();
    repl_eval_init_predef_vars();
    int r_empty = glr_export_mesh_ply(NULL, 0);
    ASSERT_INT("export fails on NULL path", r_empty, -1);

    r_empty = glr_export_mesh_ply("test_export_empty.ply", 0);
    ASSERT_INT("export fails on empty flat program", r_empty, -1);

    /* 2. Populate mock commands and export */
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");

    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("program is non-empty", repl_state_flat_program_count() > 0);

    int ntris = glr_export_mesh_ply("test_export.ply", 0);
    ASSERT_TRUE("export returns 0 or more triangles in stub mode", ntris >= 0);

    remove("test_export.ply");
}
#endif
