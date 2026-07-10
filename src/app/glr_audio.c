/*
 * glr_audio.c - miniaudio-backed playback for the immediate-mode REPL.
 *
 * This file owns the single `MINIAUDIO_IMPLEMENTATION` define for the
 * project; no other translation unit should define it. miniaudio is
 * vendored at include/miniaudio.h (same convention as stb_image.h).
 *
 * Threading model
 * ---------------
 * By default every operation that can block on the filesystem runs on a
 * dedicated background worker thread, never on the caller (render) thread:
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
 *
 * Compile-time threading toggle (GLR_AUDIO_NO_THREAD)
 * --------------------------------------------------
 * The worker thread is selectable at compile time. On Emscripten (no
 * SharedArrayBuffer / -pthread by default) it is disabled automatically;
 * a native build can force it off with -DGLR_AUDIO_NO_THREAD=1. With the
 * thread off there is no pthread dependency at all: the same lifecycle
 * requests are serviced synchronously from glr_audio_tick() on the
 * per-frame caller, the lock helpers collapse to no-ops, and no other
 * translation unit changes (glr_audio_tick() is already called once per
 * frame by the controller). This is safe in that configuration because
 * media there lives in MEMFS and never blocks on the filesystem — the
 * very hazard the worker thread exists to absorb on native builds.
 */

#include "app/glr_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__EMSCRIPTEN__)

#include <emscripten/emscripten.h>

#define GLR_AUDIO_MAX_TRACKS 64
#define GLR_AUDIO_MAX_PATH   512
#define STATE_SAVE_INTERVAL_SECS 5
#define GLR_AUDIO_NO_SEEK (-1.0f)

static int g_inited       = 0;
static int g_music_loaded = 0;
static int g_muted        = 0;
static int g_paused       = 0;
static int g_gesture_done = 0;

static char g_playlist[GLR_AUDIO_MAX_TRACKS][GLR_AUDIO_MAX_PATH];
static char g_playlist_group[GLR_AUDIO_MAX_TRACKS][64];
static char g_playlist_display_name[GLR_AUDIO_MAX_TRACKS][128];
static float g_playlist_duration_secs[GLR_AUDIO_MAX_TRACKS];
static int  g_playlist_count = 0;
static int  g_playlist_pos   = 0;

static int g_loop_mode = GLR_AUDIO_LOOP_ALL;

static int g_pending_start = 0;
static int g_pending_idx = 0;
static float g_pending_seek = GLR_AUDIO_NO_SEEK;
static int g_play_when_manifest_ready = 0;
static int g_error_skip_count = 0;

static unsigned int g_track_generation = 0;
static char  g_state_file[GLR_AUDIO_MAX_PATH] = "";
static int g_cfg_mode = -1;
static time_t g_last_save_time = 0;

EM_JS(int, web_audio_js_init, (void), {
    if (!Module.glrAudio) {
        var audio = new Audio();
        audio.preload = 'auto';
        Module.glrAudio = {
            audio: audio,
            ended: false,
            error: false,
            pendingSeek: NaN
        };
        audio.addEventListener('ended', function() {
            Module.glrAudio.ended = true;
        });
        audio.addEventListener('error', function() {
            Module.glrAudio.error = true;
        });
        audio.addEventListener('loadedmetadata', function() {
            var s = Module.glrAudio;
            if (Number.isFinite(s.pendingSeek)) {
                try { audio.currentTime = Math.max(0, s.pendingSeek); } catch (e) {}
                s.pendingSeek = NaN;
            }
        });
    }
    return 0;
});

EM_JS(void, web_audio_js_shutdown, (void), {
    var s = Module.glrAudio;
    if (!s || !s.audio) return;
    try { s.audio.pause(); } catch (e) {}
    s.audio.removeAttribute('src');
    try { s.audio.load(); } catch (e) {}
    s.ended = false;
    s.error = false;
    s.pendingSeek = NaN;
});

EM_JS(void, web_audio_js_request_manifest, (void), {
    if (Module.glrAudioManifestRequested) return;
    Module.glrAudioManifestRequested = true;

    fetch('assets/music.json', { cache: 'no-store' })
        .then(function(resp) {
            if (!resp.ok) throw new Error('HTTP ' + resp.status);
            return resp.json();
        })
        .then(function(data) {
            var tracks = Array.isArray(data) ? data : (data && data.tracks);
            if (!Array.isArray(tracks)) tracks = [];

            Module.ccall('glr_audio_web_manifest_begin', 'number',
                         ['number'], [tracks.length]);
            tracks.forEach(function(t) {
                if (!t) return;
                var path = t.path || t.url || "";
                if (!path) return;
                Module.ccall('glr_audio_web_manifest_add', 'number',
                             ['string', 'string', 'string'],
                             [path, t.group || 'Music',
                              t.display_name || t.name || ""]);
            });
            Module.ccall('glr_audio_web_manifest_finish', 'number', [], []);
        })
        .catch(function(err) {
            console.warn('gl-repl audio manifest unavailable:', err);
            Module.ccall('glr_audio_web_manifest_begin', 'number',
                         ['number'], [0]);
            Module.ccall('glr_audio_web_manifest_finish', 'number', [], []);
        });
});

EM_JS(int, web_audio_js_start, (const char *path_ptr, int loop,
                               int paused, int muted, double seek), {
    var s = Module.glrAudio;
    if (!s || !s.audio) return -1;

    var audio = s.audio;
    var path = UTF8ToString(path_ptr);
    s.ended = false;
    s.error = false;
    s.pendingSeek = seek >= 0 ? seek : NaN;

    if (audio.getAttribute('src') !== path) {
        audio.src = path;
        try { audio.load(); } catch (e) {}
    }
    audio.loop = !!loop;
    audio.muted = !!muted;

    if (Number.isFinite(s.pendingSeek)) {
        try {
            audio.currentTime = Math.max(0, s.pendingSeek);
            s.pendingSeek = NaN;
        } catch (e) {
            /* loadedmetadata will apply the queued seek. */
        }
    }

    if (paused) {
        audio.pause();
        return 0;
    }

    var p = audio.play();
    if (p && p.catch) {
        p.catch(function(err) {
            if (err && err.name === 'NotAllowedError') return;
            console.warn('gl-repl audio play failed:', err);
        });
    }
    return 0;
});

EM_JS(void, web_audio_js_set_paused, (int paused), {
    var s = Module.glrAudio;
    if (!s || !s.audio) return;
    if (paused) {
        s.audio.pause();
    } else {
        var p = s.audio.play();
        if (p && p.catch) {
            p.catch(function(err) {
                if (err && err.name === 'NotAllowedError') return;
                console.warn('gl-repl audio resume failed:', err);
            });
        }
    }
});

EM_JS(void, web_audio_js_set_muted, (int muted), {
    var s = Module.glrAudio;
    if (s && s.audio) s.audio.muted = !!muted;
});

EM_JS(void, web_audio_js_set_loop, (int loop), {
    var s = Module.glrAudio;
    if (s && s.audio) s.audio.loop = !!loop;
});

EM_JS(void, web_audio_js_seek, (double seconds), {
    var s = Module.glrAudio;
    if (!s || !s.audio) return;
    try { s.audio.currentTime = Math.max(0, seconds); } catch (e) {
        s.pendingSeek = Math.max(0, seconds);
    }
});

EM_JS(double, web_audio_js_current_time, (void), {
    var s = Module.glrAudio;
    if (!s || !s.audio || !Number.isFinite(s.audio.currentTime)) return 0;
    return s.audio.currentTime;
});

EM_JS(double, web_audio_js_duration, (void), {
    var s = Module.glrAudio;
    if (!s || !s.audio || !Number.isFinite(s.audio.duration)) return -1;
    return s.audio.duration;
});

EM_JS(int, web_audio_js_take_ended, (void), {
    var s = Module.glrAudio;
    if (!s || !s.ended) return 0;
    s.ended = false;
    return 1;
});

EM_JS(int, web_audio_js_take_error, (void), {
    var s = Module.glrAudio;
    if (!s || !s.error) return 0;
    s.error = false;
    return 1;
});

EM_JS(int, web_audio_js_load_state_track, (const char *key_ptr,
                                           char *track_ptr, int track_sz), {
    var key = UTF8ToString(key_ptr || 0);
    var track = "";
    try {
        var raw = key ? localStorage.getItem('gl-repl-audio:' + key) : null;
        var state = raw ? JSON.parse(raw) : null;
        if (state && typeof state.track === 'string') track = state.track;
    } catch (e) {}
    if (track_ptr && track_sz > 0) stringToUTF8(track, track_ptr, track_sz);
    return track ? 1 : 0;
});

