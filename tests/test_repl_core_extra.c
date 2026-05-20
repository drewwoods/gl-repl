#include "editor/state.h"
#include "app/glr_ctrl.h"
#define _DEFAULT_SOURCE
#include "app/glr_config.h"
#include "config.h"                  /* DEFAULT_SCENE_FILE */
#include "repl/command_spec.h"  /* cmd_type_name */
#include "repl/core.h"
#include "editor/input.h"
#include "repl/core_internal.h"
#include "app/glr_debug.h"
#include "widgets/replay.h"
#include "repl/executor.h"
#include "repl/state.h"
#include "ui/panels.h"
#include "editor/inline_file_prompt.h"
#include "editor/inline_rename.h"
#include "editor/undo.h"

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
const char *repl_mode_name(GLenum mode);
int repl_source_scope_in_begin_block(void);
int repl_source_scope_cmd_indent_chars(int pos);
GLenum repl_current_begin_mode(void);
int repl_count_vertices(void);
extern int repl_state_flat_program_count();

/* Capture the output of glr_debug_dump_flat_commands(, editor_buffer_view()) into a malloc'd string.
 * Returns NULL on failure; caller frees the buffer. */
static char *capture_flat_dump(void) {
    FILE *tmp = tmpfile();
    char *buf = NULL;
    long len;
    size_t nread;

    if (!tmp)
        return NULL;
    glr_debug_dump_flat_commands(tmp, editor_buffer_view());
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

    ASSERT_STR("mode_name(GL_POINTS)", repl_mode_name(GL_POINTS), "GL_POINTS");
    ASSERT_STR("mode_name(GL_TRIANGLES)", repl_mode_name(GL_TRIANGLES), "GL_TRIANGLES");
    ASSERT_STR("mode_name(unknown)", repl_mode_name(9999), "???");

    glr_app_reset_all(); declare_test_vars();
    ASSERT_INT("count_vertices initial", repl_count_vertices(), 0);
    ASSERT_INT("current_begin_mode initial", repl_current_begin_mode(), GL_TRIANGLES);
    ASSERT_INT("in_begin_block initial", repl_source_scope_in_begin_block(), 0);

    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("glVertex3f(1,0,0);");
    repl_flatten_commands();

    ASSERT_INT("count_vertices after 2 vtx", repl_count_vertices(), 2);
    ASSERT_INT("current_begin_mode in block", repl_current_begin_mode(), GL_TRIANGLES);
    ASSERT_INT("in_begin_block in block", repl_source_scope_in_begin_block(), 1);

    editor_feed_line("glEnd();");
    repl_flatten_commands();
    ASSERT_INT("current_begin_mode after end", repl_current_begin_mode(), GL_TRIANGLES);
    ASSERT_INT("in_begin_block after end", repl_source_scope_in_begin_block(), 0);

    /* cmd_indent_chars */
    ASSERT_INT("cmd_indent_chars at 0", repl_source_scope_cmd_indent_chars(0), 2);
    ASSERT_INT("cmd_indent_chars at 1", repl_source_scope_cmd_indent_chars(1), 4);
    ASSERT_INT("cmd_indent_chars at 4", repl_source_scope_cmd_indent_chars(4), 2);

    /* debug dump */
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) {
        glr_debug_dump_editor(devnull, source_document_view());
        fclose(devnull);
    }
}

void test_repl_replay_advanced() {
    printf("--- Replay advanced functions ---\n");
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("glVertex3f(1,1,1);");
    editor_feed_line("glVertex3f(2,2,2);");
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

    replay_start();
    ASSERT_INT("replay_exec_limit start", replay_exec_limit(), 0);

    replay_advance();
    ASSERT_INT("replay_exec_limit advance 1", replay_exec_limit(), 1);

    replay_advance();
    ASSERT_INT("replay_exec_limit advance 2", replay_exec_limit(), 2);

    replay_step_back();
    ASSERT_INT("replay_exec_limit step back", replay_exec_limit(), 1);

    replay_seek_to_src_line(2);
    ASSERT_INT("replay_exec_limit seek_to_src_line(2)", replay_exec_limit(), 3);

    replay_restart_from_beginning();
    ASSERT_INT("replay_exec_limit restart", replay_exec_limit(), 0);

    replay_stop();
}

