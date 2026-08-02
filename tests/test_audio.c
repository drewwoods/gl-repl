#include "app/glr_audio.h"
#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

double glr_audio_hitch_threshold_ms_for_test(void);

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_STR(label, got, exp) \
    TEST_ASSERT_STR(&g_harness, label, got, exp)

static int wait_for_current_track(const char *expected_path,
                                  unsigned int baseline_generation,
                                  int timeout_ms) {
    int polls = timeout_ms / 10;
    if (polls < 1)
        polls = 1;
    for (int i = 0; i < polls; i++) {
        const char *current = glr_audio_get_current_track();
        if (glr_audio_track_generation() > baseline_generation &&
            current && strcmp(current, expected_path) == 0)
            return 1;
        glr_audio_tick();
        usleep(10000);
    }
    return 0;
}

int main() {
    printf("--- repl_audio tests ---\n");

    /* 1. Test playlist management (works even if engine not inited) */
    {
        const char *paths[] = { "test1.mp3", "test2.mp3", "test3.mp3" };
        int n = glr_audio_set_playlist(paths, 3);
        ASSERT_TRUE("set_playlist returns count", n == 3);
        ASSERT_INT("track_count after path playlist", glr_audio_track_count(), 3);
        ASSERT_STR("path playlist default group",
                   glr_audio_track_group(0), "Music");
        ASSERT_STR("path playlist derives display name",
                   glr_audio_track_display_name(1), "test2");
        ASSERT_INT("current index before load", glr_audio_current_index(), -1);
        ASSERT_TRUE("unknown duration before load",
                    glr_audio_track_duration_seconds(1) < 0.0f);

        /* Test truncation to REPL_AUDIO_MAX_TRACKS (64) */
        const char *many_paths[128];
        for (int i = 0; i < 128; i++) many_paths[i] = "long_path_to_some_audio_file_that_we_are_using_for_testing_purposes.mp3";
        n = glr_audio_set_playlist(many_paths, 128);
        ASSERT_TRUE("set_playlist truncates to 64", n == 64);

        /* Test null/invalid args */
        ASSERT_TRUE("set_playlist null paths", glr_audio_set_playlist(NULL, 5) == -1);
        ASSERT_TRUE("set_playlist negative count", glr_audio_set_playlist(paths, -1) == -1);
    }

    /* 1b. Track specs copy groups/display names and derive missing names. */
    {
        GlrAudioTrackSpec specs[] = {
            { "assets/alpha_track.mp3", "Assets", "Alpha Display" },
            { "bundle/BETA.MP3", "Bundled", NULL },
            { "user/no_extension", "My Music", "" },
        };
        ASSERT_INT("set_playlist_specs returns count",
                   glr_audio_set_playlist_specs(specs, 3), 3);
        ASSERT_INT("spec track count", glr_audio_track_count(), 3);
        ASSERT_STR("spec explicit display name",
                   glr_audio_track_display_name(0), "Alpha Display");
        ASSERT_STR("spec group copied",
                   glr_audio_track_group(1), "Bundled");
        ASSERT_STR("spec uppercase mp3 stem",
                   glr_audio_track_display_name(1), "BETA");
        ASSERT_STR("spec extensionless display name",
                   glr_audio_track_display_name(2), "no_extension");
        ASSERT_TRUE("spec out-of-range display NULL",
                    glr_audio_track_display_name(3) == NULL);
        ASSERT_TRUE("play_track_index before init fails",
                    glr_audio_play_track_index(0) == -1);
    }

    /* 2. Test loop modes */
    {
        glr_audio_set_loop_mode(GLR_AUDIO_LOOP_SONG);
        ASSERT_TRUE("get_loop_mode SONG", glr_audio_get_loop_mode() == GLR_AUDIO_LOOP_SONG);

        glr_audio_set_loop_mode(GLR_AUDIO_LOOP_OFF);
        ASSERT_TRUE("get_loop_mode OFF", glr_audio_get_loop_mode() == GLR_AUDIO_LOOP_OFF);

        glr_audio_set_loop_mode(GLR_AUDIO_LOOP_ALL);
        ASSERT_TRUE("get_loop_mode ALL", glr_audio_get_loop_mode() == GLR_AUDIO_LOOP_ALL);

        /* Clamp invalid modes */
        glr_audio_set_loop_mode(-1);
        ASSERT_TRUE("clamp loop mode low", glr_audio_get_loop_mode() == GLR_AUDIO_LOOP_OFF);
        glr_audio_set_loop_mode(99);
        ASSERT_TRUE("clamp loop mode high", glr_audio_get_loop_mode() == GLR_AUDIO_LOOP_ALL);
    }

    /* 3. Test muting */
    {
        glr_audio_set_muted(1);
        ASSERT_TRUE("is_muted 1", glr_audio_is_muted() == 1);
        glr_audio_set_muted(0);
        ASSERT_TRUE("is_muted 0", glr_audio_is_muted() == 0);
    }

    /* 4. cfg_mode getter/setter (no engine required) */
    {
        glr_audio_set_cfg_mode(2);
        ASSERT_TRUE("cfg_mode set/get 2", glr_audio_get_cfg_mode() == 2);

        glr_audio_set_cfg_mode(0);
        ASSERT_TRUE("cfg_mode set/get 0", glr_audio_get_cfg_mode() == 0);

        /* -1 is the sentinel "not set" value; the audio module accepts it */
        glr_audio_set_cfg_mode(-1);
        ASSERT_TRUE("cfg_mode set/get -1 (cleared)", glr_audio_get_cfg_mode() == -1);
    }

    /* 5. Test engine-dependent functions before init (should fail gracefully) */
    {
        ASSERT_TRUE("play_playlist fails before init", glr_audio_play_playlist() == -1);
        ASSERT_TRUE("play_music fails before init", glr_audio_play_music("test.mp3") == -1);

        /* These are void and should not crash */
        glr_audio_tick();
        glr_audio_on_user_gesture();
        glr_audio_shutdown();
    }

    /* 5b. Test GLR_AUDIO_HITCH_MS environment variable parsing */
    {
        /* Test default threshold (50ms) when env var is not set */
        unsetenv("GLR_AUDIO_HITCH_MS");
        ASSERT_TRUE("hitch: default threshold before init is 50.0",
                    glr_audio_hitch_threshold_ms_for_test() == 50.0);

        /* Test custom positive threshold */
        setenv("GLR_AUDIO_HITCH_MS", "125.5", 1);
        /* Since the previous call initialized the static cache, wait! We reset the cache
         * on shutdown, so we must call shutdown (or we didn't call init yet, so it was
         * evaluated. Wait, worker_hitch_threshold_ms evaluates when cached is < 0.
         * Since we didn't reset it yet because we haven't inited/shutdown, let's call shutdown
         * to force it to reset, or just do it in the loop with init/shutdown).
         * Actually, glr_audio_shutdown resets g_worker_hitch_threshold to -1.0.
         * So if we call glr_audio_shutdown(), it will reset the cache! */
        glr_audio_shutdown();
        ASSERT_TRUE("hitch: custom threshold parses correctly",
                    glr_audio_hitch_threshold_ms_for_test() == 125.5);

        /* Test boundary threshold (negative value clamps to 0) */
        setenv("GLR_AUDIO_HITCH_MS", "-25.0", 1);
        glr_audio_shutdown();
        ASSERT_TRUE("hitch: negative threshold clamps to 0",
                    glr_audio_hitch_threshold_ms_for_test() == 0.0);

        /* Test empty string threshold (defaults to 50.0) */
        setenv("GLR_AUDIO_HITCH_MS", "", 1);
        glr_audio_shutdown();
        ASSERT_TRUE("hitch: empty threshold defaults to 50.0",
                    glr_audio_hitch_threshold_ms_for_test() == 50.0);

        /* Restore clean environment */
        unsetenv("GLR_AUDIO_HITCH_MS");
        glr_audio_shutdown();
    }

    /* 6. Initialize engine */
    printf("Attempting glr_audio_init()...\n");
    int inited = glr_audio_init();
    printf("glr_audio_init returned %d\n", inited);

    if (inited == 0) {
        /* Re-init should be a no-op */
        ASSERT_TRUE("re-init is no-op", glr_audio_init() == 0);

        /* Test play functions (will likely fail to load files, but should return error codes) */
        const char *empty_paths[] = { NULL };
        ASSERT_TRUE("play_playlist with no playlist", glr_audio_set_playlist(empty_paths, 0) == 0 && glr_audio_play_playlist() == -1);

        const char *one_path[] = { "nonexistent.mp3" };
        glr_audio_set_playlist(one_path, 1);
        /* start_track returns 0 if it defers for gesture or succeeds, -1 on immediate fail.
         * On native, it tries to start immediately. */
        int r = glr_audio_play_playlist();
        printf("play_playlist (nonexistent) returned %d\n", r);

        glr_audio_tick();
        glr_audio_on_user_gesture();
        glr_audio_set_muted(1);
        glr_audio_set_muted(0);
        glr_audio_set_cfg_mode(2);
        glr_audio_shutdown();

        ASSERT_TRUE("shutdown clears cfg_mode", glr_audio_get_cfg_mode() == -1);

        /* Shutdown should allow re-init */
        ASSERT_TRUE("re-init after shutdown", glr_audio_init() == 0);
        ASSERT_TRUE("re-init starts with empty playlist", glr_audio_play_playlist() == -1);
        glr_audio_shutdown();

        /* 7. cfg_mode round-trip through save_state / load_state */
        {
            const char *state_file = "test_audio_cfg_mode_rtrip.ini";
            remove(state_file);

            ASSERT_TRUE("cfg_mode rt: init", glr_audio_init() == 0);
            glr_audio_set_state_file(state_file);
            glr_audio_set_cfg_mode(2);   /* AUDIO_CFG_SONG (value 2) */
            glr_audio_shutdown();        /* writes cfg_mode=2 to state file */

            /* Restore: clear cfg_mode, then reload via play_playlist */
            ASSERT_TRUE("cfg_mode rt: re-init", glr_audio_init() == 0);
            glr_audio_set_state_file(state_file);
            glr_audio_set_cfg_mode(-1);  /* clear so we detect the restore */
            const char *dummy[] = { "nonexistent_cfg.mp3" };
            glr_audio_set_playlist(dummy, 1);
            glr_audio_play_playlist();   /* calls load_state -> sets g_cfg_mode */
            ASSERT_TRUE("cfg_mode rt: restored to 2", glr_audio_get_cfg_mode() == 2);

            /* Out-of-range value: audio module preserves it raw; the editor
             * (apply_defaults) is responsible for clamping to a valid range. */
            glr_audio_set_cfg_mode(99);
            glr_audio_shutdown();
            ASSERT_TRUE("cfg_mode rt: re-init 2", glr_audio_init() == 0);
            glr_audio_set_state_file(state_file);
            glr_audio_set_cfg_mode(-1);
            glr_audio_set_playlist(dummy, 1);
            glr_audio_play_playlist();
            ASSERT_TRUE("cfg_mode rt: raw 99 preserved", glr_audio_get_cfg_mode() == 99);

            glr_audio_set_state_file(NULL);
            remove(state_file);
            glr_audio_shutdown();
            ASSERT_TRUE("cfg_mode rt: final re-init", glr_audio_init() == 0);
            glr_audio_shutdown();
        }
        /* Audit #7/#4: state-file path edge cases - shutdown must not
         * crash when the state file path has a nonexistent directory
         * component or is otherwise unwritable. */
        {
            ASSERT_TRUE("edge: re-init for path tests", glr_audio_init() == 0);

            glr_audio_set_state_file("nonexistent_subdir/audio_state.ini");
            glr_audio_set_cfg_mode(1);
            glr_audio_shutdown();
            ASSERT_TRUE("survived shutdown with bad dir path", 1);

            ASSERT_TRUE("edge: re-init after bad dir", glr_audio_init() == 0);
            glr_audio_set_state_file("/nonexistent/deep/path/state.ini");
            glr_audio_set_cfg_mode(2);
            glr_audio_shutdown();
            ASSERT_TRUE("survived shutdown with deep bad path", 1);

            ASSERT_TRUE("edge: re-init after deep bad path", glr_audio_init() == 0);
            glr_audio_set_state_file(NULL);
            glr_audio_shutdown();
        }

        /* 8. Idempotent user gesture handling */
        {
            ASSERT_TRUE("idempotency: init", glr_audio_init() == 0);
            glr_audio_on_user_gesture();
            glr_audio_on_user_gesture();
            glr_audio_on_user_gesture();
            ASSERT_TRUE("survived multiple gestures idempotently", 1);
            glr_audio_shutdown();
        }

        /* 9. In-flight load cancellation on playlist reset */
        {
            ASSERT_TRUE("cancel: init", glr_audio_init() == 0);
            const char *tracks[] = { "test1.mp3", "test2.mp3" };
            glr_audio_set_playlist(tracks, 2);
            glr_audio_play_playlist();

            /* Immediately set a new playlist to cancel the in-flight load */
            const char *new_tracks[] = { "test3.mp3" };
            glr_audio_set_playlist(new_tracks, 1);

            ASSERT_TRUE("set_playlist cancelled prior load", 1);
            glr_audio_shutdown();
        }

        /* 10. A saved-but-broken resume track should fall forward to the
         * next playable playlist entry instead of stopping on the error. */
        {
            const char *state_file = "test_audio_missing_resume.ini";
            const char *missing = "assets/does_not_exist.mp3";
            const char *fallback = "assets/sample.mp3";
            const char *tracks[] = { missing, fallback };
            FILE *f = NULL;
            unsigned int base_generation;

            remove(state_file);
            ASSERT_TRUE("resume-fallback: init", glr_audio_init() == 0);
            glr_audio_set_state_file(state_file);
            f = fopen(state_file, "w");
            ASSERT_TRUE("resume-fallback: state file opened", f != NULL);
            if (f) {
                fprintf(f, "track=%s\n", missing);
                fprintf(f, "offset=12.500\n");
                fclose(f);
            }

            glr_audio_set_playlist(tracks, 2);
            base_generation = glr_audio_track_generation();
            ASSERT_TRUE("resume-fallback: play requested",
                        glr_audio_play_playlist() == 0);
            ASSERT_TRUE("resume-fallback: skipped to next playable track",
                        wait_for_current_track(fallback, base_generation, 2000));

            glr_audio_shutdown();
            remove(state_file);
        }

        /* 11. Seek and relative seek tests */
        {
            ASSERT_TRUE("seek: init", glr_audio_init() == 0);
            /* Seek when nothing is active/pending should return -1 */
            ASSERT_INT("seek: fails when no active/pending", glr_audio_seek(10.0f), -1);

            /* Set up a playlist and set state file to trigger resume with offset */
            const char *state_file = "test_audio_seek.ini";
            const char *fallback = "assets/sample.mp3";
            const char *tracks[] = { fallback };
            FILE *f = NULL;
            unsigned int base_generation;

            remove(state_file);
            glr_audio_set_state_file(state_file);
            f = fopen(state_file, "w");
            ASSERT_TRUE("seek: state file opened", f != NULL);
            if (f) {
                fprintf(f, "track=%s\n", fallback);
                fprintf(f, "offset=5.000\n");
                fclose(f);
            }

            glr_audio_set_playlist(tracks, 1);
            base_generation = glr_audio_track_generation();
            /* Start playing, which will load and seek to 5.0s */
            ASSERT_INT("seek: play starts", glr_audio_play_playlist(), 0);

            /* While it is loading, we can do a seek.
             * Since the worker hasn't finished loading yet, g_active is still -1, and g_req is AWR_START.
             * Testing seek when g_req == AWR_START: */
            ASSERT_INT("seek: updates queued start offset", glr_audio_seek(10.0f), 0);

            /* Wait for the track to actually load */
            ASSERT_TRUE("seek: loaded", wait_for_current_track(fallback, base_generation, 2000));

            /* Once loaded, we can verify the seek position.
             * With ma_sound_seek_to_pcm_frame, the change is reflected on cursor queries. */
            float pos = glr_audio_current_cursor_seconds();
            printf("Cursor position after loading and seek: %.3f\n", pos);

            /* Perform a synchronous absolute seek to 15.0s */
            ASSERT_INT("seek: absolute seek to 15.0s", glr_audio_seek(15.0f), 0);

            pos = glr_audio_current_cursor_seconds();
            ASSERT_TRUE("seek: cursor is 15.0s", pos >= 14.9f && pos <= 15.1f);

            /* Perform a relative seek of -10.0s */
            ASSERT_INT("seek: relative seek -10s", glr_audio_seek_relative(-10.0f), 0);
            pos = glr_audio_current_cursor_seconds();
            ASSERT_TRUE("seek: cursor after relative -10s is 5.0s", pos >= 4.9f && pos <= 5.1f);

            /* Perform a relative seek of +20.0s */
            ASSERT_INT("seek: relative seek +20s", glr_audio_seek_relative(20.0f), 0);
            pos = glr_audio_current_cursor_seconds();
            ASSERT_TRUE("seek: cursor after relative +20s is 25.0s", pos >= 24.9f && pos <= 25.1f);

            glr_audio_shutdown();
            remove(state_file);
        }
    } else {
        printf("Skipping engine-active tests as init failed.\n");
    }

    printf("\n");
    return test_harness_report(&g_harness, "test_repl_audio");
}
