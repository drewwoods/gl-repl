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
 *
 * The second half covers the *binding* those options are for (BYOE D1): which
 * file the active scene lives in, that Ctrl+S writes that file, and that
 * re-reading it reuses the active slot instead of allocating a ninth scene.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/glr_config.h"
#include "app/glr_ctrl.h"
#include "repl/bootstrap.h"
#include "repl/camera_header.h"
#include "repl/examples.h"
#include "repl/example_loader.h"
#include "repl/export.h"
#include "repl/scene_load.h"
#include "repl/scenes.h"
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
    "display() {",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "glBegin(GL_TRIANGLES);",
    "glVertex3f(0, 1, 0);",
    "glVertex3f(-1, -1, 0);",
    "glVertex3f(1, -1, 0);",
    "glEnd();",
    "}",
    NULL
};

static const char *const k_other_scene[] = {
    "display() {",
    "glClearColor(0.2, 0.2, 0.2, 1.0);",
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "glBegin(GL_POINTS);",
    "glVertex3f(9, 9, 9);",
    "glEnd();",
    "}",
    NULL
};

/* The shape that motivated one-scene-loader.md: two statements on one row.
 * The REPL is one command per line, so the tail of the row is rejected -
 * while every other row in the file is perfectly good. */
static const char *const k_scene_with_one_bad_row[] = {
    "display() {",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "glBegin(GL_TRIANGLES);",
    "glColor3f(1, 0, 0); glVertex3f(0, 1, 0);",
    "glVertex3f(-1, -1, 0);",
    "glVertex3f(1, -1, 0);",
    "glEnd();",
    "}",
    NULL
};

/* A declaration after body code: REPL_DOC_ORDER_DECL_LATE. Legal in an
 * exported `.c`, rejected in the authored `.glr` format. */
static const char *const k_out_of_order_scene[] = {
    "display() {",
    "glBegin(GL_POINTS);",
    "glVertex3f(0, 0, 0);",
    "glEnd();",
    "static float late;",
    "}",
    NULL
};

/* The exported-C comparison intentionally has no .glr frame syntax. */
static const char *const k_out_of_order_c[] = {
    "glBegin(GL_POINTS);",
    "glVertex3f(0, 0, 0);",
    "glEnd();",
    "static float late;",
    NULL
};

static const char *const k_cfg_and_camera_scene[] = {
    "// @cfg grid = GRID_THEME_SOIL",
    "display() {",
    "glTranslatef(0.0f, 0.0f, -12.0f);   // @camera dist",
    "glRotatef(41.0f, 1.0f, 0.0f, 0.0f);   // @camera rx",
    "glRotatef(52.0f, 0.0f, 1.0f, 0.0f);   // @camera ry",
    "glTranslatef(0.0f, 0.0f, 0.0f);   // @camera pan",
    "glBegin(GL_POINTS);",
    "glVertex3f(0, 0, 0);",
    "glEnd();",
    "}",
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
                    repl_scene_load_from_lines(k_out_of_order_c,
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
                repl_export_load_from_lines(k_out_of_order_c,
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

/* ----- source-file binding (BYOE D1) ------------------------------------ */

#define WATCH_PATH "/tmp/gl_repl_scene_load_bound.glr"
#define OTHER_PATH "/tmp/gl_repl_scene_load_other.glr"

static void write_file(const char *path, const char *const *lines) {
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; lines && lines[i]; i++)
        fprintf(f, "%s\n", lines[i]);
    fclose(f);
}

static int file_contains(const char *path, const char *needle) {
    char buf[8192];
    size_t n;
    FILE *f = fopen(path, "r");

    if (!f)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static ReplSceneLoadOpts watch_opts(void) {
    ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                      REPL_SCENE_LOAD_POLICY_ATOMIC);
    opts.apply_cfg    = 0;
    opts.camera_apply = REPL_CAMERA_APPLY_NONE;
    return opts;
}

static void test_cli_positional_binds_the_file(void) {
    printf("--- the positional CLI file becomes the scene's home ---\n");

    write_file(WATCH_PATH, k_good_scene);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);

    ASSERT_TRUE("a slot is active", repl_active_user_scene() >= 0);
    ASSERT_TRUE("the slot names the file",
                repl_active_scene_source_path() != NULL);
    /* Absolute, so a later chdir - or a relative argument like `./x.glr` -
     * cannot make the binding name a different file than the one loaded. */
    ASSERT_TRUE("the bound path is absolute",
                repl_active_scene_source_path() &&
                repl_active_scene_source_path()[0] == '/');
    ASSERT_STR("and it is what --watch would follow",
               repl_active_scene_bound_path(),
               repl_active_scene_source_path());
}

/* The gap D1 closes: without this, `./gl-repl foo.glr` saves to `<slug>.c` in
 * the cwd, so the watched file never changes and the round trip is broken in
 * both directions at once. */
static void test_ctrl_s_writes_the_bound_file(void) {
    printf("--- Ctrl+S writes the bound file ---\n");

    write_file(WATCH_PATH, k_good_scene);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);

    ASSERT_TRUE("the scene loaded", repl_state_document_count() > 0);
    ASSERT_TRUE("save succeeds", repl_save_active_scene(NULL) != 0);
    ASSERT_TRUE("the bound file carries the document",
                file_contains(WATCH_PATH, "glVertex3f"));
    /* The writer is chosen from the binding's own extension: a `.glr` must not
     * come back as a standalone C program, or the catalog can no longer read
     * the file it just rewrote. */
    ASSERT_TRUE("a .glr binding got the .glr writer, not exported C",
                !file_contains(WATCH_PATH, "void display(void)"));
}

