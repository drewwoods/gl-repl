/*
 * tests/test_scene_load.c - ReplSceneLoadOpts: the reader's explicit policy.
 *
 * Three things used to be implicit in src/repl/import.c and are now
 * parameters, one test group each:
 *
 *   format        canonical-document-order checking used to be decided by
 *                 sniffing ".glr" off the *diagnostic label*. Anything that
 *                 was not a filesystem path - every catalog entry - lost the
 *                 check with no diagnostic.
 *   policy        TOLERANT (warn per bad line, keep the rest) vs ATOMIC
 *                 (first bad line aborts, document restored). Both behaviors
 *                 already existed, in two different files.
 *   metadata      apply_cfg / camera_apply, so a watched external-editor
 *                 reload can replace program text and leave live cfg and
 *                 camera alone (docs/plans/active/BYOE.md, D3).
 *
 * The suite is deliberately weighted toward *failure*: every one of these
 * options only differs from the old behavior when something goes wrong, and
 * the failure paths are where the coverage was thinnest.
 */
#include <stdio.h>
#include <string.h>

#include "app/glr_camera.h"
#include "app/glr_config.h"
#include "app/glr_ctrl.h"
#include "repl/camera_header.h"
#include "repl/examples.h"
#include "repl/export.h"
#include "repl/scene_load.h"
#include "repl/state_owners.h"
#include "repl/state_views.h"
#include "source_document.h"
#include "support/camera_bridge_stub.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond)      TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, want)  TEST_ASSERT_INT(&g_harness, label, got, want)
#define ASSERT_STR(label, got, want)  TEST_ASSERT_STR(&g_harness, label, got, want)

/* A scene that loads cleanly under any policy. */
static const char *const k_good_scene[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "glBegin(GL_TRIANGLES);",
    "glVertex3f(0, 1, 0);",
    "glVertex3f(-1, -1, 0);",
    "glVertex3f(1, -1, 0);",
    "glEnd();",
    NULL
};

/* The shape that motivated one-scene-loader.md: two statements on one row.
 * The REPL is one command per line, so the tail of the row is rejected -
 * while every other row in the file is perfectly good. */
static const char *const k_scene_with_one_bad_row[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "glBegin(GL_TRIANGLES);",
    "glColor3f(1, 0, 0); glVertex3f(0, 1, 0);",
    "glVertex3f(-1, -1, 0);",
    "glVertex3f(1, -1, 0);",
    "glEnd();",
    NULL
};

/* A declaration after body code: REPL_DOC_ORDER_DECL_LATE. Legal in an
 * exported `.c`, rejected in the authored `.glr` format. */
static const char *const k_out_of_order_scene[] = {
    "glBegin(GL_POINTS);",
    "glVertex3f(0, 0, 0);",
    "glEnd();",
    "static float late;",
    NULL
};

static const char *const k_cfg_and_camera_scene[] = {
    "// @cfg grid = GRID_THEME_SOIL",
    "glTranslatef(0.0f, 0.0f, -12.0f);   // @camera dist",
    "glRotatef(41.0f, 1.0f, 0.0f, 0.0f);   // @camera rx",
    "glRotatef(52.0f, 0.0f, 1.0f, 0.0f);   // @camera ry",
    "glTranslatef(0.0f, 0.0f, 0.0f);   // @camera pan",
    "glBegin(GL_POINTS);",
    "glVertex3f(0, 0, 0);",
    "glEnd();",
    NULL
};

static ReplSceneLoadOpts opts_for(ReplExampleSourceFormat format,
                                  ReplSceneLoadPolicy policy) {
    ReplSceneLoadOpts opts;
    repl_scene_load_opts_init(&opts, format);
    opts.policy = policy;
    return opts;
}

/* Capture the live document as one flat string so a rollback can be compared
 * for real rather than by row count alone. */
