/*
 * test_scene_file_menu.c - Scene/File menu rework + save-as-scene-name.
 *
 * Covers the relocated File-menu actions and repl_save_active_scene's
 * filename derivation. Never exercises the no-active-scene fallback
 * (that path writes ./output.c in the repo root - see
 * scripts/check/check-no-test-default-output.sh); the save test always has an
 * active named scene + a mkdtemp workspace dir.
 */
#include "app/glr_ctrl.h"
#include "app/glr_ctrl_export.h"
#include "app/glr_actions.h"
#include "app/glr_config.h"
#include "app/glr_defaults.h"
#include "app/glr_modal.h"
#include "app/glr_paths.h"
#include "source_document.h"
#include "editor/inline_rename.h"
#include "repl/example_loader.h"
#include "repl/export.h"
#include "repl/examples.h"
#include "repl/scenes.h"
#include "repl/state_views.h"
#include "repl/workspace_io.h"
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
    repl_promote_transient_if_needed();
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

/* File -> New Scene creates a fresh user-scene slot with the editable display
 * baseline so the scene
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
    ASSERT_INT_EQ("New Scene seeds display baseline",
                  repl_state_document_count(), 6);
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

/* Slurp a whole file; caller frees. NULL if it can't be opened. */
static char *read_file_text(const char *path) {
    FILE *f = fopen(path, "r");
    char *buf;
    long len;
    size_t got;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)len, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Stage an active named scene in `dir` with one presentation cfg pushed off
 * its default (axes) and one left at it (grid extent), then run
 * File -> Save Scene as .glr. Returns the written path in `out_path`. */
static void save_glr_fixture(const char *dir, char *out_path, size_t out_sz) {
    reset_fixture();
    (void)seed_user_scene();
    repl_user_scene_rename(repl_active_user_scene(), "Unit Save Glr");
    repl_set_workspace_dir(dir);

    glr_config_set(GLR_CONFIG_AXES_THEME, CFG_DEFAULT_AXES_THEME + 1);
    glr_config_set(GLR_CONFIG_GRID_EXTENT, CFG_DEFAULT_GRID_EXTENT_IDX);

    ASSERT_INT_EQ("Save Scene as .glr handled",
                  glr_action_menu_item_activate(GLR_MENU_FILE,
                                                GLR_FILE_ITEM_SAVE_GLR), 1);
    snprintf(out_path, out_sz, "%s/unit_save_glr.glr", dir);
}

/* Save Scene as .glr writes the authoring format built-in examples ship in:
 * only the @cfg rows that differ from the presentation defaults, the
 * `// camera` block, and the document at column 0. None of the standalone-C
 * scaffold repl_export_save_output emits. */
static void test_save_glr_writes_scene_source(void) {
    char tmpl[] = "/tmp/glr_scene_glr_XXXXXX";
    char *dir = mkdtemp(tmpl);
    char path[512];
    char *text;

    ASSERT_TRUE("mkdtemp", dir != NULL);
    if (!dir)
        return;

    save_glr_fixture(dir, path, sizeof(path));
    text = read_file_text(path);
    ASSERT_TRUE("Save .glr wrote <workspace>/<slug>.glr", text != NULL);
    if (!text) {
        repl_set_workspace_dir(NULL);
        rmdir(dir);
        return;
    }

    ASSERT_TRUE("Save .glr emits the off-default cfg row",
                strstr(text, "// @cfg axes = ") != NULL);
    ASSERT_TRUE("Save .glr omits a cfg row left at its default",
                strstr(text, "// @cfg grid_extent") == NULL);
    {
        /* The seeded example carries its own authored heading, which the
         * writer preserves; a scene without one gets a plain "// camera". */
        const char *marker = strstr(text, "Camera");
        if (!marker)
            marker = strstr(text, "camera");
        ASSERT_TRUE("Save .glr emits a camera marker comment", marker != NULL);
        ASSERT_TRUE("Save .glr emits camera transforms at column 0 under it",
                    marker && strstr(marker, "\nglTranslatef(") != NULL);
    }
    ASSERT_TRUE("Save .glr emits no C scaffold",
                strstr(text, "#include") == NULL &&
                strstr(text, "void display(") == NULL &&
                strstr(text, "glutMainLoop") == NULL);
    ASSERT_TRUE("Save .glr emits no workspace metadata directives",
                strstr(text, "@scene-name") == NULL &&
                strstr(text, "@workspace-dir") == NULL);

    free(text);
    unlink(path);
    repl_set_workspace_dir(NULL);
    rmdir(dir);
}