EM_JS(double, web_audio_js_load_state_offset, (const char *key_ptr), {
    var key = UTF8ToString(key_ptr || 0);
    try {
        var raw = key ? localStorage.getItem('gl-repl-audio:' + key) : null;
        var state = raw ? JSON.parse(raw) : null;
        if (state && Number.isFinite(state.offset)) return state.offset;
    } catch (e) {}
    return 0;
});

EM_JS(int, web_audio_js_load_state_cfg, (const char *key_ptr), {
    var key = UTF8ToString(key_ptr || 0);
    try {
        var raw = key ? localStorage.getItem('gl-repl-audio:' + key) : null;
        var state = raw ? JSON.parse(raw) : null;
        if (state && Number.isFinite(state.cfg_mode)) return state.cfg_mode | 0;
    } catch (e) {}
    return -1;
});

EM_JS(void, web_audio_js_save_state, (const char *key_ptr,
                                      const char *track_ptr,
                                      double offset, int cfg_mode), {
    var key = UTF8ToString(key_ptr || 0);
    if (!key) return;
    var track = track_ptr ? UTF8ToString(track_ptr) : "";
    var state = {};
    if (track) {
        state.track = track;
        state.offset = Number.isFinite(offset) ? offset : 0;
    }
    if (cfg_mode >= 0) state.cfg_mode = cfg_mode;
    try {
        localStorage.setItem('gl-repl-audio:' + key, JSON.stringify(state));
    } catch (e) {}
});

static void reset_audio_module_state(void) {
    g_inited = 0;
    g_music_loaded = 0;
    g_muted = 0;
    g_paused = 0;
    g_gesture_done = 0;

    g_playlist_count = 0;
    g_playlist_pos = 0;
    for (int i = 0; i < GLR_AUDIO_MAX_TRACKS; i++) {
        g_playlist[i][0] = '\0';
        g_playlist_group[i][0] = '\0';
        g_playlist_display_name[i][0] = '\0';
        g_playlist_duration_secs[i] = -1.0f;
    }

    g_loop_mode = GLR_AUDIO_LOOP_ALL;
    g_pending_start = 0;
    g_pending_idx = 0;
    g_pending_seek = GLR_AUDIO_NO_SEEK;
    g_play_when_manifest_ready = 0;
    g_error_skip_count = 0;
    g_track_generation = 0;
    g_state_file[0] = '\0';
    g_cfg_mode = -1;
    g_last_save_time = 0;
}

static void audio_copy_string(char *dst, size_t dst_size,
                              const char *src, const char *fallback) {
    const char *s = (src && *src) ? src : fallback;
    size_t len;

    if (!dst || dst_size == 0)
        return;
    if (!s)
        s = "";
    len = strlen(s);
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, s, len);
    dst[len] = '\0';
}

static const char *audio_basename(const char *path) {
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

static void audio_derive_display_name(const char *path,
                                      char *out, size_t out_size) {
    const char *base = audio_basename(path);
    size_t len;

    if (!out || out_size == 0)
        return;
    len = strlen(base);
    if (len >= 4 &&
        base[len - 4] == '.' &&
        (base[len - 3] == 'm' || base[len - 3] == 'M') &&
        (base[len - 2] == 'p' || base[len - 2] == 'P') &&
        base[len - 1] == '3') {
        len -= 4;
    }
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, base, len);
    out[len] = '\0';
    if (!out[0])
        audio_copy_string(out, out_size, path, "(track)");
}

static int web_playlist_add(const GlrAudioTrackSpec *track) {
    int i;
    const char *p;
    size_t len;

    if (!track || !track->path || !track->path[0])
        return -1;
    if (g_playlist_count >= GLR_AUDIO_MAX_TRACKS)
        return -1;

    i = g_playlist_count;
    p = track->path;
    len = strlen(p);
    if (len >= GLR_AUDIO_MAX_PATH)
        len = GLR_AUDIO_MAX_PATH - 1;
    memcpy(g_playlist[i], p, len);
    g_playlist[i][len] = '\0';

    audio_copy_string(g_playlist_group[i], sizeof(g_playlist_group[i]),
                      track->group, "Music");
    if (track->display_name && track->display_name[0]) {
        audio_copy_string(g_playlist_display_name[i],
                          sizeof(g_playlist_display_name[i]),
                          track->display_name, NULL);
    } else {
        audio_derive_display_name(g_playlist[i],
                                  g_playlist_display_name[i],
                                  sizeof(g_playlist_display_name[i]));
    }
    g_playlist_duration_secs[i] = -1.0f;
    g_playlist_count++;
    return 0;
}

static int web_load_state(float *out_offset) {
    char saved_track[GLR_AUDIO_MAX_PATH] = "";
    int cfg_mode;

    if (out_offset)
        *out_offset = 0.0f;
    if (!g_state_file[0])
        return -1;

    cfg_mode = web_audio_js_load_state_cfg(g_state_file);
    if (cfg_mode >= 0)
        g_cfg_mode = cfg_mode;

    if (!web_audio_js_load_state_track(g_state_file, saved_track,
                                       (int)sizeof(saved_track)))
        return -1;
    if (out_offset) {
        double offset = web_audio_js_load_state_offset(g_state_file);
        *out_offset = (offset > 0.0) ? (float)offset : 0.0f;
    }

    for (int i = 0; i < g_playlist_count; i++) {
        if (strcmp(g_playlist[i], saved_track) == 0)
            return i;
    }
    return -1;
}

static void web_save_state(void) {
    const char *track = NULL;
    double offset = 0.0;

    if (!g_state_file[0])
        return;
    if (g_music_loaded &&
        g_playlist_pos >= 0 && g_playlist_pos < g_playlist_count) {
        track = g_playlist[g_playlist_pos];
        offset = web_audio_js_current_time();
    }
    web_audio_js_save_state(g_state_file, track, offset, g_cfg_mode);
    g_last_save_time = time(NULL);
}

static int web_start_track(int idx, float seek_secs) {
    double seek = (seek_secs >= 0.0f) ? (double)seek_secs : -1.0;

    if (!g_inited || idx < 0 || idx >= g_playlist_count)
        return -1;

    if (web_audio_js_start(g_playlist[idx],
                           g_loop_mode == GLR_AUDIO_LOOP_SONG,
                           g_paused, g_muted, seek) != 0)
        return -1;

    g_music_loaded = 1;
    g_playlist_pos = idx;
    g_track_generation++;
    g_error_skip_count = 0;
    return 0;
}

static int request_start(int idx, float seek_secs) {
    if (!g_inited || idx < 0 || idx >= g_playlist_count)
        return -1;

    if (!g_gesture_done) {
        g_pending_start = 1;
        g_pending_idx = idx;
        g_playlist_pos = idx;
        g_pending_seek = seek_secs;
        return 0;
    }
    return web_start_track(idx, seek_secs);
}

static void web_stop_current(void) {
    web_audio_js_set_paused(1);
    g_music_loaded = 0;
    g_pending_start = 0;
}

static void web_advance_after_end_or_error(int from_error) {
    int next;

    if (g_playlist_count <= 0) {
        web_stop_current();
        return;
    }

    if (from_error) {
        g_error_skip_count++;
        if (g_error_skip_count >= g_playlist_count) {
            web_stop_current();
            return;
        }
    } else {
        g_error_skip_count = 0;
    }

    next = g_playlist_pos + 1;
    if (next >= g_playlist_count) {
        if (g_loop_mode == GLR_AUDIO_LOOP_ALL)
            next = 0;
        else {
            web_stop_current();
            return;
        }
    }
    request_start(next, GLR_AUDIO_NO_SEEK);
}

static void web_manifest_finish_autoplay(void) {
    if (g_playlist_count <= 0)
        return;
    if (g_play_when_manifest_ready) {
        g_play_when_manifest_ready = 0;
        glr_audio_play_playlist();
    }
}