static void test_example_has_no_binding(void) {
    printf("--- a built-in example is not bound to anything ---\n");

    glr_ctrl_reset_all();
    (void)repl_load_example(0);
    ASSERT_TRUE("no source path", repl_active_scene_source_path() == NULL);
    /* A compiled-in example has no file at all; a runtime `--examples-dir`
     * entry would resolve through glr_origin_path instead, which this build's
     * catalog does not have. */
    ASSERT_TRUE("and nothing for --watch to follow",
                repl_active_scene_bound_path() == NULL);
}

/* A save is not a new scene. Allocating a slot per reload would exhaust all
 * eight in eight keystrokes and then fail with ERR_NO_SLOT - the reason this
 * is not repl_load_scene_as_new_slot. */
static void test_reload_reuses_the_active_slot(void) {
    static const char *const revised[] = {
        "display() {",
        "glClearColor(0.2, 0.2, 0.2, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(9, 9, 9);",
        "glEnd();",
        "}",
        NULL
    };
    int slot;
    int count;

    printf("--- reload reuses the active slot ---\n");

    write_file(WATCH_PATH, k_good_scene);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    slot = repl_active_user_scene();
    count = repl_user_scene_count();

    write_file(WATCH_PATH, revised);
    for (int i = 0; i < 12; i++) {
        ReplSceneLoadOpts opts = watch_opts();
        ASSERT_TRUE("each reload succeeds",
                    repl_reload_active_scene_from_path(WATCH_PATH, &opts) != 0);
    }

    ASSERT_INT("twelve reloads created no new scenes",
               repl_user_scene_count(), count);
    ASSERT_INT("and stayed in the same slot", repl_active_user_scene(), slot);
    ASSERT_INT("the document is the revised one",
               repl_state_document_count(), 5);

    /* The slot's stored copy has to move with the live document, or the next
     * scene switch writes the pre-reload snapshot back over it. */
    (void)repl_scenes_create_empty_user_scene();
    (void)repl_load_user_scene_idx(slot);
    ASSERT_INT("switching away and back keeps the reloaded document",
               repl_state_document_count(), 5);
}

static void test_failed_reload_leaves_no_trace(void) {
    char before[4096];
    char after[4096];
    int slot;

    printf("--- a failed reload leaves no trace ---\n");

    write_file(WATCH_PATH, k_good_scene);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    slot = repl_active_user_scene();
    capture_document(before, sizeof(before));

    write_file(WATCH_PATH, k_scene_with_one_bad_row);
    {
        ReplSceneLoadOpts opts = watch_opts();
        ASSERT_INT("the reload fails",
                   repl_reload_active_scene_from_path(WATCH_PATH, &opts), 0);
    }

    capture_document(after, sizeof(after));
    ASSERT_STR("the document is untouched", after, before);
    ASSERT_INT("the slot is unchanged", repl_active_user_scene(), slot);
    ASSERT_STR("and so is the binding",
               repl_active_scene_bound_path(),
               repl_active_scene_source_path());
}

