/*
 * tests/test_glr_extedit.c - the `--watch` external-editor gate.
 *
 * Everything here is failure and edge behavior, because that is all this
 * feature is: on the happy path a reload is just a scene load, already covered
 * by test_scene_load.c. What is new - and what the plan
 * (docs/plans/active/BYOE.md) says the suite was thinnest on - is what happens
 * when a save lands at a bad moment.
 *
 * Three families:
 *
 *   the change gate    a same-second save must not be missed *indefinitely*,
 *                      an editor's temp-file-and-rename must be noticed even
 *                      when it does not move the timestamp, and a file that
 *                      cannot be parsed must not be re-read every frame. All
 *                      asserted through counters, never through timing.
 *   deferral           a reload landing on a half-typed line destroys work
 *                      Ctrl+Z cannot recover. Abandon the line and the pending
 *                      version applies; commit it and the pending version is
 *                      dismissed - and the same bytes must not come back.
 *   identity           whether the reload is undoable, and whether it promotes
 *                      an unedited example into a user slot, both depend on
 *                      what the live document *is*.
 *
 * The clock is real: the tests force mtimes with utimensat() rather than
 * sleeping, so "same second, different nanosecond" is an assertion about the
 * change token and not a race.
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "gl_includes.h"
#include "app/glr_camera.h"
#include "app/glr_config.h"
#include "app/glr_ctrl.h"
#include "app/glr_defaults.h"   /* CFG_DEFAULT_* - never hardcode a default */
#include "app/glr_extedit.h"
#include "app/glr_modal.h"
#include "editor/commit.h"
#include "editor/input.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "repl/bootstrap.h"
#include "repl/compile.h"
#include "repl/eval.h"
#include "repl/example_loader.h"
#include "repl/examples.h"
#include "repl/export.h"
#include "repl/scenes.h"
#include "repl/state_owners.h"
#include "source_document.h"
#include "ui/app/state.h"
#include "subsystems/replay/replay.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "subsystems/variable_panel/variable_panel_state.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond)      TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, want)  TEST_ASSERT_INT(&g_harness, label, got, want)
#define ASSERT_STR(label, got, want)  TEST_ASSERT_STR(&g_harness, label, got, want)

#define WATCH_PATH "/tmp/gl_repl_extedit_scene.glr"
#define SWAP_PATH  "/tmp/gl_repl_extedit_scene.glr.swap"
#define OTHER_PATH "/tmp/gl_repl_extedit_other.glr"
#define WATCH_C_PATH "/tmp/gl_repl_extedit_scene.c"
#define WIP_C_PATH   WATCH_C_PATH ".wip"

static const char *const k_scene_a[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "glBegin(GL_POINTS);",
    "glVertex3f(0, 0, 0);",
    "glEnd();",
    NULL
};

/* Same row count as A so a reload is visible only in the text, and same total
 * byte count so the change gate cannot pass on size alone. */
static const char *const k_scene_b[] = {
    "glClearColor(0.2, 0.2, 0.2, 1.0);",
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "glBegin(GL_POINTS);",
    "glVertex3f(9, 9, 9);",
    "glEnd();",
    NULL
};

static const char *const k_scene_longer[] = {
    "glClearColor(0.3, 0.3, 0.3, 1.0);",
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "glBegin(GL_POINTS);",
    "glVertex3f(1, 1, 1);",
    "glVertex3f(2, 2, 2);",
    "glVertex3f(3, 3, 3);",
    "glEnd();",
    NULL
};

/* Two statements on one row: the REPL is one command per line, so the tail is
 * rejected - and under ATOMIC that fails the whole file. */
static const char *const k_scene_broken[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "glBegin(GL_POINTS);",
    "glColor3f(1, 0, 0); glVertex3f(0, 0, 0);",
    "glEnd();",
    NULL
};

static int file_contains(const char *path, const char *needle) {
    char buf[512];
    FILE *f = fopen(path, "r");
    int found = 0;

    if (!f)
        return 0;
    while (!found && fgets(buf, (int)sizeof(buf), f))
        found = strstr(buf, needle) != NULL;
    fclose(f);
    return found;
}

static void write_lines(const char *path, const char *const *lines) {
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; lines && lines[i]; i++)
        fprintf(f, "%s\n", lines[i]);
    fclose(f);
}

/* Start a session whose active scene is WATCH_PATH, with the watcher armed and
 * bound (the first poll adopts the binding without reloading). */
static void begin_watched_session(const char *const *initial) {
    glr_extedit_set_enabled(0);
    write_lines(WATCH_PATH, initial);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
}

/* Does the live document contain `needle` anywhere? Cheaper to read than a
 * row-by-row comparison and enough to tell scene A from scene B. */
static int document_mentions(const char *needle) {
    int count = repl_state_document_count();
    for (int i = 0; i < count; i++) {
        const char *line = editor_buffer_line(i);
        if (line && strstr(line, needle))
            return 1;
    }
    return 0;
}

#if defined(__EMSCRIPTEN__)

/* The web build has no external editor and no filesystem worth watching, so
 * glr_extedit_poll() compiles to a no-op and `--watch` is ignored. That
 * inertness IS the web form of this feature, so the binary stays in the wasm
 * lane rather than joining WEB_TEST_EXCLUDE: the TU keeps linking, and the
 * claim keeps being one somebody checks. */
static void test_web_watcher_is_inert(void) {
    printf("--- the web build watches nothing ---\n");

    write_lines(WATCH_PATH, k_scene_a);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    glr_extedit_set_enabled(1);

    /* The flag is accepted - nothing refuses it - and then does nothing. */
    ASSERT_TRUE("the flag is still recorded", glr_extedit_enabled());
    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_TRUE("nothing is bound", glr_extedit_bound_path() == NULL);
    ASSERT_INT("nothing is read", glr_extedit_stats().reads, 0);

    write_lines(WATCH_PATH, k_scene_b);
    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_INT("and a save changes nothing", glr_extedit_stats().reloads, 0);
    ASSERT_TRUE("the loaded scene is untouched", document_mentions("0, 0, 0"));

    /* The self-write stamp is equally inert, and equally must not crash. */
    glr_extedit_note_saved();
    glr_extedit_note_wrote(WATCH_PATH);
}

int main(void) {
    printf("=== external-editor watch (web) ===\n");
    test_web_watcher_is_inert();
    glr_extedit_set_enabled(0);
    (void)unlink(WATCH_PATH);
    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "glr_extedit");
}

#else

/* Pin a file's mtime so a test can say "same second" or "same instant" and
 * mean it, instead of sleeping and hoping. */
static void set_mtime(const char *path, long sec, long nsec) {
    struct timespec times[2];
    times[0].tv_sec = sec; times[0].tv_nsec = nsec;   /* atime */
    times[1].tv_sec = sec; times[1].tv_nsec = nsec;   /* mtime */
    (void)utimensat(AT_FDCWD, path, times, 0);
}

/* ----- binding ----------------------------------------------------------- */

static void test_bind_adopts_without_reloading(void) {
    GlrExtEditStats stats;

    printf("--- binding adopts the file without reloading it ---\n");

    begin_watched_session(k_scene_a);
    ASSERT_TRUE("the watcher bound to the CLI file",
                glr_extedit_bound_path() != NULL);
    stats = glr_extedit_stats();
    /* The document already IS this file. Reloading it would be a wholesale
     * replacement that clears the input row and pushes undo for nothing. */
    ASSERT_INT("binding performed no reload", stats.reloads, 0);

    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    stats = glr_extedit_stats();
    ASSERT_INT("and idle frames read nothing", stats.reads, 1);
    ASSERT_INT("nor reload anything", stats.reloads, 0);
}

/* --- the change gate ------------------------------------------------------ */

/* The reason the change token is nanoseconds and not `time_t`: with seconds, a
 * final save landing in the same second as the one before it is missed
 * *indefinitely*, not merely late. */
static void test_same_second_save_is_noticed(void) {
    printf("--- a same-second save is not missed ---\n");

    begin_watched_session(k_scene_a);
    set_mtime(WATCH_PATH, 1000000000L, 0L);
    glr_extedit_poll();

    write_lines(WATCH_PATH, k_scene_b);
    set_mtime(WATCH_PATH, 1000000000L, 500000000L);   /* same second */
    glr_extedit_poll();

    ASSERT_INT("the reload happened", glr_extedit_stats().reloads, 1);
    ASSERT_TRUE("the new scene is live", document_mentions("9"));
}

/* An editor's safe write is temp-file + rename: a brand new inode, and no
 * guarantee the timestamp moved at all. */
static void test_safe_write_rename_is_noticed(void) {
    struct stat before;
    struct stat after;

    printf("--- a temp-file-and-rename save is not missed ---\n");

    begin_watched_session(k_scene_a);
    set_mtime(WATCH_PATH, 1000000000L, 0L);
    glr_extedit_poll();
    (void)stat(WATCH_PATH, &before);

    write_lines(SWAP_PATH, k_scene_b);
    set_mtime(SWAP_PATH, 1000000000L, 0L);            /* identical mtime */
    ASSERT_INT("the rename succeeded", rename(SWAP_PATH, WATCH_PATH), 0);
    (void)stat(WATCH_PATH, &after);
    ASSERT_TRUE("the test really did change the inode",
                before.st_ino != after.st_ino);
    ASSERT_TRUE("and really did not change the mtime or size",
                before.st_size == after.st_size);

    glr_extedit_poll();
    ASSERT_INT("the reload happened", glr_extedit_stats().reloads, 1);
    ASSERT_TRUE("the new scene is live", document_mentions("9"));
}

static void test_size_only_change_is_noticed(void) {
    printf("--- a size change alone is not missed ---\n");

    begin_watched_session(k_scene_a);
    set_mtime(WATCH_PATH, 1000000000L, 0L);
    glr_extedit_poll();

    write_lines(WATCH_PATH, k_scene_longer);
    set_mtime(WATCH_PATH, 1000000000L, 0L);   /* rewritten in place, same time */
    glr_extedit_poll();

    ASSERT_INT("the reload happened", glr_extedit_stats().reloads, 1);
    ASSERT_INT("the longer scene is live", repl_state_document_count(), 7);
}