void test_io() {
    printf("--- IO functions ---\n");
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(1,2,3);");

    const char *tmpf = "/tmp/test_repl_core_extra_io.c";
    repl_export_save_output(tmpf, source_document_view(), NULL);

    glr_app_reset_all(); declare_test_vars();
    ASSERT_INT("num_cmds after reset", repl_count_vertices(), 0);

    int r = repl_export_load_from_file(tmpf);
    ASSERT_INT("load_from_file return", r, 1);
    repl_flatten_commands();
    ASSERT_INT("count_vertices after load", repl_count_vertices(), 1);

    unlink(tmpf);

    repl_load_initial_commands(NULL);
    const char *save_path = "/tmp/test_repl_core_extra_default_output.c";
    repl_export_save_output(save_path, source_document_view(), NULL);
    ASSERT_INT("default-path save creates file",
               access(save_path, F_OK), 0);
    unlink(save_path);
}

void test_execution() {
    printf("--- Execution functions ---\n");
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("n = 1;");
    repl_flatten_commands();

    repl_execute_commands();
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
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(1,1,1);");

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
    ASSERT_INT("count_vertices after restore", repl_count_vertices(), 1);
    ASSERT_INT("user_scene_valid after restore", repl_user_scene_valid(), 1);
    ASSERT_INT("active user scene == 0 after restore",
               repl_active_user_scene(), 0);
}

void test_user_scene_persists_across_example_switch() {
    printf("--- User scene persists across example switch ---\n");
    glr_app_reset_all(); declare_test_vars();
    if (repl_example_count() < 1) return;

    editor_feed_line("glVertex3f(1,1,1);");
    repl_load_example(0);
    ASSERT_INT("home slot used after example load", repl_user_scene_slot_used(0), 1);

    repl_load_user_scene();
    ASSERT_INT("active user scene after restore", repl_active_user_scene(), 0);

    editor_feed_line("glVertex3f(2,2,2);");
    repl_flatten_commands();
    ASSERT_INT("edited home scene vertex count", repl_count_vertices(), 2);

    repl_load_example(0);
    repl_load_user_scene();
    repl_flatten_commands();
    ASSERT_INT("persisted home scene vertex count", repl_count_vertices(), 2);
}