/* D3's no-rebind rule. An external file must not be able to rename the slot
 * or retarget the very path being watched. */
static void test_reload_cannot_rebind_the_slot(void) {
    static const char *const renaming[] = {
        "// @scene-name Hijacked",
        "// @workspace-dir /tmp/gl_repl_scene_load_hijack",
        "display() {",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        "}",
        NULL
    };
    char name_before[USER_SCENE_NAME_MAX];
    char path_before[1024];
    int slot;

    printf("--- a watched reload cannot rename or retarget the slot ---\n");

    write_file(WATCH_PATH, k_good_scene);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    slot = repl_active_user_scene();
    snprintf(name_before, sizeof(name_before), "%s",
             repl_user_scene_name(slot) ? repl_user_scene_name(slot) : "");
    snprintf(path_before, sizeof(path_before), "%s",
             repl_active_scene_source_path() ? repl_active_scene_source_path()
                                             : "");

    write_file(WATCH_PATH, renaming);
    {
        ReplSceneLoadOpts opts = watch_opts();
        ASSERT_TRUE("the reload succeeds",
                    repl_reload_active_scene_from_path(WATCH_PATH, &opts) != 0);
    }

    ASSERT_STR("the scene name is unchanged",
               repl_user_scene_name(slot) ? repl_user_scene_name(slot) : "",
               name_before);
    ASSERT_STR("the bound path is unchanged",
               repl_active_scene_source_path()
                   ? repl_active_scene_source_path() : "",
               path_before);
    ASSERT_STR("no workspace was bound", repl_workspace_dir(), "");
    ASSERT_TRUE("the new program did land",
                repl_state_document_count() == 3);
}

/* Loading a *different* file as a new scene is still a new scene, and it
 * binds to its own path - proof the reload path is the special case, not the
 * general one. */
static void test_open_binds_its_own_path(void) {
    ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
    int slot;

    printf("--- File -> Open binds the file it opened ---\n");

    write_file(WATCH_PATH, k_good_scene);
    write_file(OTHER_PATH, k_good_scene);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);

    slot = repl_load_scene_as_new_slot(OTHER_PATH, &reason);
    ASSERT_TRUE("the open succeeded", slot >= 0);
    ASSERT_TRUE("the new slot names the file it came from",
                repl_active_scene_source_path() &&
                strstr(repl_active_scene_source_path(),
                       "gl_repl_scene_load_other.glr") != NULL);
    ASSERT_INT("and it is a different slot than the CLI file's",
               slot != 0, 1);
}

static void test_open_path_wins_over_an_existing_workspace(void) {
    ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
    const char *source;

    printf("--- File -> Open overrides an older workspace binding ---\n");

    write_file(WATCH_PATH, k_good_scene);
    write_file(OTHER_PATH, k_other_scene);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    repl_set_workspace_dir("/tmp/gl_repl_scene_load_older_workspace");

    ASSERT_TRUE("the open succeeded",
                repl_load_scene_as_new_slot(OTHER_PATH, &reason) >= 0);
    source = repl_active_scene_source_path();
    ASSERT_TRUE("the opened slot retained its own source path", source != NULL);
    ASSERT_STR("watch follows the later per-slot choice",
               repl_active_scene_bound_path(), source);

    ASSERT_TRUE("Ctrl+S saves the opened file",
                repl_save_active_scene(NULL) != 0);
    ASSERT_TRUE("the opened .glr received its scene",
                file_contains(OTHER_PATH, "glVertex3f(9, 9, 9);"));
    ASSERT_STR("saving the file does not silently adopt it into the workspace",
               repl_active_scene_bound_path(), source);

    repl_set_workspace_dir(NULL);
}

/* ----- physical -> document row map (BYOE stage 2.5) ---------------------- */

/* Everything that pushes a physical row away from its document row: a consumed
 * directive, a declaration, a blank and a comment, all before the geometry. */
