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
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "app/glr_camera.h"
#include "app/glr_config.h"
#include "app/glr_ctrl.h"
#include "app/glr_extedit.h"
#include "editor/input.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "repl/bootstrap.h"
#include "repl/example_loader.h"
#include "repl/scenes.h"
#include "repl/state_owners.h"
#include "subsystems/replay/replay.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond)      TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, want)  TEST_ASSERT_INT(&g_harness, label, got, want)

#define WATCH_PATH "/tmp/gl_repl_extedit_scene.glr"
#define SWAP_PATH  "/tmp/gl_repl_extedit_scene.glr.swap"

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

/* editor_undo_push_snapshot() is the transient auto-promotion hook, so pushing
 * unconditionally would turn an unedited catalog scene into a user slot on the
 * first vim save - the exact thing D4's identity split exists to prevent. */
static void test_example_reload_does_not_promote(void) {
    int slots_before;

    printf("--- an unedited example is not promoted by a reload ---\n");

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

    write_lines(WATCH_PATH, k_scene_b);
    glr_extedit_poll();
    ASSERT_INT("B is parked, not applied", glr_extedit_stats().reloads, 0);

    /* vim undoes: the file is byte-identical to what we already have. */
    write_lines(WATCH_PATH, k_scene_a);
    glr_extedit_poll();

    editor_input_clear();                      /* gate opens */
    glr_extedit_poll();
    ASSERT_TRUE("the live document is still A", document_mentions("0, 0, 0"));

    /* The moment of truth: a real save of B must be followed, not mistaken for
     * gl-repl's own write because a stale hash was stamped as applied. */
    write_lines(WATCH_PATH, k_scene_b);
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

int main(void) {
    printf("=== external-editor watch ===\n");
    test_bind_adopts_without_reloading();
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
    test_user_scene_reload_is_undoable();
    test_example_reload_does_not_promote();
    test_failed_reload_restores_a_full_undo_ring();
    test_watched_reload_preserves_cfg_and_camera();
    test_tutorial_defers_then_dismisses();
    test_reload_stops_replay();
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
    test_disabled_watcher_does_nothing();
    glr_extedit_set_enabled(0);
    (void)unlink(WATCH_PATH);
    (void)unlink(SWAP_PATH);
    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "glr_extedit");
}

#endif /* __EMSCRIPTEN__ */
