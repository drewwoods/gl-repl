/*
 * repl_state_views.h -- REPL runtime-state value/view types and read API.
 */
#ifndef REPL_STATE_VIEWS_H
#define REPL_STATE_VIEWS_H

#include "sample.h"
#include "repl_flatten.h"
#include "ui_editor.h"

#ifndef MAX_WORKSPACE_HEADER_LINES
#define MAX_WORKSPACE_HEADER_LINES 48
#endif

#ifndef WORKSPACE_HEADER_LINE_LEN
#define WORKSPACE_HEADER_LINE_LEN  96
#endif

#ifndef REPL_WORKSPACE_DIR_MAX
#define REPL_WORKSPACE_DIR_MAX 1024
#endif

#ifndef REPL_STATUS_TEXT_MAX
#define REPL_STATUS_TEXT_MAX 256
#endif

#ifndef USER_SCENE_NAME_MAX
#define USER_SCENE_NAME_MAX 64
#endif

typedef struct {
    GLCmd cmds[MAX_COMMANDS];
    int   cmd_count;
    int   capacity;
    int   edit_line_idx;
    int   normals_dirty;
} ReplDocumentState;

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

typedef struct {
    ExprVar predef_vars[MAX_PREDEF_VARS];
    int     predef_var_count;
    int     time_var_idx;
    int     time_playing;
    float   anim_time;
} ReplVariableState;

typedef struct {
    const ExprVar *vars;
    int            var_count;
    int            time_var_idx;
    int            time_playing;
    float          anim_time;
} ReplVariableView;

/* ReplEditorInputState / ReplEditorInputView typedefs moved to
 * editor_state.h alongside the EditorState struct that owns them
 * (Phase 1 commit 5). The ReplEditorBuffer typedef moved earlier
 * (Phase 1 commit 4). */

/* ReplSelectionState / ReplClipboardState typedefs moved to
 * editor_state.h alongside the EditorState struct that owns them
 * (Phase 1 commit 6). */

/* Code-panel UI chrome: panel divider, cursor blink + pixel position
 * the renderer uses. The scroll fields used to live here too; Phase 1
 * commit 11 split them out into EditorState.scroll because scroll is
 * an editing-session concern, not a render-chrome one. */
typedef struct {
    float panel_frac;
    int   resizing_panel;
    int   cursor_visible;
    int   blink_tick;
    int   cursor_px;
    int   cursor_py;
} ReplCodePanelRuntimeState;

typedef struct {
    int visible;
    int tab_idx;
    int scroll;
} ReplHelpState;

typedef struct {
    int visible;
} ReplVariablePanelState;

typedef struct {
    int mode;
} ReplProfilePanelState;

typedef struct {
    char text[REPL_STATUS_TEXT_MAX];
    int  ttl;
} ReplStatusState;

/* ReplSearchState / ReplAutocompleteState typedefs moved to
 * editor_state.h alongside the EditorState struct that owns them
 * (Phase 1 commit 7). */

typedef struct {
    float rx;
    float ry;
    float dist;
    float tx;
    float ty;
    float tz;
    float motion_glow;
    int   auto_rotate;
} ReplCameraState;

typedef struct {
    int mouse_x;
    int mouse_y;
    int mouse_button;
} ReplPointerState;

typedef struct {
    int window_w;
    int window_h;
} ReplViewportState;

typedef struct {
    int   wireframe;
    int   grid_theme;
    int   grid_major_idx;
    int   grid_extent_idx;
    int   axes_theme;
    int   show_vertex_labels;
    int   show_normal_vectors;
    int   show_vertex_indices;
    int   show_vertex_outlines;
    int   show_vertex_points;
    int   show_vertex_guides;
    int   xform_guide_mode;
    int   autonormal;
    int   show_light_indicators;
    int   backdrop_mode;
    int   highlight_current_poly;
    int   ortho_mode;
    int   wrap_at_comma;
    int   code_panel_layout;
    float focus_vertex[3];
    int   focus_vertex_valid;
} ReplPresentationState;

typedef struct {
    int        use_accum;
    int        accum_aa_enabled;
    int        accum_samples;
    float      accum_jitter_x;
    float      accum_jitter_y;
    int        multisample_enabled;
    int        line_smooth_enabled;
    int        point_attenuation_enabled;
    SceneLight lights[MAX_LIGHTS];
    float      clear_color[4];
} ReplRenderState;

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

typedef struct {
    int  active_example_idx;
    char workspace_dir[REPL_WORKSPACE_DIR_MAX];
} ReplSceneRuntimeState;

/* ReplVariableDragState typedef moved to editor_state.h alongside the
 * EditorState struct that owns it (Phase 1 commit 9). */

typedef struct {
    char        workspace_header_lines[MAX_WORKSPACE_HEADER_LINES][WORKSPACE_HEADER_LINE_LEN];
    int         workspace_header_line_count;
    char        render_state_lines[RENDER_STATE_LINE_COUNT][64];
    char        cam_lines[CAM_LINE_COUNT][96];
    const char *export_scene_name_hint;
    char        pending_scene_name[USER_SCENE_NAME_MAX];
    char        pending_workspace_dir[REPL_WORKSPACE_DIR_MAX];
} ReplImportExportState;

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

/* Editor-input + editor-buffer accessors moved to editor_state.h
 * (Phase 1 commits 4-5). Use `editor_state_input` for the input view,
 * `editor_state_buffer` for the buffer view, and the slice-level
 * `editor_buffer_*` API for line text. */

/* Per-frame editor overlay snapshots, read-only. UI input handlers read
 * these between frames; renderers prefer the UiRenderSnapshot copy. */
/* Editor overlay snapshot list view accessors moved to editor_state.h
 * (Phase 1 commit 9). Use editor_state_transformers / _highlights /
 * _virtual_lines. */
/* Editor-input convenience getters moved to editor_state.h
 * (Phase 1 commit 10). Use editor_input_text / _len, editor_cursor_pos,
 * editor_insert_mode, editor_pending_newline_len. */

/* Selection + clipboard view accessors moved to editor_state.h
 * (Phase 1 commit 6). Use editor_state_selection / _clipboard. */

/* Code-panel / help / variable_panel / profile_panel / status /
 * camera / pointer / viewport view accessors moved to ui_state.h
 * (Phase 1 commit 8 + Phase A commits 12-14); the legacy
 * `repl_state_*` forwarders were removed in Phase A commit 14.
 * Search + autocomplete view accessors moved to editor_state.h
 * (Phase 1 commit 7). Use editor_state_search / _autocomplete. */

ReplPresentationState repl_state_presentation(void);
const float *repl_state_grid_major_steps(void);
const float *repl_state_grid_extents(void);

ReplRenderState          repl_state_render(void);

ReplReplayRuntimeState    repl_state_replay(void);

ReplSceneRuntimeState     repl_state_scenes(void);
const char *repl_state_workspace_dir(void);

ReplImportExportView repl_state_import_export(void);

#endif /* REPL_STATE_VIEWS_H */
