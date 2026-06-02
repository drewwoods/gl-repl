/*
 * test_scene_file_menu.c - Scene/File menu rework + save-as-scene-name.
 *
 * Covers the relocated File-menu actions and repl_save_active_scene's
 * filename derivation. Never exercises the no-active-scene fallback
 * (that path writes ./output.c in the repo root — see
 * scripts/check-no-test-default-output.sh); the save test always has an
 * active named scene + a mkdtemp workspace dir.
 */
#include "app/glr_ctrl.h"
#include "app/glr_ctrl_export.h"
#include "app/glr_actions.h"
#include "editor/inline_rename.h"
#include "repl/core.h"
#include "repl/examples.h"
#include "repl/scenes.h"
#include "repl/state_views.h"
#include "ui/app/snapshot.h"
#include "ui/app/state.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)   TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT_EQ(label, g, e) TEST_ASSERT_INT(&g_harness, label, g, e)

static void reset_fixture(void) {
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1000, 600);
}

/* Occupy + activate one user slot the way the editor tests do. */
static int seed_user_scene(void) {
    repl_load_example(0);
    repl_promote_example_if_needed();
    return repl_active_user_scene();
}

/* File -> New Scene creates a real empty user-scene slot so the scene
 * tab strip gets a tab immediately. It must not reuse the previous
 * active slot, or Save Scene would overwrite that scene's file with the
 * emptied buffer. */
static void test_new_scene_creates_active_tab(void) {
    reset_fixture();
    int slot = seed_user_scene();
    ASSERT_TRUE("seeded active user scene", slot >= 0);
    int count_before = repl_user_scene_count();

    int handled = glr_action_menu_item_activate(GLR_MENU_FILE,
                                                GLR_FILE_ITEM_NEW_SCENE);
    ASSERT_TRUE("New Scene handled", handled == 1);
    ASSERT_TRUE("New Scene creates an active user slot",
                repl_active_user_scene() >= 0);
    ASSERT_TRUE("New Scene does not reuse the old active slot",
                repl_active_user_scene() != slot);
    ASSERT_TRUE("New Scene grows the tab set",
                repl_user_scene_count() > count_before);
    ASSERT_TRUE("New Scene tab has a name",
                repl_user_scene_name(repl_active_user_scene()) != NULL);
    {
        UiRenderSnapshot snap;
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_TRUE("New Scene creates a visible active tab",
                    snap.scene_tabs.active_idx >= 0 &&
                    snap.scene_tabs.active_idx < snap.scene_tabs.count);
        ASSERT_TRUE("New Scene visible tab is user scene",
                    snap.scene_tabs.tabs[snap.scene_tabs.active_idx].kind ==
                    UI_SCENE_TAB_USER);
    }
    ASSERT_INT_EQ("New Scene clears the document",
                  repl_state_document_count(), 0);
    ASSERT_INT_EQ("New Scene clears the active example",
                  repl_state_scenes().active_example_idx, -1);
}

/* Save Scene writes <workspace>/<slug>.c for an active named scene. */
static void test_save_scene_uses_scene_name(void) {
    char tmpl[] = "/tmp/glr_scene_XXXXXX";
    char *dir = mkdtemp(tmpl);
    char path[512];
    FILE *f;

    ASSERT_TRUE("mkdtemp", dir != NULL);
    if (!dir)
        return;

    reset_fixture();
    int slot = seed_user_scene();
    ASSERT_TRUE("seeded active user scene", slot >= 0);
    repl_user_scene_rename(slot, "Unit Save Scene");
    repl_set_workspace_dir(dir);

    {
        ReplExportLayout layout;
        glr_ctrl_fill_export_layout(&layout);
        int handled = glr_action_menu_item_activate(GLR_MENU_FILE,
                                                    GLR_FILE_ITEM_SAVE_SCENE);
        ASSERT_TRUE("Save Scene handled", handled == 1);
        (void)layout;
    }

    /* scene_filename_slug: alnum->lower, space/-/_ -> '_'. */
    snprintf(path, sizeof(path), "%s/unit_save_scene.c", dir);
    f = fopen(path, "r");
    ASSERT_TRUE("Save Scene wrote <workspace>/<slug>.c", f != NULL);
    if (f)
        fclose(f);

    unlink(path);
    repl_set_workspace_dir(NULL);
    rmdir(dir);
}

/* Rename Scene (now under File) keeps the active-slot guard. */
static void test_rename_scene_guard(void) {
    reset_fixture();
    seed_user_scene();
    editor_inline_rename_cancel();

    int h1 = glr_action_menu_item_activate(GLR_MENU_FILE,
                                           GLR_FILE_ITEM_RENAME_SCENE);
    ASSERT_TRUE("Rename Scene handled (active)", h1 == 1);
    ASSERT_TRUE("Rename Scene begins on an active scene",
                editor_inline_rename_active());
    editor_inline_rename_cancel();

    reset_fixture();
    int h2 = glr_action_menu_item_activate(GLR_MENU_FILE,
                                           GLR_FILE_ITEM_RENAME_SCENE);
    ASSERT_TRUE("Rename Scene handled (none)", h2 == 1);
    ASSERT_TRUE("Rename Scene no-ops with no active scene",
                !editor_inline_rename_active());
}

/* Scene menu is a pure selector: header rows are inert; tag rows are hover-only
 * and keep the menu open for submenu selection. */
static void test_scene_menu_is_selector(void) {
    reset_fixture();
    seed_user_scene();

    /* Row 0 is "### EXAMPLES" — inert, no crash, consumed. */
    int h = glr_action_menu_item_activate(GLR_MENU_SCENE, 0);
    ASSERT_TRUE("Scene header row consumed", h == 1);

    /* Row 1 is the first example tag, not an example load action. */
    h = glr_action_menu_item_activate(GLR_MENU_SCENE, 1);
    ASSERT_TRUE("Scene tag row keeps menu open", h == 0);
    ASSERT_INT_EQ("Scene tag row does not load example",
                  repl_state_scenes().active_example_idx, -1);

    h = glr_action_menu_item_activate(
        GLR_MENU_SCENE,
        repl_example_visible_tag_count() + GLR_SCENE_OFF_SCENES);
    ASSERT_TRUE("Scene user row loads active user scene", h == 1);
    ASSERT_INT_EQ("Scene user row keeps user slot active",
                  repl_active_user_scene(), 0);
}

int main(void) {
    printf("--- scene_file_menu tests ---\n");
    test_new_scene_creates_active_tab();
    test_save_scene_uses_scene_name();
    test_rename_scene_guard();
    test_scene_menu_is_selector();
    return test_harness_report(&g_harness, "scene_file_menu");
}