void test_user_scene_promote_on_edit() {
    printf("--- User scene promotion on example edit ---\n");
    glr_app_reset_all(); declare_test_vars();

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
    glr_app_reset_all(); declare_test_vars();

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
    glr_app_reset_all(); declare_test_vars();
    if (repl_example_count() < 2) return;

    /* Populate: home (from feed) + two promoted example scenes. */
    editor_feed_line("glVertex3f(1,1,1);");
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
    int written = repl_save_workspace(dir, NULL);
    ASSERT_INT("save_workspace wrote all slots", written, slots_before);
    ASSERT_STR("workspace dir remembered", repl_workspace_dir(), dir);

    /* Wipe slots, load back. */
    glr_app_reset_all(); declare_test_vars();
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
    glr_app_reset_all(); declare_test_vars();
    if (repl_example_count() < 1) return;

    editor_feed_line("A[0] = 1;");
    repl_load_example(0);
    ASSERT_INT("home slot captured", repl_user_scene_slot_used(0), 1);

    ASSERT_INT("promotion creates slot 1", repl_promote_example_if_needed(), 1);
    editor_feed_line("A[0] = 5;");

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
    glr_app_reset_all(); declare_test_vars();
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
    glr_app_reset_all(); declare_test_vars();
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
    glr_app_reset_all(); declare_test_vars();
    if (repl_example_count() < 1) return;

    repl_load_example(0);
    int slot = repl_promote_example_if_needed();
    ASSERT_TRUE("promoted on edit", slot >= 0);

    /* Begin rename. */
    ASSERT_INT("not renaming before begin", editor_inline_rename_active(), 0);
    ASSERT_INT("begin_rename succeeds", editor_inline_rename_begin(slot), 1);
    ASSERT_INT("rename active", editor_inline_rename_active(), 1);

    /* Clear default and type a new name, including a path-unsafe char
     * that must be filtered. */
    for (int i = 0; i < 64; i++) editor_inline_rename_handle_key(8 /*BS*/);
    const char *input = "My Scene/Bad:Name";
    for (const char *p = input; *p; p++)
        editor_inline_rename_handle_key((unsigned char)*p);

    /* Commit. */
    editor_inline_rename_handle_key('\r');
    ASSERT_INT("rename cleared after commit", editor_inline_rename_active(), 0);
    const char *new_name = repl_user_scene_name(slot);
    ASSERT_TRUE("renamed to filtered text",
                strcmp(new_name, "My SceneBadName") == 0);

    /* Cancel path: begin again, type, then Esc. */
    ASSERT_INT("begin_rename again", editor_inline_rename_begin(slot), 1);
    editor_inline_rename_handle_key('Z');
    editor_inline_rename_handle_key(27 /*ESC*/);
    ASSERT_INT("cancel clears rename", editor_inline_rename_active(), 0);
    ASSERT_TRUE("name unchanged after cancel",
                strcmp(repl_user_scene_name(slot), "My SceneBadName") == 0);

    /* Empty commit is rejected - rename stays active so user can retry. */
    ASSERT_INT("begin_rename for empty test", editor_inline_rename_begin(slot), 1);
    for (int i = 0; i < 64; i++) editor_inline_rename_handle_key(8 /*BS*/);
    editor_inline_rename_handle_key('\r');
    ASSERT_INT("empty commit keeps rename active",
               editor_inline_rename_active(), 1);
    ASSERT_TRUE("name unchanged after empty commit",
                strcmp(repl_user_scene_name(slot), "My SceneBadName") == 0);
    editor_inline_rename_cancel();

    /* Invalid slot rejected. */
    ASSERT_INT("begin_rename rejects -1",
               editor_inline_rename_begin(-1), 0);
    ASSERT_INT("begin_rename rejects unused",
               editor_inline_rename_begin(MAX_USER_SCENES - 1), 0);
}

