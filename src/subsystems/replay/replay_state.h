/*
 * replay_state.h - Replay peer subsystem storage and accessors.
 *
 * Owns the transient playback state that should not live in the REPL document
 * model: replay on/off, machine state, program counter, playback speed, source
 * line tracking, and annotation expansion mode. The main replay logic in
 * src/subsystems/replay/replay.c writes this state; controller/UI code reads it for
 * buttons, shortcuts, and per-frame snapshots.
 *
 * Snapshot/restore is separate because tests, undo-like full-world captures,
 * and frame snapshot builders need to copy replay state alongside REPL/editor/UI
 * peers. Storage lives in a file-static here rather than in ReplRuntimeState,
 * reflecting the replay peer split that moved replay out of the core REPL
 * runtime bundle.
 */
#ifndef REPLAY_STATE_H
#define REPLAY_STATE_H

#include "config.h"
#include "repl/eval.h"

#define REPLAY_FADE_BATCH_MAX 24

/* A snapshot of geometry from [old_pc, new_pc) that fades out as new
 * geometry appears. age is the fade timestamp (incremented by the REPL
 * replay state machine). Multiple batches can be active simultaneously
 * via a ring buffer. */
typedef struct {
    int   old_pc;
    int   new_pc;
    float age;
} ReplayFadeBatch;

/* Read-only view over the active fade batches; valid for one frame. */
typedef struct {
    const ReplayFadeBatch *batches;
    int                    count;
} ReplayFadeBatchView;

/* Per-frame fade-render snapshot assembled by the controller and
 * consumed by replay_render_post_fill / replay_render_fade_batches /
 * replay_render_tess_preview. Lives in the replay subsystem header
 * (alongside ReplayFadeBatch and ReplayRuntimeState) so the renderer
 * header can avoid pulling app/glr_ctrl.h — keeping the
 * subsystems → app dependency arrow pointed the right way.
 *
 * The predef baseline carries names + count (not just floats) so the
 * fade restore can route through repl_eval_restore_predef_values_by_name
 * and assign by name. Without that, a workspace switch or scene load
 * during an active replay reshapes the predef table and a values-only
 * restore writes saved floats into slots now holding different vars
 * (audit #3). */
typedef struct ReplayFadePlan {
    int             batch_count;
    ReplayFadeBatch batches[REPLAY_FADE_BATCH_MAX];
    int             skip_limits[REPLAY_FADE_BATCH_MAX];
    float           batch_alpha[REPLAY_FADE_BATCH_MAX];
    float           baseline_predef_vals[MAX_PREDEF_VARS];
    char            baseline_predef_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX];
    int             baseline_predef_count;
    float           baseline_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    int             active;              /* 1 = post_fill_fn should render fades */
    int             base_limit;          /* clamp for the main fill */
    int             tess_preview_active; /* 1 = post_fill_fn should render tess preview */
} ReplayFadePlan;

/* Replay snapshot shape owned by the replay peer subsystem.
 *
 * The predef baseline is stored as a full (vals + names + count)
 * snapshot — not a values-only float[] — because the replay session
 * spans multiple frames and the live predef table can be reshaped by
 * a workspace switch, scene load, or undo across an @declare between
 * replay_start and a later restore. A values-only restore would then
 * land saved floats into slots now holding different variables. The
 * restore goes through repl_eval_restore_predef_values_by_name, which
 * assigns by name into whichever slot currently holds that name. */
typedef struct {
    int             active;
    int             state;
    int             pc;
    int             mode;
    float           speed;
    float           accum;
    float           fade_speed;
    int             src_line_idx;
    int             total_flat_cmds;
    int             expand_args;
    float           baseline_predef_vals[MAX_PREDEF_VARS];
    char            baseline_predef_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX];
    int             baseline_predef_count;
    float           baseline_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    int             saved_t_playing;
    int             last_src_line;
    ReplayFadeBatch fade_batches[REPLAY_FADE_BATCH_MAX];
    int             fade_batch_count;
} ReplayRuntimeState;

/* Copy or reset the full replay runtime snapshot.
 * Note: replay_state_capture/restore is exclusively a test/verification contract;
 * production code does not capture/restore replay state. */
void                     replay_state_capture(ReplayRuntimeState *snapshot);
void                     replay_state_restore(const ReplayRuntimeState *snapshot);
void                     replay_state_reset(void);

/* Snapshot-build accessor: returns the full ReplayRuntimeState by value.
 * Use the narrow accessors below for individual fields; `replay_state_view()` is
 * mainly for per-frame snapshot assembly and the few callers that genuinely need
 * the whole struct at once. */
ReplayRuntimeState   replay_state_view(void);

/* Const accessor for read-only access to live storage without heap/stack copies. */
const ReplayRuntimeState *replay_state_const(void);

/* Mutable accessor for the small set of writers that update multiple replay
 * fields together. Most readers should stay on the by-value or narrow accessors. */
ReplayRuntimeState  *replay_state_mut(void);

/* --- Narrow read accessors ---
 *
 * Single-field queries for callers that only need one replay attribute. They
 * keep most readers from depending on the full ReplayRuntimeState layout.
 */
int    replay_active(void);          /* .active */
int    replay_machine_state(void);   /* .state — REPLAY_OFF/PLAYING/PAUSED/DONE */
int    replay_pc(void);              /* .pc — current program counter */
int    replay_mode(void);            /* .mode — REPLAY_MODE_VERTEX/POLYGON */
float  replay_speed(void);           /* .speed — playback steps/sec */
int    replay_src_line(void);        /* .src_line_idx — source line of current cmd */
int    replay_total_flat(void);      /* .total_flat_cmds — captured at start */
int    replay_expand_args(void);     /* .expand_args — annotation expansion toggle */

/* --- Handler API ---
 *
 * Small routing surface for controller/editor input code. UI hit handling calls
 * replay_handle_pin_clicked(); keyboard dispatch forwards to replay_handle_key()
 * / replay_handle_special() while replay is active. Implementations delegate to
 * the main replay state machine in replay.c.
 */

/* Toggle replay state on a Replay-pin button click: starts replay
 * when stopped, pauses when playing, resumes when paused, restarts
 * from the beginning when DONE. */
void replay_handle_pin_clicked(void);



#endif /* REPLAY_STATE_H */
