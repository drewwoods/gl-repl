/*
 * repl_audio.c - miniaudio-backed playback for the immediate-mode REPL.
 *
 * This file owns the single `MINIAUDIO_IMPLEMENTATION` define for the
 * project; no other translation unit should define it. miniaudio is
 * vendored at include/miniaudio.h (same convention as stb_image.h).
 */

#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>   /* fsync, fileno */

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

#define AUDIO_MAX_TRACKS 64
#define AUDIO_MAX_PATH   512

static ma_engine g_engine;
static ma_sound  g_music;

static int g_inited       = 0;  /* engine init succeeded */
static int g_music_loaded = 0;  /* g_music holds a valid sound */
static int g_muted        = 0;
static int g_paused       = 0;  /* paused: track loaded but stopped, cursor held */
static int g_gesture_done = 0;  /* have we satisfied browser autoplay policy? */

/* Playlist: caller-registered tracks, in play order. */
static char g_playlist[AUDIO_MAX_TRACKS][AUDIO_MAX_PATH];
static int  g_playlist_count = 0;
static int  g_playlist_pos   = 0;  /* index of the currently-loaded track */

/* Default to AUDIO_LOOP_ALL so a folder of tracks just plays
 * through and repeats - the friendliest default, and it collapses to
 * "repeat forever" when there's only one file. */
static int g_loop_mode = AUDIO_LOOP_ALL;

/* Native builds don't need a gesture. Emscripten does. */
#if defined(__EMSCRIPTEN__)
#  define AUDIO_NEEDS_GESTURE 1
#else
#  define AUDIO_NEEDS_GESTURE 0
#endif

/* Deferred-start flag: if play_playlist() / play_music() is called
 * before the first user gesture on the web, remember which track was
 * requested so we can start it on the first gesture. */
static int g_pending_start = 0;

/* Bumped on every successful start_track_now(). Callers poll this to
 * notice that the current track has changed without needing a
 * callback. */
static unsigned int g_track_generation = 0;

/* ------------------------------------------------------------------ */
/* State persistence (track + offset + audio cfg across restarts)      */
/* ------------------------------------------------------------------ */

/* Path of the INI file, or empty string when disabled. */
static char  g_state_file[AUDIO_MAX_PATH] = "";

/* Opaque integer owned by repl_actions.c (maps to AUDIO_CFG_* enum).
 * Stored in the INI as cfg_mode= and handed back through
 * audio_get_cfg_mode() so repl_actions_apply_defaults() can map it
 * to paused/loop state. -1 means "not yet loaded / not set". */
static int g_cfg_mode = -1;

/* When >= 0, apply this seek (seconds) inside the next start_track_now()
 * call.  Set by audio_play_playlist() after finding a saved state;
 * cleared immediately once the seek is issued. */
static float g_pending_seek_secs = -1.0f;

/* Timestamp of the last successful state-file write. */
static time_t g_last_save_time = 0;
#define STATE_SAVE_INTERVAL_SECS 5

/* ------------------------------------------------------------------ */
/* State-file helpers                                                  */
/* ------------------------------------------------------------------ */

static float cursor_seconds(void) {
    if (!g_music_loaded || !g_inited) return 0.0f;
    ma_uint64 frames = 0;
    if (ma_sound_get_cursor_in_pcm_frames(&g_music, &frames) != MA_SUCCESS)
        return 0.0f;
    ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
    return sr > 0 ? (float)frames / (float)sr : 0.0f;
}

static void save_state(void) {
    if (!g_state_file[0]) return;

    /* Write to a temp file first, then rename() over the real path.
     * rename() is a single atomic syscall on POSIX: the destination
     * always contains either the old content or the new content, never
     * a partial write - safe even if interrupted during shutdown. */
    char tmp[AUDIO_MAX_PATH];
    if (snprintf(tmp, sizeof(tmp), ".%s.tmp", g_state_file) >= (int)sizeof(tmp))
        return;

    FILE *f = fopen(tmp, "w");
    if (!f) return;

    /* Persist playback position only when a track is actually loaded. */
    if (g_music_loaded) {
        fprintf(f, "track=%s\n", g_playlist[g_playlist_pos]);
        fprintf(f, "offset=%.3f\n", cursor_seconds());
    } else {
        /* No track loaded: copy track/offset from the existing state file so
         * a failed load, pre-gesture save, or post-playlist-end shutdown does
         * not clobber a valid resume position. */
        FILE *existing = fopen(g_state_file, "r");
        if (existing) {
            char line[AUDIO_MAX_PATH + 16];
            char saved_track[AUDIO_MAX_PATH] = "";
            float saved_offset = 0.0f;
            while (fgets(line, (int)sizeof(line), existing)) {
                if (strncmp(line, "track=", 6) == 0) {
                    char *p = line + 6;
                    size_t len = strlen(p);
                    while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) len--;
                    if (len >= AUDIO_MAX_PATH) len = AUDIO_MAX_PATH - 1;
                    memcpy(saved_track, p, len);
                    saved_track[len] = '\0';
                } else if (strncmp(line, "offset=", 7) == 0) {
                    saved_offset = (float)atof(line + 7);
                }
            }
            fclose(existing);
            if (saved_track[0]) {
                fprintf(f, "track=%s\n", saved_track);
                fprintf(f, "offset=%.3f\n", saved_offset);
            }
        }
    }
    /* cfg_mode is the authoritative audio preference: encodes pause/loop
     * policy in a single int owned by repl_actions.c (AUDIO_CFG_* enum).
     * Only write it when the action layer has registered a value. */
    if (g_cfg_mode >= 0)
        fprintf(f, "cfg_mode=%d\n", g_cfg_mode);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp, g_state_file) != 0)
        remove(tmp);  /* rename failed - discard temp, old file untouched */
    else
        g_last_save_time = time(NULL);
}

