/*
 * replay.h - Step-by-step execution visualization and state machine.
 *
 * Replay mode lets users step through an expanded command stream one command
 * at a time (or with variable speed), seeing geometry appear progressively.
 * Toggle it with Ctrl+R or the Replay pin button.
 *
 * Execution model: The replay state machine (OFF/PLAYING/PAUSED/DONE) tracks
 * a program counter (PC) into the flat command array. During playback, the
 * executor only renders commands with index < repl_replay_exec_limit() (the PC
 * clamped by replay speed/pause state). Geometry fades in/out as new geometry
 * appears via a ring buffer of fading geometry snapshots (ReplayFadeBatch).
 *
 * Controls (routed from repl_editor.c): Ctrl+R starts/stops replay; Ctrl+K
 * jumps to the cursor line; Space toggles play/pause (or restarts when done);
 * Left/Right step by one command; Up/Down or +/- adjust playback speed; M
 * switches vertex/polygon replay mode; E toggles argument expansion; Esc stops.
 *
 * State preservation: When stepping back, the executor restores the baseline
 * predefined variable values (for 't' and slider variables) and re-executes
 * from the new PC. This allows "time travel" even when expressions are
 * time-dependent (e.g., objects rotating with sin(t*speed)).
 *
 * Fade batches: Old geometry (that was rendered in a previous frame) is
 * captured in a snapshot at execution time. As the PC advances, the old
 * snapshot fades out over several frames while new geometry fades in,
 * giving visual feedback about what's being added. The ring buffer holds up
 * to REPLAY_FADE_BATCH_MAX snapshots.
 */
#ifndef REPLAY_H
#define REPLAY_H

#include "repl_eval.h"
#include "scene/replay_types.h"

typedef enum {
    REPLAY_OFF = 0,
    REPLAY_PLAYING,
    REPLAY_PAUSED,
    REPLAY_DONE
} ReplayState;

typedef enum {
    REPLAY_MODE_POLYGON = 0,
    REPLAY_MODE_VERTEX
} ReplayMode;

/* ReplayFadeBatch / ReplayFadeBatchView / REPLAY_FADE_BATCH_MAX live in
 * src/scene/replay_types.h alongside the rest of the replay-fade rendering
 * data, and are pulled in via the include below. The REPL replay state
 * machine produces batches; the scene module consumes them. */

/* --- State machine control -------------------------------------------- */

void repl_replay_start(void);                /* Enter PLAYING state */
void repl_replay_stop(void);                 /* Enter OFF state, clear history */
void repl_replay_advance(void);              /* Increment PC by 1 */
void repl_replay_tick_fade_batches(float dt);/* Age and decay active fades */

/* --- Seek operations --------------------------------------------------- */

void repl_replay_seek(int new_pc);           /* Jump to PC; restores baseline vars */
int  repl_replay_seek_to_src_line(int target_line);
void repl_replay_step_back(void);            /* Retreat PC; re-execute from new point */
void repl_replay_restart_from_beginning(void);

/* --- Speed / playback control ----------------------------------------- */

void repl_replay_speed_adjust(float factor); /* Multiply speed by factor (1.5 = faster) */

/* Toggle the Replay pin button state. Playing pauses, paused resumes, and
 * stopped/done states restart playback from the beginning. */
void repl_replay_toggle_play_pause(void);

/* --- Query / renderer helpers ----------------------------------------- */

int  repl_replay_exec_limit(void);           /* Current PC (what executor renders to) */
int  repl_replay_has_active_fades(void);     /* Any fades currently visible? */
int  repl_replay_fill_base_limit(void);      /* Highest PC reached so far */

/* Compute skip limits for performance: scene_render.c can skip rendering
 * commands in ranges where no fade is active (optimization). */
int  repl_replay_compute_fade_skip_limits(int *out_limits, int max_count);

ReplayFadeBatchView repl_replay_fade_batches_view(void);
float repl_replay_batch_alpha(const ReplayFadeBatch *batch);

/* Per-frame: updates exec_limit based on speed multiplier and pause state,
 * captures geometry snapshots for new fades. Called each frame. */
int  repl_replay_prepare_frame(int full_flat_count);

/* --- Variable state for step-back --------------------------------------- */

void repl_replay_restore_baseline_predef_values(void);
void repl_replay_copy_baseline_predef_values(float *dst, int max_vals);
void repl_replay_restore_baseline_scratch_arrays(void);
void repl_replay_copy_baseline_scratch_arrays(
    float dst[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]);

/* --- Input routing (called from repl_editor.c) ----------------------- */

int  repl_replay_handle_key(unsigned char key);
int  repl_replay_handle_special_key(int key);

/* --- Benchmark / test helpers ----------------------------------------- */

int  repl_bench_fade_install(const int *old_pcs, const int *new_pcs,
                             int count, float age);
void repl_bench_fade_clear(void);

#endif /* REPLAY_H */
