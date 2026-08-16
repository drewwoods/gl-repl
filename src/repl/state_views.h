/*
 * src/repl/state_views.h -- REPL-owned runtime slice types and read API.
 *
 * Defines the value structs stored in REPL runtime state, along with the
 * read-only/by-value accessors that expose those slices to the pipeline,
 * controller, and tests. Companion state owned by the editor, UI, replay peer,
 * or app shell lives in their own headers; this file keeps the REPL-owned
 * shapes plus a few shared snapshot structs that other owners pass around.
 */
#ifndef REPL_STATE_VIEWS_H
#define REPL_STATE_VIEWS_H

#include "repl/export_state.h"
#include "config.h"          /* REPL_STATUS_TEXT_MAX */
#include "repl/flatten.h"

#ifndef REPL_WORKSPACE_DIR_MAX
#define REPL_WORKSPACE_DIR_MAX 1024
#endif

#ifndef USER_SCENE_NAME_MAX
#define USER_SCENE_NAME_MAX 64
#endif

/* Number of GL light slots the REPL tracks enable/disable for: GL_LIGHT0
 * through GL_LIGHT0+REPL_LIGHT_SLOT_COUNT-1. The dimensional light data
 * (positions / colors / eye-space) is presentation state owned by the app
 * shell (GlrRenderState.lights, seeded from a render3d light theme); the REPL
 * pipeline owns only which slots the program enabled, as a bitmask. The
 * controller STATIC_ASSERTs this count equals render3d's MAX_LIGHTS. */
#define REPL_LIGHT_SLOT_COUNT 4

/* Non-zero when slot `slot` (0..REPL_LIGHT_SLOT_COUNT-1) is enabled in the
 * given light-enable bitmask. */
static inline int repl_light_enabled(unsigned mask, int slot) {
    return (int)((mask >> slot) & 1u);
}

/* Source document storage: canonical source commands, source-level dirty flags,
 * and cached source dependency metadata. The edit-line cursor is editor-owned;
 * REPL pipeline code receives it as an argument or through the dispatch sink. */
typedef struct {
    GLCmd cmds[MAX_EDITOR_COMMANDS];
    int   cmd_count;
    int   capacity;
    int   normals_dirty, source_uses_time, source_uses_time_dirty;
} ReplDocumentState;

/* Expanded flat program rebuilt from the source document: flattened commands,
 * per-flat-command local-variable bindings, and the current cursor-block range
 * used by highlight/guides/replay helpers. */
typedef struct {
    GLCmd            cmds[MAX_FLAT_COMMANDS];
    FlatCmdLocalVars local_vars[MAX_FLAT_COMMANDS];
    int              cmd_count;
    int              capacity;
    /* Exact command count needed after a capacity-only flatten failure.
     * Zero while cmd_count names a valid executable program. */
    int              overflow_cmd_count;
    int              dirty;
    int              user_lighting_enabled;
    int              current_block_begin_idx;
    int              current_block_end_idx;
    int              current_block_source_line_idx;
    /* Dependency-routing state. The two dep masks
     * describe the CURRENT flat program - refreshed by every full flatten:
     * structural_dep_mask holds the predef roots that can change topology
     * or frozen local snapshots (loop bounds, if conditions, call args);
     * value_dep_mask holds the roots any baked value/assignment reads.
     * args_dirty_mask accumulates value-changed roots since the last
     * refresh (routed in via repl_state_notify_predef_value_changed);
     * full dirty always subsumes and clears it. rebake_ok is 1 when every
     * has_vars flat command has compiled programs, i.e. an in-place rebake
     * can re-evaluate the whole stream; 0 escalates value changes to a
     * full flatten. */
    ReplExprDepMask  structural_dep_mask;
    ReplExprDepMask  value_dep_mask;
    ReplExprDepMask  args_dirty_mask;
    int              rebake_ok;
    /* Flat-only call-frame intern. Parallel to cmds[]: call_frame_idx[i]
     * is REPL_CALL_FRAME_NONE for top-level commands and for every
     * command after the overflow latch. Topology + argument arena are
     * rebuilt by a full flatten and left untouched by in-place rebake. */
    int              call_frame_idx[MAX_FLAT_COMMANDS];
    ReplCallFrame    call_frames[MAX_CALL_FRAMES];
    int              call_frame_count;
    float            call_frame_args[MAX_CALL_FRAME_ARGS];
    int              call_frame_arg_count;
    int              call_frame_overflow;
} ReplFlatProgramState;

/* Predefined-variable runtime: named float table, scratch arrays, optional
 * funcN aliases, and the special time variable's playhead state. */
typedef struct {
    ExprVar predef_vars[MAX_PREDEF_VARS];
    int     predef_var_count;
    float   scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    /* Per-slot user alias for funcN (0..9). Empty string == no alias,
     * the slot is referenced as bare `funcN`. Round-trips through
     * capture/restore + workspace import/export; per-scene table is
     * saved alongside predef vars in user-scene slots. */
    char    func_aliases[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX];
    int     time_var_idx;
    int     time_playing;
    float   anim_time;
} ReplVariableState;

/* Read-only projection of ReplVariableState for callers that need the current
 * bindings and scratch-array dimensions without mutable access. */
typedef struct {
    const ExprVar *vars;
    int            var_count;
    const float  (*scratch_arrays)[REPL_SCRATCH_ARRAY_LEN];
    int            scratch_array_count;
    int            scratch_array_len;
    int            time_var_idx;
    int            time_playing;
    float          anim_time;
} ReplVariableView;

