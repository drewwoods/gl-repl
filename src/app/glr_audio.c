/*
 * repl_audio.c - miniaudio-backed playback for the immediate-mode REPL.
 *
 * This file owns the single `MINIAUDIO_IMPLEMENTATION` define for the
 * project; no other translation unit should define it. miniaudio is
 * vendored at include/miniaudio.h (same convention as stb_image.h).
 *
 * Threading model
 * ---------------
 * Every operation that can block on the filesystem runs on a dedicated
 * background worker thread, never on the caller (render) thread:
 *
 *   - ma_sound_init_from_file / ma_sound_uninit. Even with
 *     MA_SOUND_FLAG_ASYNC, miniaudio opens the stream on the calling
 *     thread, and an iCloud / network "dataless" file blocks there
 *     while the OS materializes it. That froze the app, so the open
 *     and teardown are owned by the worker.
 *   - save_state(). The periodic resume-state write used to fsync()
 *     on the render thread, a visible ~5s hitch.
 *
 * The main thread only posts a request into a 1-slot mailbox and
 * signals the worker (latest-wins for track changes: a newer request
 * supersedes a queued one — a load already stuck in the OS can't be
 * cancelled, it finishes/errors then the latest request runs). The
 * sound lives in a double buffer (g_slot[2]): the worker loads into
 * the inactive slot with no lock held, then briefly locks to publish
 * it as the active slot, so control ops (pause/loop/stop) on the main
 * thread are serialized against only that quick swap, never a media
 * open.
 *
 * load_state() stays synchronous on the caller: it reads the small
 * app-owned INI (never a media file), and glr_audio_play_playlist()'s
 * cfg_mode side effect must be visible to the immediately-following
 * caller (see tests/test_audio.c). Non-blocking control calls
 * (ma_sound_stop/start/set_looping, ma_engine volume) also stay on the
 * caller under the mutex.
 */

#include "app/glr_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

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

#define GLR_AUDIO_MAX_TRACKS 64
#define GLR_AUDIO_MAX_PATH   512

static ma_engine g_engine;

/* Double-buffered sound storage. ma_sound objects register internal
 * nodes in the engine graph and cannot be relocated after init, so we
 * keep two fixed slots: the worker loads into the inactive one and
 * atomically flips g_active under g_mtx. g_active == -1 means nothing
 * is loaded. */
static ma_sound g_slot[2];
static int      g_slot_inited[2] = { 0, 0 };
static int      g_active = -1;

static int g_inited       = 0;  /* engine init succeeded */
static int g_music_loaded = 0;  /* mirror of (g_active >= 0); guarded */
static int g_loading      = 0;  /* a worker load is in flight */
static int g_muted        = 0;
static int g_paused       = 0;  /* paused: track loaded but stopped, cursor held */
static int g_gesture_done = 0;  /* have we satisfied browser autoplay policy? */

/* Playlist: caller-registered tracks, in play order. */
static char g_playlist[GLR_AUDIO_MAX_TRACKS][GLR_AUDIO_MAX_PATH];
static int  g_playlist_count = 0;
static int  g_playlist_pos   = 0;  /* index of the currently-loaded track */

/* Default to GLR_AUDIO_LOOP_ALL so a folder of tracks just plays
 * through and repeats - the friendliest default, and it collapses to
 * "repeat forever" when there's only one file. */
static int g_loop_mode = GLR_AUDIO_LOOP_ALL;

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

/* Bumped on every successful track start. Callers poll this to notice
 * that the current track has changed without needing a callback. */
static unsigned int g_track_generation = 0;

/* ------------------------------------------------------------------ */
/* Background worker (owns all file-blocking miniaudio + state I/O)     */
/* ------------------------------------------------------------------ */

