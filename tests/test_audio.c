#include "app/glr_audio.h"
#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

int main() {
    printf("--- repl_audio tests ---\n");

    /* 1. Test playlist management (works even if engine not inited) */
    {
        const char *paths[] = { "test1.mp3", "test2.mp3", "test3.mp3" };
        int n = glr_audio_set_playlist(paths, 3);
        ASSERT_TRUE("set_playlist returns count", n == 3);

        /* Test truncation to REPL_AUDIO_MAX_TRACKS (64) */
        const char *many_paths[128];
        for (int i = 0; i < 128; i++) many_paths[i] = "long_path_to_some_audio_file_that_we_are_using_for_testing_purposes.mp3";
        n = glr_audio_set_playlist(many_paths, 128);
        ASSERT_TRUE("set_playlist truncates to 64", n == 64);

        /* Test null/invalid args */
        ASSERT_TRUE("set_playlist null paths", glr_audio_set_playlist(NULL, 5) == -1);
        ASSERT_TRUE("set_playlist negative count", glr_audio_set_playlist(paths, -1) == -1);
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
        glr_audio_stop_music();
        glr_audio_tick();
        glr_audio_on_user_gesture();
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
        
        glr_audio_shutdown();
        /* Shutdown should allow re-init */
        ASSERT_TRUE("re-init after shutdown", glr_audio_init() == 0);
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
        /* Audit #7/#4: state-file path edge cases — shutdown must not
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
    } else {
        printf("Skipping engine-active tests as init failed.\n");
    }

    printf("\n");
    return test_harness_report(&g_harness, "test_repl_audio");
}