static const char *const k_mapped_scene[] = {
    "// @scene-name Mapped",           /* 1: consumed, no document row */
    "static float wob;",               /* 2: doc 0 */
    "display() {",                     /* 3: consumed, no document row */
    "",                                /* 4: doc 1 */
    "// a note",                       /* 5: doc 2 */
    "glClearColor(0, 0, 0, 1);",       /* 6: doc 3 */
    "glClear(GL_COLOR_BUFFER_BIT);",   /* 7: doc 4 */
    "glBegin(GL_POINTS);",             /* 8: doc 5 */
    "glVertex3f(0, 0, 0);",            /* 9: doc 6 */
    "glEnd();",                        /* 10: doc 7 */
    "}",                               /* 11: consumed, no document row */
    NULL
};

#define ROW_MAP_CAP 512   /* an exported .c runs to hundreds of physical rows */

/* Document rows carry the indentation the reformatter derived from block
 * scope, so a body row reads "  glEnd();". The tests below are about which row
 * landed where, not about how it is spelled. */
static const char *unindented(int doc_row) {
    const char *p = editor_buffer_line(doc_row);
    while (p && (*p == ' ' || *p == '\t'))
        p++;
    return p ? p : "";
}

typedef struct {
    ReplSceneRowMap map;
    int             storage[ROW_MAP_CAP];
} TestRowMap;

static void row_map_reset(TestRowMap *t, unsigned long nonce) {
    repl_scene_row_map_init(&t->map, t->storage, ROW_MAP_CAP, nonce);
}

/* 1-based physical row -> document row, or REPL_ROW_MAP_NONE. */
static int mapped(const TestRowMap *t, int physical_row) {
    if (physical_row < 1 || physical_row > t->map.cap)
        return REPL_ROW_MAP_NONE;
    return t->map.doc_row[physical_row - 1];
}

static void test_row_map_survives_headers_and_directives(void) {
    TestRowMap t;

    printf("--- the row map counts document rows, not file rows ---\n");

    glr_ctrl_reset_all();
    row_map_reset(&t, 0);
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.row_map = &t.map;
        ASSERT_INT("the file loads",
                   repl_scene_load_from_lines(k_mapped_scene, "mapped.glr",
                                              &opts, NULL), 1);
    }
    ASSERT_INT("eight document rows", repl_state_document_count(), 8);
    /* The directive is the whole point: without a map, "physical 5" would be
     * read as document 5 and land two rows past the clear colour. */
    ASSERT_INT("the consumed directive maps to nothing", mapped(&t, 1),
               REPL_ROW_MAP_NONE);
    ASSERT_INT("the declaration is document row 0", mapped(&t, 2), 0);
    ASSERT_INT("the display opener maps to nothing", mapped(&t, 3),
               REPL_ROW_MAP_NONE);
    ASSERT_INT("the blank is a document row too", mapped(&t, 4), 1);
    ASSERT_INT("so is the comment", mapped(&t, 5), 2);
    ASSERT_INT("glClearColor is document row 3", mapped(&t, 6), 3);
    ASSERT_INT("glEnd is document row 7", mapped(&t, 10), 7);
    ASSERT_INT("the display closer maps to nothing", mapped(&t, 11),
               REPL_ROW_MAP_NONE);
    ASSERT_INT("the mapped physical span ends at the last body row",
               t.map.count, 10);
    ASSERT_INT("no hole was asked for", t.map.hole_row, -1);
}

/* Read a file into a NULL-terminated line array. Owned by the caller; freed by
 * free_lines(). */
#define MAX_FIXTURE_LINES 512