typedef enum {
    AWR_NONE = 0,
    AWR_START,     /* load g_req_idx (g_req_seek) into the inactive slot */
    AWR_ADVANCE,   /* current track ended: pick next per loop mode       */
    AWR_UNINIT,    /* drop the current sound (playlist cleared / OFF end) */
    AWR_QUIT       /* final save + uninit, then exit                     */
} AudioWorkerReq;

static pthread_t       g_worker;
static int             g_worker_running = 0;
static pthread_mutex_t g_mtx;
static pthread_cond_t  g_cv;

static AudioWorkerReq  g_req      = AWR_NONE;  /* latest lifecycle request */
static int             g_req_idx  = 0;
static float           g_req_seek = -1.0f;
static int             g_req_save = 0;         /* independent: periodic save */

/* ------------------------------------------------------------------ */
/* State persistence (track + offset + audio cfg across restarts)      */
/* ------------------------------------------------------------------ */

/* Path of the INI file, or empty string when disabled. */
static char  g_state_file[GLR_AUDIO_MAX_PATH] = "";

/* Opaque integer owned by repl_actions.c (maps to AUDIO_CFG_* enum).
 * Stored in the INI as cfg_mode= and handed back through
 * glr_audio_get_cfg_mode() so glr_actions_apply_defaults() can map it
 * to paused/loop state. -1 means "not yet loaded / not set". */
static int g_cfg_mode = -1;

/* Timestamp of the last successful state-file write. */
static time_t g_last_save_time = 0;
#define STATE_SAVE_INTERVAL_SECS 5

/* ------------------------------------------------------------------ */
/* Small lock helpers                                                  */
/* ------------------------------------------------------------------ */

static void lock(void)   { if (g_worker_running) pthread_mutex_lock(&g_mtx); }
static void unlock(void) { if (g_worker_running) pthread_mutex_unlock(&g_mtx); }

/* ------------------------------------------------------------------ */
/* Worker hitch detector                                               */
/* ------------------------------------------------------------------ */

/* Each worker wakeup runs one blocking lifecycle op (file open, sound
 * uninit page-flush, state-file write). On Linux a slow disk / page
 * read shows up as the worker taking far longer than the usual sub-ms.
 * We time the dispatch span and log any op over the threshold so the
 * culprit op is named in the terminal. Threshold is tunable via
 * GLR_AUDIO_HITCH_MS (default 50ms); 0 disables the check. */
static double worker_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static double worker_hitch_threshold_ms(void) {
    static double cached = -1.0;
    if (cached < 0.0) {
        const char *env = getenv("GLR_AUDIO_HITCH_MS");
        cached = (env && *env) ? atof(env) : 50.0;
        if (cached < 0.0) cached = 0.0;
    }
    return cached;
}

/* ------------------------------------------------------------------ */
/* State-file helpers                                                  */
/* ------------------------------------------------------------------ */

/* Current cursor in seconds for the active slot. Caller holds g_mtx;
 * ma_sound_get_cursor_in_pcm_frames is a non-blocking flag/atomic
 * read, safe to call under the lock. */
static float cursor_seconds_locked(void) {
    if (g_active < 0 || !g_inited) return 0.0f;
    ma_uint64 frames = 0;
    if (ma_sound_get_cursor_in_pcm_frames(&g_slot[g_active], &frames)
            != MA_SUCCESS)
        return 0.0f;
    ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
    return sr > 0 ? (float)frames / (float)sr : 0.0f;
}

/* Worker-only. Writes the resume-state INI. The atomic temp-file +
 * rename() already guarantees the destination is never torn; we
 * deliberately do NOT fsync() — the only thing lost on an OS crash in
 * the seconds before a flush is a music-resume position, not worth a
 * stall (and this now runs off the render thread anyway). */