void test_inline_file_prompt_flow() {
    printf("--- Inline file-load prompt flow ---\n");

    /* --- Lifecycle: begin seeds buffer; cancel clears it -------------- */
    glr_app_reset_all(); declare_test_vars();
    ASSERT_INT("prompt inactive at start",
               editor_inline_file_prompt_active(), 0);
    ASSERT_STR("buffer empty when inactive",
               editor_inline_file_prompt_buffer(), "");

    ASSERT_INT("begin succeeds with default",
               editor_inline_file_prompt_begin(DEFAULT_SCENE_FILE), 1);
    ASSERT_INT("active after begin",
               editor_inline_file_prompt_active(), 1);
    ASSERT_STR("buffer seeded with default name",
               editor_inline_file_prompt_buffer(), DEFAULT_SCENE_FILE);

    /* begin while active is a no-op (returns 0; buffer unchanged). */
    ASSERT_INT("re-begin while active returns 0",
               editor_inline_file_prompt_begin("other.c"), 0);
    ASSERT_STR("re-begin leaves buffer untouched",
               editor_inline_file_prompt_buffer(), DEFAULT_SCENE_FILE);

    editor_inline_file_prompt_cancel();
    ASSERT_INT("cancel clears active",
               editor_inline_file_prompt_active(), 0);

    /* --- Input filtering ---------------------------------------------- */
    glr_app_reset_all(); declare_test_vars();
    ASSERT_INT("begin (empty default)",
               editor_inline_file_prompt_begin(""), 1);
    /* Mix legal (incl. '.', '/') and rejected characters. */
    const char *typed = "ws/sub.c\"<>|\\";
    for (const char *p = typed; *p; p++)
        editor_inline_file_prompt_handle_key((unsigned char)*p);
    ASSERT_STR("filter accepts '.' and '/'; rejects shell-confusables",
               editor_inline_file_prompt_buffer(), "ws/sub.c");
    /* Backspace works. */
    editor_inline_file_prompt_handle_key(8 /*BS*/);
    ASSERT_STR("backspace deletes last char",
               editor_inline_file_prompt_buffer(), "ws/sub.");
    /* Esc cancels. */
    editor_inline_file_prompt_handle_key(27 /*ESC*/);
    ASSERT_INT("esc cancels",
               editor_inline_file_prompt_active(), 0);

    /* --- Specials are swallowed while active -------------------------- */
    glr_app_reset_all(); declare_test_vars();
    editor_inline_file_prompt_begin("");
    /* The exact special-key codes don't matter; any non-zero key
     * should be consumed (and any zero key still returns 1 because
     * the prompt is the active modal). */
    ASSERT_INT("special swallowed while active",
               editor_inline_file_prompt_handle_special(101 /*F-key*/), 1);
    ASSERT_INT("special returns 0 when inactive",
               (editor_inline_file_prompt_cancel(),
                editor_inline_file_prompt_handle_special(101)), 0);

    /* --- Commit on empty path -> stays open, status set -------------- */
    glr_app_reset_all(); declare_test_vars();
    editor_inline_file_prompt_begin("");
    editor_inline_file_prompt_handle_key('\r');
    ASSERT_INT("empty commit keeps prompt open",
               editor_inline_file_prompt_active(), 1);
    editor_inline_file_prompt_cancel();

    /* --- Regression: missing file surfaces error in-prompt + status -- */
    /* Bug fix: previously the prompt stayed open silently when the
     * file was missing. The strip occludes the regular status bar, so
     * relying on repl_set_status alone left the user with no
     * feedback. Now the error is mirrored into the in-prompt buffer
     * AND repl_set_status, and stat() runs BEFORE any state mutation
     * (inside repl_load_scene_as_new_slot) so a missing-file commit
     * cannot wipe the document. */
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(11,22,33);");
    repl_flatten_commands();
    int verts_before_missing = repl_count_vertices();
    int active_before_missing = repl_active_user_scene();

    /* Stage an undo snapshot so we can verify the failed load does
     * NOT clear the undo ring (C1: undo_clear must happen on success
     * only, not preemptively before the load). */
    editor_undo_push_snapshot();
    EditorUndoRingState ring_before_missing;
    editor_undo_ring_state_capture(&ring_before_missing);
    ASSERT_TRUE("undo ring has at least one snapshot before failed load",
                ring_before_missing.undo_count > 0);

    editor_inline_file_prompt_begin("/tmp/repl_item19_missing_xyz123.c");
    editor_inline_file_prompt_handle_key('\r');
    ASSERT_INT("missing file keeps prompt open",
               editor_inline_file_prompt_active(), 1);
    ASSERT_TRUE("missing-file error visible in prompt",
                strstr(editor_inline_file_prompt_error(),
                       "File not found") != NULL);
    repl_flatten_commands();
    ASSERT_INT("missing file does not wipe document",
               repl_count_vertices(), verts_before_missing);
    ASSERT_INT("missing file does not change active scene",
               repl_active_user_scene(), active_before_missing);

    /* Regression (C1): undo ring must survive a failed load. */
    EditorUndoRingState ring_after_missing;
    editor_undo_ring_state_capture(&ring_after_missing);
    ASSERT_INT("failed load preserves undo ring (regression C1)",
               ring_after_missing.undo_count,
               ring_before_missing.undo_count);

    /* Typing a char clears the stale error. */
    editor_inline_file_prompt_handle_key('x');
    ASSERT_STR("keystroke clears stale error",
               editor_inline_file_prompt_error(), "");
    editor_inline_file_prompt_cancel();

    /* --- Commit on a directory -> stays open, no load ----------------- */
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(7,8,9);");
    repl_flatten_commands();
    int verts_before_dir = repl_count_vertices();
    editor_inline_file_prompt_begin("/tmp");  /* well-known dir */
    editor_inline_file_prompt_handle_key('\r');
    ASSERT_INT("directory keeps prompt open",
               editor_inline_file_prompt_active(), 1);
    repl_flatten_commands();
    ASSERT_INT("directory commit does not replace document",
               repl_count_vertices(), verts_before_dir);
    editor_inline_file_prompt_cancel();

    /* --- Happy path: export, then commit through the prompt ----------- */
    {
        const char *export_path = "/tmp/test_repl_inline_file_prompt.c";
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(1,2,3);");
        editor_feed_line("glVertex3f(4,5,6);");
        repl_export_save_output(export_path, source_document_view(), NULL);

        /* Reset to an empty document (the load must replace it). */
        glr_app_reset_all(); declare_test_vars();
        ASSERT_INT("doc empty before load",
                   repl_count_vertices(), 0);

        editor_inline_file_prompt_begin(export_path);
        editor_inline_file_prompt_handle_key('\r');

        ASSERT_INT("successful load closes prompt",
                   editor_inline_file_prompt_active(), 0);
        ASSERT_STR("no error after successful load",
                   editor_inline_file_prompt_error(), "");
        repl_flatten_commands();
        ASSERT_INT("two vertices loaded via prompt",
                   repl_count_vertices(), 2);

        unlink(export_path);
    }

    /* --- Regression: load creates a NEW scene slot, doesn't replace -- */
    /* Bug fix: previously the load REPLACED the current document
     * in-place. The user expected the file to come in as a NEW user
     * scene in the workspace so the previous scene stays accessible
     * via the scene tabs / F12. */
    {
        const char *src_path = "/tmp/test_repl_inline_file_prompt_load.c";

        /* Build source file: 2 vertices. */
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(1,1,1);");
        editor_feed_line("glVertex3f(2,2,2);");
        repl_export_save_output(src_path, source_document_view(), NULL);

        /* Build a CURRENT scene with different content (5 verts) and
         * promote it so it occupies a real user scene slot rather
         * than the transient state. */
        glr_app_reset_all(); declare_test_vars();
        if (repl_example_count() > 0) {
            repl_load_example(0);
            (void)repl_promote_example_if_needed();
        }
        int existing_count_before = repl_user_scene_count();
        int prev_active = repl_active_user_scene();

        /* Load the source file via the prompt. */
        editor_inline_file_prompt_begin(src_path);
        editor_inline_file_prompt_handle_key('\r');
        repl_flatten_commands();

        ASSERT_INT("load closes prompt on success",
                   editor_inline_file_prompt_active(), 0);
        /* New scene slot was created. */
        ASSERT_TRUE("scene count grew by at least 1",
                    repl_user_scene_count() > existing_count_before);
        /* Active slot is now the new scene, not the previous one. */
        int new_active = repl_active_user_scene();
        ASSERT_TRUE("active scene changed to loaded slot",
                    new_active >= 0 && new_active != prev_active);
        /* Live document is now the loaded content (2 verts). */
        ASSERT_INT("live document is the loaded scene (2 verts)",
                   repl_count_vertices(), 2);

        /* Load into an empty document still works AND allocates a
         * real slot (T1: previously the empty-doc subcase didn't
         * verify the slot was allocated). */
        glr_app_reset_all(); declare_test_vars();
        editor_inline_file_prompt_begin(src_path);
        editor_inline_file_prompt_handle_key('\r');
        repl_flatten_commands();
        ASSERT_INT("load into empty doc closes prompt",
                   editor_inline_file_prompt_active(), 0);
        ASSERT_INT("load into empty doc loads 2 verts",
                   repl_count_vertices(), 2);
        ASSERT_TRUE("load into empty doc allocates a real slot (T1)",
                    repl_active_user_scene() >= 0);

        unlink(src_path);
    }

    /* --- Regression: eviction + parse failure preserves the tab ----- */
    /* Bug P1 (round 2): when all slots are full and a workspace is
     * bound, the load path LRU-evicts a tab to disk. If the
     * subsequent parse failed, the in-memory slot was permanently
     * gone from the tabs (the data was still on disk, but the user
     * would see a tab disappear despite the "Failed to load" error).
     * Fix: snapshot the victim slot before evicting and restore it
     * on parse failure. */
    {
        /* Fill all MAX_USER_SCENES slots and bind a workspace dir. */
        glr_app_reset_all(); declare_test_vars();
        char ws_dir[] = "/tmp/test_repl_evict_restore_XXXXXX";
        const char *made_dir = mkdtemp(ws_dir);
        ASSERT_TRUE("mkdtemp eviction workspace", made_dir != NULL);
        if (made_dir) {
            repl_set_workspace_dir(made_dir);
            /* Synthesize MAX_USER_SCENES scenes by repeatedly loading
             * an example and promoting it. */
            for (int i = 0; i < MAX_USER_SCENES && repl_example_count() > 0; i++) {
                if (repl_user_scene_count() >= MAX_USER_SCENES) break;
                repl_load_example(0);
                editor_feed_line("glVertex3f(0,0,0);");  /* mutate to force promote */
                repl_promote_example_if_needed();
            }
            int filled_count = repl_user_scene_count();
            ASSERT_TRUE("workspace filled to capacity (or close)",
                        filled_count > 0);

            /* Write a *.c file that will fail to parse. */
            const char *bad_path = "/tmp/test_repl_evict_bad.c";
            FILE *f = fopen(bad_path, "w");
            ASSERT_TRUE("opened bad file for write", f != NULL);
            if (f) {
                fputs("this is not a valid REPL scene file\n", f);
                fclose(f);
            }

            /* Attempt the load — expect parse failure. */
            ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
            int new_slot = repl_load_scene_as_new_slot(bad_path, &reason);
            ASSERT_INT("bad-syntax load fails", new_slot, -1);

            /* If we evicted (only matters when all slots were full),
             * the tab count must be unchanged after failure. When the
             * synth couldn't quite fill capacity, this is still a
             * useful invariant: a failed parse never reduces tab count. */
            ASSERT_INT("failed-parse load preserves tab count (P1)",
                       repl_user_scene_count(), filled_count);

            unlink(bad_path);
            repl_set_workspace_dir(NULL);
            /* Best-effort cleanup of the temp workspace dir + files. */
            DIR *d = opendir(made_dir);
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d))) {
                    if (ent->d_name[0] == '.') continue;
                    char p[1024];
                    snprintf(p, sizeof(p), "%s/%s", made_dir, ent->d_name);
                    unlink(p);
                }
                closedir(d);
            }
            rmdir(made_dir);
        }
    }

    /* --- Mutual exclusion: at most one inline modal at a time (C2) -- */
    /* Bug fix: previously, begin'ing one modal while the other was
     * active left both active. The controller's keyboard route checks
     * rename first, so the file prompt would be invisible to
     * keystrokes while still drawing in the snapshot. Fix: each
     * _begin cancels the other modal first. */
    glr_app_reset_all(); declare_test_vars();
    if (repl_example_count() > 0) {
        repl_load_example(0);
        int rename_slot = repl_promote_example_if_needed();
        if (rename_slot >= 0) {
            ASSERT_INT("rename begins",
                       editor_inline_rename_begin(rename_slot), 1);
            ASSERT_INT("rename active",
                       editor_inline_rename_active(), 1);
            /* Opening the file prompt cancels rename. */
            ASSERT_INT("file prompt begins from active rename",
                       editor_inline_file_prompt_begin("x.c"), 1);
            ASSERT_INT("rename cancelled by file prompt begin",
                       editor_inline_rename_active(), 0);
            ASSERT_INT("file prompt now active",
                       editor_inline_file_prompt_active(), 1);

            /* Symmetric: rename begin cancels file prompt. */
            ASSERT_INT("rename begins from active file prompt",
                       editor_inline_rename_begin(rename_slot), 1);
            ASSERT_INT("file prompt cancelled by rename begin",
                       editor_inline_file_prompt_active(), 0);
            ASSERT_INT("rename now active",
                       editor_inline_rename_active(), 1);
            editor_inline_rename_cancel();
        }
    }
}