/* REPL-owned render tail: the light-enable bitmask written by user GL
 * commands. Bit i is set while the program's glEnable(GL_LIGHT0+i) is in
 * effect (recomputed each executor walk); the light-indicator overlay reads
 * it, and that consumer - which cannot see GL's own state - is the whole
 * reason this mirror exists. Nothing else is mirrored here: the frame
 * background is not a *state* the host reads back but a result the executor
 * observes while emitting the program's clears (ReplBackgroundObservation),
 * and a mirror with no consumer is not made acceptable by calling it
 * bookkeeping. Positions/colors/eye-space for each light slot are
 * presentation state on the app shell (GlrRenderState.lights). Policy toggles
 * such as msaa, line smoothing, point attenuation enablement, and grid/axes
 * visibility are app-owned in glr_state. */
typedef struct {
    unsigned light_enabled_mask;
} ReplRenderState;


/* Scene/workspace bookkeeping kept with the REPL runtime: which built-in example
 * is active, whether the live document is a retained post-tutorial result, and
 * the bound workspace directory used by scene save/load.
 *
 * `tutorial_origin_idx` is the tutorial twin of `active_example_idx` - the
 * marker that makes the first subsequent edit auto-promote the transient
 * document into a user-scene slot:
 *   -1     the live document did not come out of a completed/stopped tutorial;
 *   >= 0   the transient live document is the retained result of that tutorial.
 * It deliberately describes an *inactive post-tutorial* document, not the
 * running tutorial (which is `TutorialRuntimeState.tutorial_idx`): an ACTIVE
 * tutorial always leaves this at -1, because tutorial commands flow through
 * editor_undo_push_snapshot() and would otherwise promote - and tear the
 * tutorial down - on step 0. Only the runner's end-of-lesson path
 * (tutorial_end_keep_view in src/subsystems/tutorial/tutorial_runner.c)
 * establishes it. */
/* `example_place_idx` is the user's parked position in the example catalog,
 * kept ONLY across a promotion: an example-derived document promoted into a
 * user-scene slot clears `active_example_idx` (no example tab survives), and
 * without this the next F12 leg that walks out of the user scenes would
 * restart the catalog at example 1. It is the example twin of the completed
 * tutorial's retained `TutorialRuntimeState.tutorial_idx`.
 *   -1     no parked position - the examples leg starts at the catalog end;
 *   >= 0   resume the catalog one step past this index.
 * Loading any example supersedes it (`active_example_idx` becomes the live
 * place again), so it is set at promotion and cleared on example load. */
typedef struct {
    int  active_example_idx;
    int  example_place_idx;
    int  tutorial_origin_idx;
    char workspace_dir[REPL_WORKSPACE_DIR_MAX];
} ReplSceneRuntimeState;

/* Export/import scratch storage: prebuilt workspace-header lines, cached render
 * and camera boilerplate text, and pending scene/workspace metadata collected
 * while importing a file. */
typedef struct {
    char        workspace_header_lines[MAX_WORKSPACE_HEADER_LINES][WORKSPACE_HEADER_LINE_LEN];
    int         workspace_header_line_count;
    char        render_state_lines[RENDER_STATE_LINE_COUNT][RENDER_STATE_LINE_LEN];
    char        cam_lines[REPL_EXPORT_CAMERA_LINES][REPL_EXPORT_CAMERA_LINE_MAX];
    const char *export_scene_name_hint;
    char        pending_scene_name[USER_SCENE_NAME_MAX];
    char        pending_workspace_dir[REPL_WORKSPACE_DIR_MAX];
} ReplImportExportState;

/* Read-only view over the import/export scratch storage. Used by save/load and
 * snapshot builders that only need to inspect the cached strings. */
typedef struct {
    const char (*workspace_header_lines)[WORKSPACE_HEADER_LINE_LEN];
    int         workspace_header_line_count;
    const char (*render_state_lines)[RENDER_STATE_LINE_LEN];
    const char (*cam_lines)[REPL_EXPORT_CAMERA_LINE_MAX];
    const char *export_scene_name_hint;
    const char *pending_scene_name;
    const char *pending_workspace_dir;
} ReplImportExportView;

const GLCmd *repl_state_document_cmds(void);
const GLCmd *repl_state_document_cmd_at(int cmd_idx);
int          repl_state_document_count(void);
int          repl_state_normals_dirty(void);

const GLCmd      *repl_state_flat_program_cmds(void);
const FlatCmdLocalVars *repl_state_flat_program_local_vars(void);
int               repl_state_flat_program_count(void);
int               repl_state_flat_program_dirty(void);
ReplExprDepMask   repl_state_flat_program_structural_dep_mask(void);
ReplExprDepMask   repl_state_flat_program_value_dep_mask(void);
ReplExprDepMask   repl_state_flat_program_args_dirty_mask(void);
int               repl_state_flat_program_rebake_ok(void);
int               repl_state_flat_program_user_lighting_enabled(void);
int               repl_state_flat_program_current_block_begin(void);
int               repl_state_flat_program_current_block_end(void);
int               repl_state_flat_program_current_block_source_line(void);
FlatProgramView   repl_state_flat_program_view(void);

ReplVariableView repl_state_variables(void);

/* Read-only accessor boundary: functions below expose only REPL-owned slices.
 * For peer state, include the owning header directly:
 * `glr_state.h` for presentation policy/render config and grid tables,
 * `editor_state.h` for input/buffer/selection/search/autocomplete/overlay
 * lists, and `ui_state.h` for code-panel/help/status/pointer/viewport state. */

ReplSceneRuntimeState     repl_state_scenes(void);
int                       repl_state_active_example_idx(void);
int                       repl_state_tutorial_origin_idx(void);
const char               *repl_state_workspace_dir(void);

ReplImportExportView repl_state_import_export(void);

#endif /* REPL_STATE_VIEWS_H */