static int read_lines(const char *path, char **out, int max) {
    char buf[MAX_LINE_LEN * 2];
    FILE *f = fopen(path, "r");
    int n = 0;

    if (!f)
        return 0;
    while (n < max - 1 && fgets(buf, (int)sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        out[n] = (char *)malloc(len + 1);
        if (!out[n])
            break;
        memcpy(out[n], buf, len + 1);
        n++;
    }
    fclose(f);
    out[n] = NULL;
    return n;
}

static void free_lines(char **lines, int n) {
    for (int i = 0; i < n; i++)
        free(lines[i]);
}

static int find_line_with(char *const *lines, int n, const char *needle) {
    for (int i = 0; i < n; i++)
        if (strstr(lines[i], needle))
            return i;
    return -1;
}

static int find_doc_row_with(const char *needle) {
    int count = repl_state_document_count();
    for (int i = 0; i < count; i++) {
        const char *line = editor_buffer_line(i);
        if (line && strstr(line, needle))
            return i;
    }
    return -1;
}

/* Exported C stages a pre-snippet function body: the reader buffers the whole
 * body and replays it at `// Snippet start`, long after those physical rows
 * went by. So the row a body line was *read* at and the point it is *fed* at
 * are different, and an entry recorded at read time would name whatever the
 * document happened to hold then.
 *
 * Built by exporting a real document rather than by hand, so the fixture
 * cannot drift from what the writer actually emits, and asserted by finding
 * both ends by text - the test is about the map connecting them, not about
 * where the exporter chose to put its scaffolding. */
static void test_row_map_records_feed_order_not_read_order(void) {
    TestRowMap t;
    char *lines[MAX_FIXTURE_LINES];
    const char *path = "/tmp/gl_repl_scene_load_staged.c";
    int n, phys_body, doc_body;
    static const char *const with_func[] = {
        "func0(s) {",
        "glVertex3f(s, 0, 0);",
        "}",
        "display() {",
        "glBegin(GL_POINTS);",
        "func0(1);",
        "glEnd();",
        "}",
        NULL
    };

    printf("--- the map records where a staged row was fed ---\n");

    glr_ctrl_reset_all();
    ASSERT_INT("the source document loads",
               repl_scene_load_from_lines(with_func, "src.glr", NULL, NULL), 1);
    ASSERT_TRUE("and exports as C",
                repl_export_save_output(path, source_document_view(), NULL) != 0);

    n = read_lines(path, lines, MAX_FIXTURE_LINES);
    ASSERT_TRUE("the export reads back", n > 0);
    phys_body = find_line_with(lines, n, "glVertex3f(s, 0, 0)");
    ASSERT_TRUE("the function body is in the file", phys_body >= 0);

    glr_ctrl_reset_all();
    row_map_reset(&t, 0);
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_C,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.row_map = &t.map;
        ASSERT_INT("the exported C re-imports",
                   repl_scene_load_from_lines((const char *const *)lines, path,
                                              &opts, NULL), 1);
    }
    doc_body = find_doc_row_with("glVertex3f(s, 0, 0)");
    ASSERT_TRUE("the body row came back", doc_body >= 0);
    /* The claim: the physical row deep in the file resolves to the document
     * row the staged replay actually produced. Off-by-scaffolding here is
     * exactly the failure the map exists to prevent. */
    ASSERT_INT("the map connects the two", mapped(&t, phys_body + 1), doc_body);
    ASSERT_TRUE("and the file really did have scaffolding above it",
                phys_body + 1 > doc_body);

    free_lines(lines, n);
    (void)remove(path);
}

/* The hole: the controller removed a physical row and left a nonce-bearing
 * marker where it was, so the reader can say where it would have landed. */
static void test_cursor_hole_reports_where_the_row_would_have_gone(void) {
    TestRowMap t;
    static const char *const holed[] = {
        "// @scene-name Holed",
        "display() {",
        "glClearColor(0, 0, 0, 1);",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "// @cursor-hole 4242",           /* row 6: was glVertex3f(0, 0, */
        "glEnd();",
        "}",
        NULL
    };

    printf("--- a cursor hole names its document position ---\n");

    glr_ctrl_reset_all();
    row_map_reset(&t, 4242UL);
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.row_map = &t.map;
        ASSERT_INT("the file loads",
                   repl_scene_load_from_lines(holed, "holed.glr", &opts, NULL),
                   1);
    }
    ASSERT_INT("the marker produced no document row",
               repl_state_document_count(), 4);
    ASSERT_INT("the hole is physical row 6", t.map.hole_row, 6);
    ASSERT_INT("and would have been document row 3", t.map.hole_doc_row, 3);
    ASSERT_INT("the map agrees", mapped(&t, 6), 3);
    ASSERT_STR("the row after it kept its place", unindented(3),
               "glEnd();");
}

/* Two ways the marker must stay an ordinary comment: no nonce was requested,
 * and a different nonce was. Both are the case of a user who happens to have
 * written the words. */