static void worker_save_state(void) {
    char state_file[GLR_AUDIO_MAX_PATH];
    char cur_track[GLR_AUDIO_MAX_PATH] = "";
    int  have_track;
    float offset;
    int  cfg_mode;

    lock();
    if (!g_state_file[0]) { unlock(); return; }
    memcpy(state_file, g_state_file, sizeof(state_file));
    have_track = (g_music_loaded &&
                  g_playlist_pos >= 0 &&
                  g_playlist_pos < g_playlist_count);
    if (have_track) {
        memcpy(cur_track, g_playlist[g_playlist_pos], sizeof(cur_track));
        offset = cursor_seconds_locked();
    } else {
        offset = 0.0f;
    }
    cfg_mode = g_cfg_mode;
    unlock();

    char tmp[GLR_AUDIO_MAX_PATH];
    if (snprintf(tmp, sizeof(tmp), ".%s.tmp", state_file) >= (int)sizeof(tmp))
        return;

    FILE *f = fopen(tmp, "w");
    if (!f) return;

    if (have_track) {
        fprintf(f, "track=%s\n", cur_track);
        fprintf(f, "offset=%.3f\n", offset);
    } else {
        /* No track loaded: copy track/offset from the existing state
         * file so a failed load / pre-gesture save / post-playlist-end
         * shutdown does not clobber a valid resume position. */
        FILE *existing = fopen(state_file, "r");
        if (existing) {
            char line[GLR_AUDIO_MAX_PATH + 16];
            char saved_track[GLR_AUDIO_MAX_PATH] = "";
            float saved_offset = 0.0f;
            while (fgets(line, (int)sizeof(line), existing)) {
                if (strncmp(line, "track=", 6) == 0) {
                    char *p = line + 6;
                    size_t len = strlen(p);
                    while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) len--;
                    if (len >= GLR_AUDIO_MAX_PATH) len = GLR_AUDIO_MAX_PATH - 1;
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
    if (cfg_mode >= 0)
        fprintf(f, "cfg_mode=%d\n", cfg_mode);
    fflush(f);
    fclose(f);

    if (rename(tmp, state_file) != 0) {
        remove(tmp);  /* rename failed - discard temp, old file untouched */
    } else {
        lock();
        g_last_save_time = time(NULL);
        unlock();
    }
}

/* Loads persisted playback position and audio cfg from the state file.
 * Synchronous on the caller thread: the INI is a small app-owned file
 * (never a media file), and glr_audio_play_playlist()'s cfg_mode side
 * effect must be visible to the immediately-following caller. cfg_mode
 * is stored in g_cfg_mode for repl_actions.c to pick up via
 * glr_audio_get_cfg_mode(). Returns the playlist index of the saved track,
 * or -1 on failure. Sets *out_offset to the saved cursor in seconds. */
static int load_state(float *out_offset) {
    *out_offset = 0.0f;
    if (!g_state_file[0]) return -1;
    FILE *f = fopen(g_state_file, "r");
    if (!f) return -1;

    char saved_track[GLR_AUDIO_MAX_PATH] = "";
    float offset  = 0.0f;
    int cfg_mode  = -1;   /* -1 = not found in file */
    char line[GLR_AUDIO_MAX_PATH + 16];

    while (fgets(line, (int)sizeof(line), f)) {
        if (strncmp(line, "track=", 6) == 0) {
            char *p = line + 6;
            size_t len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) len--;
            if (len >= GLR_AUDIO_MAX_PATH) len = GLR_AUDIO_MAX_PATH - 1;
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
/* Worker-side sound lifecycle                                         */
/* ------------------------------------------------------------------ */

/* Worker-only. Uninit every inited slot and mark nothing active. The
 * ma_sound_uninit of a stream can block on pending page reads; that's
 * fine here — it is the worker thread, never the render thread. */
static void worker_uninit_all(void) {
    lock();
    g_active = -1;
    g_music_loaded = 0;
    unlock();
    for (int s = 0; s < 2; s++) {
        if (g_slot_inited[s]) {
            ma_sound_uninit(&g_slot[s]);
            g_slot_inited[s] = 0;
        }
    }
}

/* Worker-only. Load `idx` into the inactive slot (the slow
 * ma_sound_init_from_file happens with NO lock held), then briefly
 * lock to publish it as the active slot and retire the old one.
 * Returns 0 on success, -1 on failure (slot left uninited). */
static int worker_load(int idx, float seek_secs) {
    char path[GLR_AUDIO_MAX_PATH];
    int  target, old, loop_song, paused;

    lock();
    if (idx < 0 || idx >= g_playlist_count) { unlock(); return -1; }
    memcpy(path, g_playlist[idx], sizeof(path));
    old       = g_active;
    target    = (g_active < 0) ? 0 : (1 - g_active);
    loop_song = (g_loop_mode == GLR_AUDIO_LOOP_SONG);
    paused    = g_paused;
    g_loading = 1;
    unlock();

    if (g_slot_inited[target]) {           /* stale (failed prior load) */
        ma_sound_uninit(&g_slot[target]);
        g_slot_inited[target] = 0;
    }

    /* STREAM: decode on demand (small memory, fine for music).
     * ASYNC:  don't block the worker's first frame fill. The open
     * itself still happens here on the worker, never on the caller. */
    ma_result r = ma_sound_init_from_file(
        &g_engine, path,
        MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC,
        NULL, NULL, &g_slot[target]);
    if (r != MA_SUCCESS) {
        fprintf(stderr,
                "repl_audio: ma_sound_init_from_file(\"%s\") failed: %d\n",
                path, (int)r);
        lock(); g_loading = 0; unlock();
        return -1;
    }

    ma_sound_set_looping(&g_slot[target], loop_song ? MA_TRUE : MA_FALSE);

    r = ma_sound_start(&g_slot[target]);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "repl_audio: ma_sound_start(\"%s\") failed: %d\n",
                path, (int)r);
        ma_sound_uninit(&g_slot[target]);
        lock(); g_loading = 0; unlock();
        return -1;
    }

    /* miniaudio queues seeks for ASYNC stream sounds, so this is safe
     * immediately - it is applied once the background init completes. */
    if (seek_secs >= 0.0f) {
        ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
        if (sr > 0)
            ma_sound_seek_to_pcm_frame(
                &g_slot[target], (ma_uint64)(seek_secs * (float)sr));
    }

    /* Honour the paused state: stop without moving the cursor so a
     * later resume continues from here. */
    if (paused)
        ma_sound_stop(&g_slot[target]);

    lock();
    g_slot_inited[target] = 1;
    g_active              = target;
    g_music_loaded        = 1;
    g_playlist_pos        = idx;
    g_track_generation++;
    g_loading             = 0;
    unlock();

    /* Retire the previous slot now that the new one is live. Off-lock:
     * stream teardown may block, but only the worker is here. */
    if (old >= 0 && old != target && g_slot_inited[old]) {
        ma_sound_uninit(&g_slot[old]);
        g_slot_inited[old] = 0;
    }
    return 0;
}

/* Worker-only. The current track ended; advance per loop mode. Caps
 * attempts at playlist_count so a folder of broken files can't spin
 * forever (the skip-broken loop migrated here from glr_audio_tick so it
 * no longer runs on the render thread). */
static void worker_advance(void) {
    int pos, count, mode;
    lock();
    pos   = g_playlist_pos;
    count = g_playlist_count;
    mode  = g_loop_mode;
    unlock();

    int attempts = 0;
    while (attempts < count) {
        int next = pos + 1;
        if (next >= count) {
            if (mode == GLR_AUDIO_LOOP_ALL) {
                next = 0;
            } else {
                worker_uninit_all();   /* OFF: end of playlist */
                return;
            }
        }
        if (worker_load(next, -1.0f) == 0)
            return;
        pos = next;   /* broken track - skip past it */
        attempts++;
    }
    worker_uninit_all();   /* all remaining tracks failed */
}

static void *audio_worker_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mtx);
    for (;;) {
        while (g_req == AWR_NONE && !g_req_save)
            pthread_cond_wait(&g_cv, &g_mtx);

        AudioWorkerReq k    = g_req;
        int            idx  = g_req_idx;
        float          seek = g_req_seek;
        int            save = g_req_save;
        g_req      = AWR_NONE;
        g_req_save = 0;
        pthread_mutex_unlock(&g_mtx);

        if (k == AWR_QUIT) {
            worker_save_state();
            worker_uninit_all();
            return NULL;
        }

        double t0 = worker_now_ms();
        const char *op = "save-only";
        if (k == AWR_START)        { op = "load";    worker_load(idx, seek); }
        else if (k == AWR_ADVANCE) { op = "advance"; worker_advance(); }
        else if (k == AWR_UNINIT)  { op = "uninit";  worker_uninit_all(); }

        if (save)
            worker_save_state();    /* after the lifecycle op: fresh cursor */

        double thr = worker_hitch_threshold_ms();
        double dt  = worker_now_ms() - t0;
        if (thr > 0.0 && dt >= thr)
            fprintf(stderr,
                    "repl_audio: worker hitch: %s%s took %.1f ms "
                    "(threshold %.0f ms)\n",
                    op, save ? "+save" : "", dt, thr);

        pthread_mutex_lock(&g_mtx);
    }
}

/* Post a lifecycle request (latest-wins: a newer request supersedes a
 * queued one). No-op when the worker isn't running. */
static void worker_post(AudioWorkerReq kind, int idx, float seek) {
    if (!g_worker_running) return;
    pthread_mutex_lock(&g_mtx);
    g_req      = kind;
    g_req_idx  = idx;
    g_req_seek = seek;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mtx);
}

