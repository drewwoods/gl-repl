/*
 * repl_audio.c - miniaudio-backed playback for the immediate-mode REPL.
 *
 * This file owns the single `MINIAUDIO_IMPLEMENTATION` define for the
 * project; no other translation unit should define it. miniaudio is
 * vendored at include/miniaudio.h (same convention as stb_image.h).
 */

#include "repl_audio.h"

#include <stdio.h>
#include <string.h>

/* Silence a few benign warnings from miniaudio's ~100k-line header under
 * -Wall -Wfloat-conversion without polluting the rest of the build. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wunused-variable"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wfloat-conversion"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#endif

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static ma_engine g_engine;
static ma_sound  g_music;

static int g_inited       = 0;  /* engine init succeeded */
static int g_music_loaded = 0;  /* g_music holds a valid sound */
static int g_muted        = 0;
static int g_gesture_done = 0;  /* have we satisfied browser autoplay policy? */

/* Native builds don't need a gesture. Emscripten does. */
#if defined(__EMSCRIPTEN__)
#  define REPL_AUDIO_NEEDS_GESTURE 1
#else
#  define REPL_AUDIO_NEEDS_GESTURE 0
#endif

/* Cached deferred music path: if play_music() is called before the
 * first user gesture on the web, we remember the path and start it
 * once the gesture arrives. */
static char g_pending_music_path[512];
static int  g_pending_music = 0;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void uninit_music(void) {
    if (g_music_loaded) {
        ma_sound_uninit(&g_music);
        g_music_loaded = 0;
    }
}

static int start_music_now(const char *path) {
    if (!g_inited) return -1;

    uninit_music();

    /* STREAM: decode on demand (keeps memory small, fine for music).
     * ASYNC:  don't block startup on the file open / first frame. */
    ma_result r = ma_sound_init_from_file(
        &g_engine,
        path,
        MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC,
        NULL,  /* group */
        NULL,  /* fence */
        &g_music);
    if (r != MA_SUCCESS) {
        fprintf(stderr,
                "repl_audio: ma_sound_init_from_file(\"%s\") failed: %d\n",
                path, (int)r);
        return -1;
    }

    ma_sound_set_looping(&g_music, MA_TRUE);

    r = ma_sound_start(&g_music);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "repl_audio: ma_sound_start failed: %d\n", (int)r);
        ma_sound_uninit(&g_music);
        return -1;
    }

    g_music_loaded = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int repl_audio_init(void) {
    if (g_inited) return 0;

    ma_result r = ma_engine_init(NULL, &g_engine);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "repl_audio: ma_engine_init failed: %d\n", (int)r);
        return -1;
    }

    g_inited = 1;

#if REPL_AUDIO_NEEDS_GESTURE
    /* On the web, ma_engine_init starts the engine automatically but the
     * browser's AudioContext will be in a "suspended" state until a user
     * gesture resumes it. We'll call ma_engine_start() again on the
     * first key/mouse event which is a no-op on native. */
    g_gesture_done = 0;
#else
    g_gesture_done = 1;
#endif

    return 0;
}

void repl_audio_shutdown(void) {
    if (!g_inited) return;
    uninit_music();
    ma_engine_uninit(&g_engine);
    g_inited = 0;
}

int repl_audio_play_music(const char *path) {
    if (!g_inited || !path || !*path) return -1;

    if (!g_gesture_done) {
        /* Defer until the user interacts with the page. */
        size_t n = strlen(path);
        if (n >= sizeof(g_pending_music_path))
            n = sizeof(g_pending_music_path) - 1;
        memcpy(g_pending_music_path, path, n);
        g_pending_music_path[n] = '\0';
        g_pending_music = 1;
        return 0;
    }

    return start_music_now(path);
}

void repl_audio_stop_music(void) {
    if (!g_music_loaded) return;
    ma_sound_stop(&g_music);
}

void repl_audio_set_muted(int muted) {
    g_muted = muted ? 1 : 0;
    if (!g_inited) return;
    ma_engine_set_volume(&g_engine, g_muted ? 0.0f : 1.0f);
}

int repl_audio_is_muted(void) {
    return g_muted;
}

void repl_audio_on_user_gesture(void) {
    if (!g_inited || g_gesture_done) return;
    g_gesture_done = 1;

    /* Re-starting the engine is the canonical way to resume a suspended
     * AudioContext under the Emscripten backend. It's a cheap no-op on
     * native backends. */
    ma_engine_start(&g_engine);

    if (g_pending_music) {
        g_pending_music = 0;
        start_music_now(g_pending_music_path);
    }
}