static void test_cursor_hole_needs_its_nonce(void) {
    TestRowMap t;
    static const char *const holed[] = {
        "display() {",
        "glClearColor(0, 0, 0, 1);",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "// @cursor-hole 4242",
        "}",
        NULL
    };

    printf("--- only the nonce-bearing marker is transport ---\n");

    glr_ctrl_reset_all();
    row_map_reset(&t, 0);   /* no hole requested at all */
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.row_map = &t.map;
        ASSERT_INT("the file loads",
                   repl_scene_load_from_lines(holed, "holed.glr", &opts, NULL),
                   1);
    }
    ASSERT_INT("the marker stayed an ordinary comment row",
               repl_state_document_count(), 3);
    ASSERT_STR("with its text intact", unindented(2),
               "// @cursor-hole 4242");
    ASSERT_INT("nothing was reported as a hole", t.map.hole_row, -1);

    glr_ctrl_reset_all();
    row_map_reset(&t, 99UL);   /* a hole, but not this one */
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.row_map = &t.map;
        ASSERT_INT("the file loads again",
                   repl_scene_load_from_lines(holed, "holed.glr", &opts, NULL),
                   1);
    }
    ASSERT_INT("a mismatched nonce is not consumed either",
               repl_state_document_count(), 3);
    ASSERT_INT("and reports no hole", t.map.hole_row, -1);
}

/* A hole inside a function body is the case the staging path exists for: the
 * body is buffered whole and replayed later, so a marker that recorded its
 * position when it was *read* would name a row from before the function was
 * emitted. */
static void test_cursor_hole_inside_a_staged_function(void) {
    TestRowMap t;
    static const char *const staged[] = {
        "func0(s) {",
        "glVertex3f(s, 0, 0);",
        "// @cursor-hole 7",              /* row 3: inside the body */
        "}",
        "display() {",
        "glBegin(GL_POINTS);",
        "func0(1);",
        "glEnd();",
        "}",
        NULL
    };

    printf("--- a hole inside a function body lands in the body ---\n");

    glr_ctrl_reset_all();
    row_map_reset(&t, 7UL);
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.row_map = &t.map;
        ASSERT_INT("the file loads",
                   repl_scene_load_from_lines(staged, "staged.glr", &opts,
                                              NULL), 1);
    }
    ASSERT_INT("the marker produced no document row",
               repl_state_document_count(), 6);
    ASSERT_INT("the hole is physical row 3", t.map.hole_row, 3);
    /* func0(s) { = 0, glVertex3f = 1, the hole = 2, } = 3. Reported from where
     * the staged replay put it, not from where the file mentioned it. */
    ASSERT_INT("and sits inside the body at document row 2",
               t.map.hole_doc_row, 2);
    ASSERT_STR("the closing brace follows it", unindented(2), "}");
}

/* A multi-line logical statement is fed with line_no reset to its first
 * physical row, so only that row used to be recorded. Cursor-only movement
 * onto a continuation row then resolved to REPL_ROW_MAP_NONE and did nothing.
 * Every code-bearing physical row of the statement maps to the produced
 * document row; a comment dropped while the statement was open stays NONE. */
static void test_row_map_covers_continuation_rows(void) {
    TestRowMap t;
    static const char *const continued[] = {
        "display() {",                   /* 1: consumed */
        "glClearColor(0, 0, 0, 1);",     /* 2 → 0 */
        "glClear(GL_COLOR_BUFFER_BIT);", /* 3 → 1 */
        "glBegin(GL_POINTS);",           /* 4 → 2 */
        "glVertex3f(",                   /* 5 ─┐ */
        "    1,",                        /* 6  │ one document row */
        "    // mid",                    /* 7  │ comment: dropped, NONE */
        "    2,",                        /* 8  │ */
        "    3);",                       /* 9 ─┘ → 3 */
        "glEnd();",                      /* 10 → 4 */
        "}",                             /* 11: consumed */
        NULL
    };

    printf("--- continuation rows share the statement's document row ---\n");

    glr_ctrl_reset_all();
    row_map_reset(&t, 0);
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.row_map = &t.map;
        ASSERT_INT("the file loads",
                   repl_scene_load_from_lines(continued, "continued.glr",
                                              &opts, NULL), 1);
    }
    ASSERT_INT("five document rows", repl_state_document_count(), 5);
    ASSERT_INT("the opener maps to the vertex row", mapped(&t, 5), 3);
    ASSERT_INT("so does the first continuation", mapped(&t, 6), 3);
    ASSERT_INT("a comment inside the statement maps to nothing",
               mapped(&t, 7), REPL_ROW_MAP_NONE);
    ASSERT_INT("so does the later continuation", mapped(&t, 8), 3);
    ASSERT_INT("and the closer", mapped(&t, 9), 3);
    ASSERT_INT("glEnd is the next document row", mapped(&t, 10), 4);
    ASSERT_INT("the joined statement is one document row",
               find_doc_row_with("1, 2, 3"), 3);
}