/* ------------------------------------------------------------------ */
/* Deferred-gesture-aware start                                         */
/* ------------------------------------------------------------------ */

/* Request a track start. On the web this is deferred until the first
 * user gesture (browser autoplay policy); on native it posts straight
 * to the worker. */
static int request_start(int idx, float seek) {
    if (!g_inited) return -1;
    if (idx < 0 || idx >= g_playlist_count) return -1;

#if AUDIO_NEEDS_GESTURE
    if (!g_gesture_done) {
        g_pending_start = 1;
        g_playlist_pos  = idx;   /* remember which track for the gesture */
        g_req_seek      = seek;
        return 0;
    }
#endif
    worker_post(AWR_START, idx, seek);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int glr_audio_init(void) {
    if (g_inited) return 0;

    ma_result r = ma_engine_init(NULL, &g_engine);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "repl_audio: ma_engine_init failed: %d\n", (int)r);
        return -1;
    }

    g_inited = 1;
    g_active = -1;
    g_slot_inited[0] = g_slot_inited[1] = 0;

    /* Spin up the worker that owns all file-blocking operations. If it
     * can't start, the module degrades to "no sound" rather than
     * blocking the render thread. */
    if (pthread_mutex_init(&g_mtx, NULL) == 0 &&
        pthread_cond_init(&g_cv, NULL) == 0) {
        g_req      = AWR_NONE;
        g_req_save = 0;
        if (pthread_create(&g_worker, NULL, audio_worker_main, NULL) == 0) {
            g_worker_running = 1;
        } else {
            fprintf(stderr, "repl_audio: worker thread create failed; "
                            "audio disabled\n");
            pthread_cond_destroy(&g_cv);
            pthread_mutex_destroy(&g_mtx);
        }
    }

