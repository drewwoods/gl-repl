#ifndef REPL_AUDIO_H
#define REPL_AUDIO_H

/*
 * Audio playback for the immediate-mode REPL.
 *
 * Thin wrapper over miniaudio (include/miniaudio.h). The goal is one
 * looped background music track; fire-and-forget, no REPL commands.
 *
 * Lifecycle:
 *   repl_audio_init()                once, after repl_init_gl()
 *   repl_audio_play_music(path)      kicks off the looping track
 *   repl_audio_set_muted(int)        engine volume 1.0 <-> 0.0
 *   repl_audio_shutdown()            via atexit()
 *
 * Browser autoplay policy: on __EMSCRIPTEN__ the engine is held in a
 * pre-started state until repl_audio_on_user_gesture() is called from
 * the first keyboard/mouse callback. On native builds the call is a
 * no-op after the first invocation.
 *
 * All functions are safe to call even if init failed: errors are logged
 * to stderr once and the module silently becomes a no-op so the REPL
 * keeps running without sound.
 */

#ifdef __cplusplus
extern "C" {
#endif

int  repl_audio_init(void);
void repl_audio_shutdown(void);

int  repl_audio_play_music(const char *path);
void repl_audio_stop_music(void);

void repl_audio_set_muted(int muted);
int  repl_audio_is_muted(void);

void repl_audio_on_user_gesture(void);

#ifdef __cplusplus
}
#endif

#endif /* REPL_AUDIO_H */
