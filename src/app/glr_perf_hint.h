/*
 * glr_perf_hint.h - Frame-rate watchdog for expensive non-default settings.
 *
 * Controller-band, no GL and no UI. Tick takes FPS, the last callback
 * interval, and a by-value input bag so the module is deterministic and
 * unit-testable with no clock hook. When the cadence stays below a
 * display-relative threshold for a sustained interval and at least one
 * setting the user raised above its shipped default is in play, the
 * view names the heaviest culprit for the code-panel status strip.
 */
#ifndef GLR_PERF_HINT_H
#define GLR_PERF_HINT_H

/* Absolute floors used when no clean (empty-mask) frame has established
 * a display ceiling yet. */
#define GLR_PERF_HINT_FPS_TRIP   50.0
#define GLR_PERF_HINT_FPS_CLEAR  55.0
/* Relative floors once a clean ceiling exists. */
#define GLR_PERF_HINT_TRIP_FRAC  0.80
#define GLR_PERF_HINT_CLEAR_FRAC 0.90
/* Nothing trips during the first 3 s of valid frames. */
#define GLR_PERF_HINT_WARMUP_US  3000000.0
/* Sustained-slow / sustained-recovered window. */
#define GLR_PERF_HINT_TRIP_US    2000000.0
/* A raw interval longer than this is a suspension, not a slow frame. */
#define GLR_PERF_HINT_DISCONTINUITY_US 500000.0

typedef enum {
    GLR_PERF_CULPRIT_NONE = 0,
    GLR_PERF_CULPRIT_ACCUM,          /* N-pass whole-scene re-render: heaviest */
    GLR_PERF_CULPRIT_POST_FX_FRAME,  /* filter over the whole composited window */
    GLR_PERF_CULPRIT_POST_FX_VIEW,   /* filter over the 3D view only */
    GLR_PERF_CULPRIT_LINE_SMOOTH,
    GLR_PERF_CULPRIT_COUNT
} GlrPerfCulprit;

/* What the watchdog may blame this frame, plus the per-frame suppression
 * source. Flat by value; the module reads no live state of its own. */
typedef struct {
    int use_accum;              /* GlrRenderState.use_accum - see the web note */
    int accum_effect;           /* Render3dAccumEffect */
    int accum_passes;           /* resolved ladder step */
    int line_smooth_enabled;
    int post_fx_scope;          /* GLR_POST_FX_SCOPE_* */
    int post_fx_effect;         /* GLR_POST_FX_EFFECT_* */
    int pointer_script_active;  /* tour or scripted capture is driving input */
} GlrPerfHintInputs;

typedef struct {                /* snapshot-safe flat view */
    int active;
    int fps;                    /* rounded smoothed fps, refreshed while up */
    int culprit;                /* GlrPerfCulprit - the heaviest one on */
    int culprit_count;          /* how many expensive settings are on */
} GlrPerfHintView;

void            glr_perf_hint_tick(double fps, double dt_us,
                                   const GlrPerfHintInputs *in);
GlrPerfHintView glr_perf_hint_view(void);
void            glr_perf_hint_dismiss(void);  /* hide until this config changes or FPS recovers */
void            glr_perf_hint_reset(void);    /* re-arm + clear debounce; keep ceiling/latches */
void            glr_perf_hint_set_capture_session(int capturing);
/* Zero every static, including the clean ceiling and the capture-session
 * latch. Unit tests that share one process need a blank session; production
 * never calls this. */
void            glr_perf_hint_reset_for_test(void);

#endif /* GLR_PERF_HINT_H */
