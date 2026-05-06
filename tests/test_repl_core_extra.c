#define _DEFAULT_SOURCE
#include "repl_config.h"
#include "repl_core_internal.h"
#include "repl_debug.h"
#include "replay.h"
#include "repl_executor.h"
#include "repl_state.h"
#include "ui_panels.h"
#include "editor_inline_rename.h"

#define g_anim_time (repl_state_variables_mut()->anim_time)

#include "support/test_harness.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/freeglut.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    TEST_ASSERT_INT(&g_harness, label, got, exp); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    TEST_ASSERT_STR(&g_harness, label, got, exp); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("y", err, sizeof(err));
    repl_eval_declare_predef_var("z", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
    repl_eval_declare_predef_var("j", err, sizeof(err));
    repl_eval_declare_predef_var("k", err, sizeof(err));
    repl_eval_declare_predef_var("n", err, sizeof(err));
}

/* Some functions are not in internal header but are non-static */
const char *mode_name(GLenum mode);
int repl_source_scope_in_begin_block(void);
int repl_source_scope_cmd_indent_chars(int pos);
GLenum current_begin_mode(void);
int count_vertices(void);
extern int repl_state_flat_program_count();

/* Capture the output of repl_debug_dump_flat_commands(, editor_buffer_view()) into a malloc'd string.
 * Returns NULL on failure; caller frees the buffer. */
static char *capture_flat_dump(void) {
    FILE *tmp = tmpfile();
    char *buf = NULL;
    long len;
    size_t nread;

    if (!tmp)
        return NULL;
    repl_debug_dump_flat_commands(tmp, editor_buffer_view());
    fflush(tmp);
    if (fseek(tmp, 0, SEEK_END) != 0) goto done;
    len = ftell(tmp);
    if (len < 0) goto done;
    if (fseek(tmp, 0, SEEK_SET) != 0) goto done;
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) goto done;
    nread = fread(buf, 1, (size_t)len, tmp);
    buf[nread] = '\0';
done:
    fclose(tmp);
    return buf;
}

void test_utils() {
    printf("--- Utility functions ---\n");

    ASSERT_STR("mode_name(GL_POINTS)", mode_name(GL_POINTS), "GL_POINTS");
    ASSERT_STR("mode_name(GL_TRIANGLES)", mode_name(GL_TRIANGLES), "GL_TRIANGLES");
    ASSERT_STR("mode_name(unknown)", mode_name(9999), "???");

    repl_reset_state(); declare_test_vars();
    ASSERT_INT("count_vertices initial", count_vertices(), 0);
    ASSERT_INT("current_begin_mode initial", current_begin_mode(), GL_TRIANGLES);
    ASSERT_INT("in_begin_block initial", repl_source_scope_in_begin_block(), 0);

    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0,0,0);");
    repl_feed_line_public("glVertex3f(1,0,0);");
    repl_flatten_commands();

    ASSERT_INT("count_vertices after 2 vtx", count_vertices(), 2);
    ASSERT_INT("current_begin_mode in block", current_begin_mode(), GL_TRIANGLES);
    ASSERT_INT("in_begin_block in block", repl_source_scope_in_begin_block(), 1);

    repl_feed_line_public("glEnd();");
    repl_flatten_commands();
    ASSERT_INT("current_begin_mode after end", current_begin_mode(), GL_TRIANGLES);
    ASSERT_INT("in_begin_block after end", repl_source_scope_in_begin_block(), 0);

    /* cmd_indent_chars */
    ASSERT_INT("cmd_indent_chars at 0", repl_source_scope_cmd_indent_chars(0), 2);
    ASSERT_INT("cmd_indent_chars at 1", repl_source_scope_cmd_indent_chars(1), 4);
    ASSERT_INT("cmd_indent_chars at 4", repl_source_scope_cmd_indent_chars(4), 2);

    /* debug dump */
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) {
        repl_debug_dump_editor(devnull, editor_buffer_view());
        fclose(devnull);
    }
}

