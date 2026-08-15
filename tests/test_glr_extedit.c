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

#include "gl_includes.h"
#include "app/glr_camera.h"
#include "app/glr_config.h"
#include "app/glr_ctrl.h"
#include "app/glr_extedit.h"
#include "app/glr_modal.h"
#include "editor/input.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "repl/bootstrap.h"
#include "repl/example_loader.h"
#include "repl/examples.h"
#include "repl/scenes.h"
#include "repl/state_owners.h"
#include "subsystems/replay/replay.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond)      TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, want)  TEST_ASSERT_INT(&g_harness, label, got, want)
#define ASSERT_STR(label, got, want)  TEST_ASSERT_STR(&g_harness, label, got, want)

#define WATCH_PATH "/tmp/gl_repl_extedit_scene.glr"
#define SWAP_PATH  "/tmp/gl_repl_extedit_scene.glr.swap"
#define OTHER_PATH "/tmp/gl_repl_extedit_other.glr"

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
 * or a scene switch would throw away edits made in gl-repl and never saved. */
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

    /* One more keystroke in the editor lifts the hold; the update that lifts
     * it is dropped, and following resumes after that. */
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
 * path and so is not WIP_PATH here. */
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

/* A buffer that will not parse is the normal state of a file being typed. It
 * must not be retried every frame, and it must not damage what is live. */
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
    test_leaving_and_returning_to_the_parked_row();
    test_undo_exits_the_session_and_sticks();
    test_a_session_costs_one_undo_entry();
    test_sidecar_does_not_promote_a_catalog_scene();
    test_sidecar_deletion_splits_on_identity();
    test_a_sidecar_found_at_bind_time_is_offered();
    test_accepting_the_offer_starts_following();
    test_saving_during_a_session_is_not_a_second_reload();
    test_sidecar_is_geometry_not_presentation();
    test_an_unparseable_buffer_leaves_the_scene_alone();
    glr_extedit_set_enabled(0);
    glr_modal_cancel();
    (void)unlink(WIP_PATH);
    (void)unlink(WATCH_PATH);
    (void)unlink(SWAP_PATH);
    (void)unlink(OTHER_PATH);
    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "glr_extedit");
}

#endif /* __EMSCRIPTEN__ */
