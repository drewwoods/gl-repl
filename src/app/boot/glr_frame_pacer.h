#ifndef GLR_FRAME_PACER_H
#define GLR_FRAME_PACER_H

/* Absolute-deadline 60 Hz frame pacer. The host supplies monotonic wall time
 * and uses the returned integer delay with its event-loop timer API. */
typedef struct {
    double next_deadline_ms;
} GlrFramePacer;

#define GLR_FRAME_PACER_INIT { 0.0 }

/* Advance the deadline by exactly 1/60 second and return the rounded delay
 * in milliseconds, clamped to at least 1. A missed deadline resynchronizes
 * to now instead of scheduling a burst of catch-up callbacks. */
int glr_frame_pacer_next_delay_ms(GlrFramePacer *pacer, double now_ms);

#endif /* GLR_FRAME_PACER_H */
