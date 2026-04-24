#ifndef REPL_STATE_H
#define REPL_STATE_H

/*
 * repl_state.h -- Phase 2 runtime-state facade.
 *
 * This is the bridge API that will eventually own the REPL runtime contexts.
 * For now it exposes typed sub-state views over the legacy globals so the
 * module boundaries can start moving without changing behavior.
 */

#include "sample.h"
#include "repl_flatten.h"
#include "repl_executor.h"

#ifndef MAX_WORKSPACE_HEADER_LINES
#define MAX_WORKSPACE_HEADER_LINES 48
#endif

#ifndef WORKSPACE_HEADER_LINE_LEN
#define WORKSPACE_HEADER_LINE_LEN  96
#endif

#ifndef REPL_WORKSPACE_DIR_MAX
#define REPL_WORKSPACE_DIR_MAX 1024
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
    int *visible;
    int *tab_idx;
    int *scroll;
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
    GLUquadric    **quadric;
    GLUtesselator **tess;
    TessVertex     *tess_verts;
    int            *tess_vert_count;
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
    int  *active_example_idx;
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
} ReplRuntimeState;

const ReplDocumentState *repl_state_document(void);
ReplDocumentState       *repl_state_document_mut(void);
const GLCmd *repl_state_document_cmds(void);
GLCmd       *repl_state_document_cmds_mut(void);
const GLCmd *repl_state_document_cmd_at(int cmd_idx);
GLCmd       *repl_state_document_cmd_at_mut(int cmd_idx);
int          repl_state_document_count(void);
void         repl_state_document_count_set(int cmd_count);
int          repl_state_document_capacity(void);
int          repl_state_edit_line(void);
void         repl_state_edit_line_set(int edit_line_idx);
void         repl_state_edit_line_clamp(void);
int          repl_state_normals_dirty(void);
void         repl_state_normals_dirty_clear(void);
void repl_state_document_reset(void);

const ReplFlatProgramState *repl_state_flat_program(void);
ReplFlatProgramState       *repl_state_flat_program_mut(void);
const GLCmd      *repl_state_flat_program_cmds(void);
GLCmd            *repl_state_flat_program_cmds_mut(void);
FlatCmdLocalVars *repl_state_flat_program_local_vars_mut(void);
int               repl_state_flat_program_count(void);
void              repl_state_flat_program_set_count(int cmd_count);
int               repl_state_flat_program_dirty(void);
void              repl_state_flat_program_clear_dirty(void);
int               repl_state_flat_program_user_lighting_enabled(void);
void              repl_state_flat_program_set_user_lighting_enabled(int enabled);
void              repl_state_flat_program_set_current_block(int begin_idx,
                                                            int end_idx,
                                                            int source_line_idx);
void              repl_state_flat_program_clear_current_block(void);
void repl_state_flat_program_reset(void);
void repl_state_mark_flat_dirty(void);
void repl_state_mark_normals_dirty(void);
FlatProgramView repl_state_flat_program_view(void);

const ReplVariableState *repl_state_variables(void);
ReplVariableState       *repl_state_variables_mut(void);
void repl_state_variables_reset(void);
void repl_state_time_advance(float dt);
void repl_state_time_reset_to_zero(void);

const ReplEditorInputState *repl_state_editor_input(void);
ReplEditorInputState       *repl_state_editor_input_mut(void);
void repl_state_editor_input_reset(void);
const char *repl_state_input_text(void);
char       *repl_state_input_buffer_mut(void);
int         repl_state_input_len(void);
void        repl_state_input_len_set(int input_len);
void        repl_state_input_set_text(const char *text);
void        repl_state_input_clear(void);
int         repl_state_cursor_pos(void);
void        repl_state_cursor_pos_set(int cursor_pos);
int         repl_state_insert_mode(void);
void        repl_state_insert_mode_set(int insert_mode);
char       *repl_state_pending_newline_buffer_mut(void);
int         repl_state_pending_newline_len(void);
void        repl_state_pending_newline_len_set(int newline_len);
void        repl_state_pending_newline_set_text(const char *text);
void        repl_state_pending_newline_clear(void);

const ReplSelectionState *repl_state_selection(void);
ReplSelectionState       *repl_state_selection_mut(void);
void repl_state_selection_clear(void);
int  repl_state_selection_anchor(void);
int  repl_state_selection_end_idx(void);
void repl_state_selection_set(int anchor_idx, int end_idx);