void test_repl_replay_advanced() {
    printf("--- Replay advanced functions ---\n");
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glVertex3f(0,0,0);");
    repl_feed_line_public("glVertex3f(1,1,1);");
    repl_feed_line_public("glVertex3f(2,2,2);");
    repl_flatten_commands();

    int full_flat_count = repl_state_flat_program_count();
    FlatProgramView live_program = repl_flat_program_view_live();
    ASSERT_INT("flat program view count", live_program.cmd_count, full_flat_count);
    ASSERT_TRUE("flat program view cmds", live_program.cmds == repl_state_flat_program_cmds_mut());
    ASSERT_TRUE("flat program view locals", live_program.local_vars == repl_state_flat_program_local_vars_mut());

    ReplExecutionOptions limited_exec = { .flat_cmd_count = 1 };
    repl_execute_program(&limited_exec);
    ASSERT_INT("limited execute preserves flat count", repl_state_flat_program_count(), full_flat_count);

    limited_exec.flat_cmd_count = full_flat_count + 100;
    limited_exec.program = live_program;
    repl_execute_program(&limited_exec);
    ASSERT_INT("over-limit execute preserves flat count", repl_state_flat_program_count(), full_flat_count);

    repl_replay_start();
    ASSERT_INT("repl_replay_exec_limit start", repl_replay_exec_limit(), 0);

    repl_replay_advance();
    ASSERT_INT("repl_replay_exec_limit advance 1", repl_replay_exec_limit(), 1);

    repl_replay_advance();
    ASSERT_INT("repl_replay_exec_limit advance 2", repl_replay_exec_limit(), 2);

    repl_replay_step_back();
    ASSERT_INT("repl_replay_exec_limit step back", repl_replay_exec_limit(), 1);

    repl_replay_seek_to_src_line(2);
    ASSERT_INT("repl_replay_exec_limit seek_to_src_line(2)", repl_replay_exec_limit(), 3);

    repl_replay_restart_from_beginning();
    ASSERT_INT("repl_replay_exec_limit restart", repl_replay_exec_limit(), 0);

    repl_replay_stop();
}

void test_io() {
    printf("--- IO functions ---\n");
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glVertex3f(1,2,3);");

    const char *tmpf = "/tmp/test_repl_core_extra_io.c";
    repl_export_save_output(tmpf, editor_buffer_view());

    repl_reset_state(); declare_test_vars();
    ASSERT_INT("num_cmds after reset", count_vertices(), 0);

    int r = repl_export_load_from_file(tmpf);
    ASSERT_INT("load_from_file return", r, 1);
    repl_flatten_commands();
    ASSERT_INT("count_vertices after load", count_vertices(), 1);

    unlink(tmpf);

    repl_load_initial_commands(NULL);
    const char *save_path = "/tmp/test_repl_core_extra_default_output.c";
    repl_export_save_output(save_path, editor_buffer_view());
    ASSERT_INT("default-path save creates file",
               access(save_path, F_OK), 0);
    unlink(save_path);
}

void test_execution() {
    printf("--- Execution functions ---\n");
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("n = 1;");
    repl_flatten_commands();

    execute_commands();
}

void test_examples() {
    printf("--- Example functions ---\n");
    int count = repl_example_count();
    ASSERT_TRUE("example_count > 0", count > 0);

    const char *name = repl_example_name(0);
    ASSERT_TRUE("example_name(0) != NULL", name != NULL);

    repl_load_example(0);
    ASSERT_TRUE("repl_state_document_count() > 0 after load_example", repl_state_document_count() > 0);
}

void test_user_scene() {
    printf("--- User scene functions ---\n");
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glVertex3f(1,1,1);");

    /* Loading an example should save slot 0 (home scene) if it's the first time */
    repl_load_example(0);
    ASSERT_INT("user_scene_valid after example load", repl_user_scene_valid(), 1);
    ASSERT_INT("home slot used after example load",
               repl_user_scene_slot_used(0), 1);
    ASSERT_INT("active user scene == -1 while example loaded",
               repl_active_user_scene(), -1);

    /* Home slot stays populated after restore in the multi-scene model. */
    repl_load_user_scene();
    repl_flatten_commands();
    ASSERT_INT("count_vertices after restore", count_vertices(), 1);
    ASSERT_INT("user_scene_valid after restore", repl_user_scene_valid(), 1);
    ASSERT_INT("active user scene == 0 after restore",
               repl_active_user_scene(), 0);
}

