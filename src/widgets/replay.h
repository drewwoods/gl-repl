/*
 * replay.h - Step-by-step execution visualization and state machine.
 *
 * Replay mode lets users step through an expanded command stream one command
 * at a time (or with variable speed), seeing geometry appear progressively.
 * Toggle it with Ctrl+R or the Replay pin button.
 *
 * Execution model: The replay state machine (OFF/PLAYING/PAUSED/DONE) tracks
 * a program counter (PC) into the flat command array. During playback, the
 * executor only renders commands with index < replay_exec_limit() (the PC
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

#include "repl/eval.h"
#include "repl/flatten.h"    /* FlatProgramView used by ReplVertexWalkContext */
#include <gl_includes.h>     /* GLenum for ReplVertexWalkState.primitive_mode */

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

/* Snapshot the controller assembles per frame and feeds into its
 * post-fill fade-render hook. The scene module no longer touches it —
 * it's pure REPL state. */
typedef struct ReplayFadePlan {
    int             batch_count;
    ReplayFadeBatch batches[REPLAY_FADE_BATCH_MAX];
    int             skip_limits[REPLAY_FADE_BATCH_MAX];
    float           batch_alpha[REPLAY_FADE_BATCH_MAX];
    float           baseline_predef_vals[MAX_PREDEF_VARS];
    float           baseline_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
} ReplayFadePlan;

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

void replay_start(void);                /* Enter PLAYING state */
void replay_stop(void);                 /* Enter OFF state, clear history */
void replay_advance(void);              /* Increment PC by 1 */
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
int  replay_fill_base_limit(void);      /* Highest PC reached so far */

/* Compute skip limits for performance: scene_render.c can skip rendering
 * commands in ranges where no fade is active (optimization). */
int  replay_compute_fade_skip_limits(int *out_limits, int max_count);

ReplayFadeBatchView replay_fade_batches_view(void);
float replay_batch_alpha(const ReplayFadeBatch *batch);

/* Per-frame: updates exec_limit based on speed multiplier and pause state,
 * captures geometry snapshots for new fades. Called each frame. */
int  replay_prepare_frame(int full_flat_count);

/* --- Vertex-overlay walk (vertex numbers / normal vectors / vertex dots) -
 *
 * Walks the user's flat program with transform tracking, dispatching the
 * supplied callbacks at every (valid) flat cmd and at every emitted vertex.
 * The walker handles glPushMatrix / glTranslatef / glRotatef / glScalef /
 * glPopMatrix internally — scene-side overlay code never has to iterate
 * GLCmd or know how to translate REPL command kinds into GL transforms.
 *
 * The walker is a pure function: program + cursor metadata are all passed
 * in via ReplVertexWalkContext, no global state is read. That keeps the
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
typedef struct ReplVertexWalkState {
    int    flat_cmd_idx;       /* current flat-program index */
    int    src_cmd_idx;        /* source-line index of the current cmd */
    GLenum primitive_mode;     /* current BEGIN's mode (0 if not in a block) */
    int    in_block;           /* inside CMD_BEGIN..CMD_END or tess polygon */
    int    block_selected;     /* current block matches the cursor */
    int    vertex_idx_in_block;
    float  normal[3];          /* most recent CMD_NORMAL3F / CMD_TESS_NORMAL value */
} ReplVertexWalkState;

typedef struct ReplVertexWalkContext {
    FlatProgramView program;
    int          edit_line_idx;
    int          cursor_block_begin;
    int          cursor_block_end;
    unsigned int cursor_func_scope_mask;
    int          selected_block_only;
} ReplVertexWalkContext;

typedef struct ReplVertexWalkCallbacks {
    /* Fires once per valid flat cmd before the walker dispatches the cmd
     * type or applies a transform. Lets callers insert per-position
     * actions (e.g. cursor-line guide rendering). Vertex / normal coords
     * on the state are not meaningful for this hook — only the
     * positional / cursor / block fields. */
    void (*on_each_cmd)(const ReplVertexWalkState *state, void *user_data);

    /* Fires for every CMD_VERTEX2F / CMD_VERTEX3F / CMD_TESS_VERTEX hit
     * during the walk, with (vx, vy, vz) extracted from the cmd's args. */
    void (*on_vertex)(const ReplVertexWalkState *state,
                      float vx, float vy, float vz,
                      void *user_data);
} ReplVertexWalkCallbacks;

void replay_walk_user_vertices(const ReplVertexWalkContext *ctx,
                             const ReplVertexWalkCallbacks *cb,
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
typedef struct ReplTessPreviewCallbacks {
    void (*begin_contour)(void *user_data);
    void (*vertex)(float x, float y, float z, void *user_data);
    void (*end_contour)(void *user_data);
} ReplTessPreviewCallbacks;

void replay_walk_tess_preview(const ReplTessPreviewCallbacks *cb,
                                   void *user_data);

/* --- Variable state for step-back --------------------------------------- */

void replay_restore_baseline_predef_values(void);
void replay_copy_baseline_predef_values(float *dst, int max_vals);
void replay_restore_baseline_scratch_arrays(void);
void replay_copy_baseline_scratch_arrays(
    float dst[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]);

/* --- Input routing (called from repl_editor.c) ----------------------- */

int  replay_handle_key_impl(unsigned char key);
int  replay_handle_special_key_impl(int key);

/* --- Benchmark / test helpers ----------------------------------------- */

int  replay_bench_fade_install(const int *old_pcs, const int *new_pcs,
                             int count, float age);
void replay_bench_fade_clear(void);

#endif /* REPLAY_H */
