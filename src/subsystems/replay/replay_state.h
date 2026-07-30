/*
 * replay_state.h - Replay peer subsystem storage and accessors.
 *
 * Owns the transient playback state that should not live in the REPL document
 * model: replay on/off, machine state, program counter, playback speed, source
 * line tracking, annotation expansion mode, and replay-only overlay toggles.
 * The replay subsystem logic in
 * src/subsystems/replay/replay_playback.c, replay_fade.c, replay_input.c, and
 * replay.c writes this state; controller/UI code reads it for buttons,
 * shortcuts, and per-frame snapshots.
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

typedef enum {
    REPLAY_NORMAL_DISPLAY_OFF = 0,
    REPLAY_NORMAL_DISPLAY_VECTOR,
    REPLAY_NORMAL_DISPLAY_DIRECTION,
    REPLAY_NORMAL_DISPLAY_COUNT
} ReplayNormalDisplayMode;

/* Code-panel detail during replay. The middle mode keeps live value
 * annotations on assignments, loop headers, function definitions, and other
 * commands. It leaves vertex, color, and normal source rows as a single line
 * with the evaluated call appended as a comment instead of adding substituted
 * + evaluated virtual rows beneath them. */
typedef enum {
    REPLAY_EXPAND_OFF = 0,
    REPLAY_EXPAND_EXPANDED,
    REPLAY_EXPAND_VERBOSE,
    REPLAY_EXPAND_COUNT
} ReplayExpandMode;

#define REPLAY_EXPAND_DEFAULT REPLAY_EXPAND_EXPANDED

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
 * The predef baseline is a full snapshot (names + count, not just
 * floats) so fade restore can assign by name. Without that, a workspace
 * switch or scene load during an active replay reshapes the predef table
 * and a values-only restore writes saved floats into slots now holding
 * different vars. */
typedef struct ReplayFadePlan {
    int             batch_count;
    ReplayFadeBatch batches[REPLAY_FADE_BATCH_MAX];
    int             skip_limits[REPLAY_FADE_BATCH_MAX];
    float           batch_alpha[REPLAY_FADE_BATCH_MAX];
    ReplPredefSnapshot baseline_predef;
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
 * restore assigns by name into whichever slot currently holds that
 * name. */
typedef struct {
    int             active;
    int             state;
    int             pc;
    int             mode;
    float           speed;
    float           accum;
    float           fade_speed;
    int             src_line_idx;
    int             focus_call_depth;    /* funcN call-frame depth of the focused
                                          * command (0 = top-level); drives the HUD
                                          * "depth N" readout. Derived from the same
                                          * focused flat command as src_line_idx. */
    int             total_flat_cmds;
    int             step_begin;          /* flat index where the current replay
                                          * step begins (the [step_begin, pc) range).
                                          * Maintained incrementally as pc moves so
                                          * replay_focus_flat_idx() scans only the
                                          * active step instead of re-deriving the
                                          * begin via replay_prev_limit(pc) — an
                                          * O(N^2) per-frame walk — every frame. */
    int             expand_args;         /* ReplayExpandMode */
    int             normal_display;      /* ReplayNormalDisplayMode */
    int             vertex_label;        /* 1 = label focused replay vertex */
    ReplPredefSnapshot baseline_predef;
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
int    replay_normal_display(void);  /* .normal_display — ReplayNormalDisplayMode */
int    replay_vertex_label(void);    /* .vertex_label */
int    replay_src_line(void);        /* .src_line_idx — source line of current cmd */
int    replay_total_flat(void);      /* .total_flat_cmds — captured at start */

/* Flat-program index of the command the current replay step focuses on:
 * the last focus-candidate command in the active step range. Unlike
 * replay_pc() — which is the execution *limit* and points one past the
 * step's last emitted command (or at flat_count) — this resolves to the
 * actual current command, so callers can read its provenance
 * (call_src_cmd_idx, root_call_src_cmd_idx, func_scope_mask, ...). The
 * step range is derived the same way step-back is, so advance, seek,
 * step-back, and paused replay all agree. Returns -1 when replay is
 * inactive or the step has no focus-candidate command. */
int    replay_focus_flat_idx(void);

/* Flat-program index of the draw the current replay step emitted — the last
 * repl_cmd_consumes_current_color() command (glVertex / gluVertex *or* a
 * glutSolid*) in the active step range. Used to anchor the replay affecting-
 * transform highlight and the live transform guide on that exact draw. Returns
 * -1 when replay is inactive, not in vertex mode, or the step emitted no draw. */
int    replay_focus_anchor_flat_idx(void);

/* --- Handler API ---
 *
 * Small routing surface for controller/editor input code. UI hit handling calls
 * replay_handle_pin_clicked(); keyboard dispatch forwards to replay_handle_key()
 * / replay_handle_special() while replay is active. Implementations delegate to
 * the main replay state machine in replay.c.
 */

/* Handle a Replay-pin button click: starts replay when stopped, pauses when
 * playing, resumes when paused, and stops replay when the button shows DONE. */
void replay_handle_pin_clicked(void);



#endif /* REPLAY_STATE_H */
