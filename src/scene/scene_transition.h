/*
 * scene_transition.h - pure show/hide fade state machine for a scene
 * overlay (grid, axes). No GL, no globals — one instance per overlay.
 *
 * Model (plans/active/grid-axes-transitions.md): the controller diffs
 * the presentation theme and calls scene_xn_set(); each frame it calls
 * scene_xn_tick(dt). The controller owns `opacity` outright and is
 * history-agnostic — FADE_IN ramps toward 1, FADE_OUT toward 0, from
 * whatever opacity currently is. `current` only advances at the
 * opacity-0 crossing, so rapid cycling skips ephemeral themes;
 * returning to `current` mid-FADE_OUT just reverses. Renderer reads
 * {theme, opacity, phase} and scales overlay color alpha by opacity
 * (the `off` theme draws nothing regardless).
 */
#ifndef SCENE_TRANSITION_H
#define SCENE_TRANSITION_H

typedef enum {
    SCENE_XN_STEADY = 0,   /* no ramp; opacity stable (1 shown / 0 hidden) */
    SCENE_XN_FADE_IN,      /* opacity -> 1 at fade_in_secs  */
    SCENE_XN_FADE_OUT      /* opacity -> 0 at fade_out_secs */
} SceneXnPhase;

typedef struct {
    int          current;  /* theme being drawn / fading (authoritative) */
    int          next;     /* latest requested theme                     */
    SceneXnPhase phase;
    float        opacity;  /* 0..1 */
} SceneXnState;

/* Seed/snap to `theme` fully shown, no animation. Use at program init
 * and on world reset (a zero-init machine would animate the default
 * theme in on frame 1). */
void scene_xn_init(SceneXnState *s, int theme);

/* Request `theme`. Rules: ==current while STEADY/FADE_IN -> no-op;
 * ==current while FADE_OUT -> FADE_IN (reverse); !=current -> FADE_OUT. */
void scene_xn_set(SceneXnState *s, int theme);

/* Show `theme` directly with no preceding FADE_OUT: current/next jump
 * to `theme`, opacity resets to 0, phase = FADE_IN. The machine is
 * theme-index-agnostic; the controller calls this (instead of
 * scene_xn_set) when the current overlay is the "off" index, so
 * show-from-off skips the pointless OUT of an already-invisible
 * overlay (plans/active/grid-axes-transitions.md rule 6). */
void scene_xn_show(SceneXnState *s, int theme);

/* Advance by dt seconds. in_secs/out_secs are the fade durations
 * (<=0 => instant). At FADE_OUT reaching 0, `current` adopts `next`
 * and FADE_IN begins (or STEADY if unchanged). */
void scene_xn_tick(SceneXnState *s, float dt, float in_secs,
                   float out_secs);

#endif /* SCENE_TRANSITION_H */
