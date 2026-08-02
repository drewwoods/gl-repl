/*
 * render3d_transition.h - Pure show/hide transition machine for scene overlays.
 *
 * The controller keeps one of these per fading overlay such as grid or axes.
 * It is a pure CLOCK plus a phase/event policy: it owns the requested theme,
 * calls render3d_xn_set()/render3d_xn_show() when that request changes, and advances
 * the machine with render3d_xn_tick(dt). It does NOT know how long a fade takes
 * or what opacity a given moment maps to - that belongs to the overlay.
 *
 * Each overlay supplies a Render3dXnReveal (bound once at render3d_xn_init): a pair
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
 *   render3d_xn_init  - snap to theme fully shown, no animation; bind the reveal.
 *   render3d_xn_set   - request a theme change; reads `current` to decide whether
 *                    to FADE_OUT, reverse to FADE_IN, or no-op.
 *   render3d_xn_show  - fade in from invisible, skipping a dead FADE_OUT.
 *   render3d_xn_tick  - advance `elapsed` by dt; sole driver of phase changes.
 *   render3d_xn_opacity - current 0..1 opacity for the renderer (via the reveal).
 */
#ifndef RENDER3D_TRANSITION_H
#define RENDER3D_TRANSITION_H

typedef enum {
    RENDER3D_XN_STEADY = 0,   /* no ramp; fully shown (opacity 1) */
    RENDER3D_XN_FADE_IN,      /* opacity ramps 0 -> 1 over the reveal's in-duration */
    RENDER3D_XN_FADE_OUT      /* opacity ramps 1 -> 0 over the reveal's out-duration */
} Render3dXnPhase;

/* Per-overlay curve plugin: owns the durations, per-theme speed, and opacity
 * shape, so the machine (and the controller) stay duration-agnostic. Both
 * functions must be pure. `theme` is the overlay's current theme, for
 * per-theme speed. */
typedef struct Render3dXnReveal {
    /* opacity in [0,1] at `elapsed` seconds into `phase`. FADE_IN saturates
     * at 1 and FADE_OUT at 0 once `elapsed` passes the phase duration; the
     * machine watches those bounds to detect completion. */
    float (*opacity)(int theme, Render3dXnPhase phase, float elapsed);
    /* Inverse of opacity(): the `elapsed` at which opacity() would return
     * `opacity` in `phase`. Used to re-seat the clock on a mid-fade reversal
     * so the opacity stays continuous. */
    float (*elapsed_at)(int theme, Render3dXnPhase phase, float opacity);
} Render3dXnReveal;

typedef struct {
    int          current;  /* theme being drawn / fading (authoritative) */
    int          next;     /* latest requested theme                     */
    Render3dXnPhase phase;
    float        elapsed;  /* seconds into the current fade phase        */
    const Render3dXnReveal *reveal;  /* curve plugin, bound at init          */
} Render3dXnState;

/* Seed/snap to `theme` fully shown, no animation, and bind the reveal curve.
 * Use at program init and on world reset (a zero-init machine would animate
 * the default theme in on frame 1). `reveal` must outlive `s`. */
void render3d_xn_init(Render3dXnState *s, int theme, const Render3dXnReveal *reveal);

/* Request `theme`. Rules: ==current while STEADY/FADE_IN -> no-op;
 * ==current while FADE_OUT -> FADE_IN (reverse, opacity-continuous);
 * !=current -> FADE_OUT (opacity-continuous if it was fading in). */
void render3d_xn_set(Render3dXnState *s, int theme);

/* Show `theme` directly with no preceding FADE_OUT: current/next jump to
 * `theme`, the clock resets to 0, phase = FADE_IN. The controller calls this
 * (instead of render3d_xn_set) when the current overlay is the "off" index, so
 * show-from-off skips the pointless OUT of an already-invisible overlay. */
void render3d_xn_show(Render3dXnState *s, int theme);

/* Advance the phase clock by dt seconds. At FADE_IN reaching opacity 1 ->
 * STEADY; at FADE_OUT reaching opacity 0, `current` adopts `next` and FADE_IN
 * begins (or STEADY if unchanged). No-op while STEADY. */
void render3d_xn_tick(Render3dXnState *s, float dt);

/* Current opacity (0..1) for the renderer: 1 while STEADY, else the reveal's
 * opacity() at the current phase/elapsed. Safe before init (returns 1). */
float render3d_xn_opacity(const Render3dXnState *s);

#endif /* RENDER3D_TRANSITION_H */