static void capture_document(char *out, size_t out_sz) {
    SourceTextView text = source_document_view();
    int count = repl_state_document_count();
    size_t used = 0;

    out[0] = '\0';
    for (int i = 0; i < count; i++) {
        const char *line = source_text_line(text, i);
        int n = snprintf(out + used, out_sz - used, "%s\n", line ? line : "");
        if (n < 0 || (size_t)n >= out_sz - used)
            break;
        used += (size_t)n;
    }
}

/* ----- format ----------------------------------------------------------- */

/* The dropout this fixes: a catalog entry's label is its display name, so the
 * suffix sniff answered "not .glr" and canonical-order validation silently
 * did not run. */
static void test_format_drives_order_check(void) {
    printf("--- explicit format drives canonical-order checking ---\n");

    glr_ctrl_reset_all();
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_TOLERANT);
        ASSERT_INT("a non-path label declared .glr still gets order checking",
                   repl_scene_load_from_lines(k_out_of_order_scene,
                                              "Torus Knot", &opts, NULL), 0);
    }

    glr_ctrl_reset_all();
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_C,
                                          REPL_SCENE_LOAD_POLICY_TOLERANT);
        ASSERT_TRUE("the same lines declared exported-C are exempt",
                    repl_scene_load_from_lines(k_out_of_order_scene,
                                               "Torus Knot", &opts, NULL) != 0);
    }

    ASSERT_INT("repl_scene_format_from_path: .glr",
               (int)repl_scene_format_from_path("scenes/a.glr"),
               (int)REPL_EXAMPLE_SOURCE_GLR);
    ASSERT_INT("repl_scene_format_from_path: .c",
               (int)repl_scene_format_from_path("out.c"),
               (int)REPL_EXAMPLE_SOURCE_C);
    ASSERT_INT("repl_scene_format_from_path: a bare name is not .glr",
               (int)repl_scene_format_from_path("Torus Knot"),
               (int)REPL_EXAMPLE_SOURCE_C);
    ASSERT_INT("repl_scene_format_from_path: NULL is not .glr",
               (int)repl_scene_format_from_path(NULL),
               (int)REPL_EXAMPLE_SOURCE_C);
    /* ".glr" is four characters; the old sniff required len > 4, and that is
     * kept deliberately - a file called exactly ".glr" is a dotfile, not a
     * scene named the empty string. */
    ASSERT_INT("repl_scene_format_from_path: the bare suffix is a dotfile",
               (int)repl_scene_format_from_path(".glr"),
               (int)REPL_EXAMPLE_SOURCE_C);
}

/* The wrappers must keep behaving exactly as they did before the format
 * became explicit, or every existing caller changed meaning. */
static void test_default_wrapper_keeps_suffix_behavior(void) {
    printf("--- repl_export_load_from_lines still sniffs its label ---\n");

    glr_ctrl_reset_all();
    ASSERT_INT("a .glr-suffixed label is order-checked",
               repl_export_load_from_lines(k_out_of_order_scene,
                                           "scene.glr", NULL), 0);

    glr_ctrl_reset_all();
    ASSERT_TRUE("a label with no .glr suffix is not",
                repl_export_load_from_lines(k_out_of_order_scene,
                                            "scene.c", NULL) != 0);
}

/* ----- policy ----------------------------------------------------------- */

static void test_tolerant_keeps_the_good_rows(void) {
    printf("--- TOLERANT: one bad row costs one row ---\n");

    glr_ctrl_reset_all();
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_TOLERANT);
        ASSERT_TRUE("the load succeeds",
                    repl_scene_load_from_lines(k_scene_with_one_bad_row,
                                               "scene.glr", &opts, NULL) != 0);
        /* Six of the seven rows survive; the doubled row is dropped whole. */
        ASSERT_INT("every other row is in the document",
                   repl_state_document_count(), 6);
    }
}