#if AUDIO_NEEDS_GESTURE
    /* On the web, ma_engine_init starts the engine automatically but the
     * browser's AudioContext will be in a "suspended" state until a user
     * gesture resumes it. */
    g_gesture_done = 0;
#else
    g_gesture_done = 1;
#endif

    return 0;
}

void glr_audio_shutdown(void) {
    if (!g_inited) return;

    if (g_worker_running) {
        /* AWR_QUIT makes the worker do the final state save (off the
         * caller, but join() below waits for it so the file is on disk
         * before we return) and uninit the sounds, then exit. */
        pthread_mutex_lock(&g_mtx);
        g_req = AWR_QUIT;
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mtx);
        pthread_join(g_worker, NULL);
        pthread_cond_destroy(&g_cv);
        pthread_mutex_destroy(&g_mtx);
        g_worker_running = 0;
    } else {
        /* No worker (create failed): nothing was ever loaded. */
        worker_save_state();
        worker_uninit_all();
    }

    ma_engine_uninit(&g_engine);
    g_inited       = 0;
    g_music_loaded = 0;
    g_active       = -1;
}

int glr_audio_set_playlist(const char *const *paths, int count) {
    if (!paths || count < 0) return -1;

    /* Retire any current sound asynchronously; never block the caller. */
    worker_post(AWR_UNINIT, 0, -1.0f);

    lock();
    g_playlist_count = 0;
    g_playlist_pos   = 0;
    g_pending_start  = 0;

    int n = count;
    if (n > GLR_AUDIO_MAX_TRACKS) {
        fprintf(stderr,
                "repl_audio: playlist has %d tracks; truncating to %d\n",
                count, GLR_AUDIO_MAX_TRACKS);
        n = GLR_AUDIO_MAX_TRACKS;
    }

    for (int i = 0; i < n; i++) {
        const char *p = paths[i] ? paths[i] : "";
        size_t len = strlen(p);
        if (len >= GLR_AUDIO_MAX_PATH) len = GLR_AUDIO_MAX_PATH - 1;
        memcpy(g_playlist[i], p, len);
        g_playlist[i][len] = '\0';
    }
    g_playlist_count = n;
    unlock();
    return n;
}