/* The content token's job: a touch that changes no bytes must not re-parse. */
static void test_touch_without_content_change_does_not_reload(void) {
    GlrExtEditStats stats;

    printf("--- a touch that changes no bytes does not reload ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, k_scene_a);   /* byte-identical rewrite */
    glr_extedit_poll();

    stats = glr_extedit_stats();
    ASSERT_INT("the file was re-read", stats.reads, 2);
    ASSERT_INT("but the document was not replaced", stats.reloads, 0);
}

/* Without stamping the observed token before the parse, a file the user is
 * halfway through fixing is re-read and re-reported on every single frame. */
static void test_malformed_file_is_not_retried_every_frame(void) {
    GlrExtEditStats stats;

    printf("--- a malformed file is attempted once ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, k_scene_broken);
    for (int i = 0; i < 20; i++)
        glr_extedit_poll();

    stats = glr_extedit_stats();
    ASSERT_INT("read once", stats.reads, 2);
    ASSERT_INT("attempted once", stats.failures, 1);
    ASSERT_INT("never reloaded", stats.reloads, 0);
    ASSERT_TRUE("and the good scene is still live", document_mentions("0, 0, 0"));

    /* Fixing the file resumes normally - the failure is not a latch. */
    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("the fixed file loads", glr_extedit_stats().reloads, 1);
}

static void test_missing_file_reports_once_and_resumes(void) {
    printf("--- a missing file is reported once, and comes back ---\n");

    begin_watched_session(k_scene_a);
    ASSERT_INT("remove the file", unlink(WATCH_PATH), 0);
    for (int i = 0; i < 10; i++)
        glr_extedit_poll();
    ASSERT_TRUE("the last good scene is still live", document_mentions("0, 0, 0"));
    ASSERT_INT("nothing was reloaded", glr_extedit_stats().reloads, 0);

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("the path reappearing resumes the watch",
               glr_extedit_stats().reloads, 1);
}

/* --- self-write ----------------------------------------------------------- */

static void test_our_own_save_does_not_come_back(void) {
    printf("--- gl-repl's own write is not an inbound change ---\n");

    begin_watched_session(k_scene_a);
    ASSERT_TRUE("Ctrl+S writes the bound file",
                repl_save_active_scene(NULL) != 0);
    glr_extedit_note_saved();

    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_INT("and never reads back as an external edit",
               glr_extedit_stats().reloads, 0);
}

/* --- deferral (D5) -------------------------------------------------------- */

/* Park the cursor past the last row and type - the shape a user is in when a
 * save from vim arrives mid-thought. */
static void begin_typing(const char *text) {
    editor_state_edit_line_set(repl_state_document_count());
    editor_input_set_text(text);
}

static void test_dirty_input_defers_and_cancel_applies(void) {
    printf("--- an abandoned line lets the pending version through ---\n");

    begin_watched_session(k_scene_a);
    begin_typing("glVertex3f(4, 4,");
    ASSERT_TRUE("the input row is dirty",
                editor_input_has_uncommitted_change());

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("the reload is held", glr_extedit_stats().reloads, 0);
    ASSERT_INT("and recorded as deferred", glr_extedit_stats().deferrals, 1);
    ASSERT_TRUE("the old scene is still live", document_mentions("0, 0, 0"));

    /* Abandoning the row is the "cancel" case: the document never moved, so
     * the external version is what the user wants. */
    editor_input_clear();
    glr_extedit_poll();
    ASSERT_INT("the held version lands", glr_extedit_stats().reloads, 1);
    ASSERT_TRUE("the new scene is live", document_mentions("9"));
}

/* The failure this exists to prevent: both sides hold A, vim saves B while the
 * user is typing L, the user commits so the document is A+L, and applying B
 * now silently destroys L one frame after they typed it. */
static void test_commit_dismisses_and_the_bytes_do_not_return(void) {
    int rows_after_commit;

    printf("--- committing beats the pending version, permanently ---\n");

    begin_watched_session(k_scene_a);
    begin_typing("glVertex3f(4, 4, 4);");

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("the reload is held", glr_extedit_stats().reloads, 0);

    ASSERT_TRUE("the line commits", editor_feed_line("glVertex3f(4, 4, 4);") != 0);
    editor_input_clear();
    rows_after_commit = repl_state_document_count();

    glr_extedit_poll();
    ASSERT_INT("the held version is dismissed, not applied",
               glr_extedit_stats().reloads, 0);
    ASSERT_INT("dismissal was recorded", glr_extedit_stats().dismissals, 1);
    ASSERT_INT("the committed line survives",
               repl_state_document_count(), rows_after_commit);
    ASSERT_TRUE("the committed line is really there", document_mentions("4, 4, 4"));

    /* Dismissal is a third state, not "applied": while the file still holds
     * exactly those bytes, they must not re-trigger on every later frame. */
    for (int i = 0; i < 10; i++)
        glr_extedit_poll();
    ASSERT_INT("the same bytes never come back",
               glr_extedit_stats().reloads, 0);

    /* But a genuinely new save does. */
    write_lines(WATCH_PATH, k_scene_longer);
    glr_extedit_poll();
    ASSERT_INT("a different payload is followed again",
               glr_extedit_stats().reloads, 1);
}

static void test_save_supersedes_a_pending_version(void) {
    printf("--- a save resolves a pending version ---\n");

    begin_watched_session(k_scene_a);
    begin_typing("glVertex3f(4, 4,");
    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("held", glr_extedit_stats().reloads, 0);

    editor_input_clear();
    ASSERT_TRUE("Ctrl+S writes the bound file",
                repl_save_active_scene(NULL) != 0);
    glr_extedit_note_saved();

    glr_extedit_poll();
    ASSERT_INT("the pending version is gone, not applied",
               glr_extedit_stats().reloads, 0);
    ASSERT_TRUE("the local scene is what is live", document_mentions("0, 0, 0"));
}

/* Arrowing onto a row LOADS it, so the input row equals the document row -
 * with the trailing `;` stripped. A raw text compare would call that dirty and
 * the deferral would never lift. */
static void test_loaded_row_is_not_dirty(void) {
    printf("--- merely navigating onto a row does not defer ---\n");

    begin_watched_session(k_scene_a);
    editor_state_edit_line_set(3);
    editor_load_line_to_input(3);
    ASSERT_TRUE("a loaded row is clean",
                !editor_input_has_uncommitted_change());

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("so the reload is not deferred",
               glr_extedit_stats().deferrals, 0);
    ASSERT_INT("it just happens", glr_extedit_stats().reloads, 1);
}

static void test_empty_existing_row_is_dirty(void) {
    printf("--- deleting an existing row to empty still defers ---\n");

    begin_watched_session(k_scene_a);
    editor_state_edit_line_set(3);
    editor_load_line_to_input(3);
    editor_input_clear();
    ASSERT_TRUE("empty differs from the existing document row",
                editor_input_has_uncommitted_change());

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("the reload is deferred", glr_extedit_stats().deferrals, 1);
    ASSERT_INT("and has not erased the local edit",
               glr_extedit_stats().reloads, 0);
    ASSERT_TRUE("the old scene remains live", document_mentions("0, 0, 0"));

    /* Restoring the canonical input is cancellation: the document did not
     * move, so the pending external version may now land. */
    editor_load_line_to_input(3);
    glr_extedit_poll();
    ASSERT_INT("the pending version lands after cancellation",
               glr_extedit_stats().reloads, 1);
}

/* --- identity (D4) -------------------------------------------------------- */

static void test_user_scene_reload_is_undoable(void) {
    printf("--- a user scene's reload is one undoable clobber ---\n");

    begin_watched_session(k_scene_a);
    ASSERT_TRUE("the CLI file made a user scene",
                repl_active_user_scene() >= 0);

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("the reload happened", glr_extedit_stats().reloads, 1);
    ASSERT_TRUE("the new scene is live", document_mentions("9"));

    ASSERT_TRUE("undo is available", editor_undo_can_undo());
    editor_undo_pop_snapshot();
    ASSERT_TRUE("Ctrl+Z gets the gl-repl version back",
                document_mentions("0, 0, 0"));
}

/* A compiled-in example has no file, so switching to one unbinds the watcher
 * entirely - saves to the previously watched path stop mattering. This is the
 * *unbound* half; the no-promotion half needs a scene that is file-backed but
 * still not a user slot, which is test_catalog_scene_reload_does_not_promote
 * below. */
static void test_builtin_example_unbinds_the_watcher(void) {
    int slots_before;

    printf("--- a built-in example unbinds the watcher ---\n");

    begin_watched_session(k_scene_a);
    /* Switch to a built-in example: no slot, no file. */
    (void)repl_load_example(0);
    glr_extedit_poll();
    slots_before = repl_user_scene_count();
    ASSERT_INT("the example is not a user scene", repl_active_user_scene(), -1);
    ASSERT_TRUE("and the watcher unbound", glr_extedit_bound_path() == NULL);

    write_lines(WATCH_PATH, k_scene_b);
    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_INT("an unbound watcher reloads nothing",
               glr_extedit_stats().reloads, 0);
    ASSERT_INT("and creates no scene slot",
               repl_user_scene_count(), slots_before);
}

/* A failed reload has already pushed an undo snapshot by the time it fails.
 * With a full ring that push overwrote the oldest entry, and restoring
 * head/count indices cannot bring it back - which is why the rollback uses the
 * heap-backed history capture. */
#define UNDO_RING_SLOTS 32

static void test_failed_reload_restores_a_full_undo_ring(void) {
    char probe[64];
    int base_rows;
    int popped = 0;

    printf("--- a failed reload restores the undo ring losslessly ---\n");

    begin_watched_session(k_scene_a);
    base_rows = repl_state_document_count();

    /* Fill the ring to *exactly* capacity. The oldest of the 32 entries is the
     * document as it stands right now, so undoing 32 times must land back on
     * base_rows - unless something evicted it. */
    for (int i = 0; i < UNDO_RING_SLOTS; i++) {
        editor_state_edit_line_set(repl_state_document_count());
        snprintf(probe, sizeof(probe), "glVertex3f(%d, 0, 0);", i);
        editor_undo_push_snapshot();
        (void)editor_feed_line(probe);
        editor_input_clear();
    }
    ASSERT_INT("the ring is full and the document grew",
               repl_state_document_count(), base_rows + UNDO_RING_SLOTS);

    write_lines(WATCH_PATH, k_scene_broken);
    glr_extedit_poll();
    ASSERT_INT("the reload failed", glr_extedit_stats().failures, 1);
    ASSERT_INT("the document is untouched",
               repl_state_document_count(), base_rows + UNDO_RING_SLOTS);

    /* The failed attempt pushed a snapshot before it knew it would fail, and
     * on a full ring that push has already overwritten the oldest entry.
     * Restoring head/count indices - what EditorUndoRingState carries - cannot
     * bring it back, which is why the rollback captures the heap-backed
     * history instead. If it ever regresses to indices, this lands one row
     * short. */
    while (editor_undo_can_undo() && popped < UNDO_RING_SLOTS) {
        editor_undo_pop_snapshot();
        popped++;
    }
    ASSERT_INT("all 32 entries are still there", popped, UNDO_RING_SLOTS);
    ASSERT_INT("and the oldest one survived the failed push",
               repl_state_document_count(), base_rows);
}

/* --- D3: what a watched reload must NOT touch ----------------------------- */

/* An external *text* edit is geometry, not a presentation reset. A file that
 * carries `@cfg` and `@camera` rows still gets to say what the program is -
 * and gets no say at all over the live view the user has set up. Asserted on
 * both outcomes, because a failed reload must be just as inert. */
static void test_watched_reload_preserves_cfg_and_camera(void) {
    static const char *const with_metadata[] = {
        "// @cfg grid = GRID_THEME_SOIL",
        "glTranslatef(0.0f, 0.0f, -42.0f);   // @camera dist",
        "glRotatef(31.0f, 1.0f, 0.0f, 0.0f);   // @camera rx",
        "glRotatef(41.0f, 0.0f, 1.0f, 0.0f);   // @camera ry",
        "glTranslatef(0.0f, 0.0f, 0.0f);   // @camera pan",
        "glBegin(GL_POINTS);",
        "glVertex3f(6, 6, 6);",
        "glEnd();",
        NULL
    };
    static const char *const with_metadata_broken[] = {
        "// @cfg grid = GRID_THEME_SOIL",
        "glTranslatef(0.0f, 0.0f, -99.0f);   // @camera dist",
        "glBegin(GL_POINTS);",
        "glColor3f(1, 0, 0); glVertex3f(0, 0, 0);",
        "glEnd();",
        NULL
    };
    int grid_before;
    float dist_before;

    printf("--- a watched reload leaves cfg and camera alone ---\n");

    begin_watched_session(k_scene_a);
    grid_before = glr_config_get(GLR_CONFIG_GRID_THEME);
    dist_before = glr_camera_destination().dist;

    write_lines(WATCH_PATH, with_metadata);
    glr_extedit_poll();
    ASSERT_INT("the reload succeeded", glr_extedit_stats().reloads, 1);
    ASSERT_TRUE("the new program is live", document_mentions("6, 6, 6"));
    ASSERT_INT("but its @cfg did not apply",
               glr_config_get(GLR_CONFIG_GRID_THEME), grid_before);
    ASSERT_TRUE("and its @camera did not move the camera",
                glr_camera_destination().dist == dist_before);

    /* Same again on the failure path. */
    write_lines(WATCH_PATH, with_metadata_broken);
    glr_extedit_poll();
    ASSERT_INT("the second reload failed", glr_extedit_stats().failures, 1);
    ASSERT_INT("cfg is still untouched",
               glr_config_get(GLR_CONFIG_GRID_THEME), grid_before);
    ASSERT_TRUE("and so is the camera",
                glr_camera_destination().dist == dist_before);
    ASSERT_TRUE("with the previous program still live",
                document_mentions("6, 6, 6"));
}

/* --- lessons (D7) --------------------------------------------------------- */

/* A tutorial drives the document itself, and two writers on one document is
 * not something this can arbitrate. The rule is not just "ignore": the pending
 * version is DISMISSED at lesson end - a payload computed against the
 * pre-lesson document must never land on the document the lesson left behind -
 * and the observed token is stamped, or the first poll afterwards sees that
 * same old movement as new and applies it anyway. */
static void test_tutorial_defers_then_dismisses(void) {
    const char *bound_during;
    int rows_after_lesson;

    printf("--- a lesson defers, then discards, an external change ---\n");

    begin_watched_session(k_scene_a);
    tutorial_start(0);
    ASSERT_TRUE("the tutorial is running", tutorial_active());
    /* tutorial_start parks the document in a slot-less transient, so the
     * active scene resolves to no file at all. The binding must survive that,
     * or the "external change waiting" status has nothing to point at. */
    bound_during = glr_extedit_bound_path();
    ASSERT_TRUE("the binding still names the pre-lesson file",
                bound_during != NULL &&
                strstr(bound_during, "gl_repl_extedit_scene.glr") != NULL);

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("nothing was reloaded during the lesson",
               glr_extedit_stats().reloads, 0);
    ASSERT_INT("the change was parked", glr_extedit_stats().deferrals, 1);
    ASSERT_TRUE("the binding is unchanged",
                glr_extedit_bound_path() != NULL &&
                strcmp(glr_extedit_bound_path(), bound_during) == 0);

    tutorial_stop();
    ASSERT_TRUE("the tutorial ended", !tutorial_active());
    rows_after_lesson = repl_state_document_count();

    glr_extedit_poll();
    ASSERT_INT("the parked version is discarded, not applied",
               glr_extedit_stats().reloads, 0);
    ASSERT_INT("and recorded as a dismissal",
               glr_extedit_stats().dismissals, 1);
    ASSERT_INT("the document the lesson left behind is untouched",
               repl_state_document_count(), rows_after_lesson);

    /* The token-stamping half: without it the first poll after the lesson
     * would read that same old movement as new. */
    for (int i = 0; i < 10; i++)
        glr_extedit_poll();
    ASSERT_INT("and no reload happens without a further external edit",
               glr_extedit_stats().reloads, 0);
}

/* --- D7 transients -------------------------------------------------------- */

static void test_reload_stops_replay(void) {
    printf("--- a reload stops replay ---\n");

    begin_watched_session(k_scene_a);
    replay_start();
    ASSERT_TRUE("replay is running", replay_state_view().active);

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("the reload happened", glr_extedit_stats().reloads, 1);
    /* Flat identity and replay_exec_limit index a flat program that the
     * reflatten just replaced. */
    ASSERT_TRUE("replay stopped", !replay_state_view().active);
}

static void test_reload_cancels_camera_drag_and_momentum(void) {
    GlrCameraRuntimeSnapshot runtime;
    GlrCameraState before_motion;
    GlrCameraState after_motion;

    printf("--- a reload cancels camera drag and momentum ---\n");

    begin_watched_session(k_scene_a);
    glr_camera_mouse_event(GLUT_LEFT_BUTTON, GLUT_DOWN, 10, 10, 0);
    glr_camera_drag_motion(20, 15);

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("the reload happened", glr_extedit_stats().reloads, 1);

    glr_camera_runtime_capture(&runtime);
    ASSERT_INT("the held camera button was released",
               runtime.pointer_button, -1);
    ASSERT_TRUE("orbit momentum was cleared",
                runtime.vel_rx == 0.0f && runtime.vel_ry == 0.0f);

    before_motion = glr_camera();
    glr_camera_drag_motion(40, 25);
    after_motion = glr_camera();
    ASSERT_TRUE("later pointer motion no longer continues the old drag",
                before_motion.rx == after_motion.rx &&
                before_motion.ry == after_motion.ry);
}

static void test_reload_clears_the_input_row(void) {
    printf("--- a reload leaves no stale input row ---\n");

    begin_watched_session(k_scene_a);
    editor_state_edit_line_set(3);
    editor_load_line_to_input(3);

    write_lines(WATCH_PATH, k_scene_longer);
    glr_extedit_poll();
    ASSERT_INT("the reload happened", glr_extedit_stats().reloads, 1);
    ASSERT_INT("the input row is empty", editor_input_len(), 0);
    ASSERT_TRUE("and the cursor is inside the new document",
                editor_state_edit_line() <= repl_state_document_count());
}

/* --- stage 2: one incomplete final row ------------------------------------ */

/* The shape stage 2 exists for: the user is mid-command in vim when they save.
 * The row belongs in the live input buffer, where the edit-guide overlays and
 * autocomplete can see it - not in the document, where it would not parse. */
static void test_incomplete_final_row_is_parked(void) {
    static const char *const half_typed[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        "glVertex3f(1,",
        NULL
    };

    printf("--- a half-typed final row lands in the input buffer ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, half_typed);
    glr_extedit_poll();

    ASSERT_INT("the reload succeeded", glr_extedit_stats().reloads, 1);
    ASSERT_INT("a row was parked", glr_extedit_stats().parked_rows, 1);
    ASSERT_INT("the document holds only the complete rows",
               repl_state_document_count(), 5);
    ASSERT_TRUE("and not the half-typed one", !document_mentions("glVertex3f(1,"));
    ASSERT_TRUE("which is in the input row instead",
                strcmp(editor_input_text(), "glVertex3f(1,") == 0);
    ASSERT_INT("parked past the last row, so it appends",
               editor_state_edit_line(), repl_state_document_count());
    /* editor_input_set_text snaps the cursor to end-of-text, which is the
     * right answer for a row the user is still typing. */
    ASSERT_INT("with the cursor at the end", editor_cursor_pos(),
               (int)strlen("glVertex3f(1,"));
}

/* The case a weaker "trailing comma or operator" rule misses, and the reason
 * the test is the terminator: this row has depth 0 and ends in `)`, so it
 * would not be stripped - and it does not fail either. import_finish_load
 * flushes it at EOF and the parser adds the `;` back while rebuilding
 * canonical text, so it would commit silently as a document row and stage 2's
 * payoff would never fire for the most common half-typed line there is. */
static void test_missing_semicolon_counts_as_incomplete(void) {
    static const char *const no_semicolon[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        "glVertex3f(1, 2, 3)",
        NULL
    };

    printf("--- a balanced row missing only its ';' is incomplete ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, no_semicolon);
    glr_extedit_poll();

    ASSERT_INT("a row was parked", glr_extedit_stats().parked_rows, 1);
    ASSERT_TRUE("and it is the one missing its semicolon",
                strcmp(editor_input_text(), "glVertex3f(1, 2, 3)") == 0);
    ASSERT_INT("the document has the rest",
               repl_state_document_count(), 5);
}

static void test_trailing_comment_is_not_parked(void) {
    static const char *const with_comment[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        "// a note about the scene",
        NULL
    };

    printf("--- a trailing comment stays in the document ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, with_comment);
    glr_extedit_poll();

    ASSERT_INT("the reload succeeded", glr_extedit_stats().reloads, 1);
    ASSERT_INT("nothing was parked", glr_extedit_stats().parked_rows, 0);
    ASSERT_INT("the comment is a document row",
               repl_state_document_count(), 6);
    ASSERT_TRUE("really the comment", document_mentions("a note about the scene"));
    ASSERT_INT("and the input row is empty", editor_input_len(), 0);
}

/* A well-formed-but-wrong row has a terminator, so it is not stripped - and
 * ATOMIC then refuses the whole file, which is correct: the user's mistake is
 * reported instead of silently swallowed. */
static void test_wrong_but_terminated_row_fails_the_file(void) {
    static const char *const typo[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        "glVertx3f(1, 2, 3);",
        NULL
    };

    printf("--- a well-formed-but-wrong final row fails the file ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, typo);
    glr_extedit_poll();

    ASSERT_INT("the file was refused", glr_extedit_stats().failures, 1);
    ASSERT_INT("nothing was parked", glr_extedit_stats().parked_rows, 0);
    ASSERT_TRUE("and the previous scene is intact", document_mentions("0, 0, 0"));
}

/* The plan warns that stripping a *block delimiter* would unbalance what
 * remains. With the terminator rule that cannot happen: `{` and `}` are
 * statement terminators, so a row that opens or closes a block is complete by
 * construction and is never the row selected. Asserted rather than assumed,
 * because a future heuristic change could quietly reintroduce it. */
static void test_block_rows_are_never_parked(void) {
    static const char *const looped[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "for(i, 0, 3) {",
        "glVertex3f(i, 0, 0);",
        "}",
        "glEnd();",
        "glVertex3f(7,",
        NULL
    };

    printf("--- a block delimiter is never the parked row ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, looped);
    glr_extedit_poll();

    ASSERT_INT("the file loaded", glr_extedit_stats().failures, 0);
    ASSERT_INT("one row was parked", glr_extedit_stats().parked_rows, 1);
    ASSERT_TRUE("and it is the half-typed vertex, not the brace",
                strcmp(editor_input_text(), "glVertex3f(7,") == 0);
    ASSERT_TRUE("the loop survived whole", document_mentions("for"));
}

/* An unfinished statement absorbs every physical line after it, so an unclosed
 * paren mid-file makes the entire tail one logical statement. Stripping that
 * would delete rows the user never touched, so the multi-row case is refused -
 * ATOMIC then rejects the file and names the line, which is the outcome that
 * tells the user something is wrong. */
static void test_multi_row_incomplete_statement_is_refused(void) {
    static const char *const runaway[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(0,",              /* never closed */
        "glVertex3f(1, 1, 1);",
        "glEnd();",
        NULL
    };

    printf("--- a runaway statement is refused, not stripped ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, runaway);
    glr_extedit_poll();

    ASSERT_INT("nothing was parked", glr_extedit_stats().parked_rows, 0);
    ASSERT_INT("the file was refused", glr_extedit_stats().failures, 1);
    ASSERT_INT("no reload happened", glr_extedit_stats().reloads, 0);
    ASSERT_TRUE("and the previous scene is intact", document_mentions("0, 0, 0"));
}

/* D5's parked-row exclusion is conditional, not blanket. The watcher's own
 * text does not count as the user's typing - but the first local keystroke in
 * that row transfers ownership back, or the next save overwrites work undo
 * cannot restore. */
static void test_parked_row_defers_only_once_the_user_touches_it(void) {
    static const char *const half_typed[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        "glVertex3f(1,",
        NULL
    };
    static const char *const half_typed_more[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glVertex3f(5, 5, 5);",
        "glEnd();",
        "glVertex3f(2,",
        NULL
    };

    printf("--- an untouched parked row does not defer; a touched one does ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, half_typed);
    glr_extedit_poll();
    ASSERT_INT("the row is parked", glr_extedit_stats().parked_rows, 1);
    ASSERT_TRUE("the input row is non-empty",
                editor_input_has_uncommitted_change());

    /* Untouched: the next save goes straight through. */
    write_lines(WATCH_PATH, half_typed_more);
    glr_extedit_poll();
    ASSERT_INT("a second save was not deferred",
               glr_extedit_stats().deferrals, 0);
    ASSERT_INT("it applied", glr_extedit_stats().reloads, 2);
    ASSERT_TRUE("and re-parked the new tail",
                strcmp(editor_input_text(), "glVertex3f(2,") == 0);

    /* Touched: ownership transfers back to the user and deferral re-arms. */
    editor_input_set_text("glVertex3f(2, 3,");
    write_lines(WATCH_PATH, half_typed);
    glr_extedit_poll();
    ASSERT_INT("the third save is held", glr_extedit_stats().reloads, 2);
    ASSERT_INT("as a deferral", glr_extedit_stats().deferrals, 1);
    ASSERT_TRUE("with the user's typing intact",
                strcmp(editor_input_text(), "glVertex3f(2, 3,") == 0);
}

/* --- regressions found in review ------------------------------------------ */

/* A parked version can be overtaken by a *later* save that puts the file back
 * to something already known - the content we last applied, or a payload a
 * commit already dismissed. Both of those return early, so the stale pending
 * version used to survive; the reload that eventually ran then read the
 * current file but stamped the *stale* hash as "applied". After that, a
 * genuine save of those bytes looked like our own write and was ignored - the
 * watcher went deaf to one particular version of the file. */
static void test_pending_overtaken_by_a_revert(void) {
    printf("--- a revert clears the parked version, and the stamp is honest ---\n");

    begin_watched_session(k_scene_a);
    begin_typing("glVertex3f(4, 4,");          /* gate shut */

    /* A, B and the revert are all the same byte count and rewrite the same
     * inode, so the change token rests entirely on the mtime - and three
     * writes microseconds apart share one timestamp wherever the filesystem's
     * granularity is coarser than the loop (ext4 in particular). Real saves
     * are seconds apart; stamp explicit times so this exercises the pending
     * bookkeeping rather than the clock. */
    write_lines(WATCH_PATH, k_scene_b);
    set_mtime(WATCH_PATH, 1000000100L, 0L);
    glr_extedit_poll();
    ASSERT_INT("B is parked, not applied", glr_extedit_stats().reloads, 0);

    /* vim undoes: the file is byte-identical to what we already have. */
    write_lines(WATCH_PATH, k_scene_a);
    set_mtime(WATCH_PATH, 1000000200L, 0L);
    glr_extedit_poll();

    editor_input_clear();                      /* gate opens */
    glr_extedit_poll();
    ASSERT_TRUE("the live document is still A", document_mentions("0, 0, 0"));

    /* The moment of truth: a real save of B must be followed, not mistaken for
     * gl-repl's own write because a stale hash was stamped as applied. */
    write_lines(WATCH_PATH, k_scene_b);
    set_mtime(WATCH_PATH, 1000000300L, 0L);
    glr_extedit_poll();
    ASSERT_TRUE("a genuine later save of B is followed",
                document_mentions("9, 9, 9"));
}

/* find_incomplete_final_row selects the last row with *code*; blanks and
 * comments after it are not statements. Truncating the line list at that row
 * threw them away, so a note written under a half-typed command vanished from
 * the document until the next complete save. */
static void test_comment_after_the_parked_row_survives(void) {
    static const char *const trailing[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        "glVertex3f(1,",
        "// still thinking about this one",
        NULL
    };

    printf("--- a comment below the parked row is not swallowed ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, trailing);
    glr_extedit_poll();

    ASSERT_INT("the reload succeeded", glr_extedit_stats().reloads, 1);
    ASSERT_INT("the half-typed row is parked", glr_extedit_stats().parked_rows, 1);
    ASSERT_TRUE("and it is the vertex",
                strcmp(editor_input_text(), "glVertex3f(1,") == 0);
    ASSERT_TRUE("the comment below it is still a document row",
                document_mentions("still thinking about this one"));
}

/* The path/stream reader hard-fails an over-long physical line. The watcher
 * reads with fgets into the same buffer, so an over-long line arrived as two
 * shorter ones - loading, atomically, a document the file reader would have
 * refused outright. */
static void test_overlong_line_is_refused_not_split(void) {
    char huge[MAX_LINE_LEN * 2];
    FILE *f;

    printf("--- an over-long physical line is refused, not split ---\n");

    begin_watched_session(k_scene_a);

    memset(huge, 'x', sizeof(huge));
    huge[sizeof(huge) - 1] = '\0';
    f = fopen(WATCH_PATH, "w");
    ASSERT_TRUE("fixture opens", f != NULL);
    if (f) {
        fprintf(f, "glClearColor(0.1, 0.1, 0.1, 1.0);\n");
        fprintf(f, "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);\n");
        fprintf(f, "// %s\n", huge);
        fprintf(f, "glBegin(GL_POINTS);\n");
        fprintf(f, "glVertex3f(0, 0, 0);\n");
        fprintf(f, "glEnd();\n");
        fclose(f);
    }
    glr_extedit_poll();

    ASSERT_INT("nothing was loaded from it", glr_extedit_stats().reloads, 0);
    ASSERT_INT("it was refused", glr_extedit_stats().failures, 1);
    ASSERT_TRUE("the previous scene survives", document_mentions("0, 0, 0"));
}

/* The watcher's heuristic reads raw physical lines; the importer strips
 * C-style block-comment spans before its accumulator ever sees them. A shared scanner
 * that does not know about block comments therefore disagrees with the loader
 * it is supposed to agree with: a trailing C-style comment has no statement
 * terminator, so it looked like a half-typed command.
 *
 * Asserted as an equivalence rather than against a hard-coded row count: the
 * watcher's document must be what the plain file loader would have produced,
 * whatever that is. */
static void test_block_comment_matches_the_plain_loader(void) {
    static const char *const with_block_comment[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        "/* a note in C style */",
        NULL
    };
    int loader_rows;

    printf("--- a trailing C-style comment is not a half-typed command ---\n");

    /* What the ordinary file loader makes of it. */
    write_lines(WATCH_PATH, with_block_comment);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    loader_rows = repl_state_document_count();

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, with_block_comment);
    glr_extedit_poll();

    ASSERT_INT("the reload succeeded", glr_extedit_stats().reloads, 1);
    ASSERT_INT("nothing was parked", glr_extedit_stats().parked_rows, 0);
    ASSERT_INT("and the watcher agrees with the plain loader",
               repl_state_document_count(), loader_rows);
}

/* `--watch scene.glr --tutorial N` arms the watcher after the lesson has
 * already replaced the document with a slot-less transient. The binding is
 * pinned during a lesson so it cannot be *lost* - but with nothing bound yet
 * there was nothing to pin, and the watcher stayed deaf for the whole session,
 * lesson included and afterwards. */
static void test_watch_binds_even_when_armed_during_a_lesson(void) {
    printf("--- arming during a lesson still binds ---\n");

    glr_extedit_set_enabled(0);
    write_lines(WATCH_PATH, k_scene_a);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    tutorial_start(0);
    ASSERT_TRUE("the tutorial is running", tutorial_active());

    glr_extedit_set_enabled(1);       /* what main() does for --watch */
    glr_extedit_bind_path(WATCH_PATH);
    glr_extedit_poll();
    ASSERT_TRUE("the watcher found the file anyway",
                glr_extedit_bound_path() != NULL);

    tutorial_stop();
    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_TRUE("and follows saves once the lesson is over",
                document_mentions("9, 9, 9"));
}

/* Stage 2's actual first use: a brand new `.glr`, opened in vim, one command
 * typed, saved before it is finished. Removing the incomplete row leaves the
 * loader nothing at all - and "no commands loaded" is a failure under ATOMIC,
 * so the save was refused and the row was never parked. The whole point of the
 * stage is that this file is a legitimate work in progress. */
static void test_file_that_is_only_an_incomplete_row(void) {
    static const char *const only_tail[] = {
        "glVertex3f(1,",
        NULL
    };
    static const char *const comment_and_tail[] = {
        "// starting a new scene",
        "glVertex3f(1,",
        NULL
    };

    printf("--- a file that is nothing but a half-typed row still parks ---\n");

    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, only_tail);
    glr_extedit_poll();

    ASSERT_INT("the save was not refused", glr_extedit_stats().failures, 0);
    ASSERT_INT("the row was parked", glr_extedit_stats().parked_rows, 1);
    ASSERT_TRUE("it is in the input row",
                strcmp(editor_input_text(), "glVertex3f(1,") == 0);
    ASSERT_INT("and the document is empty, like the file",
               repl_state_document_count(), 0);

    /* One comment above it is the same situation with one row to load. */
    begin_watched_session(k_scene_a);
    write_lines(WATCH_PATH, comment_and_tail);
    glr_extedit_poll();
    ASSERT_INT("with a comment above, also not refused",
               glr_extedit_stats().failures, 0);
    ASSERT_INT("also parked", glr_extedit_stats().parked_rows, 1);
    ASSERT_TRUE("the comment is the document",
                document_mentions("starting a new scene"));

    /* Finishing the command turns it into an ordinary complete file. */
    {
        static const char *const finished[] = {
            "// starting a new scene",
            "glVertex3f(1, 2, 3);",
            NULL
        };
        write_lines(WATCH_PATH, finished);
        glr_extedit_poll();
        ASSERT_INT("finishing it loads normally",
                   glr_extedit_stats().parked_rows, 1);  /* still just the one */
        ASSERT_INT("the input row is released", editor_input_len(), 0);
        ASSERT_TRUE("and the command is a document row",
                    document_mentions("glVertex3f(1, 2, 3)"));
    }
}

/* Binding does not reload - the document usually already is the file, and
 * reloading would clobber unsaved slot edits for nothing. That reasoning fails
 * on the way back: switch away, let the editor save, switch back, and stamping
 * the new bytes as "applied" buries the external edit permanently. */
static void test_returning_to_a_scene_picks_up_what_changed(void) {
    int slot_a;

    printf("--- coming back to a watched scene converges ---\n");

    begin_watched_session(k_scene_a);
    slot_a = repl_active_user_scene();
    ASSERT_TRUE("the CLI file is a user scene", slot_a >= 0);

    /* Switch away to a second scene, so the binding leaves WATCH_PATH. */
    write_lines(OTHER_PATH, k_scene_longer);
    {
        ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
        ASSERT_TRUE("a second scene opens",
                    repl_load_scene_as_new_slot(OTHER_PATH, &reason) >= 0);
    }
    glr_extedit_poll();
    ASSERT_TRUE("the binding followed the switch",
                glr_extedit_bound_path() != NULL &&
                strstr(glr_extedit_bound_path(), "other") != NULL);

    /* The editor saves the file we are no longer looking at. */
    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();

    /* Switch back. */
    ASSERT_TRUE("back to the first scene", repl_load_user_scene_idx(slot_a));
    glr_extedit_poll();   /* rebinds, and notices the file moved */
    glr_extedit_poll();   /* the gate applies it */

    ASSERT_TRUE("the edit made while we were away is picked up",
                document_mentions("9, 9, 9"));
}

/* And the other half: coming back to a file nobody touched must NOT reload,
 * or a scene switch would throw away edits made in gl-repl and never saved.
 * The outbound sync must still see those edits as local: restamping the live
 * document on rebind would treat them as already on disk and leave the file
 * stale. */
static void test_returning_to_an_untouched_scene_keeps_local_edits(void) {
    int slot_a;
    int rows;

    printf("--- coming back to an untouched file keeps unsaved work ---\n");

    begin_watched_session(k_scene_a);
    slot_a = repl_active_user_scene();

    /* An edit that only exists in gl-repl. */
    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_TRUE("a local line commits",
                editor_feed_line("glVertex3f(7, 7, 7);") != 0);
    editor_input_clear();
    rows = repl_state_document_count();

    write_lines(OTHER_PATH, k_scene_longer);
    {
        ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
        (void)repl_load_scene_as_new_slot(OTHER_PATH, &reason);
    }
    glr_extedit_poll();
    ASSERT_TRUE("back to the first scene", repl_load_user_scene_idx(slot_a));
    glr_extedit_poll();
    glr_extedit_poll();

    ASSERT_INT("the unsaved local line is still there",
               repl_state_document_count(), rows);
    ASSERT_TRUE("really still there", document_mentions("7, 7, 7"));
    ASSERT_INT("and the file is brought up to date",
               glr_extedit_stats().writes, 1);
    ASSERT_TRUE("the watched file carries the local line",
                file_contains(WATCH_PATH, "7, 7, 7"));
    (void)unlink(OTHER_PATH);
}

/* D4's no-push arm, driven for real. The earlier example test only proves an
 * *unbound* watcher does nothing - it never reaches apply_reload with no
 * active user scene. A file-backed `--examples-dir` catalog entry does: the
 * binding comes from glr_origin_path while the slot index is still -1. */
static void test_catalog_scene_reload_does_not_promote(void) {
    char root[512], scenes[512], catalog[512], scene_path[512], err[512];
    int slots_before;

    printf("--- reloading a catalog scene pushes no undo and no slot ---\n");

    glr_extedit_set_enabled(0);
    snprintf(root, sizeof(root), "/tmp/gl_repl_extedit_catalog");
    snprintf(scenes, sizeof(scenes), "%s/scenes", root);
    snprintf(catalog, sizeof(catalog), "%s/catalog.ini", root);
    snprintf(scene_path, sizeof(scene_path), "%s/watched.glr", scenes);
    (void)mkdir(root, 0700);
    (void)mkdir(scenes, 0700);
    write_lines(scene_path, k_scene_a);
    {
        FILE *f = fopen(catalog, "w");
        ASSERT_TRUE("catalog written", f != NULL);
        if (f) {
            fprintf(f, "[watched]\n"
                       "file = scenes/watched.glr\n"
                       "name = Watched catalog scene\n"
                       "tags = 3D\n"
                       "group = Runtime\n");
            fclose(f);
        }
    }

    glr_ctrl_reset_all();
    err[0] = '\0';
    ASSERT_TRUE("the runtime catalog loads",
                repl_examples_load_dir(root, err, sizeof(err)));
    (void)repl_load_example(0);
    /* The production scene action refreshes the input row after the pipeline
     * load. This test calls the lower-level loader directly, so mirror that
     * adapter step before asking the dirty-input gate for its answer. */
    editor_load_line_to_input(editor_state_edit_line());
    slots_before = repl_user_scene_count();
    ASSERT_INT("a catalog scene is not a user scene",
               repl_active_user_scene(), -1);

    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    ASSERT_TRUE("the watcher bound to the catalog file",
                glr_extedit_bound_path() != NULL);

    write_lines(scene_path, k_scene_b);
    glr_extedit_poll();

    ASSERT_INT("the reload happened", glr_extedit_stats().reloads, 1);
    ASSERT_TRUE("the new scene is live", document_mentions("9, 9, 9"));
    /* The two halves of D4's transient arm: no promotion, and no undo entry -
     * editor_undo_push_snapshot() is the promotion hook, so one implies the
     * other and both have to be asserted. */
    ASSERT_INT("no scene slot was created",
               repl_user_scene_count(), slots_before);
    ASSERT_INT("still not a user scene", repl_active_user_scene(), -1);
    ASSERT_TRUE("and nothing was pushed onto the undo ring",
                !editor_undo_can_undo());

    glr_extedit_set_enabled(0);
    repl_examples_clear_runtime_catalog();
    (void)unlink(scene_path);
    (void)unlink(catalog);
    (void)rmdir(scenes);
    (void)rmdir(root);
}

/* Startup has no stage-2 parking - `repl_load_initial_commands` uses the
 * ordinary loader - so a brand new `.glr` holding only a half-typed command
 * fails to import and lands on a seeded New Scene. That is fine; what is not
 * fine is losing the file. The session is still about the path the user named,
 * so the empty scene is bound to it and the watcher recovers on the next save.
 */
static void test_watch_survives_a_failed_startup_import(void) {
    static const char *const only_tail[] = { "glVertex3f(1,", NULL };

    printf("--- a file that fails at startup is still watched ---\n");

    glr_extedit_set_enabled(0);
    write_lines(WATCH_PATH, only_tail);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    ASSERT_TRUE("the failed import still bound the file",
                repl_active_scene_source_path() != NULL);

    glr_extedit_set_enabled(1);
    glr_extedit_bind_path(WATCH_PATH);
    glr_extedit_poll();
    ASSERT_TRUE("and the watcher is following it",
                glr_extedit_bound_path() != NULL);

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("the next save loads", glr_extedit_stats().reloads, 1);
    ASSERT_TRUE("the scene is live", document_mentions("9, 9, 9"));
}

/* `gl-repl --watch new.glr` where new.glr does not exist yet - about to be
 * created in the editor. The bootstrap import fails, a seeded New Scene takes
 * over, and the binding has to survive on the *slot* (the watcher follows the
 * active scene, so a binding that is not the active scene's file is
 * overwritten on the next poll by design). */
static void test_watching_a_file_that_does_not_exist_yet(void) {
    printf("--- a file created after launch is picked up ---\n");

    glr_extedit_set_enabled(0);
    (void)unlink(OTHER_PATH);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(OTHER_PATH);   /* no such file */
    ASSERT_TRUE("a seeded scene took over",
                repl_active_user_scene() >= 0);
    ASSERT_TRUE("bound to the file that does not exist yet",
                repl_active_scene_source_path() != NULL &&
                strstr(repl_active_scene_source_path(), "other") != NULL);

    glr_extedit_set_enabled(1);
    glr_extedit_bind_path(OTHER_PATH);
    for (int i = 0; i < 3; i++)
        glr_extedit_poll();
    ASSERT_TRUE("the watcher is following it anyway",
                glr_extedit_bound_path() != NULL &&
                strstr(glr_extedit_bound_path(), "other") != NULL);
    ASSERT_INT("nothing loaded while it is absent",
               glr_extedit_stats().reloads, 0);

    write_lines(OTHER_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("creating it is picked up", glr_extedit_stats().reloads, 1);
    ASSERT_TRUE("and its contents are live", document_mentions("9, 9, 9"));
    (void)unlink(OTHER_PATH);
}

/* The per-path history must cover all user-scene slots, not just a guessed
 * working set. */
static void test_seen_history_outlasts_the_scene_catalog(void) {
    char paths[MAX_USER_SCENES][64];
    int slot_a;

    printf("--- the per-path memory covers a full catalog of scenes ---\n");

    begin_watched_session(k_scene_a);
    slot_a = repl_active_user_scene();

    /* Fill the rest of the catalog, binding a different file each time. */
    for (int i = 0; i < MAX_USER_SCENES - 1; i++) {
        ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
        snprintf(paths[i], sizeof(paths[i]),
                 "/tmp/gl_repl_extedit_seen_%d.glr", i);
        write_lines(paths[i], k_scene_longer);
        if (repl_load_scene_as_new_slot(paths[i], &reason) < 0)
            break;
        glr_extedit_poll();
    }

    /* Somebody saves the very first file while we are away at the far end. */
    write_lines(WATCH_PATH, k_scene_b);
    ASSERT_TRUE("back to the first scene", repl_load_user_scene_idx(slot_a));
    glr_extedit_poll();
    glr_extedit_poll();
    ASSERT_TRUE("the save is not lost to an evicted history entry",
                document_mentions("9, 9, 9"));

    for (int i = 0; i < MAX_USER_SCENES - 1; i++)
        (void)unlink(paths[i]);
}

/* Runtime examples are file-backed too, and a catalog may be larger than the
 * user-scene capacity. The watcher must retain the first path while cycling
 * through more than the old fixed history size, or returning to it stamps an
 * external save as already applied and loses it. */
static void test_seen_history_covers_runtime_catalog(void) {
    enum { CATALOG_SCENES = 12 };
    char root[] = "/tmp/gl_repl_extedit_seen_catalog.XXXXXX";
    char scenes[512], catalog[512], paths[CATALOG_SCENES][512], err[512];
    char *made_root;
    FILE *f;

    printf("--- path history covers a runtime catalog larger than the old cap ---\n");

    glr_extedit_set_enabled(0);
    made_root = mkdtemp(root);
    ASSERT_TRUE("runtime catalog temp root", made_root != NULL);
    if (!made_root)
        return;
    snprintf(scenes, sizeof(scenes), "%s/scenes", root);
    snprintf(catalog, sizeof(catalog), "%s/catalog.ini", root);
    ASSERT_INT("runtime catalog scenes directory", mkdir(scenes, 0700), 0);

    f = fopen(catalog, "w");
    ASSERT_TRUE("runtime catalog descriptor", f != NULL);
    if (!f) {
        (void)rmdir(scenes);
        (void)rmdir(root);
        return;
    }
    for (int i = 0; i < CATALOG_SCENES; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/seen_%d.glr", scenes, i);
        write_lines(paths[i], k_scene_a);
        fprintf(f, "[seen_%d]\n"
                   "file = scenes/seen_%d.glr\n"
                   "name = Seen %d\n"
                   "tags = 3D\n"
                   "group = Runtime\n\n",
                i, i, i);
    }
    fclose(f);

    glr_ctrl_reset_all();
    err[0] = '\0';
    ASSERT_TRUE("runtime catalog loads",
                repl_examples_load_dir(root, err, sizeof(err)));
    ASSERT_INT("catalog has more paths than the old history cap",
               repl_example_count(), CATALOG_SCENES);
    ASSERT_TRUE("first catalog scene loads", repl_load_example(0) > 0);
    editor_load_line_to_input(editor_state_edit_line());

    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    for (int i = 1; i < CATALOG_SCENES; i++) {
        ASSERT_TRUE("catalog scene loads while cycling",
                    repl_load_example(i) > 0);
        editor_load_line_to_input(editor_state_edit_line());
        glr_extedit_poll();
    }

    /* Save the first file while it is not the active example. Returning to the
     * scene rebinds the path; the following poll must apply the pending edit. */
    write_lines(paths[0], k_scene_b);
    ASSERT_TRUE("return to the first catalog scene", repl_load_example(0) > 0);
    editor_load_line_to_input(editor_state_edit_line());
    glr_extedit_poll();
    glr_extedit_poll();
    ASSERT_TRUE("the external edit survives the catalog tour",
                document_mentions("9, 9, 9"));

    glr_extedit_set_enabled(0);
    repl_examples_clear_runtime_catalog();
    for (int i = 0; i < CATALOG_SCENES; i++)
        (void)unlink(paths[i]);
    (void)unlink(catalog);
    (void)rmdir(scenes);
    (void)rmdir(root);
}

/* D7 asks for the observed token to be stamped at lesson end. This code does
 * not, and the divergence is deliberate (see glr_extedit_poll and the plan's
 * Status section): the poll already stamps on every change, lesson included,
 * so the token is current - and restamping additionally swallows a save that
 * lands between the lesson ending and the next poll. That window is the whole
 * reason for the divergence, so it gets a guard of its own. */
static void test_save_between_lesson_end_and_poll_is_not_swallowed(void) {
    printf("--- a save right after a lesson ends is not discarded ---\n");

    begin_watched_session(k_scene_a);
    tutorial_start(0);
    glr_extedit_poll();
    tutorial_stop();

    /* No poll has run since the lesson ended - exactly the window a restamp
     * would close over. */
    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();

    ASSERT_INT("it is applied, not stamped away",
               glr_extedit_stats().reloads, 1);
    ASSERT_TRUE("the saved scene is live", document_mentions("9, 9, 9"));
}

/* A dismissed payload is per file, not per binding. Switching away and back
 * used to clear the suppression and re-offer the very version the user's own
 * commit had already beaten. */
static void test_dismissal_survives_a_scene_switch(void) {
    int slot_a;
    int rows_after_commit;

    printf("--- a dismissed version stays dismissed across a switch ---\n");

    begin_watched_session(k_scene_a);
    slot_a = repl_active_user_scene();

    /* Defer a save, then commit over it so it is dismissed. */
    begin_typing("glVertex3f(4, 4, 4);");
    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_TRUE("the line commits", editor_feed_line("glVertex3f(4, 4, 4);") != 0);
    editor_input_clear();
    rows_after_commit = repl_state_document_count();
    glr_extedit_poll();
    ASSERT_INT("it was dismissed", glr_extedit_stats().dismissals, 1);

    /* Away and back. */
    write_lines(OTHER_PATH, k_scene_longer);
    {
        ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
        (void)repl_load_scene_as_new_slot(OTHER_PATH, &reason);
    }
    glr_extedit_poll();
    ASSERT_TRUE("back to the first scene", repl_load_user_scene_idx(slot_a));
    glr_extedit_poll();
    glr_extedit_poll();

    ASSERT_INT("the dismissed version is not resurrected",
               repl_state_document_count(), rows_after_commit);
    ASSERT_TRUE("the committed line is still there",
                document_mentions("4, 4, 4"));
    (void)unlink(OTHER_PATH);
}

/* --- disabled ------------------------------------------------------------- */

static void test_disabled_watcher_does_nothing(void) {
    printf("--- without --watch nothing is watched ---\n");

    begin_watched_session(k_scene_a);
    glr_extedit_set_enabled(0);
    ASSERT_TRUE("no binding", glr_extedit_bound_path() == NULL);

    write_lines(WATCH_PATH, k_scene_b);
    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_INT("no reads", glr_extedit_stats().reads, 0);
    ASSERT_TRUE("the scene did not change", document_mentions("0, 0, 0"));
}

/* --- stage 2.5: the live WIP sidecar -------------------------------------- */

#define WIP_PATH WATCH_PATH ".wip"

/* Publish a buffer the way the editor's autocmd does: the whole buffer, then
 * one `// @cursor <row> <col>` line. `row` 0 omits the directive. */
static void publish_wip_at(const char *path, const char *const *lines,
                           int row, int col) {
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; lines && lines[i]; i++)
        fprintf(f, "%s\n", lines[i]);
    if (row > 0)
        fprintf(f, "// @cursor %d %d\n", row, col);
    fclose(f);
}

static void publish_wip(const char *const *lines, int row, int col) {
    publish_wip_at(WIP_PATH, lines, row, col);
}

/* The published buffer and the scene file start out identical, so anything the
 * sidecar does afterwards is visibly the sidecar's doing. */
static void begin_wip_session(const char *const *initial) {
    (void)unlink(WIP_PATH);
    begin_watched_session(initial);
    glr_modal_cancel();
}

static void test_sidecar_content_update_follows_without_saving(void) {
    GlrExtEditStats stats;

    printf("--- the sidecar updates the scene without a save ---\n");

    begin_wip_session(k_scene_a);
    ASSERT_TRUE("the session starts on scene A", document_mentions("0, 0, 0"));

    /* vim has NOT written the scene file - only the sidecar moved. */
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();

    stats = glr_extedit_stats();
    ASSERT_INT("one content update", stats.wip_updates, 1);
    ASSERT_TRUE("the scene followed the unsaved buffer",
                document_mentions("9, 9, 9"));
    ASSERT_TRUE("and the file on disk is untouched",
                !file_contains(WATCH_PATH, "9, 9, 9"));
    (void)unlink(WIP_PATH);
}

/* The whole reason the payload hash excludes the `@cursor` line. */
static void test_cursor_only_update_does_not_reimport(void) {
    GlrExtEditStats before, after;

    printf("--- moving the cursor costs no import ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    before = glr_extedit_stats();

    /* Same buffer, cursor moved. Five times, so a per-frame reimport would be
     * unmissable in the counters. */
    for (int i = 0; i < 5; i++) {
        set_mtime(WIP_PATH, 1000 + i, 0);
        publish_wip(k_scene_b, 1 + (i % 4), 1);
        set_mtime(WIP_PATH, 2000 + i, 0);
        glr_extedit_poll();
    }
    after = glr_extedit_stats();

    ASSERT_INT("no further content updates",
               after.wip_updates - before.wip_updates, 0);
    ASSERT_INT("no further reloads", after.reloads - before.reloads, 0);
    ASSERT_INT("five cursor moves", after.cursor_moves - before.cursor_moves, 5);
    (void)unlink(WIP_PATH);
}

/* The cursor row resolves through the cached map, not by subtracting a
 * constant - the file has a directive row the document does not. */
static void test_cursor_follows_through_the_row_map(void) {
    static const char *const with_header[] = {
        "// @scene-name Sidecar",          /* physical 1: no document row */
        "glClearColor(0.1, 0.1, 0.1, 1.0);",/* physical 2 -> document 0 */
        "glClear(GL_COLOR_BUFFER_BIT);",   /* physical 3 -> document 1 */
        "glBegin(GL_POINTS);",             /* physical 4 -> document 2 */
        "glVertex3f(5, 5, 5);",            /* physical 5 -> document 3 */
        "glEnd();",                        /* physical 6 -> document 4 */
        NULL
    };

    printf("--- the caret lands through the row map ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(with_header, 5, 1);
    glr_extedit_poll();
    ASSERT_TRUE("the buffer was adopted", document_mentions("5, 5, 5"));
    ASSERT_INT("physical 5 is document 3", editor_state_edit_line(), 3);

    /* Cursor-only, onto a row the content import did resolve but which is one
     * further off from its physical index. */
    set_mtime(WIP_PATH, 3000, 0);
    publish_wip(with_header, 6, 1);
    set_mtime(WIP_PATH, 4000, 0);
    glr_extedit_poll();
    ASSERT_INT("physical 6 is document 4", editor_state_edit_line(), 4);

    /* And onto the directive, which has no editable row at all. The honest
     * answer is to leave the caret alone, not to guess. */
    set_mtime(WIP_PATH, 5000, 0);
    publish_wip(with_header, 1, 1);
    set_mtime(WIP_PATH, 6000, 0);
    glr_extedit_poll();
    ASSERT_INT("a row with no document row moves nothing",
               editor_state_edit_line(), 4);
    (void)unlink(WIP_PATH);
}

/* Stage 2 could only park the trailing row. The sidecar names the row, so a
 * half-typed command in the middle of the file is followable. */
static void test_a_mid_file_half_typed_row_is_parked(void) {
    static const char *const mid_typing[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(1, 1,",                 /* physical 4: being typed */
        "glVertex3f(2, 2, 2);",
        "glEnd();",
        NULL
    };

    printf("--- a half-typed row in the middle of the file parks ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(mid_typing, 4, 15);
    glr_extedit_poll();

    ASSERT_INT("the import succeeded", glr_extedit_stats().failures, 0);
    ASSERT_STR("the half-typed row is in the input buffer",
               editor_input_text(), "glVertex3f(1, 1,");
    ASSERT_TRUE("the rows after it are in the document",
                document_mentions("2, 2, 2"));
    ASSERT_TRUE("and so is the closing glEnd", document_mentions("glEnd"));
    /* Parked between glBegin (document 2) and glVertex3f(2,2,2) - not at the
     * end, which is all stage 2 could do. */
    ASSERT_INT("parked at the row it came from", editor_state_edit_line(), 3);
    ASSERT_INT("the caret sits at the typed column", editor_cursor_pos(), 14);
    (void)unlink(WIP_PATH);
}

/* Live guides and autocomplete prefix-match at byte 0 (`glVertex3f(`). A
 * typical sidecar row is indented inside a block; parking it verbatim left
 * the prefix at column 4 and the plane/line/point guide never appeared.
 * Canonicalize like editor_load_line_to_input, and map vim's 1-based column
 * (which still counts the indent) onto the stripped buffer. */
static void test_indented_parked_row_is_canonical_for_guides(void) {
    static const char *const indented[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "    glVertex3f(1,",                 /* physical 4: four-space indent */
        "    glVertex3f(2, 2, 2);",
        "glEnd();",
        NULL
    };
    const char *input;

    printf("--- an indented half-typed row parks without its indent ---\n");

    begin_wip_session(k_scene_a);
    /* vim col 16 is the '1' in "    glVertex3f(1," (1-based, indent included). */
    publish_wip(indented, 4, 16);
    glr_extedit_poll();

    input = editor_input_text();
    ASSERT_INT("the import succeeded", glr_extedit_stats().failures, 0);
    ASSERT_STR("the parked text is the canonical command",
               input, "glVertex3f(1,");
    ASSERT_TRUE("the prefix the live guides match sits at byte 0",
                input && strncmp(input, "glVertex3f(", 11) == 0);
    /* col 16 - 1 - 4 indent = 11, the '1'. */
    ASSERT_INT("the caret is on the '1', not still in the stripped indent",
               editor_cursor_pos(), 11);
    (void)unlink(WIP_PATH);
}

/* The input buffer holds one row at a time, so navigating away from the parked
 * row in the editor overwrites it with the row navigated to - and coming back
 * has to restore it rather than leave whatever was last loaded. */
static void test_leaving_and_returning_to_the_parked_row(void) {
    static const char *const mid_typing[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(1, 1,",                 /* physical 4: being typed */
        "glVertex3f(2, 2, 2);",
        "glEnd();",
        NULL
    };

    printf("--- the parked row survives a trip away from it ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(mid_typing, 4, 15);
    glr_extedit_poll();
    ASSERT_STR("parked to begin with", editor_input_text(), "glVertex3f(1, 1,");

    /* Cursor-only, onto an ordinary row. */
    set_mtime(WIP_PATH, 14000, 0);
    publish_wip(mid_typing, 5, 1);
    set_mtime(WIP_PATH, 15000, 0);
    glr_extedit_poll();
    ASSERT_INT("no import for a cursor move",
               glr_extedit_stats().wip_updates, 1);
    ASSERT_TRUE("the caret is on the ordinary row now",
                strstr(editor_input_text(), "2, 2, 2") != NULL);

    /* And back. */
    set_mtime(WIP_PATH, 16000, 0);
    publish_wip(mid_typing, 4, 10);
    set_mtime(WIP_PATH, 17000, 0);
    glr_extedit_poll();
    ASSERT_INT("both moves were cursor-only", glr_extedit_stats().cursor_moves, 2);
    ASSERT_STR("the half-typed row is restored, not lost",
               editor_input_text(), "glVertex3f(1, 1,");
    ASSERT_INT("at the column the editor published", editor_cursor_pos(), 9);
    ASSERT_INT("and still with no import", glr_extedit_stats().wip_updates, 1);
    (void)unlink(WIP_PATH);
}

/* Ctrl+Z during a session goes back to the pre-session document and stays
 * there - the session does not immediately re-apply itself. */
static void test_undo_exits_the_session_and_sticks(void) {
    printf("--- undo leaves live follow, and stays left ---\n");

    begin_wip_session(k_scene_a);
    /* A user scene, so D4 says the session gets its one snapshot. */
    ASSERT_TRUE("the live scene is a user scene", repl_active_user_scene() >= 0);

    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    ASSERT_TRUE("the sidecar was applied", document_mentions("9, 9, 9"));

    editor_undo_pop_snapshot();
    ASSERT_TRUE("undo restores the pre-session document",
                document_mentions("0, 0, 0"));

    /* The sidecar has not moved, so nothing should re-apply. */
    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_TRUE("and the undo sticks", document_mentions("0, 0, 0"));

    /* Ctrl+Z drops the next publication (it may have been written before or
     * after the undo). The dismissed payload stays suppressed; a genuinely
     * new payload is followed on the publication after that. */
    set_mtime(WIP_PATH, 7000, 0);
    publish_wip(k_scene_longer, 4, 1);
    set_mtime(WIP_PATH, 8000, 0);
    glr_extedit_poll();
    ASSERT_TRUE("the publication that lifts the hold is not applied",
                document_mentions("0, 0, 0"));
    set_mtime(WIP_PATH, 9000, 0);
    publish_wip(k_scene_longer, 4, 1);
    set_mtime(WIP_PATH, 10000, 0);
    glr_extedit_poll();
    ASSERT_TRUE("the next one is", document_mentions("3, 3, 3"));
    (void)unlink(WIP_PATH);
}

/* One snapshot per session, not one per keystroke: the 32-slot ring is not a
 * place to store somebody's typing. */
static void test_a_session_costs_one_undo_entry(void) {
    printf("--- a session is one undo entry, not one per keystroke ---\n");

    begin_wip_session(k_scene_a);
    for (int i = 0; i < 6; i++) {
        static const char *const step[] = {
            "glClearColor(0.1, 0.1, 0.1, 1.0);",
            "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
            "glBegin(GL_POINTS);",
            "glVertex3f(0, 0, 0);",
            "glEnd();",
            NULL
        };
        char line[64];
        const char *buf[7];
        int n = 0;
        for (; step[n]; n++)
            buf[n] = step[n];
        snprintf(line, sizeof(line), "glVertex3f(%d, %d, %d);", i, i, i);
        buf[3] = line;
        buf[n] = NULL;
        set_mtime(WIP_PATH, 11000 + i * 2, 0);
        publish_wip(buf, 4, 1);
        set_mtime(WIP_PATH, 11001 + i * 2, 0);
        glr_extedit_poll();
    }
    ASSERT_INT("six content updates", glr_extedit_stats().wip_updates, 6);
    ASSERT_TRUE("the last one is live", document_mentions("5, 5, 5"));

    /* One undo, and we are back before the session started - not five steps
     * back through vim's typing, which vim's own undo owns. */
    editor_undo_pop_snapshot();
    ASSERT_TRUE("one undo reaches the pre-session document",
                document_mentions("0, 0, 0"));
    (void)unlink(WIP_PATH);
}

/* A bound document that is NOT a user scene. A built-in example cannot be
 * one - it has no file, so the watcher unbinds and the sidecar never runs. A
 * file-backed `--examples-dir` entry is the real shape: a binding from
 * glr_origin_path with the slot index still -1. Returns the scene path, or
 * NULL if the fixture could not be built.
 *
 * `sidecar_out` receives the sidecar path, which is derived from the *bound*
 * path and so is not WIP_PATH here.
 *
 * NOTE for whoever adds a test after one of these: this **replaces the example
 * catalog for the rest of the run** and nothing puts the built-in one back. So
 * `repl_load_example(0)` below here loads this one-entry catalog's file-backed
 * scene, not a built-in - which has a binding, where a built-in has none. The
 * one test that needs a built-in
 * (test_builtin_example_unbinds_the_watcher) is ordered ahead of every
 * catalog install for exactly that reason; keep it that way, or assert against
 * whichever catalog is actually loaded. */
static const char *begin_catalog_session(char *sidecar_out, size_t sidecar_sz) {
    static char scene_path[512];
    char root[512], scenes[512], catalog[512], err[512];

    glr_extedit_set_enabled(0);
    snprintf(root, sizeof(root), "/tmp/gl_repl_extedit_wip_catalog");
    snprintf(scenes, sizeof(scenes), "%s/scenes", root);
    snprintf(catalog, sizeof(catalog), "%s/catalog.ini", root);
    snprintf(scene_path, sizeof(scene_path), "%s/watched.glr", scenes);
    snprintf(sidecar_out, sidecar_sz, "%s.wip", scene_path);
    (void)mkdir(root, 0700);
    (void)mkdir(scenes, 0700);
    (void)unlink(sidecar_out);
    write_lines(scene_path, k_scene_a);
    {
        FILE *f = fopen(catalog, "w");
        if (!f)
            return NULL;
        fprintf(f, "[watched]\n"
                   "file = scenes/watched.glr\n"
                   "name = Watched catalog scene\n"
                   "tags = 3D\n"
                   "group = Runtime\n");
        fclose(f);
    }
    glr_ctrl_reset_all();
    err[0] = '\0';
    if (!repl_examples_load_dir(root, err, sizeof(err)))
        return NULL;
    (void)repl_load_example(0);
    /* The production scene action refreshes the input row after the pipeline
     * load; the low-level loader used here does not. */
    editor_load_line_to_input(editor_state_edit_line());
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    glr_modal_cancel();
    return scene_path;
}

/* D4's identity split, from the sidecar's side: typing in vim must not turn an
 * unedited catalog scene into a user scene. */
static void test_sidecar_does_not_promote_a_catalog_scene(void) {
    char sidecar[600];
    int slots_before;

    printf("--- vim typing does not promote a catalog scene ---\n");

    if (!begin_catalog_session(sidecar, sizeof(sidecar))) {
        ASSERT_TRUE("the runtime catalog fixture builds", 0);
        return;
    }
    ASSERT_INT("a catalog scene is not a user scene",
               repl_active_user_scene(), -1);
    ASSERT_TRUE("but it is bound", glr_extedit_bound_path() != NULL);
    slots_before = repl_user_scene_count();

    publish_wip_at(sidecar, k_scene_b, 4, 1);
    glr_extedit_poll();

    /* Non-vacuous: the update has to have *happened* for the absence of a
     * promotion to mean anything. */
    ASSERT_INT("the buffer was followed", glr_extedit_stats().wip_updates, 1);
    ASSERT_TRUE("the new geometry is live", document_mentions("9, 9, 9"));
    ASSERT_INT("and no scene slot was created", repl_user_scene_count(),
               slots_before);
    (void)unlink(sidecar);
}

/* `:q` while a session is running. A user scene keeps the text; a transient
 * goes back to the file, because keeping it would silently convert discarded
 * editor buffer into the live scene. */
static void test_sidecar_deletion_splits_on_identity(void) {
    printf("--- losing the sidecar splits on who owns the document ---\n");

    begin_wip_session(k_scene_a);
    ASSERT_TRUE("a user scene", repl_active_user_scene() >= 0);
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    ASSERT_TRUE("the buffer is live", document_mentions("9, 9, 9"));

    (void)unlink(WIP_PATH);
    glr_extedit_poll();
    ASSERT_TRUE("a user scene keeps the unsaved text",
                document_mentions("9, 9, 9"));

    /* Now a catalog scene, which owns nothing: D4 gave it no slot and no undo
     * entry, so keeping the text would make discarded editor buffer the live
     * scene with no way back. */
    {
        char sidecar[600];
        if (!begin_catalog_session(sidecar, sizeof(sidecar))) {
            ASSERT_TRUE("the runtime catalog fixture builds", 0);
            return;
        }
        publish_wip_at(sidecar, k_scene_b, 4, 1);
        glr_extedit_poll();
        ASSERT_TRUE("the catalog scene followed the buffer",
                    document_mentions("9, 9, 9"));

        (void)unlink(sidecar);
        glr_extedit_poll();
        ASSERT_TRUE("but on close it goes back to the file",
                    document_mentions("0, 0, 0"));
        ASSERT_TRUE("not the discarded buffer", !document_mentions("9, 9, 9"));
    }
}

/* A sidecar that is there before the binding forms is unsaved work from a dead
 * editor, and is offered rather than applied. */
static void test_a_sidecar_found_at_bind_time_is_offered(void) {
    printf("--- a leftover sidecar is offered, never applied ---\n");

    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    write_lines(WATCH_PATH, k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    glr_extedit_set_enabled(1);
    glr_extedit_poll();

    ASSERT_TRUE("the scene is still the file", document_mentions("0, 0, 0"));
    ASSERT_INT("nothing was applied", glr_extedit_stats().wip_updates, 0);
    ASSERT_TRUE("a prompt is up", glr_modal_active());
    ASSERT_INT("and it is the recovery prompt", (int)glr_modal_kind(),
               (int)GLR_MODAL_CONFIRM_WIP_RECOVER);

    /* Decline. The file stays on disk - gl-repl did not create it - and the
     * watcher stops following it. */
    glr_modal_cancel();
    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_TRUE("declining leaves the scene alone",
                document_mentions("0, 0, 0"));
    ASSERT_TRUE("and leaves the sidecar on disk",
                file_contains(WIP_PATH, "9, 9, 9"));
    ASSERT_TRUE("with no second prompt", !glr_modal_active());
    (void)unlink(WIP_PATH);
}

static void test_accepting_the_offer_starts_following(void) {
    printf("--- accepting the offer starts live follow ---\n");

    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    write_lines(WATCH_PATH, k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    ASSERT_TRUE("the prompt is up", glr_modal_active());

    glr_modal_handle_key('y');
    glr_extedit_poll();
    ASSERT_TRUE("the recovered buffer is now live",
                document_mentions("9, 9, 9"));
    ASSERT_TRUE("and the prompt is gone", !glr_modal_active());
    (void)unlink(WIP_PATH);
}

/* A `:w` writes out the very buffer the sidecar has been feeding us. Reloading
 * it would clear the input row and the caret for nothing. */
static void test_saving_during_a_session_is_not_a_second_reload(void) {
    GlrExtEditStats before, after;

    printf("--- a save during a session is already applied ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    before = glr_extedit_stats();

    /* vim writes the same buffer to the scene file. */
    set_mtime(WATCH_PATH, 12000, 0);
    write_lines(WATCH_PATH, k_scene_b);
    set_mtime(WATCH_PATH, 13000, 0);
    glr_extedit_poll();

    after = glr_extedit_stats();
    ASSERT_INT("the save triggers no reload", after.reloads - before.reloads, 0);
    ASSERT_TRUE("the scene is still the buffer", document_mentions("9, 9, 9"));
    (void)unlink(WIP_PATH);
}

/* The sidecar is not a scene: no `@cfg`, no camera, no rename - the same D3
 * policy a watched save follows, for the same reason. */
static void test_sidecar_is_geometry_not_presentation(void) {
    static const char *const with_metadata[] = {
        "// @scene-name Renamed By Vim",
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(7, 7, 7);",
        "glEnd();",
        NULL
    };
    int slot;
    char name_before[USER_SCENE_NAME_MAX];
    char path_before[512];

    printf("--- the sidecar carries geometry, not identity ---\n");

    begin_wip_session(k_scene_a);
    slot = repl_active_user_scene();
    ASSERT_TRUE("a user scene", slot >= 0);
    snprintf(name_before, sizeof(name_before), "%s", repl_user_scene_name(slot));
    /* Captured rather than spelled: the binding holds the *resolved* path, and
     * on macOS /tmp resolves through /private. */
    snprintf(path_before, sizeof(path_before), "%s",
             repl_active_scene_bound_path());

    publish_wip(with_metadata, 5, 1);
    glr_extedit_poll();

    ASSERT_TRUE("the geometry followed", document_mentions("7, 7, 7"));
    ASSERT_STR("the scene keeps its name", repl_user_scene_name(slot),
               name_before);
    ASSERT_STR("and its file", repl_active_scene_bound_path(), path_before);
    (void)unlink(WIP_PATH);
}

/* D5: a local commit dismisses the live sidecar payload. A later CursorMoved
 * of those same bytes must not look like new content and overwrite the line
 * just committed. Following resumes only when the payload hash changes. */
static void test_local_commit_suppresses_the_dismissed_wip_payload(void) {
    GlrExtEditStats after_commit, after_cursor, after_new;
    int rows_after_commit;

    printf("--- a local commit suppresses the dismissed WIP payload ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    ASSERT_TRUE("the sidecar is live", document_mentions("9, 9, 9"));

    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_TRUE("the line commits",
                editor_feed_line("glVertex3f(4, 4, 4);") != 0);
    editor_input_clear();
    rows_after_commit = repl_state_document_count();

    /* Same buffer, caret moved - the publication that first sees the
     * fingerprint change, then another one after the session has ended. */
    set_mtime(WIP_PATH, 18000, 0);
    publish_wip(k_scene_b, 2, 1);
    set_mtime(WIP_PATH, 19000, 0);
    glr_extedit_poll();
    after_commit = glr_extedit_stats();
    ASSERT_INT("the committed line survives the first CursorMoved",
               repl_state_document_count(), rows_after_commit);
    ASSERT_TRUE("and is really there", document_mentions("4, 4, 4"));
    ASSERT_INT("that payload was dismissed", after_commit.dismissals, 1);
    ASSERT_INT("and was not reimported", after_commit.wip_updates, 1);

    set_mtime(WIP_PATH, 20000, 0);
    publish_wip(k_scene_b, 3, 1);
    set_mtime(WIP_PATH, 21000, 0);
    glr_extedit_poll();
    after_cursor = glr_extedit_stats();
    ASSERT_INT("a second CursorMoved of the same bytes still does not import",
               after_cursor.wip_updates, 1);
    ASSERT_TRUE("the committed line is still live", document_mentions("4, 4, 4"));
    ASSERT_INT("and is not a cursor-only placement onto B's map either",
               after_cursor.cursor_moves, 0);

    set_mtime(WIP_PATH, 22000, 0);
    publish_wip(k_scene_longer, 4, 1);
    set_mtime(WIP_PATH, 23000, 0);
    glr_extedit_poll();
    after_new = glr_extedit_stats();
    ASSERT_INT("a new payload is followed immediately",
               after_new.wip_updates, 2);
    ASSERT_TRUE("and is live", document_mentions("3, 3, 3"));
    (void)unlink(WIP_PATH);
}

/* Ctrl+Z drops the publication that first observes the undo (the sidecar
 * may have been written before or after the key), then keeps suppressing
 * the dismissed payload so a later CursorMoved cannot undo the undo. */
static void test_undo_same_payload_cursor_does_not_reimport(void) {
    printf("--- undo does not let the same payload CursorMoved back in ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    ASSERT_TRUE("the sidecar was applied", document_mentions("9, 9, 9"));

    editor_undo_pop_snapshot();
    ASSERT_TRUE("undo restores the pre-session document",
                document_mentions("0, 0, 0"));

    set_mtime(WIP_PATH, 24000, 0);
    publish_wip(k_scene_b, 2, 1);
    set_mtime(WIP_PATH, 25000, 0);
    glr_extedit_poll();
    ASSERT_TRUE("the observing publication is dropped",
                document_mentions("0, 0, 0"));

    set_mtime(WIP_PATH, 26000, 0);
    publish_wip(k_scene_b, 3, 1);
    set_mtime(WIP_PATH, 27000, 0);
    glr_extedit_poll();
    ASSERT_TRUE("a second CursorMoved of the dismissed bytes stays out",
                document_mentions("0, 0, 0"));
    ASSERT_INT("neither was a content update",
               glr_extedit_stats().wip_updates, 1);
    (void)unlink(WIP_PATH);
}

/* The first WIP update of a user scene pushes an undo snapshot before the
 * import. A malformed first payload must not leave that push behind: it
 * would clear redo and, on a full ring, evict the oldest entry even though
 * no content landed. */
static void test_failed_wip_import_restores_the_undo_ring(void) {
    char probe[64];
    int base_rows;
    int popped = 0;

    printf("--- a failed first WIP update restores the undo ring ---\n");

    begin_wip_session(k_scene_a);
    base_rows = repl_state_document_count();

    for (int i = 0; i < UNDO_RING_SLOTS; i++) {
        editor_state_edit_line_set(repl_state_document_count());
        snprintf(probe, sizeof(probe), "glVertex3f(%d, 0, 0);", i);
        editor_undo_push_snapshot();
        (void)editor_feed_line(probe);
        editor_input_clear();
    }
    ASSERT_INT("the ring is full and the document grew",
               repl_state_document_count(), base_rows + UNDO_RING_SLOTS);

    publish_wip(k_scene_broken, 1, 1);
    glr_extedit_poll();
    ASSERT_INT("the import failed", glr_extedit_stats().failures, 1);
    ASSERT_INT("the document is untouched",
               repl_state_document_count(), base_rows + UNDO_RING_SLOTS);
    ASSERT_INT("and no WIP session started", glr_extedit_stats().wip_updates, 0);

    while (editor_undo_can_undo() && popped < UNDO_RING_SLOTS) {
        editor_undo_pop_snapshot();
        popped++;
    }
    ASSERT_INT("all 32 entries are still there", popped, UNDO_RING_SLOTS);
    ASSERT_INT("and the oldest one survived the failed push",
               repl_state_document_count(), base_rows);
    (void)unlink(WIP_PATH);
}

/* Cursor-only movement onto a physical continuation row has to resolve
 * through the map. Recording only the statement's first row left every
 * later physical row as REPL_ROW_MAP_NONE, so the caret did not move. */
static void test_cursor_follows_onto_a_continuation_row(void) {
    static const char *const continued[] = {
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(",
        "    1,",
        "    2,",
        "    3);",
        "glEnd();",
        NULL
    };

    printf("--- the caret follows onto a continuation row ---\n");

    begin_wip_session(k_scene_a);
    /* Cursor on a finished row so the multi-line statement is imported
     * whole, not parked at its opener (which would leave `1,` as a
     * free-standing parse error). */
    publish_wip(continued, 8, 1);
    glr_extedit_poll();
    ASSERT_TRUE("the joined statement is live", document_mentions("1, 2, 3"));
    ASSERT_INT("physical 8 is glEnd", editor_state_edit_line(), 4);

    set_mtime(WIP_PATH, 28000, 0);
    publish_wip(continued, 6, 1);
    set_mtime(WIP_PATH, 29000, 0);
    glr_extedit_poll();
    ASSERT_INT("no import for a cursor move",
               glr_extedit_stats().wip_updates, 1);
    ASSERT_INT("physical 6 is the vertex document row",
               editor_state_edit_line(), 3);
    (void)unlink(WIP_PATH);
}

static float live_t(void) {
    int idx = repl_eval_find_predef_var_idx("t");
    return idx >= 0 ? g_predef_vars[idx].value : -1.0f;
}

/* A same-scene WIP content update re-inits the predef table and used to
 * recreate `t` at 0, so an animated scene restarted on every keystroke.
 * anim_time and play/pause already survived; restoration must not go
 * through repl_state_time_set(), which would slam the free-running clock. */
static void test_wip_reload_preserves_visible_t(void) {
    ReplVariableView vars;
    int t_idx;
    int i;

    printf("--- a WIP content update keeps t, anim_time and play state ---\n");

    begin_wip_session(k_scene_a);
    t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t is bound", t_idx >= 0);
    repl_state_time_set(5.25f);
    repl_state_time_set_playing(0);
    repl_state_variables_mut()->anim_time = 12.5f;
    ASSERT_TRUE("t is the paused visible clock",
                fabsf(live_t() - 5.25f) < 1e-5f);
    ASSERT_INT("paused before any sidecar write",
               repl_state_variables().time_playing, 0);
    ASSERT_TRUE("anim_time has been allowed to drift",
                fabsf(repl_state_variables().anim_time - 12.5f) < 1e-5f);

    for (i = 0; i < 3; i++) {
        char mutated[64];
        const char *buf[6];
        int n = 0;

        for (; k_scene_b[n]; n++)
            buf[n] = k_scene_b[n];
        snprintf(mutated, sizeof(mutated), "glVertex3f(%d, 9, 9);", 7 + i);
        buf[3] = mutated;
        buf[n] = NULL;
        set_mtime(WIP_PATH, 30000 + i * 2, 0);
        publish_wip(buf, 4, 1);
        set_mtime(WIP_PATH, 30001 + i * 2, 0);
        glr_extedit_poll();
    }
    ASSERT_INT("three content updates landed", glr_extedit_stats().wip_updates, 3);
    ASSERT_TRUE("visible t is unchanged", fabsf(live_t() - 5.25f) < 1e-5f);
    vars = repl_state_variables();
    ASSERT_TRUE("anim_time was not reset to match t",
                fabsf(vars.anim_time - 12.5f) < 1e-5f);
    ASSERT_INT("pause survived", vars.time_playing, 0);

    repl_state_time_set_playing(1);
    set_mtime(WIP_PATH, 30100, 0);
    publish_wip(k_scene_longer, 4, 1);
    set_mtime(WIP_PATH, 30200, 0);
    glr_extedit_poll();
    ASSERT_TRUE("t is still the pre-reload value while playing",
                fabsf(live_t() - 5.25f) < 1e-5f);
    ASSERT_INT("play survived", repl_state_variables().time_playing, 1);
    ASSERT_TRUE("anim_time is still the drifted clock",
                fabsf(repl_state_variables().anim_time - 12.5f) < 1e-5f);
    (void)unlink(WIP_PATH);
}

#define LONG_WIP_VERTS 36
#define LONG_WIP_ROWS  (3 + LONG_WIP_VERTS + 1)

static int fill_long_wip(char storage[][64], const char **ptrs,
                         int incomplete_phys) {
    int n = 0;
    int i;

    snprintf(storage[n], 64, "glClearColor(0.1, 0.1, 0.1, 1.0);");
    ptrs[n] = storage[n];
    n++;
    snprintf(storage[n], 64, "glClear(GL_COLOR_BUFFER_BIT);");
    ptrs[n] = storage[n];
    n++;
    snprintf(storage[n], 64, "glBegin(GL_POINTS);");
    ptrs[n] = storage[n];
    n++;
    for (i = 0; i < LONG_WIP_VERTS; i++) {
        int phys = n + 1;
        if (phys == incomplete_phys)
            snprintf(storage[n], 64, "glVertex3f(%d,", i);
        else
            snprintf(storage[n], 64, "glVertex3f(%d, 0, 0);", i);
        ptrs[n] = storage[n];
        n++;
    }
    snprintf(storage[n], 64, "glEnd();");
    ptrs[n] = storage[n];
    n++;
    ptrs[n] = NULL;
    return n;
}

static int cursor_row_visible_after_layout(int *out_follow, int *out_visible) {
    UiRenderSnapshot snap;

    ui_state_viewport_set_size(800, 220);
    glr_ctrl_build_ui_snapshot(&snap);
    return glr_ctrl_code_panel_apply_scroll_follow_for_test(
        &snap, out_follow, out_visible);
}

/* Sidecar cursor placement used to change the row and never raise
 * editor_scroll_follow_cursor, so an offscreen caret stayed offscreen.
 * A mapped committed row and a parked hole both request follow; an
 * unmapped physical row leaves the viewport alone. */
static void test_sidecar_cursor_requests_follow_scroll(void) {
    char storage[LONG_WIP_ROWS + 1][64];
    const char *ptrs[LONG_WIP_ROWS + 2];
    static const char *const with_header[] = {
        "// @scene-name Scroll",
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(5, 5, 5);",
        "glEnd();",
        NULL
    };
    int follow = -1, visible = 0;
    int scroll_before;
    int n;
    int last_phys;

    printf("--- a sidecar cursor move follows the caret into view ---\n");

    n = fill_long_wip(storage, ptrs, 0);
    last_phys = n;
    begin_wip_session(k_scene_a);
    publish_wip(ptrs, 1, 1);
    glr_extedit_poll();
    ASSERT_INT("the long buffer was adopted",
               glr_extedit_stats().wip_updates, 1);

    editor_scroll_set(0);
    editor_scroll_follow_cursor_set(0);
    set_mtime(WIP_PATH, 31000, 0);
    publish_wip(ptrs, last_phys, 1);
    set_mtime(WIP_PATH, 31100, 0);
    glr_extedit_poll();
    ASSERT_INT("a mapped row requests follow-scroll",
               editor_scroll_follow_cursor(), 1);
    ASSERT_INT("the caret is on the last document row",
               editor_state_edit_line(), repl_state_document_count() - 1);
    ASSERT_TRUE("and layout reveals that row",
                cursor_row_visible_after_layout(&follow, &visible));

    /* Parked hole: import with an incomplete mid-file row, navigate away,
     * then come back. */
    n = fill_long_wip(storage, ptrs, 20);
    set_mtime(WIP_PATH, 31200, 0);
    publish_wip(ptrs, 20, 10);
    set_mtime(WIP_PATH, 31300, 0);
    glr_extedit_poll();
    ASSERT_TRUE("the hole is parked",
                strncmp(editor_input_text(), "glVertex3f(", 11) == 0);

    set_mtime(WIP_PATH, 31400, 0);
    publish_wip(ptrs, 3, 1);
    set_mtime(WIP_PATH, 31500, 0);
    glr_extedit_poll();
    editor_scroll_set(0);
    editor_scroll_follow_cursor_set(0);
    set_mtime(WIP_PATH, 31600, 0);
    publish_wip(ptrs, 20, 10);
    set_mtime(WIP_PATH, 31700, 0);
    glr_extedit_poll();
    ASSERT_INT("returning to the parked row requests follow",
               editor_scroll_follow_cursor(), 1);
    ASSERT_TRUE("and layout reveals the hole",
                cursor_row_visible_after_layout(&follow, &visible));

    /* Unmapped physical row: a consumed header. Scroll and follow stay put. */
    begin_wip_session(k_scene_a);
    publish_wip(with_header, 5, 1);
    glr_extedit_poll();
    editor_scroll_set(2);
    editor_scroll_follow_cursor_set(0);
    scroll_before = editor_scroll();
    set_mtime(WIP_PATH, 31800, 0);
    publish_wip(with_header, 1, 1);
    set_mtime(WIP_PATH, 31900, 0);
    glr_extedit_poll();
    ASSERT_INT("an unmapped row does not request follow",
               editor_scroll_follow_cursor(), 0);
    ASSERT_INT("and does not move the viewport",
               editor_scroll(), scroll_before);
    (void)unlink(WIP_PATH);
}

#define EXPORTED_C_MAX_LINES 512

static int read_c_lines(const char *path, char **out, int max) {
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

static void free_c_lines(char **lines, int n) {
    int i;
    for (i = 0; i < n; i++)
        free(lines[i]);
}

static int find_c_line_with(char *const *lines, int n, const char *needle) {
    int i;
    for (i = 0; i < n; i++)
        if (strstr(lines[i], needle))
            return i;
    return -1;
}

static int find_doc_row_with(const char *needle) {
    int i, count = repl_state_document_count();
    for (i = 0; i < count; i++) {
        const char *line = editor_buffer_line(i);
        if (line && strstr(line, needle))
            return i;
    }
    return -1;
}

/* The WIP sidecar names a *physical* row. Exported C wraps the document in
 * helpers, a Snippet-start marker, camera rows and display(). The map has
 * to send vim's cursor on a snippet-body command to the document row it
 * produced, not leave the caret on whatever row the last .glr session used. */
static void test_exported_c_sidecar_cursor_follows_snippet_row(void) {
    char *lines[EXPORTED_C_MAX_LINES];
    const char *c_ptrs[EXPORTED_C_MAX_LINES];
    int n, i, phys_vertex, doc_vertex;

    printf("--- exported C sidecar cursor follows a snippet-body row ---\n");

    glr_extedit_set_enabled(0);
    write_lines(WATCH_PATH, k_scene_a);
    glr_ctrl_reset_all();
    ASSERT_TRUE("the .glr source loads",
                repl_load_initial_commands(WATCH_PATH) != 0);
    ASSERT_TRUE("and exports as C",
                repl_export_save_output(WATCH_C_PATH, source_document_view(),
                                        NULL) != 0);

    n = read_c_lines(WATCH_C_PATH, lines, EXPORTED_C_MAX_LINES);
    ASSERT_TRUE("the export reads back", n > 0);
    phys_vertex = find_c_line_with(lines, n, "glVertex3f(0, 0, 0)");
    ASSERT_TRUE("the vertex is in the exported C", phys_vertex >= 0);

    glr_ctrl_reset_all();
    ASSERT_TRUE("the exported C loads as the live scene",
                repl_load_initial_commands(WATCH_C_PATH) != 0);
    editor_load_line_to_input(editor_state_edit_line());
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    glr_modal_cancel();
    ASSERT_TRUE("the watcher bound the .c",
                glr_extedit_bound_path() != NULL);
    ASSERT_TRUE("the bound path is the exported C",
                strstr(glr_extedit_bound_path(), ".c") != NULL);

    for (i = 0; i < n; i++)
        c_ptrs[i] = lines[i];
    c_ptrs[n] = NULL;

    /* Exported snippet-body rows are indented (`    glVertex3f`). vim's
     * col('.') counts those spaces; the live input does not. Col 5 is the
     * 'g' of glVertex3f. */
    publish_wip_at(WIP_C_PATH, c_ptrs, phys_vertex + 1, 5);
    glr_extedit_poll();

    doc_vertex = find_doc_row_with("glVertex3f(0, 0, 0)");
    ASSERT_TRUE("the vertex is still in the document", doc_vertex >= 0);
    ASSERT_INT("the sidecar cursor landed on that document row",
               editor_state_edit_line(), doc_vertex);
    ASSERT_INT("as a content update, not a failed C import",
               glr_extedit_stats().failures, 0);
    ASSERT_INT("and not as a no-op on an unmapped physical row",
               glr_extedit_stats().wip_updates, 1);
    ASSERT_INT("the caret is on the 'g', not still in the C indent",
               editor_cursor_pos(), 0);

    /* Cursor-only onto glEnd, which is a different document row. */
    {
        int phys_end = find_c_line_with(lines, n, "glEnd();");
        int doc_end = find_doc_row_with("glEnd();");
        int lead = 0;
        ASSERT_TRUE("glEnd is in the file", phys_end >= 0);
        ASSERT_TRUE("and in the document", doc_end >= 0);
        while (lines[phys_end][lead] == ' ' || lines[phys_end][lead] == '\t')
            lead++;
        set_mtime(WIP_C_PATH, 40000, 0);
        publish_wip_at(WIP_C_PATH, c_ptrs, phys_end + 1, lead + 1);
        set_mtime(WIP_C_PATH, 40100, 0);
        glr_extedit_poll();
        ASSERT_INT("no reimport for a cursor-only C move",
                   glr_extedit_stats().wip_updates, 1);
        ASSERT_INT("physical glEnd is the document glEnd",
                   editor_state_edit_line(), doc_end);
        ASSERT_INT("and the caret is at the start of glEnd, not in its indent",
                   editor_cursor_pos(), 0);
    }

    /* Half-typed inside the snippet: the C import must park the hole, not
     * fail the whole file, and the caret must sit on the typed column. */
    {
        char parked_src[64];
        int lead = 0;
        while (lines[phys_vertex][lead] == ' ' ||
               lines[phys_vertex][lead] == '\t')
            lead++;
        snprintf(parked_src, sizeof(parked_src), "%.*sglVertex3f(0,",
                 lead, lines[phys_vertex]);
        free(lines[phys_vertex]);
        lines[phys_vertex] = (char *)malloc(strlen(parked_src) + 1);
        ASSERT_TRUE("the parked C row fits", lines[phys_vertex] != NULL);
        memcpy(lines[phys_vertex], parked_src, strlen(parked_src) + 1);
        c_ptrs[phys_vertex] = lines[phys_vertex];
        set_mtime(WIP_C_PATH, 40200, 0);
        /* vim insert-at-end is one past the last byte, indent included. */
        publish_wip_at(WIP_C_PATH, c_ptrs, phys_vertex + 1,
                       lead + (int)strlen("glVertex3f(0,") + 1);
        set_mtime(WIP_C_PATH, 40300, 0);
        glr_extedit_poll();
        ASSERT_INT("the incomplete C row did not fail the import",
                   glr_extedit_stats().failures, 0);
        ASSERT_STR("it is parked without the C indent",
                   editor_input_text(), "glVertex3f(0,");
        ASSERT_INT("and the caret is at the typed end",
                   editor_cursor_pos(), (int)strlen("glVertex3f(0,"));
    }

    free_c_lines(lines, n);
    (void)unlink(WIP_C_PATH);
    (void)unlink(WATCH_C_PATH);
}

/* A buffer that will not parse is the normal state of a file being typed. It
 * must not be retried every frame, and it must not damage what is live. */
/* Publish the way the editor plugin does: a sibling temp in the same
 * directory, renamed over the target. The rename is the atomicity - an
 * in-place write lets the watcher read the file between the truncate and the
 * last byte. */
static void publish_wip_atomically(const char *const *lines, int row, int col) {
    char tmp[600];

    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", WIP_PATH, (int)getpid());
    publish_wip_at(tmp, lines, row, col);
    if (rename(tmp, WIP_PATH) != 0)
        (void)unlink(tmp);
}

/* The transport line is stripped before the load, so it can never reach the
 * document - and therefore can never reach a file gl-repl writes. Worth
 * pinning rather than trusting: `@plot` deliberately rides a row's text
 * through export, and the difference between the two is a rule nothing else
 * enforces. A cursor position written into somebody's scene file would come
 * back on every later load. */
static void test_the_cursor_directive_never_reaches_a_saved_file(void) {
    printf("--- @cursor never survives into a saved scene ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(k_scene_b, 4, 7);
    glr_extedit_poll();
    ASSERT_INT("the buffer was followed", glr_extedit_stats().wip_updates, 1);

    for (int i = 0; i < repl_state_document_count(); i++) {
        const char *line = editor_buffer_line(i);
        ASSERT_TRUE("no document row mentions @cursor",
                    !line || strstr(line, "@cursor") == NULL);
    }

    /* And out through the writer, which is the half that would do the damage.
     * Ctrl+S during a session writes the document - the parked row is
     * deliberately not in it, and neither is the transport line. */
    ASSERT_TRUE("Ctrl+S writes the bound file",
                repl_save_active_scene(NULL) != 0);
    ASSERT_TRUE("the saved file carries the geometry",
                file_contains(WATCH_PATH, "9, 9, 9"));
    ASSERT_TRUE("and no cursor directive",
                !file_contains(WATCH_PATH, "@cursor"));
    (void)unlink(WIP_PATH);
}

/* Forty back-to-back publications all land, and the document never ends up
 * shorter than the one before.
 *
 * Named for what it proves, which is *not* atomicity: the helper finishes its
 * rename before the poll runs, so no read/write interleaving is possible here,
 * and it drives its own publisher rather than the shipped plugin. What
 * actually keeps a torn read off the table is the plugin's sibling-temp-plus-
 * rename, and the thing that holds it there is `make check-wip-plugin-atomic`
 * - a test cannot, because a test writes its own file. The companion below
 * covers the other half: what happens if some publisher gets it wrong anyway.
 */
static void test_repeated_publications_all_land(void) {
    GlrExtEditStats stats;
    int rows_expected;

    printf("--- repeated complete publications all land ---\n");

    begin_wip_session(k_scene_a);
    publish_wip_atomically(k_scene_longer, 4, 1);
    glr_extedit_poll();
    rows_expected = repl_state_document_count();
    ASSERT_TRUE("the first publication landed", rows_expected > 0);

    for (int i = 0; i < 40; i++) {
        char line[64];
        const char *buf[8];
        int n = 0;

        for (; k_scene_longer[n]; n++)
            buf[n] = k_scene_longer[n];
        snprintf(line, sizeof(line), "glVertex3f(%d, %d, %d);", i, i, i);
        buf[4] = line;             /* one row differs per publication */
        buf[n] = NULL;
        /* Pinned mtimes: 40 same-size writes to one inode land inside a
         * single filesystem timestamp tick on ext4, and the change token
         * would then not move. */
        set_mtime(WIP_PATH, 20000 + i * 2, 0);
        publish_wip_atomically(buf, 4, 1);
        set_mtime(WIP_PATH, 20001 + i * 2, 0);
        glr_extedit_poll();
        ASSERT_INT("the document never shrinks mid-publication",
                   repl_state_document_count(), rows_expected);
    }

    stats = glr_extedit_stats();
    ASSERT_INT("no publication failed to parse", stats.failures, 0);
    ASSERT_INT("every one of them landed", stats.wip_updates, 41);
    ASSERT_TRUE("and the last one is live", document_mentions("39, 39, 39"));
    (void)unlink(WIP_PATH);
}

/* The failure the rename exists to prevent, forced directly: a sidecar that
 * *is* half-written. A publisher without the temp-and-rename discipline - a
 * different editor, a shell redirect, a plugin regression the guard did not
 * catch - can produce exactly this, and what must not happen is a clean
 * atomic import of the truncated prefix silently replacing the scene.
 *
 * The signal is the missing `// @cursor` trailer, not the cut token: a prefix
 * of a valid program is usually itself a valid program, and before this the
 * torn file loaded *cleanly* - two rows adopted and the severed `glBegin(`
 * parked as an ordinary half-typed row. The truncation is only detectable
 * because the plugin always writes the trailer last. A cut that happens to
 * land just after a complete trailer is indistinguishable, which is why the
 * rename is the real protection and this is the backstop under it. */
static void test_a_torn_publication_cannot_damage_the_scene(void) {
    GlrExtEditStats before, after;

    printf("--- a half-written sidecar cannot replace the scene ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(k_scene_longer, 4, 1);
    glr_extedit_poll();
    ASSERT_TRUE("a session is running", document_mentions("3, 3, 3"));
    before = glr_extedit_stats();

    /* Straight into the target with no rename, cut off inside a command. */
    {
        FILE *f = fopen(WIP_PATH, "w");
        ASSERT_TRUE("the torn file is written", f != NULL);
        if (f) {
            fputs("glClearColor(0.3, 0.3, 0.3, 1.0);\n", f);
            fputs("glClear(GL_COLOR_BUFFER_BIT);\n", f);
            fputs("glBegin(GL_POIN", f);        /* cut mid-token */
            fclose(f);
        }
        set_mtime(WIP_PATH, 60000, 0);
    }
    glr_extedit_poll();
    after = glr_extedit_stats();

    ASSERT_INT("the torn payload is refused", after.wip_updates,
               before.wip_updates);
    ASSERT_INT("and refused loudly", after.failures - before.failures, 1);
    ASSERT_TRUE("the live scene is intact", document_mentions("3, 3, 3"));

    /* And the editor finishing the job is followed normally. */
    set_mtime(WIP_PATH, 60002, 0);
    publish_wip(k_scene_b, 4, 1);
    set_mtime(WIP_PATH, 60003, 0);
    glr_extedit_poll();
    ASSERT_INT("the completed publication lands",
               glr_extedit_stats().wip_updates, before.wip_updates + 1);
    ASSERT_TRUE("with its geometry", document_mentions("9, 9, 9"));
    (void)unlink(WIP_PATH);
}

/* The third row of the plan's session-exit table: a session that ended takes a
 * *fresh* entry snapshot when the next one starts, rather than reusing the
 * first one's - which would make the second Ctrl+Z land wherever the first
 * session began. */
static void test_a_second_session_takes_its_own_entry_snapshot(void) {
    printf("--- a second session gets its own undo entry ---\n");

    begin_wip_session(k_scene_a);
    ASSERT_TRUE("a user scene", repl_active_user_scene() >= 0);

    /* Session one. */
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    ASSERT_TRUE("session one is live", document_mentions("9, 9, 9"));

    /* End it the way `:q` does, on a user scene: the text is kept. */
    (void)unlink(WIP_PATH);
    glr_extedit_poll();
    ASSERT_TRUE("the text survives the editor closing",
                document_mentions("9, 9, 9"));

    /* Session two, from a document that is no longer the one session one
     * started from. */
    publish_wip(k_scene_longer, 4, 1);
    glr_extedit_poll();
    ASSERT_TRUE("session two is live", document_mentions("3, 3, 3"));

    /* One undo has to reach session two's *own* starting point - scene B -
     * not scene A, which is where session one began. */
    editor_undo_pop_snapshot();
    ASSERT_TRUE("undo lands on where session two started",
                document_mentions("9, 9, 9"));
    ASSERT_TRUE("not on where session one did", !document_mentions("0, 0, 0"));
    (void)unlink(WIP_PATH);
}

/* D7 reaches the sidecar too, and by a different route than it reaches a
 * save. A lesson drives the document itself, so a payload the editor computed
 * against the *pre*-lesson document must not land on what the lesson left
 * behind. There is no parked version to dismiss here - the gate simply
 * observes each publication and drops it - so what has to be asserted is that
 * nothing lands during the lesson, nothing lands after it either, and
 * following resumes only on a publication written after the lesson ended. */
static void test_a_lesson_holds_the_sidecar_and_drops_what_it_missed(void) {
    int rows_after_lesson;

    printf("--- a lesson holds the sidecar, and drops what it missed ---\n");

    begin_wip_session(k_scene_a);
    tutorial_start(0);
    ASSERT_TRUE("the tutorial is running", tutorial_active());

    publish_wip(k_scene_b, 4, 1);
    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_INT("nothing followed during the lesson",
               glr_extedit_stats().wip_updates, 0);

    tutorial_stop();
    rows_after_lesson = repl_state_document_count();

    /* The publication that arrived during the lesson stays dropped: the
     * observed token moved while the gate was shut, so there is nothing left
     * to re-read. */
    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_INT("and it does not land afterwards",
               glr_extedit_stats().wip_updates, 0);
    ASSERT_INT("the document the lesson left behind is untouched",
               repl_state_document_count(), rows_after_lesson);

    /* A publication written *after* the lesson is ordinary live follow. */
    set_mtime(WIP_PATH, 30000, 0);
    publish_wip(k_scene_longer, 4, 1);
    set_mtime(WIP_PATH, 30001, 0);
    glr_extedit_poll();
    ASSERT_INT("following resumes on the next real publication",
               glr_extedit_stats().wip_updates, 1);
    (void)unlink(WIP_PATH);
}

/* D1: the sidecar is derived from the binding, so a scene switch has to take
 * it with it. Leaving the old session running would mean the next keystroke in
 * an editor pointed at the *previous* scene silently overwrote the new one.
 *
 * Switching to another file-backed scene rather than to a built-in example on
 * purpose: it is the case where a binding still exists afterwards, so it
 * actually tests that the sidecar path was re-derived instead of that
 * everything was torn down. It is also the only form that does not depend on
 * whether some earlier test left a runtime catalog installed, which decides
 * whether "example 0" has a file. */
static void test_switching_scenes_ends_the_session(void) {
    GlrExtEditStats before, after;
    char sidecar[600];
    char bound_before[512];

    printf("--- switching scenes ends the session and moves the sidecar ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    ASSERT_TRUE("a session is running", document_mentions("9, 9, 9"));
    snprintf(bound_before, sizeof(bound_before), "%s",
             glr_extedit_bound_path() ? glr_extedit_bound_path() : "");

    /* Away to a different file-backed scene. */
    if (!begin_catalog_session(sidecar, sizeof(sidecar))) {
        ASSERT_TRUE("the runtime catalog fixture builds", 0);
        return;
    }
    ASSERT_TRUE("the binding followed the switch",
                glr_extedit_bound_path() != NULL &&
                strcmp(glr_extedit_bound_path(), bound_before) != 0);

    before = glr_extedit_stats();
    /* The editor keeps typing at the old file. Nothing may follow: that
     * sidecar belongs to a scene which is no longer live. */
    set_mtime(WIP_PATH, 31000, 0);
    publish_wip(k_scene_longer, 4, 1);
    set_mtime(WIP_PATH, 31001, 0);
    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    after = glr_extedit_stats();
    ASSERT_INT("the orphaned sidecar is not followed",
               after.wip_updates - before.wip_updates, 0);
    ASSERT_TRUE("and the new scene is untouched", !document_mentions("3, 3, 3"));

    /* The new binding has a sidecar of its own, and that one is followed. */
    publish_wip_at(sidecar, k_scene_longer, 4, 1);
    glr_extedit_poll();
    ASSERT_INT("the new scene's own sidecar is",
               glr_extedit_stats().wip_updates - before.wip_updates, 1);
    ASSERT_TRUE("with its geometry", document_mentions("3, 3, 3"));
    (void)unlink(sidecar);
    (void)unlink(WIP_PATH);
}

/* Switching away and back while the editor is still open must not look like a
 * recovery. The prompt is for a `.wip` left behind by an editor that died
 * before gl-repl started; one this session was following seconds ago is not
 * that, and prompting would put a modal in front of every F12 round trip.
 *
 * This is the sidecar's version of a bug the scene file already had and the
 * per-path memory already fixed - see "coming back to a watched scene
 * converges" above - so it is fixed the same way and asserted the same way: no
 * prompt, and the document converges on what the editor now holds.
 *
 * The switch is a scene load with the watcher left armed, which is what the
 * app does. Toggling glr_extedit_set_enabled() would clear the per-path
 * memory and prove nothing. */
static void test_returning_to_a_live_session_is_not_a_recovery(void) {
    int slot_a;

    printf("--- coming back to an open editor is not a recovery ---\n");

    begin_wip_session(k_scene_a);
    slot_a = repl_active_user_scene();
    ASSERT_TRUE("the CLI file is a user scene", slot_a >= 0);
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    ASSERT_INT("the session followed once", glr_extedit_stats().wip_updates, 1);

    /* Away to a second scene. The sidecar stays on disk the whole time,
     * because the editor never closed. */
    write_lines(OTHER_PATH, k_scene_longer);
    {
        ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
        ASSERT_TRUE("a second scene opens",
                    repl_load_scene_as_new_slot(OTHER_PATH, &reason) >= 0);
    }
    glr_extedit_poll();
    ASSERT_TRUE("the binding followed the switch",
                glr_extedit_bound_path() != NULL &&
                strstr(glr_extedit_bound_path(), "other") != NULL);

    /* And back. */
    ASSERT_TRUE("back to the first scene", repl_load_user_scene_idx(slot_a));
    glr_extedit_poll();   /* rebinds */
    glr_extedit_poll();   /* and follows */

    ASSERT_TRUE("no recovery prompt on the way back", !glr_modal_active());
    ASSERT_TRUE("the editor's buffer is live again",
                document_mentions("9, 9, 9"));
    (void)unlink(WIP_PATH);
}

/* Declining the recovery offer has to survive a scene switch. gl-repl does
 * not delete the `.wip` on decline - it did not create that file, and it may
 * be the only copy of the work - so the file is still sitting there on the way
 * back, and without a memory of the answer every F12 round trip re-asks a
 * question the user already answered. Keyed by the sidecar's bytes, so the
 * editor waking up and publishing is what lifts it: the plan's "ignore it
 * until its change token moves again", made to outlive one binding the same
 * way a dismissed scene-file payload does. */
static void test_declining_the_offer_survives_a_scene_switch(void) {
    int slot_a;

    printf("--- a declined sidecar stays declined across a switch ---\n");

    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
    write_lines(WATCH_PATH, k_scene_a);
    publish_wip(k_scene_b, 4, 1);          /* present before the bind */
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    ASSERT_TRUE("the leftover sidecar is offered", glr_modal_active());
    glr_modal_cancel();                    /* decline */
    slot_a = repl_active_user_scene();
    ASSERT_TRUE("the CLI file is a user scene", slot_a >= 0);

    /* Away and back, with the sidecar untouched on disk throughout. */
    write_lines(OTHER_PATH, k_scene_longer);
    {
        ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
        ASSERT_TRUE("a second scene opens",
                    repl_load_scene_as_new_slot(OTHER_PATH, &reason) >= 0);
    }
    glr_extedit_poll();
    ASSERT_TRUE("back to the first scene", repl_load_user_scene_idx(slot_a));
    glr_extedit_poll();
    glr_extedit_poll();

    ASSERT_TRUE("the answer is remembered, not re-asked", !glr_modal_active());
    ASSERT_INT("and nothing was followed behind their back",
               glr_extedit_stats().wip_updates, 0);
    ASSERT_TRUE("the scene is still the file", document_mentions("0, 0, 0"));

    /* The editor waking up is what lifts it: new bytes are a new question. */
    set_mtime(WIP_PATH, 40000, 0);
    publish_wip(k_scene_longer, 4, 1);
    set_mtime(WIP_PATH, 40001, 0);
    glr_extedit_poll();
    ASSERT_INT("a publication after the decline is followed",
               glr_extedit_stats().wip_updates, 1);
    ASSERT_TRUE("with its geometry", document_mentions("3, 3, 3"));
    (void)unlink(WIP_PATH);
}

/* The decline is keyed on the *payload*, not the raw bytes, and that is what
 * makes it useful. The usual leftover comes from an editor that is still open,
 * and the plugin rewrites `// @cursor` on every cursor motion - so keying on
 * raw bytes would make any twitch of the caret look like a new question and
 * re-ask on the next scene switch. */
static void test_a_decline_survives_a_cursor_only_republish(void) {
    int slot_a;

    printf("--- a decline outlives the editor moving its caret ---\n");

    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
    write_lines(WATCH_PATH, k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    ASSERT_TRUE("the leftover is offered", glr_modal_active());
    glr_modal_cancel();                     /* decline */
    slot_a = repl_active_user_scene();

    /* Away, and while we are away the still-open editor moves its cursor:
     * same buffer, different `@cursor` line, different bytes on disk. */
    write_lines(OTHER_PATH, k_scene_longer);
    {
        ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
        ASSERT_TRUE("a second scene opens",
                    repl_load_scene_as_new_slot(OTHER_PATH, &reason) >= 0);
    }
    glr_extedit_poll();
    set_mtime(WIP_PATH, 50000, 0);
    publish_wip(k_scene_b, 2, 9);           /* payload identical, caret moved */
    set_mtime(WIP_PATH, 50001, 0);

    ASSERT_TRUE("back to the first scene", repl_load_user_scene_idx(slot_a));
    glr_extedit_poll();
    glr_extedit_poll();
    ASSERT_TRUE("a caret move is not a new question", !glr_modal_active());
    ASSERT_INT("and nothing was followed", glr_extedit_stats().wip_updates, 0);
    ASSERT_TRUE("the scene is still the file", document_mentions("0, 0, 0"));
    (void)unlink(WIP_PATH);
}

/* Being unable to ask is not an answer. If another modal owns the keyboard
 * when the offer comes up, the question has to wait for it - recording the
 * default decline anyway would answer on the user's behalf, durably, and they
 * would never see the prompt at all. */
static int test_other_modal_commit(GlrModalKind kind, const char *text,
                                   int context) {
    (void)kind; (void)text; (void)context;
    return 1;
}

static void test_a_busy_modal_defers_the_offer_rather_than_declining_it(void) {
    printf("--- a busy modal postpones the offer, it does not answer it ---\n");

    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
    write_lines(WATCH_PATH, k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);

    /* Something else is already on screen when the watcher arms. */
    ASSERT_INT("another modal opens",
               glr_modal_begin(GLR_MODAL_SCENE_SAVE_AS, "busy", 0,
                               test_other_modal_commit), 1);
    glr_extedit_set_enabled(1);
    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_INT("the other modal still owns the keyboard",
               (int)glr_modal_kind(), (int)GLR_MODAL_SCENE_SAVE_AS);
    ASSERT_INT("and nothing was followed behind it",
               glr_extedit_stats().wip_updates, 0);

    /* It closes. The question is still owed. */
    glr_modal_cancel();
    glr_extedit_poll();
    ASSERT_INT("now the offer is made", (int)glr_modal_kind(),
               (int)GLR_MODAL_CONFIRM_WIP_RECOVER);
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
}

/* Following a path's sidecar is evidence about *that* editor, and it dies with
 * it. A sidecar found at a later bind belongs to a different editor whose
 * liveness is unknown - which is the question the prompt exists to ask, so it
 * must be asked again rather than resumed into. */
static void test_a_leftover_after_the_editor_closed_is_offered_again(void) {
    int slot_a;

    printf("--- a new leftover after a close is offered, not resumed ---\n");

    begin_wip_session(k_scene_a);
    slot_a = repl_active_user_scene();
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    ASSERT_INT("the session followed", glr_extedit_stats().wip_updates, 1);

    /* The editor exits: VimLeave removes the sidecar, and we observe it. */
    (void)unlink(WIP_PATH);
    glr_extedit_poll();

    /* Away, and while we are away another editor opens the file and dies,
     * leaving a sidecar behind. */
    write_lines(OTHER_PATH, k_scene_longer);
    {
        ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
        ASSERT_TRUE("a second scene opens",
                    repl_load_scene_as_new_slot(OTHER_PATH, &reason) >= 0);
    }
    glr_extedit_poll();
    publish_wip(k_scene_longer, 4, 1);

    ASSERT_TRUE("back to the first scene", repl_load_user_scene_idx(slot_a));
    glr_extedit_poll();
    glr_extedit_poll();
    ASSERT_INT("the stranger's leftover is offered, not applied",
               (int)glr_modal_kind(), (int)GLR_MODAL_CONFIRM_WIP_RECOVER);
    ASSERT_TRUE("and nothing of it is live", !document_mentions("3, 3, 3"));
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
}

/* The followed mark has to be dropped on *every* observed disappearance, not
 * only on one that interrupts a live session. A local commit or a Ctrl+Z ends
 * the session while deliberately leaving the mark - the editor is still open -
 * so when that editor later quits, the idle path is the only place that
 * learns it. Miss it and the next editor's leftovers are resumed into with no
 * prompt. */
static void test_a_close_after_the_session_ended_still_drops_the_mark(void) {
    int slot_a;

    printf("--- an idle close still forgets the editor ---\n");

    begin_wip_session(k_scene_a);
    slot_a = repl_active_user_scene();
    publish_wip(k_scene_b, 4, 1);
    glr_extedit_poll();
    ASSERT_INT("the session followed", glr_extedit_stats().wip_updates, 1);

    /* Ctrl+Z ends the session; the editor is still open, so the mark stays. */
    editor_undo_pop_snapshot();
    set_mtime(WIP_PATH, 70000, 0);
    publish_wip(k_scene_b, 4, 2);
    set_mtime(WIP_PATH, 70001, 0);
    glr_extedit_poll();
    ASSERT_TRUE("the undo stuck", document_mentions("0, 0, 0"));

    /* Now the editor quits, with no session running to interrupt. */
    (void)unlink(WIP_PATH);
    glr_extedit_poll();

    /* Away; a different editor opens the file, dies, and leaves a sidecar. */
    write_lines(OTHER_PATH, k_scene_longer);
    {
        ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
        ASSERT_TRUE("a second scene opens",
                    repl_load_scene_as_new_slot(OTHER_PATH, &reason) >= 0);
    }
    glr_extedit_poll();
    publish_wip(k_scene_longer, 4, 1);

    ASSERT_TRUE("back to the first scene", repl_load_user_scene_idx(slot_a));
    glr_extedit_poll();
    glr_extedit_poll();
    ASSERT_INT("the stranger's leftover is offered, not applied",
               (int)glr_modal_kind(), (int)GLR_MODAL_CONFIRM_WIP_RECOVER);
    ASSERT_TRUE("and nothing of it is live", !document_mentions("3, 3, 3"));
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
}

/* The torn-write backstop is per binding, which is right - it describes the
 * publisher currently writing this file - but `wip_forget()` clears it on
 * every rebind, and the reads a bind performs used to throw the observation
 * away. Accepting a recovery offer is the reachable form: the offer read the
 * sidecar (trailer and all) to record its payload, then the accept forces a
 * re-read, and if the publisher tore the file in between there was nothing
 * left saying it had ever written a trailer.
 */
static void test_the_torn_write_backstop_survives_a_rebind(void) {
    GlrExtEditStats before;

    printf("--- the torn-write backstop survives an accepted recovery ---\n");

    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
    write_lines(WATCH_PATH, k_scene_a);
    publish_wip(k_scene_longer, 4, 1);      /* a leftover, with its trailer */
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    ASSERT_INT("the leftover is offered", (int)glr_modal_kind(),
               (int)GLR_MODAL_CONFIRM_WIP_RECOVER);
    before = glr_extedit_stats();

    glr_modal_handle_key('y');              /* accept: forces a re-read */

    /* The publisher tears the file between the accept and the next frame. */
    {
        FILE *f = fopen(WIP_PATH, "w");
        ASSERT_TRUE("the torn file is written", f != NULL);
        if (f) {
            fputs("glClearColor(0.3, 0.3, 0.3, 1.0);\n", f);
            fputs("glBegin(GL_POIN", f);
            fclose(f);
        }
        set_mtime(WIP_PATH, 71000, 0);
    }
    glr_extedit_poll();

    ASSERT_INT("the torn read is refused, not imported",
               glr_extedit_stats().wip_updates, before.wip_updates);
    ASSERT_INT("and counted", glr_extedit_stats().failures - before.failures, 1);
    ASSERT_TRUE("the scene is untouched", document_mentions("0, 0, 0"));
    (void)unlink(WIP_PATH);
}

/* A publication that arrives while the recovery prompt is up is ordinary live
 * follow - the editor is demonstrably alive, which is the resume condition.
 * The question must not survive being answered by events: it would go on
 * asking whether to follow content that is already on screen. */
static void test_a_live_publication_retracts_the_recovery_prompt(void) {
    printf("--- a new payload retracts the recovery question ---\n");

    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
    write_lines(WATCH_PATH, k_scene_a);
    publish_wip(k_scene_b, 4, 1);          /* leftover, present before the bind */
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    ASSERT_INT("the leftover is offered", (int)glr_modal_kind(),
               (int)GLR_MODAL_CONFIRM_WIP_RECOVER);

    /* Without anyone answering, the editor wakes up and publishes. */
    set_mtime(WIP_PATH, 72000, 0);
    publish_wip(k_scene_longer, 4, 1);
    set_mtime(WIP_PATH, 72001, 0);
    glr_extedit_poll();

    ASSERT_INT("the live payload is followed",
               glr_extedit_stats().wip_updates, 1);
    ASSERT_TRUE("its geometry is live", document_mentions("3, 3, 3"));
    ASSERT_TRUE("and the stale question is retracted", !glr_modal_active());
    (void)unlink(WIP_PATH);
}

/* A recovery question names one concrete leftover. If that sidecar vanishes,
 * the question vanishes with it; accepting afterwards would ask the watcher
 * to follow a file that no longer exists. A later sidecar is a post-bind
 * publication and therefore ordinary live follow, not another recovery. */
static void test_sidecar_deletion_retracts_the_recovery_prompt(void) {
    printf("--- deleting a recovered sidecar retracts its question ---\n");

    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
    write_lines(WATCH_PATH, k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    ASSERT_INT("the leftover is offered", (int)glr_modal_kind(),
               (int)GLR_MODAL_CONFIRM_WIP_RECOVER);

    (void)unlink(WIP_PATH);
    glr_extedit_poll();
    ASSERT_TRUE("the missing sidecar retracts its question",
                !glr_modal_active());
    ASSERT_TRUE("the original scene remains live",
                document_mentions("0, 0, 0"));

    publish_wip(k_scene_longer, 4, 1);
    glr_extedit_poll();
    ASSERT_INT("a later publication follows without another prompt",
               glr_extedit_stats().wip_updates, 1);
    ASSERT_TRUE("the later payload is live", document_mentions("3, 3, 3"));
    ASSERT_TRUE("and no recovery question returned", !glr_modal_active());
    (void)unlink(WIP_PATH);
}

/* The same disappearance can happen while another modal prevents the recovery
 * question from opening. The deferred offer belongs to the vanished file and
 * must be cleared, while the unrelated modal remains untouched. */
static void test_sidecar_deletion_clears_an_offer_waiting_behind_a_modal(void) {
    printf("--- deleting a sidecar clears its deferred recovery offer ---\n");

    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
    write_lines(WATCH_PATH, k_scene_a);
    publish_wip(k_scene_b, 4, 1);
    glr_ctrl_reset_all();
    (void)repl_load_initial_commands(WATCH_PATH);
    ASSERT_INT("another modal opens",
               glr_modal_begin(GLR_MODAL_SCENE_SAVE_AS, "busy", 0,
                               test_other_modal_commit), 1);
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    ASSERT_INT("the other modal still owns the keyboard",
               (int)glr_modal_kind(), (int)GLR_MODAL_SCENE_SAVE_AS);

    (void)unlink(WIP_PATH);
    glr_extedit_poll();
    ASSERT_INT("deletion leaves the unrelated modal alone",
               (int)glr_modal_kind(), (int)GLR_MODAL_SCENE_SAVE_AS);
    glr_modal_cancel();
    glr_extedit_poll();
    ASSERT_TRUE("the vanished recovery offer does not appear",
                !glr_modal_active());

    publish_wip(k_scene_longer, 4, 1);
    glr_extedit_poll();
    ASSERT_INT("a later publication is ordinary live follow",
               glr_extedit_stats().wip_updates, 1);
    ASSERT_TRUE("the later payload is live", document_mentions("3, 3, 3"));
    ASSERT_TRUE("without a recovery prompt", !glr_modal_active());
    (void)unlink(WIP_PATH);
}

static void test_an_unparseable_buffer_leaves_the_scene_alone(void) {
    GlrExtEditStats before, after;

    printf("--- a buffer mid-thought does not damage the scene ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(k_scene_broken, 1, 1);
    glr_extedit_poll();
    before = glr_extedit_stats();

    ASSERT_INT("the import failed", before.failures, 1);
    ASSERT_TRUE("the live scene is untouched", document_mentions("0, 0, 0"));

    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    after = glr_extedit_stats();
    ASSERT_INT("and it is not retried every frame",
               after.failures - before.failures, 0);
    (void)unlink(WIP_PATH);
}

/* ----- outbound sync ------------------------------------------------------ */

/* The round trip is only closed if it runs both ways. A local edit that never
 * reaches the file is not merely stale: the editor's next save overwrites it,
 * and nothing ever said so. */
static void test_a_local_edit_is_written_back(void) {
    GlrExtEditStats stats;

    printf("--- a local edit is synced back to the watched file ---\n");

    begin_watched_session(k_scene_a);
    ASSERT_INT("binding writes nothing", glr_extedit_stats().writes, 0);
    for (int i = 0; i < 3; i++)
        glr_extedit_poll();
    ASSERT_INT("nor do idle frames", glr_extedit_stats().writes, 0);

    ASSERT_TRUE("the line commits", editor_feed_line("glVertex3f(4, 4, 4);") != 0);
    editor_input_clear();
    ASSERT_TRUE("and the file does not have it yet",
                !file_contains(WATCH_PATH, "4, 4, 4"));

    glr_extedit_poll();
    stats = glr_extedit_stats();
    ASSERT_INT("the poll wrote the file once", stats.writes, 1);
    ASSERT_INT("with no write failure", stats.write_failures, 0);
    ASSERT_TRUE("the watched file carries the local edit",
                file_contains(WATCH_PATH, "4, 4, 4"));

    /* Our own write is stamped, so it must not read back as somebody else's
     * save - and an unchanged document must not be rewritten every frame. */
    for (int i = 0; i < 10; i++)
        glr_extedit_poll();
    stats = glr_extedit_stats();
    ASSERT_INT("the write is not re-read as an external change",
               stats.reloads, 0);
    ASSERT_INT("and it happened exactly once", stats.writes, 1);
    ASSERT_INT("no failures anywhere", stats.failures, 0);
}

/* The motivating case, and the reason the trigger is the document text: a
 * variable-panel drag applies its value live on every pointer event but
 * rewrites the declaration row exactly once, on release. This is that release
 * (glr_ctrl_persist_variable_panel_drag_value's two calls), so one gesture is
 * one write. */
static void test_a_dragged_value_reaches_the_file(void) {
    static const char *const scene[] = {
        "float twist;",
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(twist, 0, 0);",
        "glEnd();",
        NULL
    };
    ReplCompiledChange compiled;
    ReplCompileContext ctx;
    char err[REPL_STATUS_TEXT_MAX] = "";

    printf("--- a dragged variable value reaches the watched file ---\n");

    begin_watched_session(scene);
    ASSERT_TRUE("the variable is declared", document_mentions("float twist"));

    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("the settled value compiles into the declaration row",
               (int)repl_compile_persist_predef_value("twist", 0.375f, &ctx,
                                                      &compiled, err,
                                                      sizeof(err)),
               (int)REPL_COMPILE_OK);
    ASSERT_TRUE("the drag rewrote a row",
                compiled.kind != REPL_COMPILED_NO_CHANGE);
    ASSERT_TRUE("the rewrite applies",
                editor_commit_apply_external_change(&compiled, 0, 0) != 0);

    glr_extedit_poll();
    ASSERT_INT("one gesture wrote the file once",
               glr_extedit_stats().writes, 1);
    ASSERT_TRUE("the watched file carries the dragged value",
                file_contains(WATCH_PATH, "0.375"));
}

static void test_drag_when_cursor_on_declaration_row(void) {
    static const char *const scene[] = {
        "float twist;",
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(twist, 0, 0);",
        "glEnd();",
        NULL
    };
    int row = -1;
    ReplPredefView predefs;

    printf("--- a dragged variable when edit line is on the declaration ---\n");

    begin_watched_session(scene);
    editor_state_edit_line_set(0);
    editor_load_line_to_input(0);

    predefs = repl_eval_predef_view();
    for (int i = 0; i < predefs.count; i++) {
        if (strcmp(predefs.vars[i].name, "twist") == 0) {
            row = i;
            break;
        }
    }
    ASSERT_TRUE("twist variable exists in predef view", row >= 0);
    variable_panel_handle_drag_begin(row, /*coarse=*/0, /*x=*/100);
    ASSERT_TRUE("drag motion produces change",
                glr_ctrl_router_handle_variable_panel_motion(150, 0));
    ASSERT_TRUE("drag release persisted the change",
                glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP));

    glr_extedit_poll();
    ASSERT_INT("one gesture wrote the file once",
               glr_extedit_stats().writes, 1);
    ASSERT_TRUE("the watched file carries the dragged value",
                file_contains(WATCH_PATH, "twist ="));
}


/* While a sidecar session is live the editor's unsaved buffer is the truth and
 * the file is deliberately behind it - writing there would save someone's
 * unsaved typing for them, and would drop the parked row, which is not in the
 * document. The local edit still ends the session, and the sync follows it. */
static void test_no_write_while_following_the_editor(void) {
    printf("--- a live sidecar session holds the sync back ---\n");

    begin_wip_session(k_scene_a);
    publish_wip(k_scene_longer, 4, 1);
    glr_extedit_poll();
    ASSERT_INT("the session is live", glr_extedit_stats().wip_updates, 1);
    ASSERT_INT("and following wrote nothing", glr_extedit_stats().writes, 0);

    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_TRUE("a local line commits",
                editor_feed_line("glVertex3f(4, 4, 4);") != 0);
    editor_input_clear();
    glr_extedit_poll();
    ASSERT_INT("the file is left to the editor while it is following",
               glr_extedit_stats().writes, 0);
    ASSERT_TRUE("and still holds the saved scene",
                !file_contains(WATCH_PATH, "4, 4, 4"));

    /* Re-publishing the same buffer is what the editor does on any keystroke
     * or cursor move. It ends the session (the document moved and this module
     * did not move it), and the sync fires in the same poll. */
    set_mtime(WIP_PATH, 18000, 0);
    publish_wip(k_scene_longer, 5, 1);
    set_mtime(WIP_PATH, 19000, 0);
    glr_extedit_poll();
    ASSERT_INT("the paused session releases the sync",
               glr_extedit_stats().writes, 1);
    ASSERT_TRUE("the local edit is in the file",
                file_contains(WATCH_PATH, "4, 4, 4"));
    (void)unlink(WIP_PATH);
}

/* Inbound wins a straight race: a version waiting behind the gate has not
 * landed yet, and writing our own bytes over it would stamp the external save
 * out of existence. The sync waits for the deferral to resolve either way. */
static void test_a_pending_version_holds_the_sync_back(void) {
    printf("--- a pending inbound version outranks the sync ---\n");

    begin_watched_session(k_scene_a);
    begin_typing("glVertex3f(4, 4, 4);");

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("the reload is held", glr_extedit_stats().reloads, 0);
    ASSERT_INT("and nothing is written over it",
               glr_extedit_stats().writes, 0);
    ASSERT_TRUE("the pending version is still on disk",
                file_contains(WATCH_PATH, "9, 9, 9"));

    /* Committing is what resolves it: the pending version is dismissed in
     * favour of the local document, and *that* is what the file should hold. */
    ASSERT_TRUE("the line commits", editor_feed_line("glVertex3f(4, 4, 4);") != 0);
    editor_input_clear();
    glr_extedit_poll();
    ASSERT_INT("the dismissal releases the sync",
               glr_extedit_stats().writes, 1);
    ASSERT_TRUE("the local document is now the file",
                file_contains(WATCH_PATH, "4, 4, 4"));
    ASSERT_TRUE("and the version it beat is gone",
                !file_contains(WATCH_PATH, "9, 9, 9"));
}

/* Emptying the document while watching is a plausible accident, and an empty
 * file is not a scene. The file keeps the last real version until something
 * real replaces it. (Ctrl+N is not this case: clearing restores the default
 * display baseline, which is a real scene and does sync.) */
static void test_an_emptied_document_is_not_written(void) {
    printf("--- an empty document never overwrites the file ---\n");

    begin_watched_session(k_scene_a);
    editor_state_edit_line_set(0);
    editor_delete_cmd_range(0, repl_state_document_count(), "Deleted");
    ASSERT_INT("the document is empty", repl_state_document_count(), 0);

    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_INT("nothing was written", glr_extedit_stats().writes, 0);
    ASSERT_TRUE("the file still holds the scene",
                file_contains(WATCH_PATH, "0, 0, 0"));
}

/* A hold-back that already suppressed the write, then F12 away and back: the
 * last-agreed fingerprint has to survive the rebind or the file stays stale
 * forever. A read-only file is the hold-back here because it is the one that
 * outlives a scene switch without also holding the write on return - the point
 * is the fingerprint, not the hold-back. */
static void test_returning_after_a_held_write_still_syncs(void) {
    int slot_a;

    printf("--- a write held across a scene switch still fires on return ---\n");

    begin_watched_session(k_scene_a);
    slot_a = repl_active_user_scene();

    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_TRUE("a local line commits",
                editor_feed_line("glVertex3f(7, 7, 7);") != 0);
    editor_input_clear();

    ASSERT_INT("the file can be made unwritable", chmod(WATCH_PATH, 0444), 0);
    glr_extedit_poll();
    ASSERT_INT("the write was held", glr_extedit_stats().writes, 0);
    ASSERT_TRUE("the file is still the original",
                !file_contains(WATCH_PATH, "7, 7, 7"));
    ASSERT_INT("the file is writable again", chmod(WATCH_PATH, 0644), 0);

    write_lines(OTHER_PATH, k_scene_longer);
    {
        ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
        (void)repl_load_scene_as_new_slot(OTHER_PATH, &reason);
    }
    /* Production F12 refreshes the input from the loaded row. The lower-level
     * loaders do not, and a leftover half-typed row would keep the gate shut. */
    editor_load_line_to_input(editor_state_edit_line());
    glr_extedit_poll();
    ASSERT_TRUE("back to the first scene", repl_load_user_scene_idx(slot_a));
    editor_load_line_to_input(editor_state_edit_line());
    glr_extedit_poll();
    ASSERT_INT("the held edit is written on return",
               glr_extedit_stats().writes, 1);
    ASSERT_TRUE("the file now has the local line",
                file_contains(WATCH_PATH, "7, 7, 7"));
    (void)unlink(OTHER_PATH);
}

/* The writer derives the file's leading `@cfg` block from live config, so a
 * scene setting changes the bytes on disk without moving a single document
 * row. Hashing rows alone left the file stale until some unrelated edit
 * happened to carry the setting out with it. */
static void test_a_scene_config_change_syncs(void) {
    int settled;
    int before, after;

    printf("--- a scene @cfg change reaches the file ---\n");

    begin_watched_session(k_scene_a);
    /* Start from the documented default rather than whatever the previous
     * test left behind, so "cycling moves it off default" is a fact and not
     * an assumption about the shipped value. */
    glr_config_set(GLR_CONFIG_WIREFRAME, CFG_DEFAULT_WIREFRAME);
    glr_extedit_poll();
    settled = glr_extedit_stats().writes;
    glr_extedit_poll();
    ASSERT_INT("the session is settled", glr_extedit_stats().writes, settled);

    before = glr_config_get(GLR_CONFIG_WIREFRAME);
    (void)glr_config_cycle(GLR_CONFIG_WIREFRAME, 1);
    after = glr_config_get(GLR_CONFIG_WIREFRAME);
    ASSERT_TRUE("the scene setting moved", after != before);

    glr_extedit_poll();
    ASSERT_INT("and is published on its own", glr_extedit_stats().writes,
               settled + 1);
    ASSERT_TRUE("the file carries the @cfg row",
                file_contains(WATCH_PATH, "@cfg wireframe"));
    ASSERT_TRUE("and still the scene", file_contains(WATCH_PATH, "0, 0, 0"));

    glr_extedit_poll();
    ASSERT_INT("one change is one write", glr_extedit_stats().writes,
               settled + 1);

    /* Session-inspection settings are not the scene - same line the camera
     * sits on. Opening a profiler must not rewrite the user's file. */
    (void)glr_config_cycle(GLR_CONFIG_CPU_PROFILE, 1);
    glr_extedit_poll();
    ASSERT_INT("a session setting is not a reason to write",
               glr_extedit_stats().writes, settled + 1);

    glr_config_set(GLR_CONFIG_WIREFRAME, CFG_DEFAULT_WIREFRAME);
}

/* The outbound half of the gate split. A half-typed row is the *inbound*
 * question - a reload would destroy typing undo cannot recover - and it used
 * to hold the outbound write too, which parked every later local change behind
 * a line the user may never finish. A row being typed is not in the document,
 * so publishing the committed document without it takes nothing away from a
 * file that never had it. */
static void test_a_half_typed_row_does_not_hold_the_write(void) {
    printf("--- a half-typed row elsewhere does not hold the write ---\n");

    begin_watched_session(k_scene_a);
    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_TRUE("a local line commits",
                editor_feed_line("glVertex3f(7, 7, 7);") != 0);
    editor_input_clear();
    begin_typing("glVertex3f(8,");

    glr_extedit_poll();
    ASSERT_INT("the committed row is published anyway",
               glr_extedit_stats().writes, 1);
    ASSERT_TRUE("the file carries it", file_contains(WATCH_PATH, "7, 7, 7"));
    ASSERT_TRUE("and not the row still being typed",
                !file_contains(WATCH_PATH, "glVertex3f(8,"));
    ASSERT_STR("which is still in the input, untouched",
               editor_input_text(), "glVertex3f(8,");

    /* Inbound is emphatically NOT relaxed: the same half-typed row still
     * defers a reload, because that reload would overwrite it. */
    write_lines(WATCH_PATH, k_scene_b);
    set_mtime(WATCH_PATH, 1000000, 0);
    glr_extedit_poll();
    ASSERT_INT("an external save still defers",
               glr_extedit_stats().deferrals, 1);
    ASSERT_INT("and nothing reloaded", glr_extedit_stats().reloads, 0);
    ASSERT_TRUE("the local document stands", document_mentions("7, 7, 7"));
}

/* The row the split keeps holding: a parked tail the user is still evolving.
 * That row IS in the file, so a write dropping it loses their typing - and
 * one more keystroke must not make it eligible. */
static void test_editing_the_parked_row_still_holds_the_write(void) {
    static const char *const twist_scene[] = {
        "float twist;",
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(twist, 0, 0);",
        "glEnd();",
        NULL
    };
    static const char *const with_tail[] = {
        "float twist;",
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(twist, 0, 0);",
        "glEnd();",
        "glVertex3f(1,",
        NULL
    };
    ReplCompiledChange compiled;
    ReplCompileContext ctx;
    char err[REPL_STATUS_TEXT_MAX] = "";

    printf("--- typing into the parked row keeps holding the write ---\n");

    begin_watched_session(twist_scene);
    write_lines(WATCH_PATH, with_tail);
    glr_extedit_poll();
    ASSERT_INT("the tail was parked", glr_extedit_stats().parked_rows, 1);

    /* One more character: no longer byte-identical to the park, but the same
     * row, still being typed, and still the row the file holds. */
    editor_input_set_text("glVertex3f(1, 2");

    ctx = repl_compile_context_from_live(0);
    ASSERT_INT("a dragged value compiles into the declaration",
               (int)repl_compile_persist_predef_value("twist", 0.625f, &ctx,
                                                      &compiled, err,
                                                      sizeof(err)),
               (int)REPL_COMPILE_OK);
    ASSERT_TRUE("the rewrite applies",
                editor_commit_apply_external_change(&compiled, 0, 0) != 0);

    glr_extedit_poll();
    ASSERT_INT("the edited parked row still held the write",
               glr_extedit_stats().writes, 0);
    ASSERT_TRUE("the file still has the tail",
                file_contains(WATCH_PATH, "glVertex3f(1,"));

    editor_input_clear();
    glr_extedit_poll();
    ASSERT_INT("abandoning it releases the sync",
               glr_extedit_stats().writes, 1);
    ASSERT_TRUE("the dragged value reached the file",
                file_contains(WATCH_PATH, "0.625"));
}

/* A failed write must not be treated as agreement. Retry when the document
 * moves, not every frame, and not when the path merely becomes writable. */
static void test_a_failed_write_is_not_treated_as_synced(void) {
    printf("--- a failed write is not treated as synced ---\n");

    begin_watched_session(k_scene_a);
    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_TRUE("a local line commits",
                editor_feed_line("glVertex3f(4, 4, 4);") != 0);
    editor_input_clear();

    ASSERT_INT("the file can be made unwritable", chmod(WATCH_PATH, 0444), 0);
    glr_extedit_poll();
    ASSERT_INT("the write failed once", glr_extedit_stats().write_failures, 1);
    ASSERT_INT("and nothing was recorded as written",
               glr_extedit_stats().writes, 0);
    ASSERT_TRUE("the file still holds the original",
                !file_contains(WATCH_PATH, "4, 4, 4"));

    for (int i = 0; i < 5; i++)
        glr_extedit_poll();
    ASSERT_INT("the same document is not retried every frame",
               glr_extedit_stats().write_failures, 1);

    ASSERT_INT("the file is writable again", chmod(WATCH_PATH, 0644), 0);
    glr_extedit_poll();
    ASSERT_INT("becoming writable does not retry by itself",
               glr_extedit_stats().writes, 0);

    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_TRUE("a further line commits",
                editor_feed_line("glVertex3f(5, 5, 5);") != 0);
    editor_input_clear();
    glr_extedit_poll();
    ASSERT_INT("the next document change retries",
               glr_extedit_stats().writes, 1);
    ASSERT_TRUE("and both local lines reach the file",
                file_contains(WATCH_PATH, "4, 4, 4") &&
                file_contains(WATCH_PATH, "5, 5, 5"));
}

/* A stage-2 parked row is not in the document. An implicit write would drop
 * it from the file while it is still the live input. Ctrl+S still may. */
static void test_a_parked_row_holds_the_sync_back(void) {
    static const char *const twist_scene[] = {
        "float twist;",
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(twist, 0, 0);",
        "glEnd();",
        NULL
    };
    static const char *const with_tail[] = {
        "float twist;",
        "glClearColor(0.1, 0.1, 0.1, 1.0);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glBegin(GL_POINTS);",
        "glVertex3f(twist, 0, 0);",
        "glEnd();",
        "glVertex3f(1,",
        NULL
    };
    ReplCompiledChange compiled;
    ReplCompileContext ctx;
    char err[REPL_STATUS_TEXT_MAX] = "";

    printf("--- a live parked row holds the sync back ---\n");

    begin_watched_session(twist_scene);
    write_lines(WATCH_PATH, with_tail);
    glr_extedit_poll();
    ASSERT_INT("the tail was parked", glr_extedit_stats().parked_rows, 1);
    ASSERT_STR("and is still the live input",
               editor_input_text(), "glVertex3f(1,");

    ctx = repl_compile_context_from_live(0);
    ASSERT_INT("a dragged value compiles into the declaration",
               (int)repl_compile_persist_predef_value("twist", 0.375f, &ctx,
                                                      &compiled, err,
                                                      sizeof(err)),
               (int)REPL_COMPILE_OK);
    ASSERT_TRUE("the rewrite applies",
                editor_commit_apply_external_change(&compiled, 0, 0) != 0);
    ASSERT_STR("without taking the parked row",
               editor_input_text(), "glVertex3f(1,");

    glr_extedit_poll();
    ASSERT_INT("the parked row held the write", glr_extedit_stats().writes, 0);
    ASSERT_TRUE("the file still has the incomplete tail",
                file_contains(WATCH_PATH, "glVertex3f(1,"));

    editor_input_clear();
    glr_extedit_poll();
    ASSERT_INT("clearing the input releases the sync",
               glr_extedit_stats().writes, 1);
    ASSERT_TRUE("the dragged value reached the file",
                file_contains(WATCH_PATH, "0.375"));
    ASSERT_TRUE("and the abandoned tail did not",
                !file_contains(WATCH_PATH, "glVertex3f(1,"));
}

/* After a lesson ends the leftover document is still pinned to the user's
 * watched file. Writing it would stamp the lesson over a scene the user
 * never asked to replace. Promotion then unbinds rather than overwriting. */
static void test_post_tutorial_pin_holds_the_sync_back(void) {
    const char *bound;

    printf("--- a leftover lesson is not written over the watched file ---\n");

    begin_watched_session(k_scene_a);
    tutorial_start(0);
    glr_extedit_poll();
    tutorial_stop();
    ASSERT_TRUE("the leftover is still pinned to the user's file",
                repl_state_tutorial_origin_idx() >= 0);

    glr_extedit_poll();
    ASSERT_INT("nothing was written over the pre-lesson scene",
               glr_extedit_stats().writes, 0);
    ASSERT_TRUE("the watched file still holds the user's scene",
                file_contains(WATCH_PATH, "0, 0, 0"));

    ASSERT_TRUE("the leftover promotes",
                repl_promote_transient_if_needed() >= 0);
    ASSERT_INT("and the pin lifts", repl_state_tutorial_origin_idx(), -1);

    glr_extedit_poll();
    ASSERT_INT("promotion does not write the leftover over the file",
               glr_extedit_stats().writes, 0);
    ASSERT_TRUE("the pre-lesson scene is still on disk",
                file_contains(WATCH_PATH, "0, 0, 0"));
    bound = glr_extedit_bound_path();
    ASSERT_TRUE("the leftover is no longer bound to the user's file",
                bound == NULL ||
                strstr(bound, "gl_repl_extedit_scene.glr") == NULL);
}

int main(void) {
    printf("=== external-editor watch ===\n");
    test_bind_adopts_without_reloading();
    test_a_local_edit_is_written_back();
    test_a_dragged_value_reaches_the_file();
    test_drag_when_cursor_on_declaration_row();
    test_no_write_while_following_the_editor();
    test_a_pending_version_holds_the_sync_back();
    test_an_emptied_document_is_not_written();
    test_returning_after_a_held_write_still_syncs();
    test_a_scene_config_change_syncs();
    test_a_half_typed_row_does_not_hold_the_write();
    test_editing_the_parked_row_still_holds_the_write();
    test_a_failed_write_is_not_treated_as_synced();
    test_a_parked_row_holds_the_sync_back();
    test_post_tutorial_pin_holds_the_sync_back();
    test_same_second_save_is_noticed();
    test_safe_write_rename_is_noticed();
    test_size_only_change_is_noticed();
    test_touch_without_content_change_does_not_reload();
    test_malformed_file_is_not_retried_every_frame();
    test_missing_file_reports_once_and_resumes();
    test_our_own_save_does_not_come_back();
    test_dirty_input_defers_and_cancel_applies();
    test_commit_dismisses_and_the_bytes_do_not_return();
    test_save_supersedes_a_pending_version();
    test_loaded_row_is_not_dirty();
    test_empty_existing_row_is_dirty();
    test_user_scene_reload_is_undoable();
    test_builtin_example_unbinds_the_watcher();
    test_failed_reload_restores_a_full_undo_ring();
    test_watched_reload_preserves_cfg_and_camera();
    test_tutorial_defers_then_dismisses();
    test_reload_stops_replay();
    test_reload_cancels_camera_drag_and_momentum();
    test_reload_clears_the_input_row();
    test_incomplete_final_row_is_parked();
    test_missing_semicolon_counts_as_incomplete();
    test_trailing_comment_is_not_parked();
    test_wrong_but_terminated_row_fails_the_file();
    test_block_rows_are_never_parked();
    test_multi_row_incomplete_statement_is_refused();
    test_parked_row_defers_only_once_the_user_touches_it();
    test_pending_overtaken_by_a_revert();
    test_comment_after_the_parked_row_survives();
    test_overlong_line_is_refused_not_split();
    test_block_comment_matches_the_plain_loader();
    test_watch_binds_even_when_armed_during_a_lesson();
    test_file_that_is_only_an_incomplete_row();
    test_watch_survives_a_failed_startup_import();
    test_watching_a_file_that_does_not_exist_yet();
    test_seen_history_outlasts_the_scene_catalog();
    test_seen_history_covers_runtime_catalog();
    test_save_between_lesson_end_and_poll_is_not_swallowed();
    test_dismissal_survives_a_scene_switch();
    test_returning_to_a_scene_picks_up_what_changed();
    test_returning_to_an_untouched_scene_keeps_local_edits();
    test_catalog_scene_reload_does_not_promote();
    test_disabled_watcher_does_nothing();
    test_sidecar_content_update_follows_without_saving();
    test_cursor_only_update_does_not_reimport();
    test_cursor_follows_through_the_row_map();
    test_a_mid_file_half_typed_row_is_parked();
    test_indented_parked_row_is_canonical_for_guides();
    test_leaving_and_returning_to_the_parked_row();
    test_undo_exits_the_session_and_sticks();
    test_a_session_costs_one_undo_entry();
    test_sidecar_does_not_promote_a_catalog_scene();
    test_sidecar_deletion_splits_on_identity();
    test_a_sidecar_found_at_bind_time_is_offered();
    test_accepting_the_offer_starts_following();
    test_saving_during_a_session_is_not_a_second_reload();
    test_sidecar_is_geometry_not_presentation();
    test_local_commit_suppresses_the_dismissed_wip_payload();
    test_undo_same_payload_cursor_does_not_reimport();
    test_failed_wip_import_restores_the_undo_ring();
    test_cursor_follows_onto_a_continuation_row();
    test_wip_reload_preserves_visible_t();
    test_sidecar_cursor_requests_follow_scroll();
    test_the_cursor_directive_never_reaches_a_saved_file();
    test_repeated_publications_all_land();
    test_a_torn_publication_cannot_damage_the_scene();
    test_a_second_session_takes_its_own_entry_snapshot();
    test_a_lesson_holds_the_sidecar_and_drops_what_it_missed();
    test_switching_scenes_ends_the_session();
    test_returning_to_a_live_session_is_not_a_recovery();
    test_declining_the_offer_survives_a_scene_switch();
    test_a_decline_survives_a_cursor_only_republish();
    test_a_busy_modal_defers_the_offer_rather_than_declining_it();
    test_a_leftover_after_the_editor_closed_is_offered_again();
    test_a_close_after_the_session_ended_still_drops_the_mark();
    test_the_torn_write_backstop_survives_a_rebind();
    test_a_live_publication_retracts_the_recovery_prompt();
    test_sidecar_deletion_retracts_the_recovery_prompt();
    test_sidecar_deletion_clears_an_offer_waiting_behind_a_modal();
    test_an_unparseable_buffer_leaves_the_scene_alone();
    test_exported_c_sidecar_cursor_follows_snippet_row();
    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
    (void)unlink(WATCH_PATH);
    (void)unlink(SWAP_PATH);
    (void)unlink(OTHER_PATH);
    (void)unlink(WIP_C_PATH);
    (void)unlink(WATCH_C_PATH);
    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "glr_extedit");
}

#endif /* __EMSCRIPTEN__ */