static void test_atomic_rejects_the_whole_file(void) {
    printf("--- ATOMIC: one bad row costs the file ---\n");

    glr_ctrl_reset_all();
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        ASSERT_INT("the load fails",
                   repl_scene_load_from_lines(k_scene_with_one_bad_row,
                                              "scene.glr", &opts, NULL), 0);
        ASSERT_INT("and nothing is left behind",
                   repl_state_document_count(), 0);
    }
}

/* The rollback proper: an ATOMIC load that fails must leave the document it
 * found - not an empty one, and not a half-loaded one. */
static void test_atomic_restores_the_previous_document(void) {
    char before[4096];
    char after[4096];

    printf("--- ATOMIC: a failed load leaves no trace ---\n");

    glr_ctrl_reset_all();
    ASSERT_TRUE("seed a good document",
                repl_export_load_from_lines(k_good_scene, "seed.glr", NULL) != 0);
    capture_document(before, sizeof(before));
    ASSERT_INT("the seed loaded", repl_state_document_count(), 7);

    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        ASSERT_INT("the second, broken load fails",
                   repl_scene_load_from_lines(k_scene_with_one_bad_row,
                                              "broken.glr", &opts, NULL), 0);
    }

    capture_document(after, sizeof(after));
    ASSERT_INT("the row count is unchanged", repl_state_document_count(), 7);
    ASSERT_STR("the document text is unchanged", after, before);
}

/* An order violation is a whole-file verdict rather than a per-line one, so it
 * takes a different exit inside import_finish_load. It must roll back too. */
static void test_atomic_restores_after_an_order_violation(void) {
    char before[4096];
    char after[4096];

    printf("--- ATOMIC: an order violation rolls back too ---\n");

    glr_ctrl_reset_all();
    (void)repl_export_load_from_lines(k_good_scene, "seed.glr", NULL);
    capture_document(before, sizeof(before));

    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        ASSERT_INT("the out-of-order load fails",
                   repl_scene_load_from_lines(k_out_of_order_scene,
                                              "late.glr", &opts, NULL), 0);
    }

    capture_document(after, sizeof(after));
    ASSERT_STR("the seed document survives", after, before);
}

/* TOLERANT keeps its historical behavior on the same input: it does NOT roll
 * back, because there is nothing transactional about it. Asserting this stops
 * a future refactor from quietly making every load atomic. */
static void test_tolerant_does_not_roll_back(void) {
    printf("--- TOLERANT does not roll back ---\n");

    glr_ctrl_reset_all();
    (void)repl_export_load_from_lines(k_good_scene, "seed.glr", NULL);
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_TOLERANT);
        (void)repl_scene_load_from_lines(k_scene_with_one_bad_row,
                                         "broken.glr", &opts, NULL);
    }
    ASSERT_TRUE("the second load appended to the first",
                repl_state_document_count() > 7);
}

/* A good file must be unaffected by the policy: ATOMIC is only a failure
 * policy, and a scene that loads must load identically either way. */
static void test_atomic_and_tolerant_agree_on_a_good_file(void) {
    char tolerant_doc[4096];
    char atomic_doc[4096];

    printf("--- the policies agree on a file that loads ---\n");

    glr_ctrl_reset_all();
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_TOLERANT);
        ASSERT_TRUE("TOLERANT loads it",
                    repl_scene_load_from_lines(k_good_scene, "s.glr",
                                               &opts, NULL) != 0);
    }
    capture_document(tolerant_doc, sizeof(tolerant_doc));

    glr_ctrl_reset_all();
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        ASSERT_TRUE("ATOMIC loads it",
                    repl_scene_load_from_lines(k_good_scene, "s.glr",
                                               &opts, NULL) != 0);
    }
    capture_document(atomic_doc, sizeof(atomic_doc));

    ASSERT_STR("byte-identical documents", atomic_doc, tolerant_doc);
}

/* ----- metadata policy (BYOE D3) ---------------------------------------- */