const ReplClipboardState *repl_state_clipboard(void);
ReplClipboardState       *repl_state_clipboard_mut(void);
void repl_state_clipboard_clear(void);
GLCmd *repl_state_clipboard_cmds_mut(void);
int    repl_state_clipboard_count(void);
void   repl_state_clipboard_count_set(int cmd_count);

const ReplCodePanelRuntimeState *repl_state_code_panel(void);
ReplCodePanelRuntimeState       *repl_state_code_panel_mut(void);
void repl_state_code_panel_reset(void);

const ReplHelpState *repl_state_help(void);
ReplHelpState       *repl_state_help_mut(void);
void repl_state_help_reset(void);

const ReplVariablePanelState *repl_state_variable_panel(void);
ReplVariablePanelState       *repl_state_variable_panel_mut(void);

const ReplVariableDragState *repl_state_variable_drag(void);
ReplVariableDragState       *repl_state_variable_drag_mut(void);
void repl_state_variable_drag_reset(void);

const ReplProfilePanelState *repl_state_profile_panel(void);
ReplProfilePanelState       *repl_state_profile_panel_mut(void);

const ReplStatusState *repl_state_status(void);
void repl_status_set(const char *message);
void repl_status_clear(void);
void repl_status_tick(void);

const ReplSearchState *repl_state_search(void);
ReplSearchState       *repl_state_search_mut(void);
void repl_state_search_clear(void);

const ReplAutocompleteState *repl_state_autocomplete(void);
ReplAutocompleteState       *repl_state_autocomplete_mut(void);
void repl_state_autocomplete_clear(void);

const ReplCameraState *repl_state_camera(void);
ReplCameraState       *repl_state_camera_mut(void);
ReplCameraState        repl_state_camera_snapshot(void);
void repl_state_camera_set(float rx, float ry, float dist,
                           float tx, float ty, float tz,
                           float motion_glow);
void repl_state_camera_set_orbit(float rx, float ry);
void repl_state_camera_set_pan(float tx, float ty, float tz);
void repl_state_camera_set_distance(float dist);
void repl_state_camera_set_motion_glow(float motion_glow);
void repl_state_camera_reset_default(void);

const ReplPointerState *repl_state_pointer(void);
ReplPointerState       *repl_state_pointer_mut(void);
void repl_state_pointer_set(int mouse_x, int mouse_y, int mouse_button);
void repl_state_pointer_set_pos(int mouse_x, int mouse_y);
void repl_state_pointer_set_button(int mouse_button);

const ReplViewportState *repl_state_viewport(void);
ReplViewportState       *repl_state_viewport_mut(void);
void repl_state_viewport_set_size(int window_w, int window_h);

const ReplPresentationState *repl_state_presentation(void);
ReplPresentationState       *repl_state_presentation_mut(void);
ReplPresentationState        repl_state_presentation_snapshot(void);
void repl_state_presentation_reset_defaults(void);
void repl_state_presentation_reset_example_defaults(void);

const ReplRenderState *repl_state_render(void);
ReplRenderState       *repl_state_render_mut(void);
void repl_state_render_reset_defaults(void);
void repl_state_render_init_resources(void);
void repl_state_render_destroy_resources(void);

const ReplRenderDerivedState *repl_state_render_derived(void);
ReplRenderDerivedState       *repl_state_render_derived_mut(void);

const ReplReplayRuntimeState *repl_state_replay(void);
ReplReplayRuntimeState       *repl_state_replay_mut(void);
void repl_state_replay_reset(void);

const ReplSceneRuntimeState *repl_state_scenes(void);
ReplSceneRuntimeState       *repl_state_scenes_mut(void);
void repl_state_workspace_set_dir(const char *dir);
const char *repl_state_workspace_dir(void);

const ReplImportExportState *repl_state_import_export(void);
ReplImportExportState       *repl_state_import_export_mut(void);
void repl_state_import_export_reset(void);
void repl_state_refresh_workspace_header_lines(void);
int  repl_state_parse_workspace_header_line(const char *line);

void repl_state_init_defaults(void);
void repl_state_reset_all(void);

#endif /* REPL_STATE_H */