int glr_audio_play_playlist(void) {
    if (!g_inited || g_playlist_count == 0) return -1;

    /* Resume from persisted state if possible. load_state() is
     * deliberately synchronous (small app INI, and the cfg_mode side
     * effect must be visible right after this returns - see
     * tests/test_audio.c). The slow media open happens on the worker. */
    float offset;
    int idx = load_state(&offset);
    if (idx >= 0)
        return request_start(idx, offset);
    return request_start(0, -1.0f);
}

int glr_audio_play_music(const char *path) {
    if (!path || !*path) return -1;
    const char *arr[1] = { path };
    if (glr_audio_set_playlist(arr, 1) < 0) return -1;
    return glr_audio_play_playlist();
}

void glr_audio_stop_music(void) {
    lock();
    if (g_active >= 0)
        ma_sound_stop(&g_slot[g_active]);   /* non-blocking */
    unlock();
}

int glr_audio_next_track(void) {
    if (!g_inited || g_playlist_count == 0) return -1;
    lock();
    int next = g_playlist_pos + 1;
    if (next >= g_playlist_count) next = 0;
    unlock();
    return request_start(next, -1.0f);
}

int glr_audio_prev_track(void) {
    if (!g_inited || g_playlist_count == 0) return -1;
    lock();
    int prev = g_playlist_pos - 1;
    if (prev < 0) prev = g_playlist_count - 1;
    unlock();
    return request_start(prev, -1.0f);
}

void glr_audio_tick(void) {
    /* All work here is messaging the worker; with no worker nothing is
     * (or can be) playing, so there is nothing to do. */
    if (!g_inited || !g_worker_running) return;

    /* Periodically persist track + offset so a crash or forced quit
     * still leaves a reasonably up-to-date state file. The write
     * itself happens on the worker (no fsync, no render-thread I/O). */
    lock();
    if (g_state_file[0] && g_music_loaded) {
        time_t now = time(NULL);
        if (difftime(now, g_last_save_time) >= STATE_SAVE_INTERVAL_SECS) {
            g_req_save = 1;
            pthread_cond_signal(&g_cv);
        }
    }

    /* In SONG mode miniaudio loops internally; there is no end to
     * detect. Otherwise, when the track has ended and nothing is
     * already loading/queued, ask the worker to advance. */
    if (g_loop_mode != GLR_AUDIO_LOOP_SONG &&
        g_active >= 0 && !g_loading && g_req == AWR_NONE &&
        ma_sound_at_end(&g_slot[g_active])) {
        g_req = AWR_ADVANCE;
        pthread_cond_signal(&g_cv);
    }
    unlock();
}