EMSCRIPTEN_KEEPALIVE int glr_audio_web_manifest_begin(int count) {
    (void)count;
    g_playlist_count = 0;
    g_playlist_pos = 0;
    g_music_loaded = 0;
    for (int i = 0; i < GLR_AUDIO_MAX_TRACKS; i++) {
        g_playlist[i][0] = '\0';
        g_playlist_group[i][0] = '\0';
        g_playlist_display_name[i][0] = '\0';
        g_playlist_duration_secs[i] = -1.0f;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE int glr_audio_web_manifest_add(const char *path,
                                                    const char *group,
                                                    const char *display_name) {
    GlrAudioTrackSpec track;
    track.path = path;
    track.group = group;
    track.display_name = display_name;
    return web_playlist_add(&track);
}

EMSCRIPTEN_KEEPALIVE int glr_audio_web_manifest_finish(void) {
    web_manifest_finish_autoplay();
    return g_playlist_count;
}

double glr_audio_hitch_threshold_ms_for_test(void) {
    return 0.0;
}

void glr_audio_set_hitch_log_elapsed_fn(GlrAudioElapsedSecondsFn fn) {
    (void)fn;
}

int glr_audio_init(void) {
    if (g_inited)
        return 0;
    if (web_audio_js_init() != 0)
        return -1;
    g_inited = 1;
    web_audio_js_request_manifest();
    return 0;
}

void glr_audio_shutdown(void) {
    if (!g_inited)
        return;
    web_save_state();
    web_audio_js_shutdown();
    reset_audio_module_state();
}

int glr_audio_set_playlist(const char *const *paths, int count) {
    if (!paths || count < 0) return -1;

    GlrAudioTrackSpec specs[GLR_AUDIO_MAX_TRACKS];
    int n = count;
    if (n > GLR_AUDIO_MAX_TRACKS)
        n = GLR_AUDIO_MAX_TRACKS;
    for (int i = 0; i < n; i++) {
        specs[i].path = paths[i];
        specs[i].group = "Music";
        specs[i].display_name = NULL;
    }
    return glr_audio_set_playlist_specs(specs, count);
}

int glr_audio_set_playlist_specs(const GlrAudioTrackSpec *tracks, int count) {
    int n;

    if (!tracks || count < 0) return -1;

    web_stop_current();
    g_playlist_count = 0;
    g_playlist_pos = 0;
    g_pending_start = 0;

    n = count;
    if (n > GLR_AUDIO_MAX_TRACKS) {
        fprintf(stderr,
                "repl_audio: playlist has %d tracks; truncating to %d\n",
                count, GLR_AUDIO_MAX_TRACKS);
        n = GLR_AUDIO_MAX_TRACKS;
    }

    for (int i = 0; i < n; i++)
        web_playlist_add(&tracks[i]);
    for (int i = g_playlist_count; i < GLR_AUDIO_MAX_TRACKS; i++) {
        g_playlist[i][0] = '\0';
        g_playlist_group[i][0] = '\0';
        g_playlist_display_name[i][0] = '\0';
        g_playlist_duration_secs[i] = -1.0f;
    }

    web_manifest_finish_autoplay();
    return g_playlist_count;
}

int glr_audio_play_playlist(void) {
    float offset = 0.0f;
    int idx;

    if (!g_inited)
        return -1;
    idx = web_load_state(&offset);
    if (g_playlist_count == 0) {
        g_play_when_manifest_ready = 1;
        return 0;
    }
    if (idx >= 0)
        return request_start(idx, offset);
    return request_start(0, GLR_AUDIO_NO_SEEK);
}

int glr_audio_play_music(const char *path) {
    if (!path || !*path) return -1;
    const char *arr[1] = { path };
    if (glr_audio_set_playlist(arr, 1) < 0) return -1;
    return glr_audio_play_playlist();
}

int glr_audio_next_track(void) {
    int next;

    if (!g_inited || g_playlist_count == 0) return -1;
    next = g_playlist_pos + 1;
    if (next >= g_playlist_count) next = 0;
    return request_start(next, GLR_AUDIO_NO_SEEK);
}

int glr_audio_prev_track(void) {
    int prev;

    if (!g_inited || g_playlist_count == 0) return -1;
    prev = g_playlist_pos - 1;
    if (prev < 0) prev = g_playlist_count - 1;
    return request_start(prev, GLR_AUDIO_NO_SEEK);
}

int glr_audio_play_track_index(int idx) {
    if (!g_inited || idx < 0 || idx >= g_playlist_count) return -1;
    return request_start(idx, GLR_AUDIO_NO_SEEK);
}

int glr_audio_seek(float seek_secs) {
    if (!g_inited)
        return -1;
    if (seek_secs < 0.0f)
        seek_secs = 0.0f;
    if (g_music_loaded) {
        int idx = g_playlist_pos;
        float dur = g_playlist_duration_secs[idx];
        if (dur >= 0.0f && seek_secs > dur)
            seek_secs = dur;
        web_audio_js_seek((double)seek_secs);
        return 0;
    }
    if (g_pending_start) {
        g_pending_seek = seek_secs;
        return 0;
    }
    return -1;
}

int glr_audio_seek_relative(float offset_secs) {
    float current = 0.0f;

    if (!g_inited)
        return -1;
    if (g_music_loaded)
        current = (float)web_audio_js_current_time();
    else if (g_pending_start)
        current = (g_pending_seek >= 0.0f) ? g_pending_seek : 0.0f;
    else
        return -1;
    return glr_audio_seek(current + offset_secs);
}

void glr_audio_tick(void) {
    if (!g_inited)
        return;

    if (g_music_loaded &&
        g_playlist_pos >= 0 && g_playlist_pos < g_playlist_count) {
        double dur = web_audio_js_duration();
        if (dur >= 0.0)
            g_playlist_duration_secs[g_playlist_pos] = (float)dur;
    }

    if (g_state_file[0] && g_music_loaded) {
        time_t now = time(NULL);
        if (difftime(now, g_last_save_time) >= STATE_SAVE_INTERVAL_SECS)
            web_save_state();
    }

    if (web_audio_js_take_error()) {
        web_advance_after_end_or_error(1);
    } else if (g_loop_mode != GLR_AUDIO_LOOP_SONG &&
               web_audio_js_take_ended()) {
        web_advance_after_end_or_error(0);
    }
}

void glr_audio_set_loop_mode(int mode) {
    if (mode < GLR_AUDIO_LOOP_OFF)  mode = GLR_AUDIO_LOOP_OFF;
    if (mode > GLR_AUDIO_LOOP_ALL)  mode = GLR_AUDIO_LOOP_ALL;
    g_loop_mode = mode;
    web_audio_js_set_loop(g_loop_mode == GLR_AUDIO_LOOP_SONG);
}

int glr_audio_get_loop_mode(void) {
    return g_loop_mode;
}

void glr_audio_set_paused(int paused) {
    g_paused = paused ? 1 : 0;
    if (g_music_loaded && !g_pending_start)
        web_audio_js_set_paused(g_paused);
}

int glr_audio_is_paused(void) {
    return g_paused;
}

void glr_audio_set_muted(int muted) {
    g_muted = muted ? 1 : 0;
    web_audio_js_set_muted(g_muted);
}

int glr_audio_is_muted(void) {
    return g_muted;
}

const char *glr_audio_get_current_track(void) {
    if (!g_music_loaded ||
        g_playlist_pos < 0 || g_playlist_pos >= g_playlist_count)
        return NULL;
    return g_playlist[g_playlist_pos];
}

int glr_audio_track_count(void) {
    return g_playlist_count;
}

const char *glr_audio_track_display_name(int idx) {
    if (idx < 0 || idx >= g_playlist_count)
        return NULL;
    return g_playlist_display_name[idx];
}

const char *glr_audio_track_group(int idx) {
    if (idx < 0 || idx >= g_playlist_count)
        return NULL;
    return g_playlist_group[idx];
}

int glr_audio_current_index(void) {
    if (g_music_loaded &&
        g_playlist_pos >= 0 && g_playlist_pos < g_playlist_count)
        return g_playlist_pos;
    return -1;
}

float glr_audio_current_cursor_seconds(void) {
    return g_music_loaded ? (float)web_audio_js_current_time() : 0.0f;
}

float glr_audio_track_duration_seconds(int idx) {
    if (idx < 0 || idx >= g_playlist_count)
        return -1.0f;
    if (idx == g_playlist_pos && g_music_loaded) {
        double dur = web_audio_js_duration();
        if (dur >= 0.0)
            g_playlist_duration_secs[idx] = (float)dur;
    }
    return g_playlist_duration_secs[idx];
}

unsigned int glr_audio_track_generation(void) {
    return g_track_generation;
}

void glr_audio_set_state_file(const char *path) {
    if (!path) {
        g_state_file[0] = '\0';
    } else {
        size_t len = strlen(path);
        if (len >= GLR_AUDIO_MAX_PATH) len = GLR_AUDIO_MAX_PATH - 1;
        memcpy(g_state_file, path, len);
        g_state_file[len] = '\0';
    }
    g_last_save_time = 0;
}

void glr_audio_on_user_gesture(void) {
    if (!g_inited || g_gesture_done)
        return;
    g_gesture_done = 1;
    if (g_pending_start) {
        int idx = g_pending_idx;
        float seek = g_pending_seek;
        g_pending_start = 0;
        g_pending_seek = GLR_AUDIO_NO_SEEK;
        request_start(idx, seek);
    }
}

void glr_audio_set_cfg_mode(int mode) {
    g_cfg_mode = mode;
}

int glr_audio_get_cfg_mode(void) {
    return g_cfg_mode;
}

#else  /* !__EMSCRIPTEN__ */

/* Threading model selection. Default: use the background worker thread.
 * Auto-disabled on Emscripten; overridable on any platform by defining
 * GLR_AUDIO_NO_THREAD on the command line (0 forces the thread on). */
#ifndef GLR_AUDIO_NO_THREAD
#  if defined(__EMSCRIPTEN__)
#    define GLR_AUDIO_NO_THREAD 1
#  else
#    define GLR_AUDIO_NO_THREAD 0
#  endif
#endif

#if !GLR_AUDIO_NO_THREAD
#  include <pthread.h>
#endif

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
#define STATE_SAVE_INTERVAL_SECS 5

static ma_engine g_engine;

/* Double-buffered sound storage. ma_sound objects register internal
 * nodes in the engine graph and cannot be relocated after init, so we
 * keep two fixed slots: the worker loads into the inactive one and
 * atomically flips g_active under g_mtx. g_active == -1 means nothing
 * is loaded. */
static ma_sound g_slot[2];
/* worker-thread-only: read/written only from audio_worker_main and its
 * helpers (worker_uninit_all, worker_load); no lock required. The render
 * thread never observes this array directly. */
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
static char g_playlist_group[GLR_AUDIO_MAX_TRACKS][64];
static char g_playlist_display_name[GLR_AUDIO_MAX_TRACKS][128];
static float g_playlist_duration_secs[GLR_AUDIO_MAX_TRACKS];
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

#define GLR_AUDIO_NO_SEEK (-1.0f)

/* Deferred-start flag: if play_playlist() / play_music() is called
 * before the first user gesture on the web, remember which track was
 * requested so we can start it on the first gesture. */
static int g_pending_start = 0;
static float g_pending_seek = GLR_AUDIO_NO_SEEK;

/* Bumped on every successful track start. Callers poll this to notice
 * that the current track has changed without needing a callback. */
static unsigned int g_track_generation = 0;

/* Flag set to cancel a load in-flight when playlist is reset. */
static int g_load_cancelled = 0;

/* Seek requested while a worker load is in flight (g_active still -1).
 * Recorded by glr_audio_seek() and picked up by worker_load() just before
 * it publishes the slot, so a seek issued mid-load isn't dropped.
 * GLR_AUDIO_NO_SEEK means "no pending mid-load seek". */
static float g_load_seek = GLR_AUDIO_NO_SEEK;

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

#if !GLR_AUDIO_NO_THREAD
static pthread_t       g_worker;
static int             g_worker_running = 0;
static pthread_mutex_t g_mtx;
static pthread_cond_t  g_cv;
#endif

static AudioWorkerReq  g_req      = AWR_NONE;  /* latest lifecycle request */
static int             g_req_idx  = 0;
static float           g_req_seek = GLR_AUDIO_NO_SEEK;
static int             g_req_save = 0;         /* independent: periodic save */

static GlrAudioElapsedSecondsFn g_hitch_log_elapsed_fn = NULL;
static ma_log g_miniaudio_log;
static int    g_miniaudio_log_inited = 0;
static ma_uint32 g_miniaudio_log_max_level = MA_LOG_LEVEL_WARNING;

/* ------------------------------------------------------------------ */
/* State persistence (track + offset + audio cfg across restarts)      */
/* ------------------------------------------------------------------ */

/* Path of the INI file, or empty string when disabled. */
static char  g_state_file[GLR_AUDIO_MAX_PATH] = "";

/* Opaque integer owned by glr_actions.c (maps to AUDIO_CFG_* enum).
 * Stored in the INI as cfg_mode= and handed back through
 * glr_audio_get_cfg_mode() so glr_actions_apply_defaults() can map it
 * to paused/loop state. -1 means "not yet loaded / not set". */
static int g_cfg_mode = -1;

/* Timestamp of the last successful state-file write. */
static time_t g_last_save_time = 0;

static void reset_audio_module_state(void) {
    g_slot_inited[0] = 0;
    g_slot_inited[1] = 0;
    g_active = -1;

    g_inited = 0;
    g_music_loaded = 0;
    g_loading = 0;
    g_muted = 0;
    g_paused = 0;
    g_gesture_done = 0;

    g_playlist_count = 0;
    g_playlist_pos = 0;
    for (int i = 0; i < GLR_AUDIO_MAX_TRACKS; i++) {
        g_playlist[i][0] = '\0';
        g_playlist_group[i][0] = '\0';
        g_playlist_display_name[i][0] = '\0';
        g_playlist_duration_secs[i] = -1.0f;
    }
    g_loop_mode = GLR_AUDIO_LOOP_ALL;

    g_pending_start = 0;
    g_pending_seek = GLR_AUDIO_NO_SEEK;
    g_track_generation = 0;
    g_load_cancelled = 0;
    g_load_seek = GLR_AUDIO_NO_SEEK;

#if !GLR_AUDIO_NO_THREAD
    g_worker_running = 0;
#endif
    g_req = AWR_NONE;
    g_req_idx = 0;
    g_req_seek = GLR_AUDIO_NO_SEEK;
    g_req_save = 0;

    g_state_file[0] = '\0';
    g_cfg_mode = -1;
    g_last_save_time = 0;
}

static void audio_copy_string(char *dst, size_t dst_size,
                              const char *src, const char *fallback) {
    const char *s = (src && *src) ? src : fallback;
    size_t len;

    if (!dst || dst_size == 0)
        return;
    if (!s)
        s = "";
    len = strlen(s);
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, s, len);
    dst[len] = '\0';
}

static const char *audio_basename(const char *path) {
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

static void audio_derive_display_name(const char *path,
                                      char *out, size_t out_size) {
    const char *base = audio_basename(path);
    size_t len;

    if (!out || out_size == 0)
        return;
    len = strlen(base);
    if (len >= 4 &&
        base[len - 4] == '.' &&
        (base[len - 3] == 'm' || base[len - 3] == 'M') &&
        (base[len - 2] == 'p' || base[len - 2] == 'P') &&
        base[len - 1] == '3') {
        len -= 4;
    }
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, base, len);
    out[len] = '\0';
    if (!out[0])
        audio_copy_string(out, out_size, path, "(track)");
}

/* ------------------------------------------------------------------ */
/* Small lock helpers                                                  */
/* ------------------------------------------------------------------ */

/* Lifetime-guarding lock helpers for the audio subsystem.
 * Guard access to all static module state to ensure thread safety
 * between the render/main thread and the background worker thread.
 *
 * NOTE ON LIFETIME DISCIPLINE AND MINIAUDIO-UNDER-LOCK RISK:
 * To avoid use-after-uninit hazards between the render thread and the
 * worker thread's off-lock uninit paths (e.g. worker_load and worker_uninit_all),
 * all active-slot miniaudio control operations (ma_sound_start, ma_sound_stop,
 * ma_sound_set_looping, etc.) are explicitly kept under the audio_lock() guard
 * to protect g_slot[] lifetime. While calling miniaudio functions under a lock
 * carries a theoretical risk of deadlock if miniaudio internally takes locks
 * shared by the device callback thread, this module-level serialization is
 * accepted here to maintain solid lifetime correctness for double-buffered slots.
 * The recursive g_mtx mutex is a dynamically-initialized implementation detail
 * to simplify nested control calls, not a correctness guarantee for lock ordering.
 *
 * In the single-threaded build (GLR_AUDIO_NO_THREAD) there is no second
 * thread to serialize against, so the lock helpers and the worker-wake
 * signal collapse to no-ops. */
#if GLR_AUDIO_NO_THREAD
static void audio_lock(void)   { }
static void audio_unlock(void) { }
static void worker_wake(void)  { }
#else
static void audio_lock(void)   { if (g_inited) pthread_mutex_lock(&g_mtx); }
static void audio_unlock(void) { if (g_inited) pthread_mutex_unlock(&g_mtx); }
/* Wake the worker. Callers hold the mutex (recommended for cond_signal). */
static void worker_wake(void)  { pthread_cond_signal(&g_cv); }
#endif

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

static double g_worker_hitch_threshold = -1.0;

static double worker_hitch_threshold_ms(void) {
    if (g_worker_hitch_threshold < 0.0) {
        const char *env = getenv("GLR_AUDIO_HITCH_MS");
        g_worker_hitch_threshold = (env && *env) ? atof(env) : 50.0;
        if (g_worker_hitch_threshold < 0.0) g_worker_hitch_threshold = 0.0;
    }
    return g_worker_hitch_threshold;
}

double glr_audio_hitch_threshold_ms_for_test(void) {
    return worker_hitch_threshold_ms();
}

static int audio_no_device_requested(void) {
    const char *env = getenv("GLR_AUDIO_NO_DEVICE");
    return env && *env;
}

static ma_uint32 miniaudio_log_level_from_env(const char *env) {
    if (!env || !*env)
        return MA_LOG_LEVEL_WARNING;
    if (strcmp(env, "debug") == 0 || strcmp(env, "all") == 0 ||
        strcmp(env, "1") == 0)
        return MA_LOG_LEVEL_DEBUG;
    if (strcmp(env, "info") == 0)
        return MA_LOG_LEVEL_INFO;
    if (strcmp(env, "error") == 0)
        return MA_LOG_LEVEL_ERROR;
    return MA_LOG_LEVEL_WARNING;
}

static void miniaudio_log_stderr(void *user, ma_uint32 level,
                                 const char *message) {
    size_t len;

    (void)user;
    if (level > g_miniaudio_log_max_level)
        return;

    if (!message)
        message = "";

    if (g_hitch_log_elapsed_fn)
        fprintf(stderr, "[init +%6.3fs] ", g_hitch_log_elapsed_fn());
    fprintf(stderr, "repl_audio: miniaudio %s: %s",
            ma_log_level_to_string(level), message);

    len = strlen(message);
    if (len == 0 || message[len - 1] != '\n')
        fputc('\n', stderr);
}

static int miniaudio_log_init_for_engine(void) {
    ma_result r;

    if (g_miniaudio_log_inited)
        return 0;

    {
        const char *env = getenv("GLR_MINIAUDIO_LOG");
        if (env && (strcmp(env, "0") == 0 || strcmp(env, "off") == 0 ||
                    strcmp(env, "none") == 0))
            return -1;
        g_miniaudio_log_max_level = miniaudio_log_level_from_env(env);
    }

    r = ma_log_init(NULL, &g_miniaudio_log);
    if (r != MA_SUCCESS)
        return -1;

    r = ma_log_register_callback(
        &g_miniaudio_log,
        ma_log_callback_init(miniaudio_log_stderr, NULL));
    if (r != MA_SUCCESS) {
        ma_log_uninit(&g_miniaudio_log);
        return -1;
    }

    g_miniaudio_log_inited = 1;
    return 0;
}

static void miniaudio_log_uninit_for_engine(void) {
    if (!g_miniaudio_log_inited)
        return;
    ma_log_uninit(&g_miniaudio_log);
    g_miniaudio_log_inited = 0;
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

/* Helper to parse a single key=value line from the audio state INI file. */
static void parse_ini_line(const char *line, char *out_track, float *out_offset, int *out_cfg_mode) {
    if (strncmp(line, "track=", 6) == 0) {
        if (out_track) {
            const char *p = line + 6;
            size_t len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) len--;
            if (len >= GLR_AUDIO_MAX_PATH) len = GLR_AUDIO_MAX_PATH - 1;
            memcpy(out_track, p, len);
            out_track[len] = '\0';
        }
    } else if (strncmp(line, "offset=", 7) == 0) {
        if (out_offset) {
            *out_offset = (float)atof(line + 7);
        }
    } else if (strncmp(line, "cfg_mode=", 9) == 0) {
        if (out_cfg_mode) {
            *out_cfg_mode = atoi(line + 9);
        }
    }
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

    audio_lock();
    if (!g_state_file[0]) { audio_unlock(); return; }
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
    audio_unlock();

    char tmp[GLR_AUDIO_MAX_PATH];
    const char *last_slash = strrchr(state_file, '/');
    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - state_file) + 1;
        if (dir_len >= sizeof(tmp)) return;
        memcpy(tmp, state_file, dir_len);
        if (snprintf(tmp + dir_len, sizeof(tmp) - dir_len, ".%s.tmp", last_slash + 1) >= (int)(sizeof(tmp) - dir_len))
            return;
    } else {
        if (snprintf(tmp, sizeof(tmp), ".%s.tmp", state_file) >= (int)sizeof(tmp))
            return;
    }

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
                parse_ini_line(line, saved_track, &saved_offset, NULL);
            }
            fclose(existing);
            if (saved_track[0]) {
                fprintf(f, "track=%s\n", saved_track);
                fprintf(f, "offset=%.3f\n", saved_offset);
            }
        }
    }
    /* cfg_mode is the authoritative audio preference: encodes pause/loop
     * policy in a single int owned by glr_actions.c (AUDIO_CFG_* enum).
     * Only write it when the action layer has registered a value. */
    if (cfg_mode >= 0)
        fprintf(f, "cfg_mode=%d\n", cfg_mode);

    if (fflush(f) != 0 || fclose(f) != 0) {
        remove(tmp);
        return;
    }

    if (rename(tmp, state_file) != 0) {
        remove(tmp);  /* rename failed - discard temp, old file untouched */
    } else {
        audio_lock();
        g_last_save_time = time(NULL);
        audio_unlock();
    }
}

