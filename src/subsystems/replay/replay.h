/*
 * replay.h - Replay state machine, fade planning, and replay-side walkers.
 *
 * Replay lets the app step through the flat program one command at a time or at
 * variable speed, showing geometry accumulate progressively. The controller uses
 * this API to start/stop playback, seek to the cursor line, advance or rewind
 * the program counter, and build the fade batches and overlay walks the render
 * path needs.
 *
 * Execution model: the replay machine (OFF/PLAYING/PAUSED/DONE) tracks a
 * program counter into the flat command array. During playback, the executor
 * renders only commands with index `< replay_exec_limit()`. As the PC advances,
 * older geometry is captured into fading batches so it can fade out while new
 * geometry fades in.
 *
 * Current controls forwarded by the controller/editor input chain:
 *   - Ctrl+R toggles replay through the controller's config-shortcut path.
 *   - Ctrl+K jumps to the current edit line and pauses there.
 *   - Space toggles play/pause or restarts from DONE.
 *   - Left/Right step one replay unit; Up/Down and +/- adjust speed.
 *   - M switches vertex/polygon replay mode; E cycles code expansion.
 *   - N cycles replay normal display: off, vector, vector + direction.
 *   - V toggles the replay focused-vertex label.
 *   - Esc stops replay.
 *
 * Step-back / seek restore baseline predefined-variable and scratch-array state
 * before re-executing from the new PC, so replay stays deterministic even when
 * expressions depend on time or slider-controlled values.
 */
#ifndef REPLAY_H
#define REPLAY_H

#include "subsystems/replay/replay_state.h"
#include "repl/flatten.h"
#include "gl_includes.h"



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

/* ReplayFadeBatch / ReplayFadeBatchView / REPLAY_FADE_BATCH_MAX are defined above.
 * The replay peer produces batches; the controller consumes them in its
 * per-frame fade-render hook. */

/* --- State machine control -------------------------------------------- */

void replay_start(void);                /* Enter PLAYING state */
void replay_stop(void);                 /* Enter OFF state, clear history */
void replay_advance(FlatProgramView flat_program);              /* Increment PC by 1 */
void replay_tick_fade_batches(float dt);/* Age and decay active fades */

/* --- Seek operations --------------------------------------------------- */

void replay_seek(int new_pc);           /* Jump to PC; restores baseline vars */
int  replay_seek_to_src_line(int target_line);
void replay_step_back(void);            /* Retreat PC; re-execute from new point */
void replay_restart_from_beginning(void);

/* --- Speed / playback control ----------------------------------------- */

void replay_speed_adjust(float factor); /* Multiply speed by factor (1.5 = faster) */

/* Toggle the Replay pin button state. Playing pauses, paused resumes, and
 * stopped/done states restart playback from the beginning. */
void replay_toggle_play_pause(void);

/* --- Query / renderer helpers ----------------------------------------- */

int  replay_exec_limit(void);           /* Current PC (what executor renders to) */
int  replay_has_active_fades(void);     /* Any fades currently visible? */
int  replay_fill_base_limit(FlatProgramView flat_program);      /* Highest PC reached so far */
/* Pc just past the program's leading glClear (0 if it doesn't open with
 * one): the floor below which the replay clamps never cut a frame, so
 * the scene rect's clear still runs while the PC / oldest fade batch
 * sits at 0. */
int  replay_frame_setup_limit(FlatProgramView flat_program);

/* Compute skip limits for performance: render3d_render.c can skip rendering
 * commands in ranges where no fade is active (optimization). */
int  replay_compute_fade_skip_limits(FlatProgramView flat_program, int *out_limits, int max_count);

ReplayFadeBatchView replay_fade_batches_view(void);
float replay_batch_alpha(const ReplayFadeBatch *batch);

/* Per-frame: updates exec_limit based on speed multiplier and pause state,
 * captures geometry snapshots for new fades. Called each frame. */
int  replay_prepare_frame(FlatProgramView flat_program, int full_flat_count);

/* --- Vertex-overlay walk (vertex numbers / normal vectors / vertex dots) -
 *
 * Walks the user's flat program with transform tracking, dispatching the
 * supplied callbacks at every (valid) flat cmd and at every emitted vertex.
 * The walker handles glPushMatrix / glTranslatef / glRotatef / glScalef /
 * glPopMatrix internally - scene-side overlay code never has to iterate
 * GLCmd or know how to translate REPL command kinds into GL transforms.
 *
 * The walker is a pure function: program + cursor metadata are all passed
 * in via ReplayVertexWalkContext, no global state is read. That keeps the
 * call testable in isolation and lets non-REPL callers (the standalone
 * teapot demo) skip the walker entirely without dragging in REPL state.
 *
 * If ctx->selected_block_only is non-zero, on_vertex is only fired for
 * vertices inside the cursor's selected block (block_selected is 1
 * then); otherwise on_vertex fires for every vertex with block_selected
 * forced to 1.
 *
 * Modelview state on entry is the caller's; the walker pushes/pops to
 * cover its own transform tracking and leaves the caller's state intact. */
