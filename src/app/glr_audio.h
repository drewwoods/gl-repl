#ifndef GLR_AUDIO_H
#define GLR_AUDIO_H

/*
 * Audio playback for the immediate-mode REPL.
 *
 * Thin wrapper over miniaudio (include/miniaudio.h). Supports a small
 * playlist of background music tracks with three loop modes:
 *
 *   GLR_AUDIO_LOOP_OFF   play the playlist through once, then stop
 *   GLR_AUDIO_LOOP_SONG  loop the current track forever
 *   GLR_AUDIO_LOOP_ALL   play the playlist; wrap to first at the end
 *
 * Lifecycle:
 *   glr_audio_init()                once, after the app/controller/GL bootstrap
 *   glr_audio_set_playlist(...)     register the tracks (filename order)
 *   glr_audio_play_playlist()       start the first track
 *   glr_audio_tick()                call each frame; advances on song end
 *   glr_audio_set_loop_mode(mode)   change loop policy at any time
 *   glr_audio_set_muted(int)        engine volume 1.0 <-> 0.0
 *   glr_audio_shutdown()            via atexit()
 *
 * glr_audio_play_music(path) remains as a convenience: it sets a
 * single-track playlist and starts it.
 *
 * Browser autoplay policy: on __EMSCRIPTEN__ the engine is held in a
 * pre-started state until glr_audio_on_user_gesture() is called from
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
    GLR_AUDIO_LOOP_OFF  = 0,
    GLR_AUDIO_LOOP_SONG = 1,
    GLR_AUDIO_LOOP_ALL  = 2,
} GlrAudioLoopMode;

typedef struct {
    const char *path;
    const char *group;
    const char *display_name;
} GlrAudioTrackSpec;

typedef double (*GlrAudioElapsedSecondsFn)(void);
typedef void (*GlrAudioTraceFn)(const char *phase);

int  glr_audio_init(void);
void glr_audio_shutdown(void);

/* Complete audio system bootstrap: initializes audio engine, sets state file,
 * scans assets/music directories, registers and starts playlist playback.
 * Preserves init-trace phases via optional trace/trace_detail callbacks.
 * Returns 0 on success, nonzero if glr_audio_init() failed. */
int  glr_audio_bootstrap(const char *assets_override, GlrAudioTraceFn trace, GlrAudioTraceFn trace_detail);

/* Optional diagnostic clock for worker-hitch logs. The callback must be safe
 * to call from the audio worker and return seconds since the app's log origin.
 * Set before glr_audio_init(); pass NULL to restore unprefixed hitch logs. */
void glr_audio_set_hitch_log_elapsed_fn(GlrAudioElapsedSecondsFn fn);

/* Register a list of file paths as the playlist. Paths are copied
 * internally. Extra entries past the internal cap are dropped (a
 * warning is printed). Returns the number of tracks accepted, or -1
 * on error. */
int  glr_audio_set_playlist(const char *const *paths, int count);
int  glr_audio_set_playlist_specs(const GlrAudioTrackSpec *tracks, int count);

/* Start playing track 0 of the registered playlist (respects the
 * current loop mode and the deferred-gesture rule on the web). */
int  glr_audio_play_playlist(void);

/* Convenience: set a single-track playlist and start it. */
int  glr_audio_play_music(const char *path);


/* Skip to the next / previous track in the playlist. Both wrap at
 * the ends regardless of loop mode (so the user can always seek).
 * No-op when the playlist is empty or the audio engine never
 * initialized. Return 0 on success, -1 otherwise. */
int  glr_audio_next_track(void);
int  glr_audio_prev_track(void);
int  glr_audio_play_track_index(int idx);
int  glr_audio_seek(float seek_secs);
int  glr_audio_seek_relative(float offset_secs);

int         glr_audio_track_count(void);
const char *glr_audio_track_display_name(int idx);
const char *glr_audio_track_group(int idx);
int         glr_audio_current_index(void);
float       glr_audio_current_cursor_seconds(void);
float       glr_audio_track_duration_seconds(int idx);

/* Poll once per frame. When the current track is finished and the
 * loop mode is OFF or ALL, advances to the next track (or wraps, or
 * stops, depending on mode). No-op when loop mode is SONG. */
void glr_audio_tick(void);

void glr_audio_set_loop_mode(int mode);   /* GlrAudioLoopMode */
int  glr_audio_get_loop_mode(void);

/* Pause/resume without losing the playback position.  Pausing calls
 * ma_sound_stop() (cursor preserved); resuming calls ma_sound_start().
 * Safe to call before a track is loaded - the flag is honoured when the
 * next track starts. */
void glr_audio_set_paused(int paused);
int  glr_audio_is_paused(void);
int  glr_audio_is_enabled(void);

void glr_audio_set_muted(int muted);
int  glr_audio_is_muted(void);

/* Returns the path of the currently-loaded track, or NULL if nothing
 * is playing. The returned pointer is an internal static copy and
 * stays valid until the next glr_audio_get_current_track() call. */
const char *glr_audio_get_current_track(void);

/* Monotonic counter bumped each time a new track actually starts
 * playing. Callers remember the last value they saw to detect track
 * changes (e.g. to display the song name in a status bar). Starts at
 * 0; the first successful start advances it to 1. */
unsigned int glr_audio_track_generation(void);

void glr_audio_on_user_gesture(void);

/* Set the path for the INI state file used to persist track, offset, and audio
 * preferences across restarts.  Call before glr_audio_play_playlist().  Pass
 * NULL to disable.  The file is written at shutdown and periodically by
 * glr_audio_tick().
 * On startup, if the saved track is present in the current playlist the cursor
 * is restored; if that file cannot be opened, playback skips forward through
 * the playlist to the next playable track. If the saved track is absent from
 * the current playlist, playback starts from track 0 as normal. */
void glr_audio_set_state_file(const char *path);

/* Opaque audio-config integer owned by the action/config layer. The audio
 * module stores this value in the state file and restores it on load so
 * startup can call glr_actions_apply_defaults() with the right mode. Pass -1
 * to clear (no cfg_mode= line written). */
void glr_audio_set_cfg_mode(int mode);
int  glr_audio_get_cfg_mode(void);  /* returns -1 when not yet loaded */

#ifdef __cplusplus
}
#endif

#endif /* GLR_AUDIO_H */
