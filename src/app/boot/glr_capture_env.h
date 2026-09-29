/*
 * glr_capture_env.h - Headless-capture environment hooks.
 *
 * A family of GLR_* environment variables poses otherwise-interactive states
 * (mid-typing guides, floating popups, view-mode transitions) for headless
 * screenshot/GIF/video capture. They split by WHEN they fire:
 *
 *   glr_capture_env_apply()       once, at bootstrap (after the file/example
 *                                 load), before the main loop starts. Applies
 *                                 the initial-time override plus the pointer-
 *                                 script / splash / tick-per-frame / edit-line /
 *                                 type-keys / accum-pass hooks and the Config-
 *                                 row overrides (accum effect, code focus,
 *                                 syntax highlight).
 *
 *   glr_capture_env_frame_hook()  every display callback. Applies the hooks
 *                                 that need a live viewport (popup placement
 *                                 clamps against it) or a frame clock (the
 *                                 view-toggle schedule): the color picker, the
 *                                 GL-state popup, the F1 help overlay, and the
 *                                 scheduled 2D/3D view toggle. Each is a
 *                                 one-shot / self-scheduling no-op when its env
 *                                 var is unset.
 *
 * Unset env vars => no-ops; production behavior is unchanged.
 */
#ifndef GLR_CAPTURE_ENV_H
#define GLR_CAPTURE_ENV_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bootstrap-time capture-env block. `time_arg` is the --time CLI value (NULL
 * when absent); it wins over GLR_TIME. Ordering inside is load-bearing:
 * pointer-script load precedes the tick-per-frame resolve (an active script
 * implies the mode), and GLR_TYPE_KEYS applies after GLR_EDIT_LINE so typed
 * input can extend a parked line. */
void glr_capture_env_apply(const char *time_arg);

/* Per-frame capture-env hook. Call once at the top of the display callback,
 * before rendering, so the posed state is in the frame that renders. */
void glr_capture_env_frame_hook(void);

/* GLR_FREEZE_AFTER_FRAMES=N: non-zero from the (N+1)th display callback on.
 * gl_repl.c then returns from the callback before any GL work or sim tick, so
 * the last rendered frame stays on screen unchanged. With GLR_TICK_PER_FRAME
 * that frame is a function of N, the scene and the GL alone - scenes that carry
 * state from frame to frame never settle by themselves - which is what the
 * gl4es screenshot A/B (scripts/gl4es-ab-catalog.mjs) needs. Logs one
 * "GLR_FREEZE_AFTER_FRAMES: frozen after N frames" line on stderr when it
 * trips; the harness waits for it. Counts its own calls: call it once per
 * display callback, first. Zero forever when the variable is unset. */
int glr_capture_env_frozen(void);

/* Non-zero when GLR_NO_INPUT is set to anything but "0": the window ignores
 * real keyboard and mouse events for the run.
 *
 * A capture run raises a real window that takes focus, so anything typed at
 * the launching terminal - or a stray click, or the pointer merely crossing
 * the window - lands in the editor and corrupts the shot. The states posed by
 * the rest of this file are worth nothing if the document can drift out from
 * under them mid-capture.
 *
 * This gates the six GLUT input callbacks in gl_repl.c and nothing else, so
 * the deliberate input paths are unaffected: GLR_TYPE_KEYS and
 * GLR_POINTER_SCRIPT both drive glr_ctrl_scripted_*, never the callbacks.
 * Read on first use and cached, so it is order-independent with respect to
 * glr_capture_env_apply(). */
int glr_capture_env_input_locked(void);

#ifdef __cplusplus
}
#endif

#endif /* GLR_CAPTURE_ENV_H */