/* Loads persisted playback position and audio cfg from the state file.
 * Synchronous on the caller thread: the INI is a small app-owned file
 * (never a media file), and glr_audio_play_playlist()'s cfg_mode side
 * effect must be visible to the immediately-following caller. cfg_mode
 * is stored in g_cfg_mode for glr_actions.c to pick up via
 * glr_audio_get_cfg_mode(). Returns the playlist index of the saved track,
 * or -1 on failure. Sets *out_offset to the saved cursor in seconds. */
static int load_state(float *out_offset) {
    char state_file[GLR_AUDIO_MAX_PATH];

    *out_offset = 0.0f;
    audio_lock();
    memcpy(state_file, g_state_file, sizeof(state_file));
    audio_unlock();

    if (!state_file[0]) return -1;
    FILE *f = fopen(state_file, "r");
    if (!f) return -1;

    char saved_track[GLR_AUDIO_MAX_PATH] = "";
    float offset  = 0.0f;
    int cfg_mode  = -1;   /* -1 = not found in file */
    char line[GLR_AUDIO_MAX_PATH + 16];

    while (fgets(line, (int)sizeof(line), f)) {
        parse_ini_line(line, saved_track, &offset, &cfg_mode);
    }
    fclose(f);

    /* Store cfg_mode for the editor to retrieve and apply. */
    if (cfg_mode >= 0) {
        audio_lock();
        g_cfg_mode = cfg_mode;
        audio_unlock();
    }

    if (!saved_track[0]) return -1;
    audio_lock();
    for (int i = 0; i < g_playlist_count; i++) {
        if (strcmp(g_playlist[i], saved_track) == 0) {
            *out_offset = (offset > 0.0f ? offset : 0.0f);
            audio_unlock();
            return i;
        }
    }
    audio_unlock();
    return -1;  /* track not found in current playlist - start fresh */
}

