/*
 * repl_state_owners.h -- mutating REPL runtime-state facade.
 */
#ifndef REPL_STATE_OWNERS_H
#define REPL_STATE_OWNERS_H

#include "repl_state_views.h"

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
void         repl_state_document_reset(void);

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
void              repl_state_flat_program_reset(void);
void              repl_state_mark_flat_dirty(void);
void              repl_state_mark_normals_dirty(void);
FlatProgramView   repl_state_flat_program_view(void);

const ReplVariableState *repl_state_variables(void);
ReplVariableState       *repl_state_variables_mut(void);
void                     repl_state_variables_reset(void);
void                     repl_state_time_advance(float dt);
void                     repl_state_time_reset_to_zero(void);

const ReplEditorInputState *repl_state_editor_input(void);
ReplEditorInputState       *repl_state_editor_input_mut(void);
void                        repl_state_editor_input_reset(void);
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

ReplSelectionState       repl_state_selection(void);
ReplSelectionState       *repl_state_selection_mut(void);
void                      repl_state_selection_clear(void);
int                       repl_state_selection_anchor(void);
int                       repl_state_selection_end_idx(void);
void                      repl_state_selection_set(int anchor_idx, int end_idx);

const ReplClipboardState *repl_state_clipboard(void);
ReplClipboardState       *repl_state_clipboard_mut(void);
void                      repl_state_clipboard_clear(void);
GLCmd                    *repl_state_clipboard_cmds_mut(void);
int                       repl_state_clipboard_count(void);
void                      repl_state_clipboard_count_set(int cmd_count);

const ReplCodePanelRuntimeState *repl_state_code_panel(void);
ReplCodePanelRuntimeState       *repl_state_code_panel_mut(void);
void                             repl_state_code_panel_reset(void);

ReplHelpState        repl_state_help(void);
ReplHelpState       *repl_state_help_mut(void);
void                 repl_state_help_reset(void);

const ReplVariablePanelState *repl_state_variable_panel(void);
ReplVariablePanelState       *repl_state_variable_panel_mut(void);

const ReplVariableDragState *repl_state_variable_drag(void);
ReplVariableDragState       *repl_state_variable_drag_mut(void);
void                         repl_state_variable_drag_reset(void);

ReplProfilePanelState    repl_state_profile_panel(void);
ReplProfilePanelState       *repl_state_profile_panel_mut(void);

const ReplStatusState *repl_state_status(void);
ReplStatusState       *repl_state_status_mut(void);
void                   repl_state_status_set(const char *message);
void                   repl_state_status_clear(void);
void                   repl_state_status_tick(void);

const ReplSearchState *repl_state_search(void);
ReplSearchState       *repl_state_search_mut(void);
void                   repl_state_search_clear(void);

const ReplAutocompleteState *repl_state_autocomplete(void);
ReplAutocompleteState       *repl_state_autocomplete_mut(void);
void                        repl_state_autocomplete_clear(void);

const ReplCameraState *repl_state_camera(void);
ReplCameraState       *repl_state_camera_mut(void);
ReplCameraState        repl_state_camera_snapshot(void);
void                   repl_state_camera_set(float rx, float ry, float dist,
                                            float tx, float ty, float tz,
                                            float motion_glow);
void                   repl_state_camera_set_orbit(float rx, float ry);
void                   repl_state_camera_set_pan(float tx, float ty, float tz);
void                   repl_state_camera_set_distance(float dist);
void                   repl_state_camera_set_motion_glow(float motion_glow);
void                   repl_state_camera_reset_default(void);

const ReplPointerState *repl_state_pointer(void);
ReplPointerState       *repl_state_pointer_mut(void);
void                    repl_state_pointer_set(int mouse_x, int mouse_y, int mouse_button);
void                    repl_state_pointer_set_pos(int mouse_x, int mouse_y);
void                    repl_state_pointer_set_button(int mouse_button);

const ReplViewportState *repl_state_viewport(void);
ReplViewportState       *repl_state_viewport_mut(void);
void                    repl_state_viewport_set_size(int window_w, int window_h);

const ReplPresentationState *repl_state_presentation(void);
ReplPresentationState       *repl_state_presentation_mut(void);
ReplPresentationState        repl_state_presentation_snapshot(void);
void                         repl_state_presentation_reset_defaults(void);
void                         repl_state_presentation_reset_example_defaults(void);

const ReplRenderState *repl_state_render(void);
ReplRenderState       *repl_state_render_mut(void);
void                   repl_state_render_reset_defaults(void);

const ReplRenderDerivedState *repl_state_render_derived(void);
ReplRenderDerivedState       *repl_state_render_derived_mut(void);

const ReplReplayRuntimeState *repl_state_replay(void);
ReplReplayRuntimeState       *repl_state_replay_mut(void);
void                          repl_state_replay_reset(void);

const ReplSceneRuntimeState *repl_state_scenes(void);
ReplSceneRuntimeState       *repl_state_scenes_mut(void);
void                         repl_state_workspace_set_dir(const char *dir);
const char                  *repl_state_workspace_dir(void);

const ReplImportExportState *repl_state_import_export(void);
ReplImportExportState       *repl_state_import_export_mut(void);
void                         repl_state_import_export_reset(void);
void                         repl_state_refresh_workspace_header_lines(void);
int                          repl_state_parse_workspace_header_line(const char *line);

void repl_state_init_defaults(void);
void repl_state_reset_all(void);

#endif /* REPL_STATE_OWNERS_H */