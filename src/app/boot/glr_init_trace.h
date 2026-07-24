/*
 * glr_init_trace.h - Startup stall diagnostic (init trace).
 *
 * Each phase logs wall-clock seconds elapsed since the app started so a slow
 * phase (e.g. ma_engine_init opening the audio device) is visible in the
 * terminal. gettimeofday keeps this portable/C99 without the per-platform
 * timebase branching in prof.c.
 *
 * Two granularity levels:
 *   glr_init_trace()          always emits - the baseline boot phases.
 *   glr_init_trace_detail()   emits only when the detailed flag is set
 *                             (--detailed-prof or GLR_DETAILED_PROF); covers
 *                             the finer-grained phases (glutInit split, audio
 *                             playlist sub-steps, first two frames).
 *
 * The elapsed-seconds clock is also handed to the audio worker-hitch log and
 * the controller init log via glr_init_trace_elapsed_seconds (a
 * double(*)(void) that both subsystems accept).
 */
#ifndef GLR_INIT_TRACE_H
#define GLR_INIT_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Seconds since the first trace call (the app's log origin). Safe to hand to
 * glr_audio_set_hitch_log_elapsed_fn / glr_ctrl_set_init_log_elapsed_fn. */
double glr_init_trace_elapsed_seconds(void);

/* Emit a baseline boot phase to stderr. */
void glr_init_trace(const char *phase);

/* Emit a fine-grained phase to stderr, but only when the detailed flag is on. */
void glr_init_trace_detail(const char *phase);

/* Enable/disable the fine-grained phases (--detailed-prof / GLR_DETAILED_PROF).
 * Set once at startup, before the first detail phase. */
void glr_init_trace_set_detailed(int on);

/* Query the detailed flag (the display callback gates its first-two-frames
 * trace on it). */
int glr_init_trace_detailed(void);

#ifdef __cplusplus
}
#endif

#endif /* GLR_INIT_TRACE_H */