/* Loads persisted playback position and audio cfg from the state file.
 * cfg_mode is stored in g_cfg_mode for repl_actions.c to pick up via
 * audio_get_cfg_mode() and apply via repl_actions_apply_defaults().
 * Returns the playlist index of the saved track, or -1 on failure.
 * Sets *out_offset to the saved cursor in seconds (0 on failure). */
static int load_state(float *out_offset) {
    *out_offset = 0.0f;
    if (!g_state_file[0]) return -1;
    FILE *f = fopen(g_state_file, "r");
    if (!f) return -1;

    char saved_track[AUDIO_MAX_PATH] = "";
    float offset  = 0.0f;
    int cfg_mode  = -1;   /* -1 = not found in file */
    char line[AUDIO_MAX_PATH + 16];

    while (fgets(line, (int)sizeof(line), f)) {
        if (strncmp(line, "track=", 6) == 0) {
            char *p = line + 6;
            size_t len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) len--;
            if (len >= AUDIO_MAX_PATH) len = AUDIO_MAX_PATH - 1;
            memcpy(saved_track, p, len);
            saved_track[len] = '\0';
        } else if (strncmp(line, "offset=", 7) == 0) {
            offset = (float)atof(line + 7);
        } else if (strncmp(line, "cfg_mode=", 9) == 0) {
            cfg_mode = atoi(line + 9);
        }
    }
    fclose(f);

    /* Store cfg_mode for the editor to retrieve and apply. */
    if (cfg_mode >= 0)
        g_cfg_mode = cfg_mode;

    if (!saved_track[0]) return -1;
    for (int i = 0; i < g_playlist_count; i++) {
        if (strcmp(g_playlist[i], saved_track) == 0) {
            *out_offset = (offset > 0.0f ? offset : 0.0f);
            return i;
        }
    }
    return -1;  /* track not found in current playlist - start fresh */
}

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
     * auto-advance from audio_tick() when the sound reaches end. */
    ma_sound_set_looping(
        &g_music,
        (g_loop_mode == AUDIO_LOOP_SONG) ? MA_TRUE : MA_FALSE);

    r = ma_sound_start(&g_music);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "repl_audio: ma_sound_start(\"%s\") failed: %d\n",
                path, (int)r);
        ma_sound_uninit(&g_music);
        return -1;
    }

    g_music_loaded = 1;
    g_playlist_pos = idx;
    g_track_generation++;

    /* Apply a saved cursor position if one was staged (e.g. by
     * audio_play_playlist() restoring persisted state).
     * miniaudio queues seek operations for ASYNC stream sounds, so
     * issuing it immediately is safe - it will be processed once the
     * background init job completes. */
    if (g_pending_seek_secs >= 0.0f) {
        ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
        if (sr > 0) {
            ma_uint64 frame = (ma_uint64)(g_pending_seek_secs * (float)sr);
            ma_sound_seek_to_pcm_frame(&g_music, frame);
        }
        g_pending_seek_secs = -1.0f;
    }

    /* Honour the paused state: stop without moving the cursor so that
     * audio_set_paused(0) / ma_sound_start() resumes from here. */
    if (g_paused)
        ma_sound_stop(&g_music);

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

int audio_init(void) {
    if (g_inited) return 0;

    ma_result r = ma_engine_init(NULL, &g_engine);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "repl_audio: ma_engine_init failed: %d\n", (int)r);
        return -1;
    }

    g_inited = 1;

#if AUDIO_NEEDS_GESTURE
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

void audio_shutdown(void) {
    if (!g_inited) return;
    save_state();
    uninit_music();
    ma_engine_uninit(&g_engine);
    g_inited = 0;
}