/* ------------------------------------------------------------------ */
/* Worker-side sound lifecycle                                         */
/* ------------------------------------------------------------------ */

/* Worker-only. Uninit every inited slot and mark nothing active. The
 * ma_sound_uninit of a stream can block on pending page reads; that's
 * fine here — it is the worker thread, never the render thread. */
static void worker_uninit_all(void) {
    audio_lock();
    g_active = -1;
    g_music_loaded = 0;
    audio_unlock();
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
    float duration_secs = -1.0f;

    audio_lock();
    if (idx < 0 || idx >= g_playlist_count) { audio_unlock(); return -1; }
    memcpy(path, g_playlist[idx], sizeof(path));
    old       = g_active;
    target    = (g_active < 0) ? 0 : (1 - g_active);
    loop_song = (g_loop_mode == GLR_AUDIO_LOOP_SONG);
    paused    = g_paused;
    g_loading = 1;
    g_load_cancelled = 0;
    /* g_load_seek is reset at dequeue time (audio_dequeue_and_run), not
     * here: a seek arriving between dequeue and this point must survive. */
    audio_unlock();

    if (g_slot_inited[target]) {           /* stale (failed prior load) */
        ma_sound_uninit(&g_slot[target]);
        g_slot_inited[target] = 0;
    }

    /* Pre-flight the path with a plain fopen before handing it to
     * miniaudio. A streamed sound (MA_SOUND_FLAG_STREAM) that fails to
     * open triggers a heap-use-after-free *inside miniaudio*:
     * ma_sound_init_from_file_internal ma_free()s the resource-manager
     * data-stream object on the failure path while the resource manager's
     * own job thread is still processing the queued load_data_stream job
     * for it (ma_job_process__resource_manager__load_data_stream writes
     * into the freed region). The fallback walk hits missing files
     * routinely (a saved resume track that was deleted, a stale playlist),
     * so this is a live crash, not just a test artifact. Refusing to open
     * an unreadable path sidesteps miniaudio's buggy failed-stream
     * teardown entirely; the caller's fallback walk then moves on to the
     * next entry exactly as it would on an ma_sound_init_from_file error. */
    {
        FILE *probe = fopen(path, "rb");
        if (!probe) {
            fprintf(stderr,
                    "repl_audio: cannot open \"%s\" for streaming (skipping)\n",
                    path);
            audio_lock(); g_loading = 0; audio_unlock();
            return -1;
        }
        fclose(probe);
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
        audio_lock(); g_loading = 0; audio_unlock();
        return -1;
    }

    {
        ma_uint64 length_frames = 0;
        ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
        if (sr > 0 &&
            ma_sound_get_length_in_pcm_frames(&g_slot[target],
                                              &length_frames) == MA_SUCCESS) {
            duration_secs = (float)length_frames / (float)sr;
        }
    }

    ma_sound_set_looping(&g_slot[target], loop_song ? MA_TRUE : MA_FALSE);

    r = ma_sound_start(&g_slot[target]);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "repl_audio: ma_sound_start(\"%s\") failed: %d\n",
                path, (int)r);
        ma_sound_uninit(&g_slot[target]);
        audio_lock(); g_loading = 0; audio_unlock();
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

    float mid_load_seek;
    audio_lock();
    if (g_load_cancelled) {
        g_loading = 0;
        g_load_cancelled = 0;
        audio_unlock();
        ma_sound_uninit(&g_slot[target]);
        return -1;
    }
    g_slot_inited[target] = 1;
    g_active              = target;
    g_music_loaded        = 1;
    g_playlist_pos        = idx;
    g_playlist_duration_secs[idx] = duration_secs;
    g_track_generation++;
    /* A seek that arrived while g_loading was set recorded itself in
     * g_load_seek rather than touching the not-yet-published slot. Clear
     * g_loading atomically with capturing it so a seek racing right here
     * either lands in g_load_seek (applied below) or, once g_loading is 0
     * and g_active is set, takes the direct-seek path — no lost-seek gap. */
    mid_load_seek = g_load_seek;
    g_load_seek   = GLR_AUDIO_NO_SEEK;
    g_loading             = 0;
    audio_unlock();

    if (mid_load_seek >= 0.0f) {
        ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
        if (sr > 0)
            ma_sound_seek_to_pcm_frame(
                &g_slot[target], (ma_uint64)(mid_load_seek * (float)sr));
    }

    /* Retire the previous slot now that the new one is live. Off-lock:
     * stream teardown may block, but only the worker is here. */
    if (old >= 0 && old != target && g_slot_inited[old]) {
        ma_sound_uninit(&g_slot[old]);
        g_slot_inited[old] = 0;
    }
    return 0;
}

