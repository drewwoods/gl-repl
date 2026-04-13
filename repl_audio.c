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

#define REPL_AUDIO_MAX_TRACKS 64
#define REPL_AUDIO_MAX_PATH   512

static ma_engine g_engine;
static ma_sound  g_music;

static int g_inited       = 0;  /* engine init succeeded */
static int g_music_loaded = 0;  /* g_music holds a valid sound */
static int g_muted        = 0;
static int g_gesture_done = 0;  /* have we satisfied browser autoplay policy? */

/* Playlist: caller-registered tracks, in play order. */
static char g_playlist[REPL_AUDIO_MAX_TRACKS][REPL_AUDIO_MAX_PATH];
static int  g_playlist_count = 0;
static int  g_playlist_pos   = 0;  /* index of the currently-loaded track */

/* Default to REPL_AUDIO_LOOP_ALL so a folder of tracks just plays
 * through and repeats — the friendliest default, and it collapses to
 * "repeat forever" when there's only one file. */
static int g_loop_mode = REPL_AUDIO_LOOP_ALL;

/* Native builds don't need a gesture. Emscripten does. */
#if defined(__EMSCRIPTEN__)
#  define REPL_AUDIO_NEEDS_GESTURE 1
#else
#  define REPL_AUDIO_NEEDS_GESTURE 0
#endif

/* Deferred-start flag: if play_playlist() / play_music() is called
 * before the first user gesture on the web, remember which track was
 * requested so we can start it on the first gesture. */
static int g_pending_start = 0;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void uninit_music(void) {
    if (g_music_loaded) {
        ma_sound_uninit(&g_music);
        g_music_loaded = 0;
    }
}

static int start_track_now(int idx) {
    if (!g_inited) return -1;
    if (idx < 0 || idx >= g_playlist_count) return -1;

    uninit_music();

    const char *path = g_playlist[idx];

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

    /* Only SONG mode hands looping off to miniaudio. OFF and ALL both
     * auto-advance from repl_audio_tick() when the sound reaches end. */
    ma_sound_set_looping(
        &g_music,
        (g_loop_mode == REPL_AUDIO_LOOP_SONG) ? MA_TRUE : MA_FALSE);

    r = ma_sound_start(&g_music);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "repl_audio: ma_sound_start(\"%s\") failed: %d\n",
                path, (int)r);
        ma_sound_uninit(&g_music);
        return -1;
    }

    g_music_loaded = 1;
    g_playlist_pos = idx;
    return 0;
}

/* Wrapper that honors the browser autoplay deferral. On native this
 * just forwards to start_track_now(). */
static int start_track(int idx) {
    if (!g_inited) return -1;
    if (idx < 0 || idx >= g_playlist_count) return -1;

    if (!g_gesture_done) {
        g_playlist_pos = idx;
        g_pending_start = 1;
        return 0;
    }
    return start_track_now(idx);
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

int repl_audio_set_playlist(const char *const *paths, int count) {
    if (!paths || count < 0) return -1;

    uninit_music();
    g_playlist_count = 0;
    g_playlist_pos   = 0;
    g_pending_start  = 0;

    int n = count;
    if (n > REPL_AUDIO_MAX_TRACKS) {
        fprintf(stderr,
                "repl_audio: playlist has %d tracks; truncating to %d\n",
                count, REPL_AUDIO_MAX_TRACKS);
        n = REPL_AUDIO_MAX_TRACKS;
    }

    for (int i = 0; i < n; i++) {
        const char *p = paths[i] ? paths[i] : "";
        size_t len = strlen(p);
        if (len >= REPL_AUDIO_MAX_PATH) len = REPL_AUDIO_MAX_PATH - 1;
        memcpy(g_playlist[i], p, len);
        g_playlist[i][len] = '\0';
    }
    g_playlist_count = n;
    return n;
}

int repl_audio_play_playlist(void) {
    if (!g_inited || g_playlist_count == 0) return -1;
    return start_track(0);
}

int repl_audio_play_music(const char *path) {
    if (!path || !*path) return -1;
    const char *arr[1] = { path };
    if (repl_audio_set_playlist(arr, 1) < 0) return -1;
    return repl_audio_play_playlist();
}

void repl_audio_stop_music(void) {
    if (!g_music_loaded) return;
    ma_sound_stop(&g_music);
}

void repl_audio_tick(void) {
    if (!g_inited || !g_music_loaded) return;

    /* In SONG mode miniaudio handles looping internally; there is no
     * end to detect so tick() is a no-op. */
    if (g_loop_mode == REPL_AUDIO_LOOP_SONG) return;

    if (!ma_sound_at_end(&g_music)) return;

    /* Current track finished. Advance to the next playable one.
     * Cap attempts at playlist_count so a folder full of broken files
     * can't spin forever. */
    int attempts = 0;
    while (attempts < g_playlist_count) {
        int next = g_playlist_pos + 1;
        if (next >= g_playlist_count) {
            if (g_loop_mode == REPL_AUDIO_LOOP_ALL) {
                next = 0;
            } else {
                /* OFF: end of playlist, stop cleanly. */
                uninit_music();
                return;
            }
        }
        if (start_track_now(next) == 0) return;
        /* start_track_now() failed; skip past the broken track. */
        g_playlist_pos = next;
        attempts++;
    }
    /* All remaining tracks failed to start. Give up silently. */
    uninit_music();
}

void repl_audio_set_loop_mode(int mode) {
    if (mode < REPL_AUDIO_LOOP_OFF)  mode = REPL_AUDIO_LOOP_OFF;
    if (mode > REPL_AUDIO_LOOP_ALL)  mode = REPL_AUDIO_LOOP_ALL;
    g_loop_mode = mode;

    if (g_music_loaded) {
        ma_sound_set_looping(
            &g_music,
            (g_loop_mode == REPL_AUDIO_LOOP_SONG) ? MA_TRUE : MA_FALSE);
    }
}

int repl_audio_get_loop_mode(void) {
    return g_loop_mode;
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

    if (g_pending_start) {
        g_pending_start = 0;
        start_track_now(g_playlist_pos);
    }
}
