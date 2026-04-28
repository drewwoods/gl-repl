/*
 * repl_state_views.h -- read-only REPL runtime-state facade.
 */
#ifndef REPL_STATE_VIEWS_H
#define REPL_STATE_VIEWS_H

#include "sample.h"
#include "repl_flatten.h"

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
    GLCmd *cmds;
    int   *cmd_count;
    int    capacity;
    int   *edit_line_idx;
    int   *normals_dirty;
} ReplDocumentState;

typedef struct {
    GLCmd            *cmds;
    FlatCmdLocalVars *local_vars;
    int              *cmd_count;
    int               capacity;
    int              *dirty;
    int              *user_lighting_enabled;
    int              *current_block_begin_idx;
    int              *current_block_end_idx;
    int              *current_block_source_line_idx;
} ReplFlatProgramState;

typedef struct {
    ExprVar *vars;
    int     *var_count;
    int     *time_var_idx;
    int     *time_playing;
    float   *anim_time;
} ReplVariableState;

typedef struct {
    char *input;
    int   input_capacity;
    int  *input_len;
    int  *cursor_pos;
    int  *edit_line_idx;
    char *pending_newline;
    int   pending_newline_capacity;
    int  *pending_newline_len;
    int  *insert_mode;
} ReplEditorInputState;

typedef struct {
    int *anchor_idx;
    int *end_idx;
} ReplSelectionState;

typedef struct {
    GLCmd *cmds;
    int   *cmd_count;
} ReplClipboardState;

typedef struct {
    float *panel_frac;
    int   *resizing_panel;
    int   *scroll;
    int   *scroll_follow_cursor;
    int   *cursor_visible;
    int   *blink_tick;
    int   *cursor_px;
    int   *cursor_py;
} ReplCodePanelRuntimeState;

typedef struct {
    int visible;
    int tab_idx;
    int scroll;
} ReplHelpState;

typedef struct {
    int *visible;
} ReplVariablePanelState;

typedef struct {
    int *mode;
} ReplProfilePanelState;

typedef struct {
    char *text;
    int   capacity;
    int  *ttl;
} ReplStatusState;

typedef struct {
    int  *active;
    char *query;
    int   query_capacity;
    int  *query_len;
    int  *cursor_pos;
    int  *hit_line_idx;
    int  *hit_char_idx;
    int  *hit_ordinal;
    int  *match_count;
} ReplSearchState;

typedef struct {
    const char **matches;
    const char **insert_matches;
    int         *match_count;
    int         *selected_idx;
    char        *ghost;
    int          ghost_capacity;
    char        *hint;
    int          hint_capacity;
} ReplAutocompleteState;

typedef struct {
    float *rx;
    float *ry;
    float *dist;
    float *tx;
    float *ty;
    float *tz;
    float *motion_glow;
    int   *auto_rotate;
} ReplCameraState;

typedef struct {
    int *mouse_x;
    int *mouse_y;
    int *mouse_button;
} ReplPointerState;

typedef struct {
    int *window_w;
    int *window_h;
} ReplViewportState;

typedef struct {
    int        *wireframe;
    int        *grid_theme;
    int        *grid_major_idx;
    int        *grid_extent_idx;
    int        *axes_theme;
    int        *show_vertex_labels;
    int        *show_normal_vectors;
    int        *show_vertex_indices;
    int        *show_vertex_outlines;
    int        *show_vertex_points;
    int        *show_vertex_guides;
    int        *xform_guide_mode;
    int        *autonormal;
    int        *show_light_indicators;
    int        *backdrop_mode;
    int        *highlight_current_poly;
    int        *ortho_mode;
    int        *wrap_at_comma;
    int        *code_panel_layout;
    const float *grid_major_steps;
    const float *grid_extents;
    float      *focus_vertex;
    int        *focus_vertex_valid;
} ReplPresentationState;

typedef struct {
    int            *use_accum;
    int            *accum_aa_enabled;
    int            *accum_samples;
    float          *accum_jitter_x;
    float          *accum_jitter_y;
    int            *multisample_enabled;
    int            *line_smooth_enabled;
    int            *point_attenuation_enabled;
    SceneLight     *lights;
    float          *clear_color;
} ReplRenderState;

typedef struct {
    float *focus_vertex;
    int   *focus_vertex_valid;
} ReplRenderDerivedState;