void test_activate_home_slot_no_duplicate_name() {
    printf("--- activate_home_slot produces no duplicate name ---\n");
    glr_app_reset_all(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Simulate the startup path: load example 0 (which captures the empty
     * state as "My Scene" in slot 0 via repl_scenes_capture_home_if_needed),
     * then activate_home_slot seeds slot 0 with the example content.  The two
     * writes target the same slot, so the name must stay plain "My Scene"
     * with no "(2)" suffix. */
    repl_load_example(0);
    repl_scenes_activate_home_slot();

    ASSERT_INT("slot 0 active after activate_home_slot",
               repl_active_user_scene(), 0);
    const char *name = repl_user_scene_name(0);
    ASSERT_TRUE("slot 0 name non-null", name != NULL);
    if (name)
        ASSERT_STR("slot 0 name is My Scene (no (2) suffix)",
                   name, "My Scene");

    /* No spurious slot 1 should exist. */
    ASSERT_INT("slot 1 unused", repl_user_scene_slot_used(1), 0);
}

void test_my_scene_persists_edits_from_startup() {
    printf("--- My Scene persists edits across example switch ---\n");
    glr_app_reset_all(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Activate an empty slot 0 as "My Scene" (mirrors the startup path when
     * the live state is empty, e.g. before any example content is shown). */
    repl_scenes_activate_home_slot();
    ASSERT_INT("slot 0 active (My Scene mode)", repl_active_user_scene(), 0);

    /* User adds a vertex in "My Scene". */
    editor_feed_line("glVertex3f(9,9,9);");
    repl_flatten_commands();
    ASSERT_INT("vertex present in My Scene", repl_count_vertices(), 1);

    /* Switch to example 0 -- this auto-saves slot 0 before overwriting. */
    repl_load_example(0);
    ASSERT_INT("active scene cleared after example load",
               repl_active_user_scene(), -1);

    /* Return to My Scene -- should have exactly the user's vertex. */
    repl_load_user_scene_idx(0);
    repl_flatten_commands();
    ASSERT_INT("vertex restored after returning to My Scene",
               repl_count_vertices(), 1);
    ASSERT_INT("slot 0 active again", repl_active_user_scene(), 0);
}

void test_scene_cfg_persists_across_example_switch() {
    printf("--- Scene cfg persists across example switch ---\n");
    glr_app_reset_all(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Start in My Scene mode. */
    repl_load_example(0);
    repl_scenes_activate_home_slot();

    /* Record the default backdrop value, then set a different one. */
    int default_backdrop = glr_config_get(GLR_CONFIG_BACKDROP);
    int custom_backdrop  = (default_backdrop + 1) % glr_config_state_count(GLR_CONFIG_BACKDROP);
    glr_config_set(GLR_CONFIG_BACKDROP, custom_backdrop);

    int default_grid = glr_config_get(GLR_CONFIG_GRID_THEME);
    int custom_grid  = (default_grid + 1) % glr_config_state_count(GLR_CONFIG_GRID_THEME);
    glr_config_set(GLR_CONFIG_GRID_THEME, custom_grid);

    /* Switch to an example -- example load resets cfg to defaults, then
     * applies its own @cfg.  The user scene should be saved first. */
    repl_load_example(0);
    ASSERT_INT("example load resets backdrop to default",
               glr_config_get(GLR_CONFIG_BACKDROP), default_backdrop);

    /* Return to My Scene -- our custom cfg must be restored. */
    repl_load_user_scene_idx(0);
    ASSERT_INT("backdrop restored from My Scene",
               glr_config_get(GLR_CONFIG_BACKDROP), custom_backdrop);
    ASSERT_INT("grid theme restored from My Scene",
               glr_config_get(GLR_CONFIG_GRID_THEME), custom_grid);
}

void test_scene_cfg_not_inherited_from_example() {
    printf("--- Scene cfg not inherited from subsequent example ---\n");
    glr_app_reset_all(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Build a user scene with default cfg (no custom overrides). */
    repl_load_example(0);
    repl_scenes_activate_home_slot();
    int scene_backdrop = glr_config_get(GLR_CONFIG_BACKDROP);

    /* View an example that has a different backdrop. */
    repl_load_example(0);
    int example_backdrop = (scene_backdrop + 1) %
                           glr_config_state_count(GLR_CONFIG_BACKDROP);
    glr_config_set(GLR_CONFIG_BACKDROP, example_backdrop);
    ASSERT_INT("example backdrop different from scene backdrop",
               glr_config_get(GLR_CONFIG_BACKDROP), example_backdrop);

    /* Return to My Scene -- must NOT inherit the example's backdrop. */
    repl_load_user_scene_idx(0);
    ASSERT_INT("My Scene backdrop not overwritten by example",
               glr_config_get(GLR_CONFIG_BACKDROP), scene_backdrop);
}

/* Option A sandbox: in-example cfg toggles must not leak across an
 * example->user-scene transition. A user scene has full scene_cfg
 * coverage so the destination's saved cfg dominates regardless, but
 * this test pins the rollback path so a future move to sparse /
 * inherited-aware scene_cfg keeps the contract. */
void test_in_example_toggles_dont_leak_to_user_scene() {
    printf("--- In-example cfg toggles do not leak to user scene ---\n");
    glr_app_reset_all(); declare_test_vars();
    if (repl_example_count() < 1) return;

    /* Establish home (slot 0) with a known backdrop. */
    repl_scenes_activate_home_slot();
    int home_backdrop = glr_config_get(GLR_CONFIG_BACKDROP);

    /* Enter an example, then toggle the backdrop while inside. */
    repl_load_example(0);
    int example_toggled = (home_backdrop + 1) %
                          glr_config_state_count(GLR_CONFIG_BACKDROP);
    glr_config_set(GLR_CONFIG_BACKDROP, example_toggled);
    ASSERT_INT("in-example toggle visible in live cfg",
               glr_config_get(GLR_CONFIG_BACKDROP), example_toggled);

    /* Return to home: the in-example toggle must be gone. */
    repl_load_user_scene_idx(0);
    ASSERT_INT("home backdrop after example trip",
               glr_config_get(GLR_CONFIG_BACKDROP), home_backdrop);
}

void test_debug_dump_flat_commands() {
    printf("--- Debug dump flat commands ---\n");

    /* Empty state: header, count=0, end marker - and no crash on NULL out. */
    glr_app_reset_all(); declare_test_vars();
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
    glr_debug_dump_flat_commands(NULL, editor_buffer_view());
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
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glFrontFace(GL_CW);");
    editor_feed_line("glColor3f(1,0,0);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("glVertex3f(1,0,0);");
    editor_feed_line("glVertex3f(0,1,0);");
    editor_feed_line("glEnd();");
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
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(i,0,0);");
    editor_feed_line("}");
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
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0() {");
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("}");
    editor_feed_line("func0();");
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
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(0,0,0);");
    repl_state_mark_flat_dirty();
    repl_state_flat_program_set_count(0);
    FILE *dn = fopen("/dev/null", "w");
    if (dn) {
        glr_debug_dump_flat_commands(dn, editor_buffer_view());
        fclose(dn);
    }
    ASSERT_TRUE("dump re-flattens commands", repl_state_flat_program_count() >= 1);
}

/* Capture the output of glr_debug_dump_editor(, source_document_view()) into a malloc'd string. */
static char *capture_editor_dump(void) {
    FILE *tmp = tmpfile();
    char *buf = NULL;
    long len;
    size_t nread;

    if (!tmp)
        return NULL;
    glr_debug_dump_editor(tmp, source_document_view());
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
    glr_app_reset_all();
    editor_feed_line("float a, b, c;");

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
    glr_app_reset_all(); declare_test_vars();
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
    test_inline_file_prompt_flow();
    test_workspace_round_trip();
    test_activate_home_slot_no_duplicate_name();
    test_my_scene_persists_edits_from_startup();
    test_scene_cfg_persists_across_example_switch();
    test_scene_cfg_not_inherited_from_example();
    test_in_example_toggles_dont_leak_to_user_scene();
    test_debug_dump_flat_commands();
    test_var_declare_cmd();
    test_time();

    printf("\n%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