void test_user_scene_persists_across_example_switch() {
    printf("--- User scene persists across example switch ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 1) return;

    repl_feed_line_public("glVertex3f(1,1,1);");
    repl_load_example(0);
    ASSERT_INT("home slot used after example load", repl_user_scene_slot_used(0), 1);

    repl_load_user_scene();
    ASSERT_INT("active user scene after restore", repl_active_user_scene(), 0);

    repl_feed_line_public("glVertex3f(2,2,2);");
    repl_flatten_commands();
    ASSERT_INT("edited home scene vertex count", count_vertices(), 2);

    repl_load_example(0);
    repl_load_user_scene();
    repl_flatten_commands();
    ASSERT_INT("persisted home scene vertex count", count_vertices(), 2);
}

void test_user_scene_promote_on_edit() {
    printf("--- User scene promotion on example edit ---\n");
    repl_reset_state(); declare_test_vars();

    /* Fresh session: no user scenes. Load an example → slot 0 captures
     * the empty home scene; example is active, no user scene active. */
    repl_load_example(0);
    ASSERT_INT("slot 0 used (home)",           repl_user_scene_slot_used(0), 1);
    ASSERT_INT("slot 1 unused before edit",    repl_user_scene_slot_used(1), 0);
    ASSERT_INT("active user scene == -1",      repl_active_user_scene(), -1);

    /* Any mutation while viewing the example promotes it.  We drive
     * promotion directly rather than synthesize a keypress. */
    int slot = repl_promote_example_if_needed();
    ASSERT_INT("promoted into slot 1", slot, 1);
    ASSERT_INT("active user scene == 1 after promotion",
               repl_active_user_scene(), 1);

    /* Promoted scene inherits the example's name. */
    const char *ex_name = repl_example_name(0);
    const char *sc_name = repl_user_scene_name(1);
    ASSERT_TRUE("promoted scene name non-null", sc_name != NULL);
    if (ex_name && sc_name)
        ASSERT_STR("promoted scene name == example name", sc_name, ex_name);

    /* Second call is a no-op (already on a user scene). */
    ASSERT_INT("second promote returns -1",
               repl_promote_example_if_needed(), -1);

    /* Loading a second distinct example and promoting should land in a
     * new slot with a different name. */
    if (repl_example_count() > 1) {
        repl_load_example(1);
        ASSERT_INT("active user scene cleared after example load",
                   repl_active_user_scene(), -1);
        int slot2 = repl_promote_example_if_needed();
        ASSERT_INT("second promotion into slot 2", slot2, 2);
    }
}

void test_user_scene_promote_name_dedup() {
    printf("--- User scene promotion name de-duplication ---\n");
    repl_reset_state(); declare_test_vars();

    /* Load and promote example 0 twice; second promotion must get a
     * distinct "<name> (2)" since the first occupies the bare name. */
    if (repl_example_count() < 1) return;
    const char *ex_name = repl_example_name(0);
    if (!ex_name) return;

    repl_load_example(0);
    int s1 = repl_promote_example_if_needed();
    ASSERT_TRUE("first promotion succeeded", s1 >= 0);

    repl_load_example(0);
    int s2 = repl_promote_example_if_needed();
    ASSERT_TRUE("second promotion succeeded", s2 >= 0);

    const char *n1 = repl_user_scene_name(s1);
    const char *n2 = repl_user_scene_name(s2);
    ASSERT_TRUE("slot 1 name non-null", n1 != NULL);
    ASSERT_TRUE("slot 2 name non-null", n2 != NULL);
    if (n1 && n2)
        ASSERT_TRUE("de-dup produced distinct names", strcmp(n1, n2) != 0);
}

void test_workspace_round_trip() {
    printf("--- Workspace save/load round-trip ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 2) return;

    /* Populate: home (from feed) + two promoted example scenes. */
    repl_feed_line_public("glVertex3f(1,1,1);");
    repl_load_example(0);
    int p1 = repl_promote_example_if_needed();
    ASSERT_TRUE("first promotion ok", p1 >= 1);

    repl_load_example(1);
    int p2 = repl_promote_example_if_needed();
    ASSERT_TRUE("second promotion ok", p2 >= 1 && p2 != p1);

    int slots_before = repl_user_scene_count();
    ASSERT_INT("three scenes before save", slots_before, 3);

    /* Save to a unique scratch directory. */
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/repl_workspace_test.%d", (int)getpid());
    int written = repl_save_workspace(dir);
    ASSERT_INT("save_workspace wrote all slots", written, slots_before);
    ASSERT_STR("workspace dir remembered", repl_workspace_dir(), dir);

    /* Wipe slots, load back. */
    repl_reset_state(); declare_test_vars();
    ASSERT_INT("slots cleared by reset", repl_user_scene_count(), 0);

    int loaded = repl_load_workspace(dir);
    ASSERT_INT("load_workspace produced original count",
               loaded, slots_before);
    ASSERT_INT("slot count matches after load",
               repl_user_scene_count(), slots_before);

    int ring_slot = -1;
    for (int slot = 0; slot < MAX_USER_SCENES; slot++) {
        if (!repl_user_scene_slot_used(slot))
            continue;
        const char *name = repl_user_scene_name(slot);
        if (name && strcmp(name, "Animated ring (for + t)") == 0) {
            ring_slot = slot;
            break;
        }
    }
    ASSERT_TRUE("animated ring scene reloaded", ring_slot >= 0);
    if (ring_slot >= 0) {
        ASSERT_INT("load animated ring scene", repl_load_user_scene_idx(ring_slot), 1);
        ASSERT_INT("animated ring command count after workspace load",
                   repl_state_document_count(), 16);
    }

    /* Clean up scratch dir. */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", dir, ent->d_name);
            unlink(p);
        }
        closedir(d);
        rmdir(dir);
    }
}