typedef struct {
    int   *active;
    int   *state;
    int   *pc;
    int   *mode;
    float *speed;
    float *accum;
    float *fade_speed;
    int   *src_line_idx;
    int   *total_flat_cmds;
    int   *expand_args;
} ReplReplayRuntimeState;

typedef struct {
    int   *active_example_idx;
    char *workspace_dir;
    int   workspace_dir_capacity;
} ReplSceneRuntimeState;

typedef struct {
    int   *var_idx;
    int   *log_mode;
    float *start_value;
    int   *start_x;
} ReplVariableDragState;

typedef struct {
    char (*workspace_header_lines)[WORKSPACE_HEADER_LINE_LEN];
    int   *workspace_header_line_count;
    char (*render_state_lines)[64];
    char (*cam_lines)[96];
    const char **export_scene_name_hint;
    char  *pending_scene_name;
    char  *pending_workspace_dir;
} ReplImportExportState;

typedef struct {
    ReplDocumentState         document;
    ReplFlatProgramState      flat_program;
    ReplVariableState         variables;
    ReplEditorInputState      editor_input;
    ReplSelectionState        selection;
    ReplClipboardState        clipboard;
    ReplCodePanelRuntimeState code_panel;
    ReplHelpState             help;
    ReplVariablePanelState    variable_panel;
    ReplVariableDragState     variable_drag;
    ReplProfilePanelState     profile_panel;
    ReplStatusState           status;
    ReplSearchState           search;
    ReplAutocompleteState     autocomplete;
    ReplCameraState           camera;
    ReplPointerState          pointer;
    ReplViewportState         viewport;
    ReplPresentationState     presentation;
    ReplRenderState           render;
    ReplRenderDerivedState    render_derived;
    ReplReplayRuntimeState    replay;
    ReplSceneRuntimeState     scenes;
    ReplImportExportState     import_export;
} ReplRuntimeFacade;

const ReplDocumentState *repl_state_document(void);
const GLCmd *repl_state_document_cmds(void);
const GLCmd *repl_state_document_cmd_at(int cmd_idx);
int          repl_state_document_count(void);
int          repl_state_document_capacity(void);
int          repl_state_edit_line(void);
int          repl_state_normals_dirty(void);
void         repl_state_document_reset(void);

const ReplFlatProgramState *repl_state_flat_program(void);
const GLCmd      *repl_state_flat_program_cmds(void);
int               repl_state_flat_program_count(void);
int               repl_state_flat_program_dirty(void);
int               repl_state_flat_program_user_lighting_enabled(void);
FlatProgramView   repl_state_flat_program_view(void);

const ReplVariableState *repl_state_variables(void);

const ReplEditorInputState *repl_state_editor_input(void);
const char *repl_state_input_text(void);
int         repl_state_input_len(void);
int         repl_state_cursor_pos(void);
int         repl_state_insert_mode(void);
int         repl_state_pending_newline_len(void);

const ReplSelectionState *repl_state_selection(void);
int  repl_state_selection_anchor(void);
int  repl_state_selection_end_idx(void);

const ReplClipboardState *repl_state_clipboard(void);
int    repl_state_clipboard_count(void);

const ReplCodePanelRuntimeState *repl_state_code_panel(void);

const ReplHelpState *repl_state_help(void);

const ReplVariablePanelState *repl_state_variable_panel(void);

const ReplVariableDragState *repl_state_variable_drag(void);

const ReplProfilePanelState *repl_state_profile_panel(void);

const ReplStatusState *repl_state_status(void);

const ReplSearchState *repl_state_search(void);

const ReplAutocompleteState *repl_state_autocomplete(void);

const ReplCameraState *repl_state_camera(void);
ReplCameraState        repl_state_camera_snapshot(void);

const ReplPointerState *repl_state_pointer(void);

const ReplViewportState *repl_state_viewport(void);

const ReplPresentationState *repl_state_presentation(void);
ReplPresentationState        repl_state_presentation_snapshot(void);

const ReplRenderState *repl_state_render(void);

const ReplRenderDerivedState *repl_state_render_derived(void);

const ReplReplayRuntimeState *repl_state_replay(void);

const ReplSceneRuntimeState *repl_state_scenes(void);
const char *repl_state_workspace_dir(void);

const ReplImportExportState *repl_state_import_export(void);

#endif /* REPL_STATE_VIEWS_H */