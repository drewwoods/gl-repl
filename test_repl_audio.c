#include "repl_audio.h"
#include <stdio.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s]\n", label); \
} while (0)

int main() {
    printf("--- repl_audio tests ---\n");

    /* 1. Test playlist management (works even if engine not inited) */
    {
        const char *paths[] = { "test1.mp3", "test2.mp3", "test3.mp3" };
        int n = repl_audio_set_playlist(paths, 3);
        ASSERT_TRUE("set_playlist returns count", n == 3);

        /* Test truncation to REPL_AUDIO_MAX_TRACKS (64) */
        const char *many_paths[128];
        for (int i = 0; i < 128; i++) many_paths[i] = "long_path_to_some_audio_file_that_we_are_using_for_testing_purposes.mp3";
        n = repl_audio_set_playlist(many_paths, 128);
        ASSERT_TRUE("set_playlist truncates to 64", n == 64);

        /* Test null/invalid args */
        ASSERT_TRUE("set_playlist null paths", repl_audio_set_playlist(NULL, 5) == -1);
        ASSERT_TRUE("set_playlist negative count", repl_audio_set_playlist(paths, -1) == -1);
    }

    /* 2. Test loop modes */
    {
        repl_audio_set_loop_mode(REPL_AUDIO_LOOP_SONG);
        ASSERT_TRUE("get_loop_mode SONG", repl_audio_get_loop_mode() == REPL_AUDIO_LOOP_SONG);
        
        repl_audio_set_loop_mode(REPL_AUDIO_LOOP_OFF);
        ASSERT_TRUE("get_loop_mode OFF", repl_audio_get_loop_mode() == REPL_AUDIO_LOOP_OFF);

        repl_audio_set_loop_mode(REPL_AUDIO_LOOP_ALL);
        ASSERT_TRUE("get_loop_mode ALL", repl_audio_get_loop_mode() == REPL_AUDIO_LOOP_ALL);

        /* Clamp invalid modes */
        repl_audio_set_loop_mode(-1);
        ASSERT_TRUE("clamp loop mode low", repl_audio_get_loop_mode() == REPL_AUDIO_LOOP_OFF);
        repl_audio_set_loop_mode(99);
        ASSERT_TRUE("clamp loop mode high", repl_audio_get_loop_mode() == REPL_AUDIO_LOOP_ALL);
    }

    /* 3. Test muting */
    {
        repl_audio_set_muted(1);
        ASSERT_TRUE("is_muted 1", repl_audio_is_muted() == 1);
        repl_audio_set_muted(0);
        ASSERT_TRUE("is_muted 0", repl_audio_is_muted() == 0);
    }

    /* 4. Test engine-dependent functions before init (should fail gracefully) */
    {
        ASSERT_TRUE("play_playlist fails before init", repl_audio_play_playlist() == -1);
        ASSERT_TRUE("play_music fails before init", repl_audio_play_music("test.mp3") == -1);
        
        /* These are void and should not crash */
        repl_audio_stop_music();
        repl_audio_tick();
        repl_audio_on_user_gesture();
        repl_audio_shutdown();
    }

    /* 5. Initialize engine */
    printf("Attempting repl_audio_init()...\n");
    int inited = repl_audio_init();
    printf("repl_audio_init returned %d\n", inited);

    if (inited == 0) {
        /* Re-init should be a no-op */
        ASSERT_TRUE("re-init is no-op", repl_audio_init() == 0);

        /* Test play functions (will likely fail to load files, but should return error codes) */
        const char *empty_paths[] = { NULL };
        ASSERT_TRUE("play_playlist with no playlist", repl_audio_set_playlist(empty_paths, 0) == 0 && repl_audio_play_playlist() == -1);
        
        const char *one_path[] = { "nonexistent.mp3" };
        repl_audio_set_playlist(one_path, 1);
        /* start_track returns 0 if it defers for gesture or succeeds, -1 on immediate fail.
         * On native, it tries to start immediately. */
        int r = repl_audio_play_playlist();
        printf("play_playlist (nonexistent) returned %d\n", r);
        
        repl_audio_tick();
        repl_audio_on_user_gesture();
        repl_audio_set_muted(1);
        repl_audio_set_muted(0);
        
        repl_audio_shutdown();
        /* Shutdown should allow re-init */
        ASSERT_TRUE("re-init after shutdown", repl_audio_init() == 0);
        repl_audio_shutdown();
    } else {
        printf("Skipping engine-active tests as init failed.\n");
    }

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