void glr_audio_set_loop_mode(int mode) {
    if (mode < GLR_AUDIO_LOOP_OFF)  mode = GLR_AUDIO_LOOP_OFF;
    if (mode > GLR_AUDIO_LOOP_ALL)  mode = GLR_AUDIO_LOOP_ALL;
    lock();
    g_loop_mode = mode;
    if (g_active >= 0)
        ma_sound_set_looping(
            &g_slot[g_active],
            (g_loop_mode == GLR_AUDIO_LOOP_SONG) ? MA_TRUE : MA_FALSE);
    unlock();
}

int glr_audio_get_loop_mode(void) {
    return g_loop_mode;
}

void glr_audio_set_paused(int paused) {
    int p = paused ? 1 : 0;
    lock();
    int was_paused = g_paused;
    g_paused = p;
    if (g_active >= 0) {
        if (p && !was_paused)
            ma_sound_stop(&g_slot[g_active]);
        else if (!p && was_paused)
            ma_sound_start(&g_slot[g_active]);
    }
    unlock();
}

int glr_audio_is_paused(void) {
    return g_paused;
}

void glr_audio_set_muted(int muted) {
    g_muted = muted ? 1 : 0;
    if (!g_inited) return;
    /* Engine-level volume is a non-blocking atomic store; safe on the
     * caller thread. */
    ma_engine_set_volume(&g_engine, g_muted ? 0.0f : 1.0f);
}

int glr_audio_is_muted(void) {
    return g_muted;
}

const char *glr_audio_get_current_track(void) {
    /* Only the main thread calls this (status bar); copy under the
     * lock into a static buffer so a concurrent worker swap of
     * g_playlist_pos can't tear the read. Valid until the next call. */
    static char buf[GLR_AUDIO_MAX_PATH];
    lock();
    if (!g_music_loaded ||
        g_playlist_pos < 0 || g_playlist_pos >= g_playlist_count) {
        unlock();
        return NULL;
    }
    memcpy(buf, g_playlist[g_playlist_pos], sizeof(buf));
    unlock();
    return buf;
}

unsigned int glr_audio_track_generation(void) {
    lock();
    unsigned int v = g_track_generation;
    unlock();
    return v;
}

void glr_audio_set_state_file(const char *path) {
    lock();
    if (!path) {
        g_state_file[0] = '\0';
    } else {
        size_t len = strlen(path);
        if (len >= GLR_AUDIO_MAX_PATH) len = GLR_AUDIO_MAX_PATH - 1;
        memcpy(g_state_file, path, len);
        g_state_file[len] = '\0';
    }
    g_last_save_time = 0;  /* force a write on the next tick after first play */
    unlock();
}

void glr_audio_on_user_gesture(void) {
    if (!g_inited || g_gesture_done) return;
    g_gesture_done = 1;

    /* Re-starting the engine is the canonical way to resume a suspended
     * AudioContext under the Emscripten backend. It's a cheap no-op on
     * native backends. */
    ma_engine_start(&g_engine);

    if (g_pending_start) {
        g_pending_start = 0;
        worker_post(AWR_START, g_playlist_pos, g_req_seek);
    }
}

void glr_audio_set_cfg_mode(int mode) {
    lock();
    g_cfg_mode = mode;
    unlock();
}

int glr_audio_get_cfg_mode(void) {
    lock();
    int v = g_cfg_mode;
    unlock();
    return v;
}