/* Worker-only. Start at `idx`, and if that file fails to open, walk
 * forward through the rest of the playlist once looking for the next
 * playable entry. Only the originally-requested track inherits the
 * saved seek offset; fallback tracks start from the top. Retries abort
 * if a newer request supersedes this one or the playlist changes under
 * us. */
static int worker_start_or_fallback(int idx, float seek_secs) {
    int count;

    audio_lock();
    count = g_playlist_count;
    audio_unlock();

    if (idx < 0 || idx >= count)
        return -1;

    for (int attempt = 0; attempt < count; attempt++) {
        if (attempt > 0) {
            int superseded;
            int cur_count;

            audio_lock();
            superseded = (g_req != AWR_NONE) || g_load_cancelled;
            cur_count = g_playlist_count;
            audio_unlock();

            if (superseded || cur_count != count)
                return -1;
        }

        {
            int cur = idx + attempt;
            if (cur >= count)
                cur -= count;
            if (worker_load(cur, attempt == 0 ? seek_secs : GLR_AUDIO_NO_SEEK) == 0)
                return 0;
        }
    }

    return -1;
}

/* Worker-only. The current track ended; advance per loop mode. Caps
 * attempts at playlist_count so a folder of broken files can't spin
 * forever (the skip-broken loop migrated here from glr_audio_tick so it
 * no longer runs on the render thread). */
static void worker_advance(void) {
    int pos, count, mode;
    audio_lock();
    pos   = g_playlist_pos;
    count = g_playlist_count;
    mode  = g_loop_mode;
    audio_unlock();

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
        if (worker_load(next, GLR_AUDIO_NO_SEEK) == 0)
            return;
        pos = next;   /* broken track - skip past it */
        attempts++;
    }
    worker_uninit_all();   /* all remaining tracks failed */
}

/* Run a single already-dequeued lifecycle request (the caller cleared
 * the mailbox under the lock). Times the blocking work and logs a hitch
 * over the threshold. Returns the kind run; AWR_QUIT tells the worker
 * loop to exit. Shared by the threaded worker and the single-threaded
 * service path. */
static AudioWorkerReq audio_run_request(AudioWorkerReq k, int idx,
                                        float seek, int save) {
    if (k == AWR_QUIT) {
        worker_save_state();
        worker_uninit_all();
        return AWR_QUIT;
    }

    double t0 = worker_now_ms();
    const char *op = "save";
    if (k == AWR_START) {
        op = "load";
        if (worker_start_or_fallback(idx, seek) != 0) {
            /* No track was published (bad index, or every candidate failed
             * to open). g_loading was claimed at dequeue time; a failing
             * worker_load clears it, but the early bad-index return in
             * worker_start_or_fallback doesn't run worker_load at all, so
             * clear it here to guarantee the flag is resolved. */
            audio_lock(); g_loading = 0; audio_unlock();
        }
    }
    else if (k == AWR_ADVANCE) { op = "advance"; worker_advance(); }
    else if (k == AWR_UNINIT)  { op = "uninit";  worker_uninit_all(); }

    if (save)
        worker_save_state();    /* after the lifecycle op: fresh cursor */

    double thr = worker_hitch_threshold_ms();
    double dt  = worker_now_ms() - t0;
    if (thr > 0.0 && dt >= thr) {
        const char *save_suffix = (save && k != AWR_NONE) ? "+save" : "";
        if (g_hitch_log_elapsed_fn) {
            fprintf(stderr,
                    "[init +%6.3fs] repl_audio: worker hitch: %s%s took %.1f ms "
                    "(threshold %.0f ms)\n",
                    g_hitch_log_elapsed_fn(), op, save_suffix, dt, thr);
        } else {
            fprintf(stderr,
                    "repl_audio: worker hitch: %s%s took %.1f ms "
                    "(threshold %.0f ms)\n",
                    op, save_suffix, dt, thr);
        }
    }
    return k;
}

/* Snapshot + clear the 1-slot mailbox under the lock, then run the
 * request. Returns AWR_NONE when nothing was pending. The mailbox is
 * latest-wins, so a request posted during the brief unlocked handoff is
 * intentionally allowed to supersede this one. */
static AudioWorkerReq audio_dequeue_and_run(void) {
    audio_lock();
    AudioWorkerReq k    = g_req;
    int            idx  = g_req_idx;
    float          seek = g_req_seek;
    int            save = g_req_save;
    g_req      = AWR_NONE;
    g_req_save = 0;
    /* For a start request, mark "load in flight" the instant we clear the
     * request, before releasing the lock. worker_load() sets g_loading
     * again once it starts, but that leaves a gap here (g_req cleared,
     * g_loading not yet set) in which a concurrent glr_audio_seek() would
     * see neither AWR_START nor g_loading and drop the seek. Claiming
     * g_loading now, atomically with clearing g_req, closes that gap;
     * audio_run_request clears it if the start ultimately publishes no
     * track. Reset g_load_seek here too (not in worker_load) so a seek that
     * arrives in this window isn't wiped by a later reset.
     *
     * AWR_ADVANCE is deliberately excluded: worker_advance may end the
     * playlist without ever calling worker_load, which would strand
     * g_loading at 1; its own worker_load sets the flag when it does load. */
    if (k == AWR_START) {
        g_loading   = 1;
        g_load_seek = GLR_AUDIO_NO_SEEK;
    }
    audio_unlock();

    if (k == AWR_NONE && !save)
        return AWR_NONE;
    return audio_run_request(k, idx, seek, save);
}

#if !GLR_AUDIO_NO_THREAD
static void *audio_worker_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mtx);
        while (g_req == AWR_NONE && !g_req_save)
            pthread_cond_wait(&g_cv, &g_mtx);
        pthread_mutex_unlock(&g_mtx);

        if (audio_dequeue_and_run() == AWR_QUIT)
            return NULL;
    }
}
#endif

/* True when lifecycle requests can be serviced: the worker thread is up
 * (threaded build) or the engine is initialized and tick() will drain
 * the mailbox (single-threaded build). Both coincide with g_inited once
 * glr_audio_init() has returned successfully. */
static int audio_dispatch_active(void) {
    return g_inited;
}

/* Post a lifecycle request (latest-wins: a newer request supersedes a
 * queued one). No-op when audio isn't running. In the threaded build the
 * worker picks it up immediately; in the single-threaded build it waits
 * in the mailbox until the next glr_audio_tick() drains it. */
static void worker_post(AudioWorkerReq kind, int idx, float seek) {
    if (!audio_dispatch_active()) return;
    audio_lock();
    if (g_req != AWR_QUIT) {
        g_req      = kind;
        g_req_idx  = idx;
        g_req_seek = seek;
        worker_wake();
    }
    audio_unlock();
}

/* ------------------------------------------------------------------ */
/* Deferred-gesture-aware start                                         */
/* ------------------------------------------------------------------ */

/* Request a track start. On the web this is deferred until the first
 * user gesture (browser autoplay policy); on native it posts straight
 * to the worker. */
