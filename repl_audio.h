#ifndef REPL_AUDIO_H
#define REPL_AUDIO_H

/*
 * Audio playback for the immediate-mode REPL.
 *
 * Thin wrapper over miniaudio (include/miniaudio.h). Supports a small
 * playlist of background music tracks with three loop modes:
 *
 *   REPL_AUDIO_LOOP_OFF   play the playlist through once, then stop
 *   REPL_AUDIO_LOOP_SONG  loop the current track forever
 *   REPL_AUDIO_LOOP_ALL   play the playlist; wrap to first at the end
 *
 * Lifecycle:
 *   repl_audio_init()                once, after repl_init_gl()
 *   repl_audio_set_playlist(...)     register the tracks (filename order)
 *   repl_audio_play_playlist()       start the first track
 *   repl_audio_tick()                call each frame; advances on song end
 *   repl_audio_set_loop_mode(mode)   change loop policy at any time
 *   repl_audio_set_muted(int)        engine volume 1.0 <-> 0.0
 *   repl_audio_shutdown()            via atexit()
 *
 * repl_audio_play_music(path) remains as a convenience: it sets a
 * single-track playlist and starts it.
 *
 * Browser autoplay policy: on __EMSCRIPTEN__ the engine is held in a
 * pre-started state until repl_audio_on_user_gesture() is called from
 * the first keyboard/mouse callback. On native builds the call is a
 * no-op after the first invocation. Any play_playlist()/play_music()
 * request that lands before the gesture is deferred and kicks in on
 * the first gesture.
 *
 * All functions are safe to call even if init failed: errors are logged
 * to stderr once and the module silently becomes a no-op so the REPL
 * keeps running without sound.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REPL_AUDIO_LOOP_OFF  = 0,
    REPL_AUDIO_LOOP_SONG = 1,
    REPL_AUDIO_LOOP_ALL  = 2,
} ReplAudioLoopMode;

int  repl_audio_init(void);
void repl_audio_shutdown(void);

/* Register a list of file paths as the playlist. Paths are copied
 * internally. Extra entries past the internal cap are dropped (a
 * warning is printed). Returns the number of tracks accepted, or -1
 * on error. */
int  repl_audio_set_playlist(const char *const *paths, int count);

/* Start playing track 0 of the registered playlist (respects the
 * current loop mode and the deferred-gesture rule on the web). */
int  repl_audio_play_playlist(void);

/* Convenience: set a single-track playlist and start it. */
int  repl_audio_play_music(const char *path);

void repl_audio_stop_music(void);

/* Skip to the next / previous track in the playlist. Both wrap at
 * the ends regardless of loop mode (so the user can always seek).
 * No-op when the playlist is empty or the audio engine never
 * initialized. Return 0 on success, -1 otherwise. */
int  repl_audio_next_track(void);
int  repl_audio_prev_track(void);

/* Poll once per frame. When the current track is finished and the
 * loop mode is OFF or ALL, advances to the next track (or wraps, or
 * stops, depending on mode). No-op when loop mode is SONG. */
void repl_audio_tick(void);

void repl_audio_set_loop_mode(int mode);   /* ReplAudioLoopMode */
int  repl_audio_get_loop_mode(void);

void repl_audio_set_muted(int muted);
int  repl_audio_is_muted(void);

/* Returns the path of the currently-loaded track, or NULL if nothing
 * is playing. The returned pointer is owned by the audio module and
 * stays valid until the next start_track call. */
const char *repl_audio_get_current_track(void);

/* Monotonic counter bumped each time a new track actually starts
 * playing. Callers remember the last value they saw to detect track
 * changes (e.g. to display the song name in a status bar). Starts at
 * 0; the first successful start advances it to 1. */
unsigned int repl_audio_track_generation(void);

void repl_audio_on_user_gesture(void);

#ifdef __cplusplus
}
#endif

#endif /* REPL_AUDIO_H */
