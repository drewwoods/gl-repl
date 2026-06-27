/*
 * test_scene_file_menu.c - Scene/File menu rework + save-as-scene-name.
 *
 * Covers the relocated File-menu actions and repl_save_active_scene's
 * filename derivation. Never exercises the no-active-scene fallback
 * (that path writes ./output.c in the repo root — see
 * scripts/check/check-no-test-default-output.sh); the save test always has an
 * active named scene + a mkdtemp workspace dir.
 */
#include "app/glr_ctrl.h"
#include "app/glr_ctrl_export.h"
#include "app/glr_actions.h"
#include "app/glr_paths.h"
#include "editor/inline_rename.h"
#include "repl/example_loader.h"
#include "repl/export.h"
#include "repl/examples.h"
#include "repl/scenes.h"
#include "repl/state_views.h"
#include "ui/app/snapshot.h"
#include "ui/app/state.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void rmdir_empty_parents_to(const char *path, const char *stop) {
    char cur[GLR_PATH_MAX];
    size_t stop_len = stop ? strlen(stop) : 0;

    if (!path || !stop || !stop[0])
        return;
    snprintf(cur, sizeof(cur), "%s", path);
    while (strncmp(cur, stop, stop_len) == 0 &&
           cur[stop_len] == '/' &&
           strlen(cur) > stop_len) {
        rmdir(cur);
        char *slash = strrchr(cur, '/');
        if (!slash)
            break;
        *slash = '\0';
    }
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

/* Finder-launched macOS apps have cwd "/"; relative saves then target an
 * unwritable location. With no bound workspace, Save Scene and Save
 * Workspace should fall back to the per-user Application Support / XDG
 * workspace while CLI/dev launches still use ./workspace. */
static void test_app_save_falls_back_to_user_workspace(void) {
    char home_tmpl[] = "/tmp/glr_home_XXXXXX";
    char cwd_tmpl[] = "/tmp/glr_nowrite_XXXXXX";
    char prev_cwd[1024];
    char old_home[GLR_PATH_MAX];
    char old_xdg[GLR_PATH_MAX];
    char workspace[GLR_PATH_MAX];
    char scene_path[GLR_PATH_MAX + 64];
    char *home = mkdtemp(home_tmpl);
    char *nowrite = mkdtemp(cwd_tmpl);
    const char *home_env = getenv("HOME");
    const char *xdg_env = getenv("XDG_DATA_HOME");
    int had_home = home_env && home_env[0];
    int had_xdg = xdg_env && xdg_env[0];
    int have_cwd = getcwd(prev_cwd, sizeof(prev_cwd)) != NULL;

    if (had_home)
        snprintf(old_home, sizeof(old_home), "%s", home_env);
    else
        old_home[0] = '\0';
    if (had_xdg)
        snprintf(old_xdg, sizeof(old_xdg), "%s", xdg_env);
    else
        old_xdg[0] = '\0';

    ASSERT_TRUE("mkdtemp fallback home", home != NULL);
    ASSERT_TRUE("mkdtemp fallback cwd", nowrite != NULL);
    ASSERT_TRUE("getcwd before fallback save", have_cwd);
    if (!home || !nowrite || !have_cwd)
        return;

    ASSERT_INT_EQ("set fallback HOME", setenv("HOME", home, 1), 0);
    ASSERT_INT_EQ("unset fallback XDG_DATA_HOME",
                  unsetenv("XDG_DATA_HOME"), 0);
    ASSERT_TRUE("resolve fallback workspace",
                glr_paths_user_workspace_dir(workspace, sizeof(workspace)));
    snprintf(scene_path, sizeof(scene_path), "%s/bundle_save_scene.c",
             workspace);

    reset_fixture();
    int slot = seed_user_scene();
    ASSERT_TRUE("fallback save seeded scene", slot >= 0);
    repl_user_scene_rename(slot, "Bundle Save Scene");
    repl_set_workspace_dir(NULL);

    ASSERT_INT_EQ("chmod fallback cwd read-only",
                  chmod(nowrite, 0555), 0);
    ASSERT_INT_EQ("chdir fallback cwd", chdir(nowrite), 0);

    int h1 = glr_action_menu_item_activate(GLR_MENU_FILE,
                                           GLR_FILE_ITEM_SAVE_SCENE);
    ASSERT_TRUE("Save Scene handled under unwritable cwd", h1 == 1);
    ASSERT_INT_EQ("Save Scene wrote user workspace file",
                  access(scene_path, F_OK), 0);
    unlink(scene_path);

    repl_set_workspace_dir(NULL);
    int h2 = glr_action_menu_item_activate(GLR_MENU_FILE,
                                           GLR_FILE_ITEM_SAVE_WORKSPACE);
    ASSERT_TRUE("Save Workspace handled under unwritable cwd", h2 == 1);
    ASSERT_INT_EQ("Save Workspace wrote user workspace scene",
                  access(scene_path, F_OK), 0);
    unlink(scene_path);

    ASSERT_TRUE("workspace bound to user fallback",
                strcmp(repl_workspace_dir(), workspace) == 0);

    ASSERT_INT_EQ("restore cwd after fallback save", chdir(prev_cwd), 0);
    chmod(nowrite, 0755);
    rmdir_empty_parents_to(workspace, home);
    rmdir(nowrite);
    rmdir(home);
    if (had_home)
        setenv("HOME", old_home, 1);
    else
        unsetenv("HOME");
    if (had_xdg)
        setenv("XDG_DATA_HOME", old_xdg, 1);
    else
        unsetenv("XDG_DATA_HOME");
    repl_set_workspace_dir(NULL);
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
    test_app_save_falls_back_to_user_workspace();
    test_rename_scene_guard();
    test_scene_menu_is_selector();
    return test_harness_report(&g_harness, "scene_file_menu");
}