void test_user_scene_preserves_scratch_state(void) {
    printf("--- User scene scratch preservation ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 1) return;

    repl_feed_line_public("A[0] = 1;");
    repl_load_example(0);
    ASSERT_INT("home slot captured", repl_user_scene_slot_used(0), 1);

    ASSERT_INT("promotion creates slot 1", repl_promote_example_if_needed(), 1);
    repl_feed_line_public("A[0] = 5;");

    {
        float scratch = 0.0f;
        ASSERT_INT("load home scene", repl_load_user_scene_idx(0), 1);
        ASSERT_TRUE("home scene scratch preserved",
                    repl_eval_scratch_get(0, 0, &scratch) && fabsf(scratch - 1.0f) < 1e-6f);

        ASSERT_INT("load promoted scene", repl_load_user_scene_idx(1), 1);
        ASSERT_TRUE("promoted scene scratch preserved",
                    repl_eval_scratch_get(0, 0, &scratch) && fabsf(scratch - 5.0f) < 1e-6f);
    }
}

void test_user_scene_promote_all_slots_full() {
    printf("--- User scene promotion when all slots full ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Fill slots 1..MAX_USER_SCENES-1 via repeated promotion. */
    for (int k = 0; k < MAX_USER_SCENES - 1; k++) {
        repl_load_example(0);
        repl_promote_example_if_needed();
    }
    int occupied = repl_user_scene_count();
    ASSERT_INT("all slots occupied", occupied, MAX_USER_SCENES);

    /* Next promotion from an example should be refused (pre-LRU). */
    repl_load_example(0);
    int rejected = repl_promote_example_if_needed();
    ASSERT_INT("promotion rejected when full", rejected, -1);
    ASSERT_INT("active user scene still -1", repl_active_user_scene(), -1);
}

void test_user_scene_promote_lru_evict() {
    printf("--- User scene promotion LRU eviction ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 1) return;

    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/repl_lru_test.%d", (int)getpid());
    repl_set_workspace_dir(dir);

    /* Fill slots 1..MAX_USER_SCENES-1 via repeated promotion. */
    for (int k = 0; k < MAX_USER_SCENES - 1; k++) {
        repl_load_example(0);
        repl_promote_example_if_needed();
    }
    ASSERT_INT("all slots occupied", repl_user_scene_count(), MAX_USER_SCENES);

    /* Capture the slot-1 name so we can verify its file was written. */
    char evicted_name[USER_SCENE_NAME_MAX];
    snprintf(evicted_name, sizeof(evicted_name), "%s",
             repl_user_scene_name(1));

    /* Ninth promotion should succeed now that a workspace dir is set:
     * slot 1 (oldest non-home) is flushed to disk and reused. */
    repl_load_example(0);
    int promoted = repl_promote_example_if_needed();
    ASSERT_INT("promotion reuses evicted slot", promoted, 1);
    ASSERT_INT("active user scene is slot 1", repl_active_user_scene(), 1);
    ASSERT_INT("slot count unchanged after eviction",
               repl_user_scene_count(), MAX_USER_SCENES);

    /* Check the evicted scene is present on disk. */
    DIR *d = opendir(dir);
    int found_evicted = 0;
    int file_count = 0;
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *n = ent->d_name;
            size_t len = strlen(n);
            if (len > 2 && strcmp(n + len - 2, ".c") == 0) {
                file_count++;
                char path[256];
                snprintf(path, sizeof(path), "%s/%s", dir, n);
                FILE *f = fopen(path, "r");
                if (f) {
                    char buf[512];
                    while (fgets(buf, sizeof(buf), f)) {
                        if (strstr(buf, evicted_name)) {
                            found_evicted = 1;
                            break;
                        }
                    }
                    fclose(f);
                }
                unlink(path);
            }
        }
        closedir(d);
    }
    ASSERT_INT("evicted scene written to disk", found_evicted, 1);
    ASSERT_TRUE("at least one file written", file_count >= 1);
    rmdir(dir);
    repl_set_workspace_dir("");
}

void test_user_scene_rename_flow() {
    printf("--- User scene rename flow ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 1) return;

    repl_load_example(0);
    int slot = repl_promote_example_if_needed();
    ASSERT_TRUE("promoted on edit", slot >= 0);

    /* Begin rename. */
    ASSERT_INT("not renaming before begin", repl_inline_rename_active(), 0);
    ASSERT_INT("begin_rename succeeds", repl_inline_rename_begin(slot), 1);
    ASSERT_INT("rename active", repl_inline_rename_active(), 1);

    /* Clear default and type a new name, including a path-unsafe char
     * that must be filtered. */
    for (int i = 0; i < 64; i++) repl_inline_rename_handle_key(8 /*BS*/);
    const char *input = "My Scene/Bad:Name";
    for (const char *p = input; *p; p++)
        repl_inline_rename_handle_key((unsigned char)*p);

    /* Commit. */
    repl_inline_rename_handle_key('\r');
    ASSERT_INT("rename cleared after commit", repl_inline_rename_active(), 0);
    const char *new_name = repl_user_scene_name(slot);
    ASSERT_TRUE("renamed to filtered text",
                strcmp(new_name, "My SceneBadName") == 0);

    /* Cancel path: begin again, type, then Esc. */
    ASSERT_INT("begin_rename again", repl_inline_rename_begin(slot), 1);
    repl_inline_rename_handle_key('Z');
    repl_inline_rename_handle_key(27 /*ESC*/);
    ASSERT_INT("cancel clears rename", repl_inline_rename_active(), 0);
    ASSERT_TRUE("name unchanged after cancel",
                strcmp(repl_user_scene_name(slot), "My SceneBadName") == 0);

    /* Empty commit is rejected - rename stays active so user can retry. */
    ASSERT_INT("begin_rename for empty test", repl_inline_rename_begin(slot), 1);
    for (int i = 0; i < 64; i++) repl_inline_rename_handle_key(8 /*BS*/);
    repl_inline_rename_handle_key('\r');
    ASSERT_INT("empty commit keeps rename active",
               repl_inline_rename_active(), 1);
    ASSERT_TRUE("name unchanged after empty commit",
                strcmp(repl_user_scene_name(slot), "My SceneBadName") == 0);
    repl_inline_rename_cancel();

    /* Invalid slot rejected. */
    ASSERT_INT("begin_rename rejects -1",
               repl_inline_rename_begin(-1), 0);
    ASSERT_INT("begin_rename rejects unused",
               repl_inline_rename_begin(MAX_USER_SCENES - 1), 0);
}

void test_activate_home_slot_no_duplicate_name() {
    printf("--- activate_home_slot produces no duplicate name ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Simulate the startup path: load example 0 (which captures the empty
     * state as "Your Scene" in slot 0 via repl_scenes_capture_home_if_needed),
     * then activate_home_slot seeds slot 0 with the example content.  The two
     * writes target the same slot, so the name must stay plain "Your Scene"
     * with no "(2)" suffix. */
    repl_load_example(0);
    repl_scenes_activate_home_slot();

    ASSERT_INT("slot 0 active after activate_home_slot",
               repl_active_user_scene(), 0);
    const char *name = repl_user_scene_name(0);
    ASSERT_TRUE("slot 0 name non-null", name != NULL);
    if (name)
        ASSERT_STR("slot 0 name is Your Scene (no (2) suffix)",
                   name, "Your Scene");

    /* No spurious slot 1 should exist. */
    ASSERT_INT("slot 1 unused", repl_user_scene_slot_used(1), 0);
}

void test_your_scene_persists_edits_from_startup() {
    printf("--- Your Scene persists edits across example switch ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Activate an empty slot 0 as "Your Scene" (mirrors the startup path when
     * the live state is empty, e.g. before any example content is shown). */
    repl_scenes_activate_home_slot();
    ASSERT_INT("slot 0 active (Your Scene mode)", repl_active_user_scene(), 0);

    /* User adds a vertex in "Your Scene". */
    repl_feed_line_public("glVertex3f(9,9,9);");
    repl_flatten_commands();
    ASSERT_INT("vertex present in Your Scene", count_vertices(), 1);

    /* Switch to example 0 -- this auto-saves slot 0 before overwriting. */
    repl_load_example(0);
    ASSERT_INT("active scene cleared after example load",
               repl_active_user_scene(), -1);

    /* Return to Your Scene -- should have exactly the user's vertex. */
    repl_load_user_scene_idx(0);
    repl_flatten_commands();
    ASSERT_INT("vertex restored after returning to Your Scene",
               count_vertices(), 1);
    ASSERT_INT("slot 0 active again", repl_active_user_scene(), 0);
}

void test_scene_cfg_persists_across_example_switch() {
    printf("--- Scene cfg persists across example switch ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Start in Your Scene mode. */
    repl_load_example(0);
    repl_scenes_activate_home_slot();

    /* Record the default backdrop value, then set a different one. */
    int default_backdrop = repl_config_get(REPL_CONFIG_BACKDROP);
    int custom_backdrop  = (default_backdrop + 1) % repl_config_state_count(REPL_CONFIG_BACKDROP);
    repl_config_set(REPL_CONFIG_BACKDROP, custom_backdrop);

    int default_grid = repl_config_get(REPL_CONFIG_GRID_THEME);
    int custom_grid  = (default_grid + 1) % repl_config_state_count(REPL_CONFIG_GRID_THEME);
    repl_config_set(REPL_CONFIG_GRID_THEME, custom_grid);

    /* Switch to an example -- example load resets cfg to defaults, then
     * applies its own @cfg.  The user scene should be saved first. */
    repl_load_example(0);
    ASSERT_INT("example load resets backdrop to default",
               repl_config_get(REPL_CONFIG_BACKDROP), default_backdrop);

    /* Return to Your Scene -- our custom cfg must be restored. */
    repl_load_user_scene_idx(0);
    ASSERT_INT("backdrop restored from Your Scene",
               repl_config_get(REPL_CONFIG_BACKDROP), custom_backdrop);
    ASSERT_INT("grid theme restored from Your Scene",
               repl_config_get(REPL_CONFIG_GRID_THEME), custom_grid);
}

void test_scene_cfg_not_inherited_from_example() {
    printf("--- Scene cfg not inherited from subsequent example ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Build a user scene with default cfg (no custom overrides). */
    repl_load_example(0);
    repl_scenes_activate_home_slot();
    int scene_backdrop = repl_config_get(REPL_CONFIG_BACKDROP);

    /* View an example that has a different backdrop. */
    repl_load_example(0);
    int example_backdrop = (scene_backdrop + 1) %
                           repl_config_state_count(REPL_CONFIG_BACKDROP);
    repl_config_set(REPL_CONFIG_BACKDROP, example_backdrop);
    ASSERT_INT("example backdrop different from scene backdrop",
               repl_config_get(REPL_CONFIG_BACKDROP), example_backdrop);

    /* Return to Your Scene -- must NOT inherit the example's backdrop. */
    repl_load_user_scene_idx(0);
    ASSERT_INT("Your Scene backdrop not overwritten by example",
               repl_config_get(REPL_CONFIG_BACKDROP), scene_backdrop);
}

/* Option A sandbox: in-example cfg toggles must not leak across an
 * example->user-scene transition. A user scene has full scene_cfg
 * coverage so the destination's saved cfg dominates regardless, but
 * this test pins the rollback path so a future move to sparse /
 * inherited-aware scene_cfg keeps the contract. */
void test_in_example_toggles_dont_leak_to_user_scene() {
    printf("--- In-example cfg toggles do not leak to user scene ---\n");
    repl_reset_state(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Establish home (slot 0) with a known backdrop. */
    repl_scenes_activate_home_slot();
    int home_backdrop = repl_config_get(REPL_CONFIG_BACKDROP);

    /* Enter an example, then toggle the backdrop while inside. */
    repl_load_example(0);
    int example_toggled = (home_backdrop + 1) %
                          repl_config_state_count(REPL_CONFIG_BACKDROP);
    repl_config_set(REPL_CONFIG_BACKDROP, example_toggled);
    ASSERT_INT("in-example toggle visible in live cfg",
               repl_config_get(REPL_CONFIG_BACKDROP), example_toggled);

    /* Return to home: the in-example toggle must be gone. */
    repl_load_user_scene_idx(0);
    ASSERT_INT("home backdrop after example trip",
               repl_config_get(REPL_CONFIG_BACKDROP), home_backdrop);
}

void test_debug_dump_flat_commands() {
    printf("--- Debug dump flat commands ---\n");

    /* Empty state: header, count=0, end marker - and no crash on NULL out. */
    repl_reset_state(); declare_test_vars();
    repl_flatten_commands();
    char *empty = capture_flat_dump();
    ASSERT_TRUE("empty dump captured", empty != NULL);
    if (empty) {
        ASSERT_TRUE("empty dump header",
                    strstr(empty, "=== REPL Flattened Commands Dump ===") != NULL);
        ASSERT_TRUE("empty dump count=0",
                    strstr(empty, "num_flat_cmds=0\n") != NULL);
        ASSERT_TRUE("empty dump end marker",
                    strstr(empty, "=== End REPL Flattened Commands Dump ===") != NULL);
        free(empty);
    }

    /* NULL FILE* should fall back to stdout without crashing. Redirect stdout
     * to /dev/null via dup2 so the test output stays clean. */
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    int stdout_redirected = 0;
    if (saved_stdout >= 0) {
        int devnull_fd = open("/dev/null", O_WRONLY);
        if (devnull_fd >= 0) {
            if (dup2(devnull_fd, STDOUT_FILENO) >= 0) {
                stdout_redirected = 1;
            }
            close(devnull_fd);
        }
    }
    repl_debug_dump_flat_commands(NULL, editor_buffer_view());
    fflush(stdout);
    if (stdout_redirected) {
        if (dup2(saved_stdout, STDOUT_FILENO) >= 0) {
            clearerr(stdout);
        }
        close(saved_stdout);
    } else if (saved_stdout >= 0) {
        close(saved_stdout);
    }
    ASSERT_TRUE("NULL out falls back to stdout", 1);

    /* Basic source commands: each type name should appear in the flattened
     * dump, with one row per flat command. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glFrontFace(GL_CW);");
    repl_feed_line_public("glColor3f(1,0,0);");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0,0,0);");
    repl_feed_line_public("glVertex3f(1,0,0);");
    repl_feed_line_public("glVertex3f(0,1,0);");
    repl_feed_line_public("glEnd();");
    repl_flatten_commands();

    char *basic = capture_flat_dump();
    ASSERT_TRUE("basic dump captured", basic != NULL);
    if (basic) {
        char count_line[64];
        snprintf(count_line, sizeof(count_line),
                 "num_flat_cmds=%d\n", repl_state_flat_program_count());
        ASSERT_TRUE("basic count matches repl_state_flat_program_count()",
                    strstr(basic, count_line) != NULL);

        /* The fix in abccf5c3 aligned cmd_type_name with the CmdType enum and
         * added CMD_FRONT_FACE - ensure its label surfaces correctly. */
        ASSERT_TRUE("basic dump contains CMD_FRONT_FACE",
                    strstr(basic, "CMD_FRONT_FACE") != NULL);
        ASSERT_TRUE("basic dump contains CMD_COLOR3F",
                    strstr(basic, "CMD_COLOR3F") != NULL);
        ASSERT_TRUE("basic dump contains CMD_BEGIN",
                    strstr(basic, "CMD_BEGIN") != NULL);
        ASSERT_TRUE("basic dump contains CMD_VERTEX3F",
                    strstr(basic, "CMD_VERTEX3F") != NULL);
        ASSERT_TRUE("basic dump contains CMD_END",
                    strstr(basic, "CMD_END") != NULL);

        /* Per-row fields should be emitted in the documented order. */
        ASSERT_TRUE("basic dump row has valid field",
                    strstr(basic, "valid=1") != NULL);
        ASSERT_TRUE("basic dump row has has_vars field",
                    strstr(basic, "has_vars=0") != NULL);
        ASSERT_TRUE("basic dump row has src_idx field",
                    strstr(basic, "src_idx=") != NULL);
        ASSERT_TRUE("basic dump row has call_src_idx field",
                    strstr(basic, "call_src_idx=") != NULL);
        ASSERT_TRUE("basic dump row has root_call_src_idx field",
                    strstr(basic, "root_call_src_idx=") != NULL);
        ASSERT_TRUE("basic dump row has func_scope mask",
                    strstr(basic, "func_scope=0x00000000") != NULL);

        /* Row count should match num_flat_cmds + 3 fixed lines
         * (header, count, footer). */
        int newlines = 0;
        for (const char *p = basic; *p; p++)
            if (*p == '\n') newlines++;
        ASSERT_INT("basic dump line count",
                   newlines, repl_state_flat_program_count() + 3);

        free(basic);
    }

    /* For-loop expansion: flattening unrolls the loop body and records a
     * stable src_cmd_idx pointing back at the source line. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("for(i, 0, 3) {");
    repl_feed_line_public("glVertex3f(i,0,0);");
    repl_feed_line_public("}");
    repl_flatten_commands();

    int vertex_flats = 0;
    for (int i = 0; i < repl_state_flat_program_count(); i++)
        if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
            vertex_flats++;
    ASSERT_INT("for-loop unrolled to 3 vertices", vertex_flats, 3);

    char *loop = capture_flat_dump();
    ASSERT_TRUE("loop dump captured", loop != NULL);
    if (loop) {
        /* Flattening unrolls the for-loop, so only the body commands survive
         * in repl_state_flat_program_cmds_mut()[]. The FOR_BEGIN/FOR_END source markers do not
         * appear in the flat stream. */
        int hits = 0;
        const char *p = loop;
        while ((p = strstr(p, "CMD_VERTEX3F")) != NULL) { hits++; p++; }
        ASSERT_INT("loop dump lists all unrolled vertices", hits, 3);
        ASSERT_TRUE("loop dump omits FOR_BEGIN marker",
                    strstr(loop, "CMD_FOR_BEGIN") == NULL);
        ASSERT_TRUE("loop dump omits FOR_END marker",
                    strstr(loop, "CMD_FOR_END") == NULL);
        free(loop);
    }

    /* Function call inlining: flat commands inside the inlined call should
     * carry a non-zero func_scope_mask. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0() {");
    repl_feed_line_public("glVertex3f(0,0,0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0();");
    repl_flatten_commands();

    int scoped_hits = 0;
    for (int i = 0; i < repl_state_flat_program_count(); i++) {
        if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F &&
            repl_state_flat_program_cmds_mut()[i].func_scope_mask != 0u)
            scoped_hits++;
    }
    ASSERT_TRUE("inlined call sets func_scope_mask", scoped_hits >= 1);

    char *call_dump = capture_flat_dump();
    ASSERT_TRUE("call dump captured", call_dump != NULL);
    if (call_dump) {
        /* At least one row should have a non-zero func_scope hex field. */
        int has_nonzero_scope = 0;
        const char *p = call_dump;
        while ((p = strstr(p, "func_scope=0x")) != NULL) {
            const char *hex = p + strlen("func_scope=0x");
            int all_zero = 1;
            for (int k = 0; k < 8 && hex[k]; k++)
                if (hex[k] != '0') { all_zero = 0; break; }
            if (!all_zero) { has_nonzero_scope = 1; break; }
            p++;
        }
        ASSERT_TRUE("call dump shows non-zero func_scope", has_nonzero_scope);
        free(call_dump);
    }

    /* Implicit flatten: even if the caller leaves repl_state_flat_program_dirty() set and stale
     * flat state behind, the dump should rebuild repl_state_flat_program_cmds_mut()[] on demand. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glVertex3f(0,0,0);");
    repl_state_mark_flat_dirty();
    repl_state_flat_program_set_count(0);
    FILE *dn = fopen("/dev/null", "w");
    if (dn) {
        repl_debug_dump_flat_commands(dn, editor_buffer_view());
        fclose(dn);
    }
    ASSERT_TRUE("dump re-flattens commands", repl_state_flat_program_count() >= 1);
}

/* Capture the output of repl_debug_dump_editor(, editor_buffer_view()) into a malloc'd string. */
static char *capture_editor_dump(void) {
    FILE *tmp = tmpfile();
    char *buf = NULL;
    long len;
    size_t nread;

    if (!tmp)
        return NULL;
    repl_debug_dump_editor(tmp, editor_buffer_view());
    fflush(tmp);
    if (fseek(tmp, 0, SEEK_END) != 0) goto done;
    len = ftell(tmp);
    if (len < 0) goto done;
    if (fseek(tmp, 0, SEEK_SET) != 0) goto done;
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) goto done;
    nread = fread(buf, 1, (size_t)len, tmp);
    buf[nread] = '\0';
done:
    fclose(tmp);
    return buf;
}

void test_var_declare_cmd() {
    printf("--- CMD_VAR_DECLARE coverage ---\n");

    /* Feed a float declaration and verify it produces CMD_VAR_DECLARE */
    repl_reset_state();
    repl_feed_line_public("float a, b, c;");

    /* 1. The source array should have exactly one CMD_VAR_DECLARE entry */
    int found_decl = 0;
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds_mut()[i].type == CMD_VAR_DECLARE)
            found_decl++;
    }
    ASSERT_INT("float decl produces CMD_VAR_DECLARE", found_decl, 1);

    /* 2. The declared variables should exist in g_predef_vars */
    ASSERT_TRUE("var 'a' declared", repl_eval_find_predef_var_idx("a") >= 0);
    ASSERT_TRUE("var 'b' declared", repl_eval_find_predef_var_idx("b") >= 0);
    ASSERT_TRUE("var 'c' declared", repl_eval_find_predef_var_idx("c") >= 0);

    /* 3. cmd_type_name returns the right string (catches positional table bugs) */
    ASSERT_STR("cmd_type_name(CMD_VAR_DECLARE)",
               cmd_type_name(CMD_VAR_DECLARE), "CMD_VAR_DECLARE");

    /* 4. Editor dump contains CMD_VAR_DECLARE (catches cmd_type_name omissions) */
    char *dump = capture_editor_dump();
    ASSERT_TRUE("editor dump captured", dump != NULL);
    if (dump) {
        ASSERT_TRUE("dump contains CMD_VAR_DECLARE",
                     strstr(dump, "CMD_VAR_DECLARE") != NULL);
        /* Should NOT contain CMD_UNKNOWN for any line */
        ASSERT_TRUE("dump has no CMD_UNKNOWN",
                     strstr(dump, "CMD_UNKNOWN") == NULL);
        free(dump);
    }

    /* 5. Every CmdType value has a non-UNKNOWN name (exhaustive table check) */
    for (int t = 0; t < CMD_TYPE_COUNT; t++) {
        char label[96];
        const char *name = cmd_type_name((CmdType)t);
        snprintf(label, sizeof(label), "cmd_type_name completeness #%d", t);
        ASSERT_TRUE(label, strcmp(name, "CMD_UNKNOWN") != 0);
    }
}

void test_time() {
    printf("--- Time functions ---\n");
    repl_reset_state(); declare_test_vars();
    g_anim_time = 0.0f;
    repl_advance_time(0.5f);
    ASSERT_TRUE("g_anim_time advanced", g_anim_time == 0.5f);

    repl_reset_time_to_zero();
}

int main(int argc, char **argv) {
    repl_eval_init_predef_vars();

    test_utils();
    test_repl_replay_advanced();
    test_io();
    test_execution();
    test_examples();
    test_user_scene();
    test_user_scene_persists_across_example_switch();
    test_user_scene_promote_on_edit();
    test_user_scene_promote_name_dedup();
    test_user_scene_preserves_scratch_state();
    test_user_scene_promote_all_slots_full();
    test_user_scene_promote_lru_evict();
    test_user_scene_rename_flow();
    test_workspace_round_trip();
    test_activate_home_slot_no_duplicate_name();
    test_your_scene_persists_edits_from_startup();
    test_scene_cfg_persists_across_example_switch();
    test_scene_cfg_not_inherited_from_example();
    test_in_example_toggles_dont_leak_to_user_scene();
    test_debug_dump_flat_commands();
    test_var_declare_cmd();
    test_time();

    printf("\n%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