/* Keep the continuation map complete for unusually tall but still valid
 * logical statements. This expression is split one token per physical row:
 * it remains well below MAX_LINE_LEN after joining, but crosses the old
 * 64-row bookkeeping cap. */
static void test_row_map_covers_more_than_64_continuation_rows(void) {
    enum { PIECES = 67, MAX_LINES = 1 + 3 + 1 + PIECES + 1 + 1 + 1 + 1 };
    const char *lines[MAX_LINES];
    TestRowMap t;
    int n = 0;
    int late_piece_row;
    int closer_row;

    printf("--- continuation mapping has no separate 64-row cap ---\n");

    lines[n++] = "display() {";
    lines[n++] = "glClearColor(0, 0, 0, 1);";
    lines[n++] = "glClear(GL_COLOR_BUFFER_BIT);";
    lines[n++] = "glBegin(GL_POINTS);";
    lines[n++] = "glVertex3f(";
    for (int i = 0; i < PIECES; i++)
        lines[n++] = (i & 1) ? "+" : "0";
    late_piece_row = n; /* 1-based row of the final piece, beyond the old cap. */
    lines[n++] = ", 0, 0);";
    closer_row = n;
    lines[n++] = "glEnd();";
    lines[n++] = "}";
    lines[n] = NULL;

    glr_ctrl_reset_all();
    row_map_reset(&t, 0);
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.row_map = &t.map;
        ASSERT_INT("the tall statement loads",
                   repl_scene_load_from_lines(lines, "tall.glr", &opts, NULL),
                   1);
    }
    ASSERT_INT("the tall statement still produces five document rows",
               repl_state_document_count(), 5);
    ASSERT_TRUE("the checked piece is beyond physical row 64",
                late_piece_row > 64);
    ASSERT_INT("a continuation beyond the old cap maps to the vertex row",
               mapped(&t, late_piece_row), 3);
    ASSERT_INT("the closer beyond the old cap maps too",
               mapped(&t, closer_row), 3);
}

/* A load that fails describes a document that no longer exists. */
static void test_row_map_is_cleared_by_a_failed_load(void) {
    TestRowMap t;

    printf("--- a failed load leaves no map behind ---\n");

    glr_ctrl_reset_all();
    row_map_reset(&t, 0);
    {
        ReplSceneLoadOpts opts = opts_for(REPL_EXAMPLE_SOURCE_GLR,
                                          REPL_SCENE_LOAD_POLICY_ATOMIC);
        opts.row_map = &t.map;
        ASSERT_INT("the load fails",
                   repl_scene_load_from_lines(k_scene_with_one_bad_row,
                                              "bad.glr", &opts, NULL), 0);
    }
    ASSERT_INT("no rows are claimed", t.map.count, 0);
    ASSERT_INT("and the rows read before the failure are cleared",
               mapped(&t, 1), REPL_ROW_MAP_NONE);
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
    test_cli_positional_binds_the_file();
    test_ctrl_s_writes_the_bound_file();
    test_example_has_no_binding();
    test_reload_reuses_the_active_slot();
    test_failed_reload_leaves_no_trace();
    test_reload_cannot_rebind_the_slot();
    test_open_binds_its_own_path();
    test_open_path_wins_over_an_existing_workspace();
    test_row_map_survives_headers_and_directives();
    test_row_map_records_feed_order_not_read_order();
    test_cursor_hole_reports_where_the_row_would_have_gone();
    test_cursor_hole_needs_its_nonce();
    test_cursor_hole_inside_a_staged_function();
    test_row_map_covers_continuation_rows();
    test_row_map_covers_more_than_64_continuation_rows();
    test_row_map_is_cleared_by_a_failed_load();
    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "scene_load");
}
