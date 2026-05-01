/*
 * repl_state_views.h -- REPL runtime-state value/view types and read API.
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

typedef struct {
    char input[MAX_INPUT_LEN];
    int  input_capacity;
    int  input_len;
    int  cursor_pos;
    int  edit_line_idx;
    char pending_newline[MAX_INPUT_LEN];
    int  pending_newline_capacity;
    int  pending_newline_len;
    int  insert_mode;
} ReplEditorInputState;

typedef struct {
    const char *input;
    int         input_capacity;
    int         input_len;
    int         cursor_pos;
    int         edit_line_idx;
    const char *pending_newline;
    int         pending_newline_capacity;
    int         pending_newline_len;
    int         insert_mode;
} ReplEditorInputView;

/* Editor-owned text buffer (Phase: editor-owns-text spike).
 *
 * One canonical text line per source command. Indexed by source command
 * index. The text is the raw user-typed form (no trailing ';', no
 * leading whitespace) — the same shape `load_line_to_input()` produces
 * after stripping. During the spike `cmds[idx].source` keeps the
 * normalized form for backwards compatibility; this slice is the
 * load-bearing buffer the redesign will eventually consume. */
typedef struct {
    char lines[MAX_COMMANDS][MAX_LINE_LEN];
    int  line_count;
} ReplEditorBuffer;

typedef struct {
    int anchor_idx;
    int end_idx;
} ReplSelectionState;

typedef struct {
    GLCmd cmds[MAX_COMMANDS];
    char  lines[MAX_COMMANDS][MAX_LINE_LEN];
    int   cmd_count;
} ReplClipboardState;

typedef struct {
    float panel_frac;
    int   resizing_panel;
    int   scroll;
    int   scroll_follow_cursor;
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

typedef struct {
    int  active;
    char query[MAX_INPUT_LEN];
    int  query_len;
    int  cursor_pos;
    int  hit_line_idx;
    int  hit_char_idx;
    int  hit_ordinal;
    int  match_count;
} ReplSearchState;

typedef struct {
    const char *matches[MAX_AC_MATCHES];
    const char *insert_matches[MAX_AC_MATCHES];
    int         match_count;
    int         selected_idx;
    char        ghost[MAX_LINE_LEN];
    char        hint[MAX_LINE_LEN];
} ReplAutocompleteState;

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

typedef struct {
    int   var_idx;
    int   log_mode;
    float start_value;
    int   start_x;
} ReplVariableDragState;

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

ReplEditorInputView repl_state_editor_input(void);
/* Editor-owns-text spike: read-only access to the per-line text buffer. */
const ReplEditorBuffer *repl_state_editor_buffer(void);
const char             *repl_state_editor_buffer_line(int idx);
int                     repl_state_editor_buffer_count(void);
const char *repl_state_input_text(void);
int         repl_state_input_len(void);
int         repl_state_cursor_pos(void);
int         repl_state_insert_mode(void);
int         repl_state_pending_newline_len(void);

ReplSelectionState        repl_state_selection(void);
int  repl_state_selection_anchor(void);
int  repl_state_selection_end_idx(void);

ReplClipboardState        repl_state_clipboard(void);
int    repl_state_clipboard_count(void);

ReplCodePanelRuntimeState repl_state_code_panel(void);

ReplHelpState        repl_state_help(void);

ReplVariablePanelState    repl_state_variable_panel(void);

ReplVariableDragState     repl_state_variable_drag(void);

ReplProfilePanelState     repl_state_profile_panel(void);

ReplStatusState          repl_state_status(void);

ReplSearchState          repl_state_search(void);

ReplAutocompleteState    repl_state_autocomplete(void);

ReplCameraState        repl_state_camera(void);
ReplCameraState        repl_state_camera_snapshot(void);

ReplPointerState         repl_state_pointer(void);

ReplViewportState       repl_state_viewport(void);

ReplPresentationState repl_state_presentation(void);
const float *repl_state_grid_major_steps(void);
const float *repl_state_grid_extents(void);

ReplRenderState          repl_state_render(void);

ReplReplayRuntimeState    repl_state_replay(void);

ReplSceneRuntimeState     repl_state_scenes(void);
const char *repl_state_workspace_dir(void);

ReplImportExportView repl_state_import_export(void);

#endif /* REPL_STATE_VIEWS_H */
