/*
 * repl_state_owners.h -- REPL runtime-state owner/mutation API.
 */
#ifndef REPL_STATE_OWNERS_H
#define REPL_STATE_OWNERS_H

#include "repl_state_views.h"
#include "ui_editor.h"

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

ReplFlatProgramState       *repl_state_flat_program_mut(void);
const GLCmd      *repl_state_flat_program_cmds(void);
GLCmd            *repl_state_flat_program_cmds_mut(void);
FlatCmdLocalVars *repl_state_flat_program_local_vars_mut(void);
int               repl_state_flat_program_count(void);
void              repl_state_flat_program_set_count(int cmd_count);
int               repl_state_flat_program_dirty(void);
void              repl_state_flat_program_clear_dirty(void);
int               repl_state_flat_program_user_lighting_enabled(void);
int               repl_state_flat_program_current_block_begin(void);
int               repl_state_flat_program_current_block_end(void);
int               repl_state_flat_program_current_block_source_line(void);
void              repl_state_flat_program_set_user_lighting_enabled(int enabled);
void              repl_state_flat_program_set_current_block(int begin_idx,
                                                            int end_idx,
                                                            int source_line_idx);
void              repl_state_flat_program_clear_current_block(void);
void              repl_state_flat_program_reset(void);
void              repl_state_mark_flat_dirty(void);
void              repl_state_mark_normals_dirty(void);
FlatProgramView   repl_state_flat_program_view(void);

ReplVariableView         repl_state_variables(void);
ReplVariableState       *repl_state_variables_mut(void);
void                     repl_state_variables_reset(void);
void                     repl_state_time_advance(float dt);
void                     repl_state_time_reset_to_zero(void);

/* Editor-input + editor-buffer accessors moved to editor_state.h
 * (Phase 1 commits 4-5). Use `editor_state_input / _mut / _reset` for
 * the input slice, `editor_state_buffer / _mut` for the whole-buffer
 * struct, and `editor_buffer_line / set_line / count / set_count` for
 * slice-level access. */

/* Per-frame editor transformer snapshot pushed by the controller after
 * flatten so renderers/UI can iterate inline swatch/slider affordances
 * without walking the document themselves. */
/* Editor overlay snapshot list accessors moved to editor_state.h
 * (Phase 1 commit 9). Use editor_state_transformers / _highlights /
 * _virtual_lines. */

/* Per-frame editor highlight snapshot. The controller refills the list
 * each frame with feeding-cmd, replay-PC, search-match, etc. entries so
 * UI render code can iterate it instead of recomputing positions inline. */
/* Editor highlight + virtual-line accessors moved to editor_state.h
 * (Phase 1 commit 9). Use editor_state_highlights /
 * _virtual_lines. */
/* Editor-input convenience getters/setters moved to editor_state.h
 * (Phase 1 commit 10). Use editor_input_* / editor_cursor_pos* /
 * editor_insert_mode* / editor_pending_newline_*. */

/* Selection + clipboard accessors moved to editor_state.h (Phase 1
 * commit 6). Use editor_state_selection / _clipboard and friends. */

ReplCodePanelRuntimeState       repl_state_code_panel(void);
ReplCodePanelRuntimeState       *repl_state_code_panel_mut(void);
void                             repl_state_code_panel_reset(void);

ReplHelpState        repl_state_help(void);
ReplHelpState       *repl_state_help_mut(void);
void                 repl_state_help_reset(void);

ReplVariablePanelState    repl_state_variable_panel(void);
ReplVariablePanelState       *repl_state_variable_panel_mut(void);

/* editor_state_variable_drag accessors moved to editor_state.h
 * (Phase 1 commit 9). Use editor_state_variable_drag / _mut / _reset. */

ReplProfilePanelState    repl_state_profile_panel(void);
ReplProfilePanelState       *repl_state_profile_panel_mut(void);

ReplStatusState          repl_state_status(void);
ReplStatusState       *repl_state_status_mut(void);
void                   repl_state_status_set(const char *message);
void                   repl_state_status_clear(void);
void                   repl_state_status_tick(void);

/* Search + autocomplete accessors moved to editor_state.h (Phase 1
 * commit 7). Use editor_state_search / _autocomplete and friends. */

ReplCameraState        repl_state_camera(void);
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

ReplPointerState       repl_state_pointer(void);
ReplPointerState       *repl_state_pointer_mut(void);
void                    repl_state_pointer_set(int mouse_x, int mouse_y, int mouse_button);
void                    repl_state_pointer_set_pos(int mouse_x, int mouse_y);
void                    repl_state_pointer_set_button(int mouse_button);

ReplViewportState      repl_state_viewport(void);
ReplViewportState       *repl_state_viewport_mut(void);
void                    repl_state_viewport_set_size(int window_w, int window_h);

ReplPresentationState           repl_state_presentation(void);
ReplPresentationState       *repl_state_presentation_mut(void);
const float                  *repl_state_grid_major_steps(void);
const float                  *repl_state_grid_extents(void);
void                         repl_state_presentation_reset_defaults(void);
void                         repl_state_presentation_reset_example_defaults(void);

ReplRenderState        repl_state_render(void);
ReplRenderState       *repl_state_render_mut(void);
void                   repl_state_render_reset_defaults(void);

ReplReplayRuntimeState repl_state_replay(void);
ReplReplayRuntimeState       *repl_state_replay_mut(void);
void                          repl_state_replay_reset(void);

ReplSceneRuntimeState    repl_state_scenes(void);
ReplSceneRuntimeState       *repl_state_scenes_mut(void);
void                         repl_state_workspace_set_dir(const char *dir);
const char                  *repl_state_workspace_dir(void);

ReplImportExportView        repl_state_import_export(void);
ReplImportExportState       *repl_state_import_export_mut(void);
void                         repl_state_import_export_reset(void);
void                         repl_state_refresh_workspace_header_lines(void);
int                          repl_state_parse_workspace_header_line(const char *line);

void repl_state_init_defaults(void);
void repl_state_reset_all(void);

#endif /* REPL_STATE_OWNERS_H */
