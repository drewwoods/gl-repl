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
#include "scene/render_types.h"
#include "repl/flatten.h"

#ifndef REPL_WORKSPACE_DIR_MAX
#define REPL_WORKSPACE_DIR_MAX 1024
#endif

#ifndef USER_SCENE_NAME_MAX
#define USER_SCENE_NAME_MAX 64
#endif

/* Source document storage: canonical source commands, current edit-line index,
 * and the source-level auto-normal dirty flag. */
typedef struct {
    GLCmd cmds[MAX_COMMANDS];
    int   cmd_count;
    int   capacity;
    int   edit_line_idx;
    int   normals_dirty;
} ReplDocumentState;

/* Expanded flat program rebuilt from the source document: flattened commands,
 * per-flat-command local-variable bindings, and the current cursor-block range
 * used by highlight/guides/replay helpers. */
typedef struct {
    GLCmd            cmds[MAX_COMMANDS];
    FlatCmdLocalVars local_vars[MAX_COMMANDS];
    int              cmd_count;
    int              capacity;
    int              dirty;
    int              user_lighting_enabled;
    int              current_block_begin_idx;
    int              current_block_end_idx;
    int              current_block_source_line_idx;
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

/* State declared elsewhere:
 * - editor-owned input/buffer/selection/search/autocomplete and editor overlay
 *   list types live in editor_state.h
 * - UI chrome types live in src/ui/state_types.h
 * - app-owned presentation policy and render-config toggles live in glr_state.h
 *
 * Phase 1 of the state split and step 7a of
 * feature/decouple-repl-from-gl-repl-alt.md moved those declarations to their
 * owning modules. This header keeps only the REPL-owned render tail that the
 * executor mutates directly. */
/* REPL-owned render tail: mutable per-light state and clear color written by
 * user GL commands. Policy toggles such as msaa, line smoothing, point
 * attenuation enablement, and grid/axes visibility are app-owned in glr_state. */
typedef struct {
    SceneLight lights[MAX_LIGHTS];
    float      clear_color[4];
} ReplRenderState;

/* Replay snapshot shape shared with the replay peer subsystem. Storage lives in
 * src/widgets/replay_state.c; this value type stays here so snapshots and UI
 * bundles can pass replay state by value without depending on the peer's API. */
typedef struct {
    int   active;
    int   state;
    int   pc;
    int   mode;
    float speed;
    float accum;
    float fade_speed;
    int   src_line_idx;
    int   total_flat_cmds;
    int   expand_args;
} ReplReplayRuntimeState;

/* Scene/workspace bookkeeping kept with the REPL runtime: which built-in example
 * is active, and the bound workspace directory used by scene save/load. */
typedef struct {
    int  active_example_idx;
    char workspace_dir[REPL_WORKSPACE_DIR_MAX];
} ReplSceneRuntimeState;

/* Export/import scratch storage: prebuilt workspace-header lines, cached render
 * and camera boilerplate text, and pending scene/workspace metadata collected
 * while importing a file. */
typedef struct {
    char        workspace_header_lines[MAX_WORKSPACE_HEADER_LINES][WORKSPACE_HEADER_LINE_LEN];
    int         workspace_header_line_count;
    char        render_state_lines[RENDER_STATE_LINE_COUNT][64];
    char        cam_lines[CAM_LINE_COUNT][96];
    const char *export_scene_name_hint;
    char        pending_scene_name[USER_SCENE_NAME_MAX];
    char        pending_workspace_dir[REPL_WORKSPACE_DIR_MAX];
} ReplImportExportState;

/* Read-only view over the import/export scratch storage. Used by save/load and
 * snapshot builders that only need to inspect the cached strings. */
typedef struct {
    const char (*workspace_header_lines)[WORKSPACE_HEADER_LINE_LEN];
    int         workspace_header_line_count;
    const char (*render_state_lines)[64];
    const char (*cam_lines)[96];
    const char *export_scene_name_hint;
    const char *pending_scene_name;
    const char *pending_workspace_dir;
} ReplImportExportView;

const GLCmd *repl_state_document_cmds(void);
const GLCmd *repl_state_document_cmd_at(int cmd_idx);
int          repl_state_document_count(void);
int          repl_state_document_capacity(void);
int          repl_state_edit_line(void);
int          repl_state_normals_dirty(void);
void         repl_state_document_reset(void);

const GLCmd      *repl_state_flat_program_cmds(void);
int               repl_state_flat_program_count(void);
int               repl_state_flat_program_dirty(void);
int               repl_state_flat_program_user_lighting_enabled(void);
int               repl_state_flat_program_current_block_begin(void);
int               repl_state_flat_program_current_block_end(void);
int               repl_state_flat_program_current_block_source_line(void);
FlatProgramView   repl_state_flat_program_view(void);

ReplVariableView repl_state_variables(void);

/* Read-only accessor boundary: functions below expose only REPL-owned slices.
 * For the rest of runtime state, include the owning header directly:
 * `glr_state.h` for presentation policy/render config and grid tables,
 * `editor_state.h` for input/buffer/selection/search/autocomplete/overlay
 * lists, and `ui_state.h` for code-panel/help/status/pointer/viewport state.
 * Phase 1 and step 7a removed the old `repl_state_*` forwarders. */

ReplSceneRuntimeState     repl_state_scenes(void);
const char *repl_state_workspace_dir(void);

ReplImportExportView repl_state_import_export(void);

#endif /* REPL_STATE_VIEWS_H */