static int request_start(int idx, float seek) {
    audio_lock();
    if (!g_inited) {
        audio_unlock();
        return -1;
    }
    if (idx < 0 || idx >= g_playlist_count) {
        audio_unlock();
        return -1;
    }

#if AUDIO_NEEDS_GESTURE
    if (!g_gesture_done) {
        g_pending_start = 1;
        g_playlist_pos  = idx;   /* remember which track for the gesture */
        g_pending_seek  = seek;
        audio_unlock();
        return 0;
    }
#endif
    audio_unlock();
    worker_post(AWR_START, idx, seek);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int glr_audio_init(void) {
    ma_engine_config engine_config;
    const ma_engine_config *engine_config_ptr = NULL;
    ma_result r;

    if (g_inited) return 0;

    if (miniaudio_log_init_for_engine() == 0) {
        engine_config = ma_engine_config_init();
        engine_config.pLog = &g_miniaudio_log;
        engine_config_ptr = &engine_config;
    }

    if (audio_no_device_requested()) {
        if (!engine_config_ptr) {
            engine_config = ma_engine_config_init();
            engine_config_ptr = &engine_config;
        }
        engine_config.noDevice = MA_TRUE;
        engine_config.channels = 2;
        engine_config.sampleRate = 48000;
    }

    r = ma_engine_init(engine_config_ptr, &g_engine);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "repl_audio: ma_engine_init failed: %d\n", (int)r);
        miniaudio_log_uninit_for_engine();
        return -1;
    }

    g_active = -1;
    g_slot_inited[0] = g_slot_inited[1] = 0;

    g_req      = AWR_NONE;
    g_req_save = 0;

#if !GLR_AUDIO_NO_THREAD
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        ma_engine_uninit(&g_engine);
        miniaudio_log_uninit_for_engine();
        return -1;
    }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(&g_mtx, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        ma_engine_uninit(&g_engine);
        miniaudio_log_uninit_for_engine();
        return -1;
    }
    pthread_mutexattr_destroy(&attr);

    if (pthread_cond_init(&g_cv, NULL) != 0) {
        pthread_mutex_destroy(&g_mtx);
        ma_engine_uninit(&g_engine);
        miniaudio_log_uninit_for_engine();
        return -1;
    }

    if (pthread_create(&g_worker, NULL, audio_worker_main, NULL) != 0) {
        fprintf(stderr, "repl_audio: worker thread create failed\n");
        pthread_cond_destroy(&g_cv);
        pthread_mutex_destroy(&g_mtx);
        ma_engine_uninit(&g_engine);
        miniaudio_log_uninit_for_engine();
        return -1;
    }

    g_worker_running = 1;
#endif  /* !GLR_AUDIO_NO_THREAD */

#if AUDIO_NEEDS_GESTURE
    g_gesture_done = 0;
#else
    g_gesture_done = 1;
#endif

    g_inited = 1;
    return 0;
}

void glr_audio_shutdown(void) {
    g_worker_hitch_threshold = -1.0;
    if (!g_inited) return;

#if !GLR_AUDIO_NO_THREAD
    if (g_worker_running) {
        /* AWR_QUIT makes the worker do the final state save (off the
         * caller, but join() below waits for it so the file is on disk
         * before we return) and uninit the sounds, then exit. */
        audio_lock();
        g_req = AWR_QUIT;
        worker_wake();
        audio_unlock();
        pthread_join(g_worker, NULL);

        g_inited = 0; /* Clear g_inited immediately so late locks are safe no-ops */

        pthread_cond_destroy(&g_cv);
        pthread_mutex_destroy(&g_mtx);
    } else
#endif
    {
        /* Single-threaded build, or worker create failed: run the final
         * state save + sound teardown synchronously on the caller. */
        worker_save_state();
        worker_uninit_all();
    }

    ma_engine_uninit(&g_engine);
    miniaudio_log_uninit_for_engine();
    reset_audio_module_state();
    g_worker_hitch_threshold = -1.0;
}

void glr_audio_set_hitch_log_elapsed_fn(GlrAudioElapsedSecondsFn fn) {
    g_hitch_log_elapsed_fn = fn;
}

int glr_audio_set_playlist(const char *const *paths, int count) {
    if (!paths || count < 0) return -1;

    GlrAudioTrackSpec specs[GLR_AUDIO_MAX_TRACKS];
    int n = count;
    if (n > GLR_AUDIO_MAX_TRACKS)
        n = GLR_AUDIO_MAX_TRACKS;
    for (int i = 0; i < n; i++) {
        specs[i].path = paths[i];
        specs[i].group = "Music";
        specs[i].display_name = NULL;
    }
    return glr_audio_set_playlist_specs(specs, count);
}

int glr_audio_set_playlist_specs(const GlrAudioTrackSpec *tracks, int count) {
    if (!tracks || count < 0) return -1;

    /* Retire any current sound asynchronously; never block the caller. */
    worker_post(AWR_UNINIT, 0, GLR_AUDIO_NO_SEEK);

    audio_lock();
    if (g_loading) {
        g_load_cancelled = 1;
    }
    g_playlist_count = 0;
    g_playlist_pos   = 0;
    g_pending_start  = 0;
    g_music_loaded   = 0;

    int n = count;
    if (n > GLR_AUDIO_MAX_TRACKS) {
        fprintf(stderr,
                "repl_audio: playlist has %d tracks; truncating to %d\n",
                count, GLR_AUDIO_MAX_TRACKS);
        n = GLR_AUDIO_MAX_TRACKS;
    }

    for (int i = 0; i < n; i++) {
        const char *p = tracks[i].path ? tracks[i].path : "";
        size_t len = strlen(p);
        if (len >= GLR_AUDIO_MAX_PATH) len = GLR_AUDIO_MAX_PATH - 1;
        memcpy(g_playlist[i], p, len);
        g_playlist[i][len] = '\0';

        audio_copy_string(g_playlist_group[i], sizeof(g_playlist_group[i]),
                          tracks[i].group, "Music");
        if (tracks[i].display_name && tracks[i].display_name[0]) {
            audio_copy_string(g_playlist_display_name[i],
                              sizeof(g_playlist_display_name[i]),
                              tracks[i].display_name, NULL);
        } else {
            audio_derive_display_name(g_playlist[i],
                                      g_playlist_display_name[i],
                                      sizeof(g_playlist_display_name[i]));
        }
        g_playlist_duration_secs[i] = -1.0f;
    }
    for (int i = n; i < GLR_AUDIO_MAX_TRACKS; i++) {
        g_playlist[i][0] = '\0';
        g_playlist_group[i][0] = '\0';
        g_playlist_display_name[i][0] = '\0';
        g_playlist_duration_secs[i] = -1.0f;
    }
    g_playlist_count = n;
    audio_unlock();
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
    return request_start(0, GLR_AUDIO_NO_SEEK);
}

int glr_audio_play_music(const char *path) {
    if (!path || !*path) return -1;
    const char *arr[1] = { path };
    if (glr_audio_set_playlist(arr, 1) < 0) return -1;
    return glr_audio_play_playlist();
}



int glr_audio_next_track(void) {
    if (!g_inited || g_playlist_count == 0) return -1;
    audio_lock();
    int next = g_playlist_pos + 1;
    if (next >= g_playlist_count) next = 0;
    audio_unlock();
    return request_start(next, GLR_AUDIO_NO_SEEK);
}

int glr_audio_prev_track(void) {
    if (!g_inited || g_playlist_count == 0) return -1;
    audio_lock();
    int prev = g_playlist_pos - 1;
    if (prev < 0) prev = g_playlist_count - 1;
    audio_unlock();
    return request_start(prev, GLR_AUDIO_NO_SEEK);
}

int glr_audio_play_track_index(int idx) {
    if (!g_inited || g_playlist_count == 0) return -1;
    audio_lock();
    if (idx < 0 || idx >= g_playlist_count) {
        audio_unlock();
        return -1;
    }
    audio_unlock();
    return request_start(idx, GLR_AUDIO_NO_SEEK);
}

int glr_audio_seek(float seek_secs) {
    audio_lock();
    if (!g_inited) {
        audio_unlock();
        return -1;
    }

    if (seek_secs < 0.0f) {
        seek_secs = 0.0f;
    }

    if (g_active >= 0 && g_slot_inited[g_active]) {
        int idx = g_playlist_pos;
        float dur = g_playlist_duration_secs[idx];
        if (dur >= 0.0f && seek_secs > dur) {
            seek_secs = dur;
        }
        ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
        if (sr > 0) {
            ma_sound_seek_to_pcm_frame(&g_slot[g_active], (ma_uint64)(seek_secs * (float)sr));
            audio_unlock();
            return 0;
        }
        audio_unlock();
        return -1;
    }

    /* Active slot doesn't exist yet. Check if a start is pending. */
    if (g_pending_start) {
        g_pending_seek = seek_secs;
        audio_unlock();
        return 0;
    }

    if (g_req == AWR_START) {
        g_req_seek = seek_secs;
        audio_unlock();
        return 0;
    }

    /* The worker has consumed AWR_START (g_req back to AWR_NONE) but hasn't
     * published the slot yet: g_loading is still 1 and g_active is -1.
     * Record the seek so worker_load() applies it at publish time instead
     * of dropping it on the floor. */
    if (g_loading) {
        g_load_seek = seek_secs;
        audio_unlock();
        return 0;
    }

    audio_unlock();
    return -1;
}

