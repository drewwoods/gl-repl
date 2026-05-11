/*
 * src/repl/state_owners.h -- REPL runtime-state owner/mutation API.
 */
#ifndef REPL_STATE_OWNERS_H
#define REPL_STATE_OWNERS_H

#include "repl/state_views.h"

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

/* Code-panel / help / variable_panel / profile_panel / status /
 * camera / pointer / viewport accessors moved to ui_state.h (Phase 1
 * commit 8 + Phase A commits 12-14). Use the canonical
 * `ui_state_*` API directly; the legacy `repl_state_*` forwarders
 * were removed in Phase A commit 14.
 * Search + autocomplete accessors moved to editor_state.h (Phase 1
 * commit 7). Use editor_state_search / _autocomplete and friends.
 * Variable-drag accessors live on the variable_panel peer
 * (variable_panel.h). Use `variable_panel_drag` /
 * `variable_panel_handle_drag_*` directly. */

/* `repl_state_presentation*` accessors and reset helpers moved to
 * `glr_state.h` (step 7a of feature/decouple-repl-from-gl-repl-alt.md).
 * REPL pipeline TUs MUST NOT include `glr_state.h`; controller /
 * editor / UI / scene callers use it directly. The grid-step /
 * grid-extent tables moved alongside as
 * `glr_state_grid_major_steps` / `_grid_extents`.
 *
 * The render-config toggles (msaa, line_smooth, accum_aa, etc.) moved
 * to `glr_state.h` as `GlrRenderState`. The runtime-mutated halves
 * (`lights[]`, `clear_color[]`) keep these accessors because the
 * executor still writes them in response to user GL commands. */

ReplRenderState        repl_state_render(void);
ReplRenderState       *repl_state_render_mut(void);
/* Reset the runtime-mutated render halves (`lights[]`, `clear_color[]`)
 * to defaults. The render-config toggles (msaa, line_smooth, accum_*)
 * moved to glr_state — call `glr_state_render_reset_defaults()` for
 * those. Both reset paths fire from `glr_app_reset_all`. */
void                   repl_state_render_reset_defaults(void);

ReplSceneRuntimeState    repl_state_scenes(void);
ReplSceneRuntimeState       *repl_state_scenes_mut(void);
void                         repl_state_workspace_set_dir(const char *dir);
const char                  *repl_state_workspace_dir(void);

ReplImportExportView        repl_state_import_export(void);
ReplImportExportState       *repl_state_import_export_mut(void);
void                         repl_state_import_export_reset(void);
void                         repl_state_refresh_workspace_header_lines(void);
int                          repl_state_parse_workspace_header_line(const char *line);

/* Initialise REPL-owned program state to defaults. Does NOT reset
 * peer/editor/UI state — that's glr_app_reset_all in glr_ctrl.h.
 * The demo and any pure-REPL test fixture call this; production
 * startup and tests that need full-world reset call
 * glr_app_reset_all instead. */
void repl_state_init_defaults(void);

/* Lower-level synonym of repl_state_init_defaults — resets just the
 * REPL slices. Either entry is fine for REPL-only callers. */
void repl_state_reset_program(void);

#endif /* REPL_STATE_OWNERS_H */