static void test_apply_cfg_off_leaves_live_config(void) {
    int before;

    printf("--- apply_cfg = 0 leaves live cfg alone ---\n");

    glr_ctrl_reset_all();
    before = glr_config_get(GLR_CONFIG_GRID_THEME);

    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.apply_cfg = 0;
        opts.camera_apply = REPL_CAMERA_APPLY_NONE;
        ASSERT_TRUE("the scene loads",
                    repl_scene_load_from_lines(k_cfg_and_camera_scene,
                                               "watched.glr", &opts, NULL) != 0);
    }
    ASSERT_INT("its @cfg grid row did not reach live state",
               glr_config_get(GLR_CONFIG_GRID_THEME), before);

    /* And the bag was still *cleared*: a following default load must not
     * inherit the skipped scene's accumulator. That is the trap D3 names -
     * skipping the whole apply-and-reset call leaves it behind. */
    glr_ctrl_reset_all();
    before = glr_config_get(GLR_CONFIG_GRID_THEME);
    ASSERT_TRUE("a plain scene with no @cfg loads",
                repl_export_load_from_lines(k_good_scene, "plain.glr",
                                            NULL) != 0);
    ASSERT_INT("and inherits no stale @cfg from the skipped load",
               glr_config_get(GLR_CONFIG_GRID_THEME), before);
}

static void test_apply_cfg_on_is_still_the_default(void) {
    printf("--- apply_cfg defaults to on ---\n");

    glr_ctrl_reset_all();
    ASSERT_TRUE("the scene loads",
                repl_export_load_from_lines(k_cfg_and_camera_scene,
                                            "normal.glr", NULL) != 0);
    ASSERT_INT("its @cfg grid row was applied",
               glr_config_get(GLR_CONFIG_GRID_THEME),
               (int)GRID_THEME_SOIL);
}

/* The trap D3 spells out: glr_camera_export.c's apply_pose snaps for anything
 * that is not EXAMPLE, so a NONE that reached a bridge would snap the camera
 * and violate the rule it exists to state. The refusal lives in
 * repl_camera_header_finish, so the bridge is never called at all. */
static void test_camera_apply_none_never_reaches_the_bridge(void) {
    printf("--- camera_apply = NONE makes no bridge call ---\n");

    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.apply_cfg = 0;
        opts.camera_apply = REPL_CAMERA_APPLY_NONE;
        ASSERT_TRUE("the scene loads",
                    repl_scene_load_from_lines(k_cfg_and_camera_scene,
                                               "watched.glr", &opts, NULL) != 0);
    }
    ASSERT_INT("apply_pose was never called",
               g_camera_bridge_stub.apply_count, 0);

    /* Same file, default mode: proof the header itself is well-formed and it
     * is the mode - not a malformed pose - that suppressed the call. */
    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);
    ASSERT_TRUE("the same scene loads under the default mode",
                repl_export_load_from_lines(k_cfg_and_camera_scene,
                                            "normal.glr", NULL) != 0);
    ASSERT_INT("apply_pose was called once",
               g_camera_bridge_stub.apply_count, 1);
    ASSERT_INT("with IMPORT", (int)g_camera_bridge_stub.mode,
               (int)REPL_CAMERA_APPLY_IMPORT);
}

int main(void) {
    printf("=== scene load options ===\n");
    test_format_drives_order_check();
    test_default_wrapper_keeps_suffix_behavior();
    test_tolerant_keeps_the_good_rows();
    test_atomic_rejects_the_whole_file();
    test_atomic_restores_the_previous_document();
    test_atomic_restores_after_an_order_violation();
    test_tolerant_does_not_roll_back();
    test_atomic_and_tolerant_agree_on_a_good_file();
    test_apply_cfg_off_leaves_live_config();
    test_apply_cfg_on_is_still_the_default();
    test_camera_apply_none_never_reaches_the_bridge();
    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "scene_load");
}