int glr_audio_seek_relative(float offset_secs) {
    audio_lock();
    if (!g_inited) {
        audio_unlock();
        return -1;
    }

    float current = 0.0f;
    if (g_active >= 0 && g_slot_inited[g_active]) {
        current = cursor_seconds_locked();
    } else if (g_pending_start) {
        current = (g_pending_seek >= 0.0f) ? g_pending_seek : 0.0f;
    } else if (g_req == AWR_START) {
        current = (g_req_seek >= 0.0f) ? g_req_seek : 0.0f;
    } else if (g_loading) {
        /* Mid-load window: base the relative seek on any recorded mid-load
         * seek, else the request's start offset, else 0. */
        current = (g_load_seek >= 0.0f) ? g_load_seek
                : (g_req_seek >= 0.0f)  ? g_req_seek : 0.0f;
    } else {
        audio_unlock();
        return -1;
    }

    float target = current + offset_secs;
    audio_unlock();
    return glr_audio_seek(target);
}

void glr_audio_tick(void) {
    /* All work here is messaging the dispatch path; with audio down
     * nothing is (or can be) playing, so there is nothing to do. */
    if (!audio_dispatch_active()) return;

    /* Periodically persist track + offset so a crash or forced quit
     * still leaves a reasonably up-to-date state file. The write
     * itself happens on the worker / drain (no fsync, no render-thread
     * I/O). */
    audio_lock();
    if (g_state_file[0] && g_music_loaded) {
        time_t now = time(NULL);
        if (difftime(now, g_last_save_time) >= STATE_SAVE_INTERVAL_SECS) {
            g_req_save = 1;
            worker_wake();
        }
    }

    /* In SONG mode miniaudio loops internally; there is no end to
     * detect. Otherwise, when the track has ended and nothing is
     * already loading/queued, ask the worker to advance. */
    if (g_loop_mode != GLR_AUDIO_LOOP_SONG &&
        g_active >= 0 && !g_loading && g_req == AWR_NONE &&
        ma_sound_at_end(&g_slot[g_active])) {
        g_req = AWR_ADVANCE;
        worker_wake();
    }
    audio_unlock();

#if GLR_AUDIO_NO_THREAD
    /* Single-threaded build: no background worker exists, so service the
     * request mailbox synchronously here, on the per-frame caller. The
     * mailbox is 1-slot latest-wins and the lifecycle ops never re-post,
     * so this drains in a single pass; the loop is purely defensive. */
    while (g_req != AWR_NONE || g_req_save)
        audio_dequeue_and_run();
#endif
}

void glr_audio_set_loop_mode(int mode) {
    if (mode < GLR_AUDIO_LOOP_OFF)  mode = GLR_AUDIO_LOOP_OFF;
    if (mode > GLR_AUDIO_LOOP_ALL)  mode = GLR_AUDIO_LOOP_ALL;
    audio_lock();
    g_loop_mode = mode;
    if (g_active >= 0)
        ma_sound_set_looping(
            &g_slot[g_active],
            (g_loop_mode == GLR_AUDIO_LOOP_SONG) ? MA_TRUE : MA_FALSE);
    audio_unlock();
}

int glr_audio_get_loop_mode(void) {
    audio_lock();
    int m = g_loop_mode;
    audio_unlock();
    return m;
}

void glr_audio_set_paused(int paused) {
    int p = paused ? 1 : 0;
    audio_lock();
    int was_paused = g_paused;
    g_paused = p;
    if (g_active >= 0) {
        if (p && !was_paused)
            ma_sound_stop(&g_slot[g_active]);
        else if (!p && was_paused)
            ma_sound_start(&g_slot[g_active]);
    }
    audio_unlock();
}

int glr_audio_is_paused(void) {
    audio_lock();
    int p = g_paused;
    audio_unlock();
    return p;
}

void glr_audio_set_muted(int muted) {
    audio_lock();
    g_muted = muted ? 1 : 0;
    if (g_inited) {
        ma_engine_set_volume(&g_engine, g_muted ? 0.0f : 1.0f);
    }
    audio_unlock();
}

int glr_audio_is_muted(void) {
    audio_lock();
    int m = g_muted;
    audio_unlock();
    return m;
}

const char *glr_audio_get_current_track(void) {
    /* Only the main thread calls this (status bar); copy under the
     * lock into a static buffer so a concurrent worker swap of
    * g_playlist_pos can't tear the read. Valid until the next call. */
    static char buf[GLR_AUDIO_MAX_PATH];
    audio_lock();
    if (!g_music_loaded ||
        g_playlist_pos < 0 || g_playlist_pos >= g_playlist_count) {
        audio_unlock();
        return NULL;
    }
    memcpy(buf, g_playlist[g_playlist_pos], sizeof(buf));
    audio_unlock();
    return buf;
}

int glr_audio_track_count(void) {
    audio_lock();
    int n = g_playlist_count;
    audio_unlock();
    return n;
}

/* Returns a borrowed pointer into g_playlist_display_name[idx] that
 * outlives the lock. Safe because the name/group tables are mutated
 * only by glr_audio_set_playlist_specs() on the main thread and read
 * only on the main thread; the audio worker never touches them (it
 * writes g_playlist_duration_secs[] alone — hence the duration accessor
 * below returns by value). Keep that invariant if the worker ever gains
 * a reason to rewrite track metadata. */
const char *glr_audio_track_display_name(int idx) {
    audio_lock();
    if (idx < 0 || idx >= g_playlist_count) {
        audio_unlock();
        return NULL;
    }
    const char *name = g_playlist_display_name[idx];
    audio_unlock();
    return name;
}

const char *glr_audio_track_group(int idx) {
    audio_lock();
    if (idx < 0 || idx >= g_playlist_count) {
        audio_unlock();
        return NULL;
    }
    const char *group = g_playlist_group[idx];
    audio_unlock();
    return group;
}

int glr_audio_current_index(void) {
    audio_lock();
    int idx = -1;
    if (g_music_loaded &&
        g_playlist_pos >= 0 && g_playlist_pos < g_playlist_count)
        idx = g_playlist_pos;
    audio_unlock();
    return idx;
}

float glr_audio_current_cursor_seconds(void) {
    audio_lock();
    float s = (g_music_loaded ? cursor_seconds_locked() : 0.0f);
    audio_unlock();
    return s;
}

float glr_audio_track_duration_seconds(int idx) {
    audio_lock();
    float s = -1.0f;
    if (idx >= 0 && idx < g_playlist_count)
        s = g_playlist_duration_secs[idx];
    audio_unlock();
    return s;
}

unsigned int glr_audio_track_generation(void) {
    audio_lock();
    unsigned int v = g_track_generation;
    audio_unlock();
    return v;
}

void glr_audio_set_state_file(const char *path) {
    audio_lock();
    if (!path) {
        g_state_file[0] = '\0';
    } else {
        size_t len = strlen(path);
        if (len >= GLR_AUDIO_MAX_PATH) len = GLR_AUDIO_MAX_PATH - 1;
        memcpy(g_state_file, path, len);
        g_state_file[len] = '\0';
    }
    g_last_save_time = 0;  /* force a write on the next tick after first play */
    audio_unlock();
}

void glr_audio_on_user_gesture(void) {
    audio_lock();
    if (!g_inited || g_gesture_done) {
        audio_unlock();
        return;
    }
    g_gesture_done = 1;
    audio_unlock();

    /* Re-starting the engine is the canonical way to resume a suspended
     * AudioContext under the Emscripten backend. It's a cheap no-op on
     * native backends. */
    ma_engine_start(&g_engine);

    audio_lock();
    if (g_pending_start) {
        g_pending_start = 0;
        float seek = g_pending_seek;
        g_pending_seek = GLR_AUDIO_NO_SEEK;
        worker_post(AWR_START, g_playlist_pos, seek);
    }
    audio_unlock();
}

void glr_audio_set_cfg_mode(int mode) {
    audio_lock();
    g_cfg_mode = mode;
    audio_unlock();
}

int glr_audio_get_cfg_mode(void) {
    audio_lock();
    int v = g_cfg_mode;
    audio_unlock();
    return v;
}

#endif  /* !__EMSCRIPTEN__ */