/* The written file is a loadable example: run it back through the runtime
 * examples-dir catalog and the document plus the off-default cfg return
 * unchanged. This is the property the format exists for - a scene saved
 * here can be dropped into examples/scenes/ as-is. */
static void test_save_glr_round_trips_through_example_loader(void) {
    char tmpl[] = "/tmp/glr_scene_rt_XXXXXX";
    char *dir = mkdtemp(tmpl);
    char scenes_dir[512];
    char catalog_path[512];
    char glr_path[512];
    char catalog_text[512];
    char err[512];
    /* Bounded copy of the pre-save document. Sized for a seeded example,
     * not MAX_EDITOR_COMMANDS - a full-capacity buffer here would be a
     * quarter-megabyte of stack. */
    enum { SAVED_LINE_CAP = 128 };
    char saved_lines[SAVED_LINE_CAP][MAX_LINE_LEN];
    FILE *cat;
    int saved_count;

    ASSERT_TRUE("mkdtemp", dir != NULL);
    if (!dir)
        return;

    snprintf(scenes_dir, sizeof(scenes_dir), "%s/scenes", dir);
    ASSERT_INT_EQ("round-trip scenes mkdir", mkdir(scenes_dir, 0700), 0);

    save_glr_fixture(scenes_dir, glr_path, sizeof(glr_path));

    saved_count = repl_state_document_count();
    ASSERT_TRUE("round-trip source has commands",
                saved_count > 0 && saved_count <= SAVED_LINE_CAP);
    if (saved_count > SAVED_LINE_CAP)
        saved_count = SAVED_LINE_CAP;
    for (int i = 0; i < saved_count; i++) {
        const char *line = source_text_line(source_document_view(), i);
        snprintf(saved_lines[i], sizeof(saved_lines[i]), "%s",
                 line ? line : "");
    }

    snprintf(catalog_path, sizeof(catalog_path), "%s/catalog.ini", dir);
    snprintf(catalog_text, sizeof(catalog_text),
             "[unit-save-glr]\n"
             "file = scenes/unit_save_glr.glr\n"
             "name = Unit Save Glr\n"
             "tags = 3D, Polygons\n"
             "group = Runtime\n");
    cat = fopen(catalog_path, "w");
    ASSERT_TRUE("round-trip catalog written", cat != NULL);
    if (cat) {
        fputs(catalog_text, cat);
        fclose(cat);
    }

    err[0] = '\0';
    ASSERT_TRUE("round-trip catalog loads",
                repl_examples_load_dir(dir, err, sizeof(err)));
    ASSERT_INT_EQ("round-trip catalog holds one example",
                  repl_example_count(), 1);

    reset_fixture();
    repl_load_example(0);

    ASSERT_INT_EQ("round-trip restores the command count",
                  repl_state_document_count(), saved_count);
    for (int i = 0; i < saved_count && i < repl_state_document_count(); i++) {
        const char *line = source_text_line(source_document_view(), i);
        char label[64];
        snprintf(label, sizeof(label), "round-trip line %d matches", i);
        ASSERT_TRUE(label, line && strcmp(line, saved_lines[i]) == 0);
    }
    ASSERT_INT_EQ("round-trip reapplies the off-default cfg",
                  glr_config_get(GLR_CONFIG_AXES_THEME),
                  CFG_DEFAULT_AXES_THEME + 1);

    unlink(glr_path);
    unlink(catalog_path);
    rmdir(scenes_dir);
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

/* Delete a managed workspace directory: its manifest scenes, the manifest,
 * then the directory itself. */
static void remove_managed_workspace(const char *dir) {
    WorkspaceManifest manifest;
    char path[GLR_PATH_MAX + WORKSPACE_IO_FILE_MAX + 8];

    if (workspace_io_manifest_read(dir, &manifest, NULL, 0)) {
        for (int i = 0; i < manifest.scene_count; i++)
            if (workspace_io_path_join(dir, manifest.scene_files[i],
                                       path, sizeof(path)))
                unlink(path);
    }
    if (workspace_io_path_join(dir, WORKSPACE_IO_MANIFEST_FILE,
                               path, sizeof(path)))
        unlink(path);
    rmdir(dir);
}

/* File -> New Workspace on an unbound session adopts the in-memory scenes
 * instead of leaving them behind: before the fix the new (empty) workspace was
 * opened, which reset the catalog and left the collection reachable only in the
 * hidden recovery workspace. Creating a workspace while a managed one is bound
 * still starts empty - those scenes remain saved in the old workspace. */
static void test_new_workspace_adopts_unbound_scenes(void) {
    char cwd_tmpl[] = "/tmp/glr_newws_XXXXXX";
    char prev_cwd[1024];
    char ws_dir[GLR_PATH_MAX];
    char err[256];
    WorkspaceManifest manifest;
    char *root = mkdtemp(cwd_tmpl);
    int have_cwd = getcwd(prev_cwd, sizeof(prev_cwd)) != NULL;

    ASSERT_TRUE("mkdtemp new-workspace cwd", root != NULL);
    ASSERT_TRUE("getcwd before new workspace", have_cwd);
    if (!root || !have_cwd)
        return;
    ASSERT_INT_EQ("chdir new-workspace cwd", chdir(root), 0);

    reset_fixture();
    repl_set_workspace_dir(NULL);
    int slot = seed_user_scene();
    ASSERT_TRUE("new-workspace seeded scene", slot >= 0);
    repl_user_scene_rename(slot, "Carried Scene");
    ASSERT_TRUE("second slot for new workspace",
                repl_scenes_create_empty_user_scene() >= 0);
    repl_user_scene_rename(repl_active_user_scene(), "Carried Two");
    int count_before = repl_user_scene_count();

    ASSERT_INT_EQ("New Workspace handled",
                  glr_action_menu_item_activate(GLR_MENU_FILE,
                                                GLR_FILE_ITEM_NEW_WORKSPACE), 1);
    ASSERT_TRUE("New Workspace opened the prompt",
                glr_modal_kind() == GLR_MODAL_WORKSPACE_NEW);
    const char *name = "Carried";
    for (int i = 0; name[i]; i++)
        glr_modal_handle_key((unsigned char)name[i]);
    glr_modal_handle_key('\r');
    ASSERT_TRUE("New Workspace prompt closed on commit", !glr_modal_active());

    ASSERT_TRUE("New Workspace resolves the created dir",
                glr_paths_workspace_dir_for_name(name, ws_dir, sizeof(ws_dir)));
    ASSERT_TRUE("New Workspace binds the created dir",
                strcmp(repl_workspace_dir(), ws_dir) == 0);
    ASSERT_INT_EQ("New Workspace keeps the in-memory scenes",
                  repl_user_scene_count(), count_before);
    ASSERT_TRUE("New Workspace manifest reloads",
                workspace_io_manifest_read(ws_dir, &manifest,
                                           err, sizeof(err)));
    ASSERT_INT_EQ("New Workspace carried every scene",
                  manifest.scene_count, count_before);

    /* Second New Workspace, now from a managed one: starts empty. */
    ASSERT_INT_EQ("New Workspace (bound) handled",
                  glr_action_menu_item_activate(GLR_MENU_FILE,
                                                GLR_FILE_ITEM_NEW_WORKSPACE), 1);
    const char *name2 = "Fresh";
    for (int i = 0; name2[i]; i++)
        glr_modal_handle_key((unsigned char)name2[i]);
    glr_modal_handle_key('\r');
    ASSERT_TRUE("New Workspace (bound) prompt closed", !glr_modal_active());
    ASSERT_INT_EQ("New Workspace from a managed one starts empty",
                  repl_user_scene_count(), 1);

    repl_set_workspace_dir(NULL);
    reset_fixture();
    {
        char fresh_dir[GLR_PATH_MAX];
        char ws_root[GLR_PATH_MAX];
        char state_path[GLR_PATH_MAX];
        if (glr_paths_workspace_dir_for_name(name2, fresh_dir,
                                             sizeof(fresh_dir)))
            remove_managed_workspace(fresh_dir);
        remove_managed_workspace(ws_dir);
        if (glr_paths_workspaces_root(ws_root, sizeof(ws_root)))
            rmdir(ws_root);
        /* The second (bound) New Workspace goes through the switch path, which
         * rescues the live document to the recovery file. */
        if (glr_paths_app_state_path(QUIT_RECOVERY_FILE,
                                     state_path, sizeof(state_path)))
            unlink(state_path);
        if (glr_paths_app_state_path("recovery-workspace",
                                     state_path, sizeof(state_path)))
            remove_managed_workspace(state_path);
    }
    ASSERT_INT_EQ("restore cwd after new workspace", chdir(prev_cwd), 0);
    rmdir(root);
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

    /* Row 0 is "### EXAMPLES" - inert, no crash, consumed. */
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
    test_new_workspace_adopts_unbound_scenes();
    test_rename_scene_guard();
    test_scene_menu_is_selector();
    test_save_glr_writes_scene_source();
    /* Last: it swaps the process-wide example catalog for a runtime one. */
    test_save_glr_round_trips_through_example_loader();
    return test_harness_report(&g_harness, "scene_file_menu");
}
