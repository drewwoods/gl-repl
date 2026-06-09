/*
 * src/repl/state_owners.h -- REPL runtime-state owner/mutation API.
 *
 * Declares the mutable accessors and reset helpers for REPL-owned runtime
 * slices: source document, flat program, predefined variables, runtime-mutated
 * render state, scene bookkeeping, and import/export scratch storage. Companion
 * state owned by the editor, UI, replay peer, or app shell is mutated through
 * their own owner headers rather than through repl_state_* forwarders.
 */
#ifndef REPL_STATE_OWNERS_H
#define REPL_STATE_OWNERS_H

#include "repl/state_views.h"
#include "repl/state_notify.h"

ReplDocumentState       *repl_state_document_mut(void);
const GLCmd *repl_state_document_cmds(void);
GLCmd       *repl_state_document_cmds_mut(void);
const GLCmd *repl_state_document_cmd_at(int cmd_idx);
GLCmd       *repl_state_document_cmd_at_mut(int cmd_idx);
int          repl_state_document_count(void);
void         repl_state_document_count_set(int cmd_count);
int          repl_state_document_capacity(void);
/* repl_state_edit_line / _set / _clamp deleted. Use
 * editor_state_edit_line / _set / _clamp from src/editor/state.h
 * (editor / app / widget / test callers) or the
 * repl_dispatch_edit_line_get / _set sink in src/repl/core.h
 * (REPL pipeline callers that can't reach editor headers),
 * implemented in Phase 4 of plans/in-review/edit-line-ownership.md. */
int          repl_state_normals_dirty(void);
void         repl_state_normals_dirty_clear(void);
/* Clears the source-command array + source-text document only.
 * The edit-line cursor lives on EditorState and is NOT touched
 * here — wholesale-reset callers must reposition the cursor at
 * the same boundary (via editor_state_edit_line_set above β, or
 * repl_dispatch_edit_line_set from REPL pipeline files). */
void         repl_state_document_reset(void);

ReplFlatProgramState       *repl_state_flat_program_mut(void);
ReplFlatProgramState       *repl_state_flat_program_writable(void);
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
FlatProgramView   repl_state_flat_program_view(void);

ReplVariableView         repl_state_variables(void);
ReplVariableState       *repl_state_variables_mut(void);
void                     repl_state_time_advance(float dt);
void                     repl_state_time_reset_to_zero(void);
void                     repl_state_time_set(float value);
/* Set only the predef 't' value for a single motion-blur sub-pass re-bake.
 * Unlike repl_state_time_set it leaves the free-running clock (g_anim_time)
 * and the flat-dirty flag untouched; the caller reflattens then restores the
 * frame baseline. */
void                     repl_state_time_set_transient(float value);

/* Mutable accessor boundary: this header intentionally stops at REPL-owned
 * slices. For other runtime state, use the owner module directly:
 * - `editor_state.h` for input/buffer/selection/search/autocomplete and editor
 *   overlay lists
 * - `ui_state.h` for code-panel/help/status/pointer/viewport state
 * - `variable_panel_state.h` / variable-panel drag helpers for variable drag
 * - `glr_state.h` for presentation policy, render config, and grid tables
 *
 * The older
 * `repl_state_*` forwarders for those owners. REPL pipeline TUs still avoid
 * including `glr_state.h`; controller/editor/UI/scene callers can include it
 * directly (implemented in Phase 1 of the state split and step 7a of
 * feature/decouple-repl-from-gl-repl-alt.md). */

/* Runtime-mutated render tail only: executor writes `light_enabled_mask`
 * and `clear_color[]` in response to user GL commands, so those bytes remain
 * REPL-owned. The dimensional per-light data (positions/colors/eye-space) is
 * app-owned in glr_state (GlrRenderState.lights), as are policy toggles such
 * as msaa, line smoothing, accumulation AA, and point-attenuation enablement. */
ReplRenderState        repl_state_render(void);
ReplRenderState       *repl_state_render_mut(void);
/* Reset the runtime-mutated render halves (`light_enabled_mask`,
 * `clear_color[]`) to defaults. The render-config toggles (msaa,
 * line_smooth, accum_*) and the dimensional `lights[]` table moved to
 * glr_state — call `glr_state_render_reset_defaults()` for those. Both
 * reset paths fire from `glr_ctrl_reset_all`. */
void                   repl_state_render_reset_defaults(void);
void                   repl_state_render_set_clear_color(const float rgba[4]);
void                   repl_state_render_set_light_enabled(int light_idx, int enabled);
void                   repl_state_render_clear_light_enabled_mask(void);

ReplSceneRuntimeState    repl_state_scenes(void);
ReplSceneRuntimeState       *repl_state_scenes_mut(void);
ReplSceneRuntimeState       *repl_state_scenes_writable(void);
void                         repl_state_scenes_set_active_example_idx(int idx);

ReplImportExportView        repl_state_import_export(void);
ReplImportExportState       *repl_state_import_export_mut(void);
ReplImportExportState       *repl_state_import_export_writable(void);
void                         repl_state_import_export_reset(void);
void                         repl_state_refresh_workspace_header_lines(void);



/* Reset REPL-owned program state to defaults. Does NOT reset
 * peer/editor/UI state — that's glr_ctrl_reset_all in glr_ctrl.h.
 * The demo and any pure-REPL test fixture call this; production
 * startup and tests that need full-world reset call
 * glr_ctrl_reset_all instead. */
void repl_state_reset_program(void);

/* Patch the live REPL state with its non-zero sentinel defaults (most
 * importantly the document/flat-program capacities, which are 0 under raw
 * BSS zero-fill and would otherwise reject every command-store insert).
 * Idempotent and process-global: a no-op once any reset entry has run.
 * For callers that load into the store WITHOUT a prior reset_program /
 * glr_ctrl_reset_all — e.g. the --dump-code / --dump-flat CLI path, which
 * skips glr_ctrl_init_gl. */
void repl_state_ensure_sentinels(void);

#endif /* REPL_STATE_OWNERS_H */
