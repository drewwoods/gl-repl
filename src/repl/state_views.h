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
 * shell (GlrRenderState.lights, seeded from a scene light theme); the REPL
 * pipeline owns only which slots the program enabled, as a bitmask. The
 * controller STATIC_ASSERTs this count equals scene's MAX_LIGHTS. */
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

/* REPL-owned render tail: the light-enable bitmask and clear color written by
 * user GL commands. `light_enabled_mask` bit i is set while the program's
 * glEnable(GL_LIGHT0+i) is in effect (recomputed each executor walk); the
 * light-indicator overlay reads it. Positions/colors/eye-space for each slot
 * are presentation state on the app shell (GlrRenderState.lights). Policy
 * toggles such as msaa, line smoothing, point attenuation enablement, and
 * grid/axes visibility are app-owned in glr_state. */
typedef struct {
    unsigned light_enabled_mask;
    float    clear_color[4];
} ReplRenderState;


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
int          repl_state_document_capacity(void);
int          repl_state_normals_dirty(void);

const GLCmd      *repl_state_flat_program_cmds(void);
const FlatCmdLocalVars *repl_state_flat_program_local_vars(void);
int               repl_state_flat_program_count(void);
int               repl_state_flat_program_dirty(void);
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
const char               *repl_state_workspace_dir(void);

ReplImportExportView repl_state_import_export(void);

#endif /* REPL_STATE_VIEWS_H */
