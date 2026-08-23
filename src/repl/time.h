/*
 * src/repl/time.h - Public helpers for the predefined animation clock.
 */
#ifndef REPL_TIME_H
#define REPL_TIME_H

/* Advance the predefined `t` variable by `dt` seconds. The controller's
 * timer tick calls this every frame when the animation toggle (Ctrl+T)
 * is on. */
void repl_advance_time(float dt);

/* Step `t` by `dt` seconds whether or not the animation toggle is on. The
 * variable panel's frame stepper calls this with GLR_FRAME_DT_SECS to advance
 * the paused sim exactly one frame. Not a document edit: no undo snapshot, no
 * source rewrite - the clock is not a declared variable. */
void repl_step_time(float dt);

/* Reset `t` to 0. Called from controller/test paths that need a
 * deterministic time origin. */
void repl_reset_time_to_zero(void);

/* Set the predefined `t` variable to an explicit value (seconds). Used by
 * the startup `--time` flag / `GLR_TIME` env override so animations can be
 * captured starting from a later point in their timeline. */
void repl_set_time(float value);

#endif /* REPL_TIME_H */