int audio_set_playlist(const char *const *paths, int count) {
    if (!paths || count < 0) return -1;

    uninit_music();
    g_playlist_count = 0;
    g_playlist_pos   = 0;
    g_pending_start  = 0;

    int n = count;
    if (n > AUDIO_MAX_TRACKS) {
        fprintf(stderr,
                "repl_audio: playlist has %d tracks; truncating to %d\n",
                count, AUDIO_MAX_TRACKS);
        n = AUDIO_MAX_TRACKS;
    }

    for (int i = 0; i < n; i++) {
        const char *p = paths[i] ? paths[i] : "";
        size_t len = strlen(p);
        if (len >= AUDIO_MAX_PATH) len = AUDIO_MAX_PATH - 1;
        memcpy(g_playlist[i], p, len);
        g_playlist[i][len] = '\0';
    }
    g_playlist_count = n;
    return n;
}

int audio_play_playlist(void) {
    if (!g_inited || g_playlist_count == 0) return -1;

    /* Try to resume from a persisted state.  If the saved track is in the
     * current playlist, stage the seek offset BEFORE start_track() so
     * start_track_now() can apply it immediately.  On any failure (file
     * absent, track not found) fall back to track 0. */
    float offset;
    int idx = load_state(&offset);
    if (idx >= 0) {
        g_pending_seek_secs = offset;
        return start_track(idx);
    }
    return start_track(0);
}

int audio_play_music(const char *path) {
    if (!path || !*path) return -1;
    const char *arr[1] = { path };
    if (audio_set_playlist(arr, 1) < 0) return -1;
    return audio_play_playlist();
}

void audio_stop_music(void) {
    if (!g_music_loaded) return;
    ma_sound_stop(&g_music);
}

int audio_next_track(void) {
    if (!g_inited || g_playlist_count == 0) return -1;
    int next = g_playlist_pos + 1;
    if (next >= g_playlist_count) next = 0;
    return start_track(next);
}

int audio_prev_track(void) {
    if (!g_inited || g_playlist_count == 0) return -1;
    int prev = g_playlist_pos - 1;
    if (prev < 0) prev = g_playlist_count - 1;
    return start_track(prev);
}

void audio_tick(void) {
    if (!g_inited || !g_music_loaded) return;

    /* Periodically persist track + offset so a crash or forced quit
     * still leaves a reasonably up-to-date state file. */
    if (g_state_file[0]) {
        time_t now = time(NULL);
        if (difftime(now, g_last_save_time) >= STATE_SAVE_INTERVAL_SECS)
            save_state();
    }

    /* In SONG mode miniaudio handles looping internally; there is no
     * end to detect so tick() is a no-op. */
    if (g_loop_mode == AUDIO_LOOP_SONG) return;

    if (!ma_sound_at_end(&g_music)) return;

    /* Current track finished. Advance to the next playable one.
     * Cap attempts at playlist_count so a folder full of broken files
     * can't spin forever. */
    int attempts = 0;
    while (attempts < g_playlist_count) {
        int next = g_playlist_pos + 1;
        if (next >= g_playlist_count) {
            if (g_loop_mode == AUDIO_LOOP_ALL) {
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

void audio_set_loop_mode(int mode) {
    if (mode < AUDIO_LOOP_OFF)  mode = AUDIO_LOOP_OFF;
    if (mode > AUDIO_LOOP_ALL)  mode = AUDIO_LOOP_ALL;
    g_loop_mode = mode;

    if (g_music_loaded) {
        ma_sound_set_looping(
            &g_music,
            (g_loop_mode == AUDIO_LOOP_SONG) ? MA_TRUE : MA_FALSE);
    }
}

int audio_get_loop_mode(void) {
    return g_loop_mode;
}

void audio_set_paused(int paused) {
    int was_paused = g_paused;
    g_paused = paused ? 1 : 0;
    if (!g_inited || !g_music_loaded) return;
    if (g_paused && !was_paused)
        ma_sound_stop(&g_music);
    else if (!g_paused && was_paused)
        ma_sound_start(&g_music);
}

int audio_is_paused(void) {
    return g_paused;
}

void audio_set_muted(int muted) {
    g_muted = muted ? 1 : 0;
    if (!g_inited) return;
    ma_engine_set_volume(&g_engine, g_muted ? 0.0f : 1.0f);
}

int audio_is_muted(void) {
    return g_muted;
}

const char *audio_get_current_track(void) {
    if (!g_music_loaded) return NULL;
    if (g_playlist_pos < 0 || g_playlist_pos >= g_playlist_count) return NULL;
    return g_playlist[g_playlist_pos];
}

unsigned int audio_track_generation(void) {
    return g_track_generation;
}

void audio_set_state_file(const char *path) {
    if (!path) { g_state_file[0] = '\0'; return; }
    size_t len = strlen(path);
    if (len >= AUDIO_MAX_PATH) len = AUDIO_MAX_PATH - 1;
    memcpy(g_state_file, path, len);
    g_state_file[len] = '\0';
    g_last_save_time = 0;  /* force a write on the next tick after first play */
}

void audio_on_user_gesture(void) {
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

void audio_set_cfg_mode(int mode) {
    g_cfg_mode = mode;
}

int audio_get_cfg_mode(void) {
    return g_cfg_mode;
}
