/*
 * scene_transition.h - Pure show/hide transition machine for scene overlays.
 *
 * The controller keeps one of these per fading overlay such as grid or axes.
 * It is a pure CLOCK plus a phase/event policy: it owns the requested theme,
 * calls scene_xn_set()/scene_xn_show() when that request changes, and advances
 * the machine with scene_xn_tick(dt). It does NOT know how long a fade takes
 * or what opacity a given moment maps to — that belongs to the overlay.
 *
 * Each overlay supplies a SceneXnReveal (bound once at scene_xn_init): a pair
 * of pure functions that own the durations, the per-theme speed, and the
 * opacity curve. The machine tracks `elapsed` seconds into the current phase
 * and calls reveal->opacity() to read the value back; it never names a
 * duration. So the grid/axes module decides the "end time" (opacity hits
 * 0 or 1), and the controller just feeds it dt. reveal->elapsed_at() inverts
 * the curve so a mid-fade reversal resumes from the current opacity instead
 * of snapping.
 *
 * The machine is GL-free and history-light: fade-out keeps `current`
 * authoritative until opacity reaches zero, so rapid cycling drops transient
 * themes and reversing back to the current theme flips direction continuously.
 *
 * API verb cheat-sheet:
 *   scene_xn_init  — snap to theme fully shown, no animation; bind the reveal.
 *   scene_xn_set   — request a theme change; reads `current` to decide whether
 *                    to FADE_OUT, reverse to FADE_IN, or no-op.
 *   scene_xn_show  — fade in from invisible, skipping a dead FADE_OUT.
 *   scene_xn_tick  — advance `elapsed` by dt; sole driver of phase changes.
 *   scene_xn_opacity — current 0..1 opacity for the renderer (via the reveal).
 */
#ifndef SCENE_TRANSITION_H
#define SCENE_TRANSITION_H

typedef enum {
    SCENE_XN_STEADY = 0,   /* no ramp; fully shown (opacity 1) */
    SCENE_XN_FADE_IN,      /* opacity ramps 0 -> 1 over the reveal's in-duration */
    SCENE_XN_FADE_OUT      /* opacity ramps 1 -> 0 over the reveal's out-duration */
} SceneXnPhase;

/* Per-overlay curve plugin: owns the durations, per-theme speed, and opacity
 * shape, so the machine (and the controller) stay duration-agnostic. Both
 * functions must be pure. `theme` is the overlay's current theme, for
 * per-theme speed. */
typedef struct SceneXnReveal {
    /* opacity in [0,1] at `elapsed` seconds into `phase`. FADE_IN saturates
     * at 1 and FADE_OUT at 0 once `elapsed` passes the phase duration; the
     * machine watches those bounds to detect completion. */
    float (*opacity)(int theme, SceneXnPhase phase, float elapsed);
    /* Inverse of opacity(): the `elapsed` at which opacity() would return
     * `opacity` in `phase`. Used to re-seat the clock on a mid-fade reversal
     * so the opacity stays continuous. */
    float (*elapsed_at)(int theme, SceneXnPhase phase, float opacity);
} SceneXnReveal;

typedef struct {
    int          current;  /* theme being drawn / fading (authoritative) */
    int          next;     /* latest requested theme                     */
    SceneXnPhase phase;
    float        elapsed;  /* seconds into the current fade phase        */
    const SceneXnReveal *reveal;  /* curve plugin, bound at init          */
} SceneXnState;

/* Seed/snap to `theme` fully shown, no animation, and bind the reveal curve.
 * Use at program init and on world reset (a zero-init machine would animate
 * the default theme in on frame 1). `reveal` must outlive `s`. */
void scene_xn_init(SceneXnState *s, int theme, const SceneXnReveal *reveal);

/* Request `theme`. Rules: ==current while STEADY/FADE_IN -> no-op;
 * ==current while FADE_OUT -> FADE_IN (reverse, opacity-continuous);
 * !=current -> FADE_OUT (opacity-continuous if it was fading in). */
void scene_xn_set(SceneXnState *s, int theme);

/* Show `theme` directly with no preceding FADE_OUT: current/next jump to
 * `theme`, the clock resets to 0, phase = FADE_IN. The controller calls this
 * (instead of scene_xn_set) when the current overlay is the "off" index, so
 * show-from-off skips the pointless OUT of an already-invisible overlay. */
void scene_xn_show(SceneXnState *s, int theme);

/* Advance the phase clock by dt seconds. At FADE_IN reaching opacity 1 ->
 * STEADY; at FADE_OUT reaching opacity 0, `current` adopts `next` and FADE_IN
 * begins (or STEADY if unchanged). No-op while STEADY. */
void scene_xn_tick(SceneXnState *s, float dt);

/* Current opacity (0..1) for the renderer: 1 while STEADY, else the reveal's
 * opacity() at the current phase/elapsed. Safe before init (returns 1). */
float scene_xn_opacity(const SceneXnState *s);

#endif /* SCENE_TRANSITION_H */