typedef struct ReplayVertexWalkState {
    int    flat_cmd_idx;       /* current flat-program index */
    int    src_cmd_idx;        /* source-line index of the current cmd */
    GLenum primitive_mode;     /* current BEGIN's mode (0 if not in a block) */
    int    in_block;           /* inside CMD_BEGIN..CMD_END or tess polygon */
    int    block_selected;     /* current block matches the cursor */
    int    vertex_idx_in_block;
    int    lighting_enabled;   /* current GL_LIGHTING state before this vertex */
    float  normal[3];          /* most recent CMD_NORMAL3F value */
    float  tess_normal[3];     /* most recent CMD_TESS_NORMAL value */
} ReplayVertexWalkState;

typedef struct ReplayVertexWalkContext {
    FlatProgramView  program;
    CursorBlockState cursor;
    int              selected_block_only;
    /* Optional early-stop signal. When non-NULL, the walker checks
     * *stop_flag after every on_each_cmd / on_vertex callback and
     * breaks out of the iteration if it has been set non-zero. Lets
     * callers (e.g. cursor-guide rendering) skip the rest of a huge
     * flat program once the work they care about has been done. */
    int             *stop_flag;
} ReplayVertexWalkContext;

typedef struct ReplayVertexWalkCallbacks {
    /* Fires once per valid flat cmd before the walker dispatches the cmd
     * type or applies a transform. Lets callers insert per-position
     * actions (e.g. cursor-line guide rendering). Vertex / normal coords
     * on the state are not meaningful for this hook - only the
     * positional / cursor / block fields. */
    void (*on_each_cmd)(const ReplayVertexWalkState *state, void *user_data);

    /* Fires once after the last valid command, before the walker unwinds
     * its tracked modelview stack. This gives cursor overlays a virtual
     * EOF position inside an unterminated glBegin block, matching the
     * executor's tolerated trailing glEnd cleanup. */
    void (*on_end)(const ReplayVertexWalkState *state, void *user_data);

    /* Fires for every CMD_VERTEX2F / CMD_VERTEX3F / CMD_TESS_VERTEX hit
     * during the walk, with (vx, vy, vz) extracted from the cmd's args. */
    void (*on_vertex)(const ReplayVertexWalkState *state,
                      float vx, float vy, float vz,
                      void *user_data);
} ReplayVertexWalkCallbacks;

void replay_walk_user_vertices(const ReplayVertexWalkContext *ctx,
                             const ReplayVertexWalkCallbacks *cb,
                             void *user_data);

/* --- Tess-preview walk (replay polygon-mode wireframe overlay) ----------
 *
 * Walks the flat program, applies the user's CMD_TRANSLATE/SCALE/ROTATE/
 * PUSH/POP transforms via GL, and dispatches the supplied callbacks at
 * CMD_TESS_BEGIN_CONTOUR / CMD_TESS_VERTEX / CMD_TESS_END so the visual
 * layer can emit line-strip primitives at the right transformed
 * positions. Lets the scene module stay free of GLCmd iteration: the
 * scene controller installs callbacks that just call glBegin / glVertex /
 * glEnd, with no knowledge of the REPL flat program.
 *
 * Modelview state on entry is the caller's; the walker pushes/pops to
 * cover its own transform tracking and leaves the caller's state intact. */
typedef struct ReplayTessPreviewCallbacks {
    void (*begin_contour)(void *user_data);
    void (*vertex)(float x, float y, float z, void *user_data);
    void (*end_contour)(void *user_data);
} ReplayTessPreviewCallbacks;

void replay_walk_tess_preview(const ReplayTessPreviewCallbacks *cb,
                                   void *user_data);

/* --- Variable state for step-back --------------------------------------- */

/* Restore the saved predef table by NAME (full snapshot) - see the
 * ReplayRuntimeState.baseline_predef comment for why a values-only
 * restore is unsafe across a multi-frame replay. */
void replay_restore_baseline_predef_values(void);

/* Copy the full predef baseline snapshot. Used by glr_ctrl.c to seed the
 * per-frame fade plan without exposing the snapshot's storage layout. */
void replay_copy_baseline_predef_snapshot(ReplPredefSnapshot *dst);

void replay_restore_baseline_scratch_arrays(void);
void replay_copy_baseline_scratch_arrays(
    float dst[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]);

/* --- Input routing (called from controller/editor dispatch) ----------- */

int  replay_handle_key(unsigned char key);
int  replay_handle_special(int key);

/* --- Benchmark / test helpers ----------------------------------------- */

int  replay_bench_fade_install(const int *old_pcs, const int *new_pcs,
                             int count, float age);
void replay_bench_fade_clear(void);

#endif /* REPLAY_H */
