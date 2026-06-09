#define REPL_STATE_IMPLEMENTATION
#include "repl/state.h"
#include "repl/command_store.h"
#include "source_document.h" /* source_document_clear */
#include "repl/core.h"
#include "repl/eval.h"
#include "repl/pipeline.h"
#include "repl/scenes.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#undef REPL_STATE_IMPLEMENTATION

#include <string.h>  /* snprintf for cam_lines init */

/* Presentation + render slices moved to glr_state.c, dropping the
 * last `glr_camera.h`
 * include from this TU. The previous `glr_camera_mut()->auto_rotate`
 * resets in `_presentation_reset_*` are gone — `auto_rotate` is part
 * of the per-scene cfg snapshot and the cfg bridge's `apply` callback
 * restores it during scene switches (implemented as step 7a of
 * feature/decouple-repl-from-gl-repl-alt.md). */

static ReplRuntimeState g_repl_state;  /* BSS zero-initialised */

/* Writes the non-zero default values that distinguish a valid default
 * ReplRuntimeState from raw zero-fill.  Mirrors the old
 * state_defaults.inc designator initialiser. */
static void repl_state_apply_sentinels(ReplRuntimeState *s) {
    /* --- document --- */
    s->document.capacity       = MAX_COMMANDS;
    s->document.normals_dirty  = 1;

    /* --- flat_program --- */
    s->flat_program.capacity                    = MAX_COMMANDS;
    s->flat_program.dirty                       = 1;
    s->flat_program.current_block_begin_idx      = -1;
    s->flat_program.current_block_end_idx        = -1;
    s->flat_program.current_block_source_line_idx = -1;

    /* --- variables --- */
    s->variables.time_var_idx  = -1;
    s->variables.time_playing  = 1;

    /* --- render: lights ---
     * The REPL pipeline owns only what the executor mutates: the
     * light-enable bitmask, set/cleared in response to glEnable(GL_LIGHTn)
     * / glDisable(GL_LIGHTn) and recomputed each walk (defaults to 0 via
     * the zero-init above — no light is enabled until the program says so).
     * Positions, colors, eye-space, and the active lighting theme are
     * *presentation* concerns owned by the app shell (GlrRenderState.lights,
     * seeded by the controller via scene_lights_apply_theme). */
    s->render.light_enabled_mask = 0;

    s->render.clear_color[0] = 0.10f;
    s->render.clear_color[1] = 0.10f;
    s->render.clear_color[2] = 0.10f;
    s->render.clear_color[3] = 1.0f;

    /* --- scenes --- */
    s->scenes.active_example_idx = -1;

    /* --- import_export: default render-state + cam lines --- */
    snprintf(s->import_export.render_state_lines[0], RENDER_STATE_LINE_LEN,
             "  glEnable(GL_MULTISAMPLE);");
    snprintf(s->import_export.render_state_lines[1], RENDER_STATE_LINE_LEN,
             "  glDisable(GL_LINE_SMOOTH);");
    snprintf(s->import_export.cam_lines[0], CAM_LINE_LEN,
             "  glTranslatef(0.0000f, 0.0000f, -5.0000f);");
    snprintf(s->import_export.cam_lines[1], CAM_LINE_LEN,
             "  glRotatef(20.0000f, 1.0f, 0.0f, 0.0f);");
    snprintf(s->import_export.cam_lines[2], CAM_LINE_LEN,
             "  glRotatef(30.0000f, 0.0f, 1.0f, 0.0f);");
    snprintf(s->import_export.cam_lines[3], CAM_LINE_LEN,
             "  glTranslatef(0.0000f, 0.0000f, 0.0000f);");
}

/* Lazily-initialised defaults — avoids embedding a 4.6 MB const copy
 * of ReplRuntimeState in the object file. Pure read: just fills the
 * file-static `defaults` buffer on first call and hands back a const
 * pointer to it. Callers that also need the live g_repl_state
 * sentinel-patched must invoke repl_state_ensure_sentinels (or any
 * reset entry, which already chains through it). */
static const ReplRuntimeState *repl_state_defaults(void) {
    static ReplRuntimeState defaults;
    static int inited;
    if (!inited) {
        repl_state_apply_sentinels(&defaults);
        inited = 1;
    }
    return &defaults;
}

/* Patch the live g_repl_state with the sentinel/default field values
 * once per process. Used by callers that may run before an explicit
 * reset_program() and would otherwise see uninitialized state.
 * Idempotent — the second-and-later calls are cheap no-ops. */
void repl_state_ensure_sentinels(void) {
    static int patched;
    if (patched) return;
    repl_state_apply_sentinels(&g_repl_state);
    patched = 1;
}

#define g_cmds                      (g_repl_state.document.cmds)
#define g_num_cmds                  (g_repl_state.document.cmd_count)
/* g_edit_line removed; edit-line cursor moved to EditorState
 * (implemented in Phase 4 of plans/in-review/edit-line-ownership.md). */
#define g_normals_dirty             (g_repl_state.document.normals_dirty)
#define g_num_flat_cmds             (g_repl_state.flat_program.cmd_count)
#define g_flat_dirty                (g_repl_state.flat_program.dirty)
#define g_user_lighting_enabled     (g_repl_state.flat_program.user_lighting_enabled)
#define g_current_block_begin       (g_repl_state.flat_program.current_block_begin_idx)
#define g_current_block_end         (g_repl_state.flat_program.current_block_end_idx)
#define g_current_block_line        (g_repl_state.flat_program.current_block_source_line_idx)
/* g_input and related macros removed (Phase 1 commit 5). */
/* g_clipboard and related macros removed (Phase 1 commit 6). */
#define g_anim_time                 (g_repl_state.variables.anim_time)
#define g_t_playing                 (g_repl_state.variables.time_playing)
#define g_t_var_idx                 (g_repl_state.variables.time_var_idx)
/* code_panel, scroll, help, variable_panel macros removed. */
/* g_drag_* macros removed (Phase 1 commit 9). */
/* g_show_profile_panel macro removed (Phase 1 commit 8); profile_panel
 * lives on g_ui_state.profile_panel in ui_state.c. */
/* g_cam_* macros removed (Phase A commit 13); the camera slice lives
 * on g_ui_state.camera in ui_state.c, and the camera setters call
 * ui_state_camera_* directly.
 * g_mouse_* and g_win_* macros removed (Phase 1 commit 8); pointer and
 * viewport slices live on g_ui_state.{pointer,viewport} in ui_state.c. */
/* presentation storage macros + render-config toggles removed (step 7a).
 * Those fields live on `g_glr_state.{presentation,render}` in
 * glr_state.c. The dead `g_focus_vtx` / `g_focus_vtx_valid` fields
 * were dropped along with the move. The runtime-mutated render halves
 * (`lights[]`, `clear_color[]`) stay here because the executor writes
 * them in response to user GL commands. */
/* g_status / g_status_ttl macros removed (Phase 1 commit 8); status
 * lives on g_ui_state.status in ui_state.c.
 * g_cursor_px / g_cursor_py macros removed (Phase A commit 12); the
 * code_panel slice lives on g_ui_state.code_panel in ui_state.c. */
/* g_replay_* macros removed (Phase F commit 33); replay state lives
 * on the replay peer (replay_state.c). Callers use replay_state_view
 * / replay_state_mut directly (Phase J7 retired the legacy
 * repl_state_replay forwarders). */
static void repl_state_bind_eval_predef_storage(void) {
    repl_eval_bind_predef_storage(g_repl_state.variables.predef_vars,
                                  &g_repl_state.variables.predef_var_count);
    repl_eval_bind_scratch_storage(g_repl_state.variables.scratch_arrays);
    repl_func_alias_bind_storage(g_repl_state.variables.func_aliases);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
static void repl_state_bind_eval_predef_storage_at_load(void) {
    repl_state_bind_eval_predef_storage();
}
#endif

static void ensure_t_var_idx(void) {
    if (g_t_var_idx >= 0 && g_t_var_idx < g_num_predef_vars &&
        strcmp(g_predef_vars[g_t_var_idx].name, "t") == 0)
        return;
    g_t_var_idx = repl_eval_find_predef_var_idx("t");
}

static void reset_time_state(void) {
    g_anim_time = 0.0f;
    repl_eval_init_predef_vars();
    ensure_t_var_idx();
}

ReplDocumentState *repl_state_document_mut(void) {
    return &g_repl_state.document;
}

const GLCmd *repl_state_document_cmds(void) {
    return g_repl_state.document.cmds;
}

GLCmd *repl_state_document_cmds_mut(void) {
    return g_repl_state.document.cmds;
}

const GLCmd *repl_state_document_cmd_at(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= g_num_cmds)
        return NULL;
    return &g_cmds[cmd_idx];
}

GLCmd *repl_state_document_cmd_at_mut(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= g_num_cmds)
        return NULL;
    return &g_cmds[cmd_idx];
}

int repl_state_document_count(void) {
    return g_num_cmds;
}

void repl_state_document_count_set(int cmd_count) {
    g_num_cmds = cmd_count;
}

int repl_state_document_capacity(void) {
    return MAX_COMMANDS;
}

/* repl_state_edit_line / _set / _clamp were deleted. Edit-line cursor storage
 * moved to EditorState; editor code uses
 * editor_state_edit_line / _set / _clamp directly, and REPL pipeline
 * code receives the cursor as a function parameter or via the
 * repl_dispatch_edit_line_get / _set sink (implemented in phase 4 of
 * the edit-line-ownership plan). */

int repl_state_normals_dirty(void) {
    return g_normals_dirty;
}

void repl_state_normals_dirty_clear(void) {
    g_normals_dirty = 0;
}

/* Clears the source-command array and the source-text document.
 * Does NOT touch the edit-line cursor — that storage moved to
 * EditorState and
 * sits on the other side of the β boundary from this TU.
 *
 * Wholesale-reset callers must position the cursor themselves at
 * the same boundary. Above the β boundary use
 * editor_state_edit_line_set(0) directly; from REPL pipeline files
 * route through repl_dispatch_edit_line_set(0). The "reset-and-
 * reposition" pair is what repl_scenes_reset_for_transient() does
 * for transient scene switches; editor_clear_all_cmds() does the
 * same dance for the editor-side Clear All (implemented in phase 4 of
 * the edit-line-ownership plan). */
void repl_state_document_reset(void) {
    ReplCommandStore store = repl_command_store_live();
    repl_command_store_load(&store, NULL, 0);
    source_document_clear();
}

ReplFlatProgramState *repl_state_flat_program_mut(void) { return &g_repl_state.flat_program; }
ReplFlatProgramState *repl_state_flat_program_writable(void) { return &g_repl_state.flat_program; }

const GLCmd *repl_state_flat_program_cmds(void) {
    return g_repl_state.flat_program.cmds;
}

const FlatCmdLocalVars *repl_state_flat_program_local_vars(void) {
    return g_repl_state.flat_program.local_vars;
}

GLCmd *repl_state_flat_program_cmds_mut(void) {
    return g_repl_state.flat_program.cmds;
}

FlatCmdLocalVars *repl_state_flat_program_local_vars_mut(void) {
    return g_repl_state.flat_program.local_vars;
}

int repl_state_flat_program_count(void) {
    return g_num_flat_cmds;
}

void repl_state_flat_program_set_count(int cmd_count) {
    if (cmd_count < 0)
        cmd_count = 0;
    if (cmd_count > MAX_COMMANDS)
        cmd_count = MAX_COMMANDS;
    g_num_flat_cmds = cmd_count;
}

int repl_state_flat_program_dirty(void) {
    return g_flat_dirty;
}

void repl_state_flat_program_clear_dirty(void) {
    g_flat_dirty = 0;
}

int repl_state_flat_program_user_lighting_enabled(void) {
    return g_user_lighting_enabled;
}

int repl_state_flat_program_current_block_begin(void) {
    return g_current_block_begin;
}

int repl_state_flat_program_current_block_end(void) {
    return g_current_block_end;
}

int repl_state_flat_program_current_block_source_line(void) {
    return g_current_block_line;
}

void repl_state_flat_program_set_user_lighting_enabled(int enabled) {
    g_user_lighting_enabled = enabled ? 1 : 0;
}

void repl_state_flat_program_set_current_block(int begin_idx, int end_idx,
                                               int source_line_idx) {
    g_current_block_begin = begin_idx;
    g_current_block_end = end_idx;
    g_current_block_line = source_line_idx;
}

void repl_state_flat_program_clear_current_block(void) {
    repl_state_flat_program_set_current_block(-1, -1, -1);
}

void repl_state_flat_program_reset(void) {
    g_num_flat_cmds = 0;
    g_flat_dirty = 1;
    g_user_lighting_enabled = 0;
    repl_state_flat_program_clear_current_block();
}

void repl_state_mark_flat_dirty(void) {
    g_flat_dirty = 1;
}

void repl_state_mark_source_dirty(void) {
    /* Source changed: every cache derived from it must be invalidated.
     * - The auto-normal pass rebuilds on demand.
     * - The flat program rebuilds on demand.
     * - The source-scope depth cache (block depths, indent) rebuilds
     *   on demand.
     *
     * Don't try to invalidate caches independently — every source
     * mutation goes through here, and that's the contract callers rely
     * on. */
    g_normals_dirty = 1;
    g_flat_dirty = 1;
    repl_source_scope_depth_cache_invalidate();
}

FlatProgramView repl_state_flat_program_view(void) {
    FlatProgramView view = {
        .cmds = g_repl_state.flat_program.cmds,
        .local_vars = g_repl_state.flat_program.local_vars,
        .cmd_count = g_repl_state.flat_program.cmd_count,
    };
    return view;
}

ReplVariableView repl_state_variables(void) {
    return (ReplVariableView){
        .vars = g_predef_vars,
        .var_count = g_num_predef_vars,
        .scratch_arrays = g_repl_state.variables.scratch_arrays,
        .scratch_array_count = REPL_SCRATCH_ARRAY_COUNT,
        .scratch_array_len = REPL_SCRATCH_ARRAY_LEN,
        .time_var_idx = g_t_var_idx,
        .time_playing = g_t_playing,
        .anim_time = g_anim_time,
    };
}

ReplVariableState *repl_state_variables_mut(void) {
    return &g_repl_state.variables;
}

void repl_state_time_advance(float dt) {
    if (dt <= 0.0f)
        return;

    ensure_t_var_idx();
    g_anim_time += dt;
    if (g_t_playing && g_t_var_idx >= 0) {
        g_predef_vars_mut[g_t_var_idx].value += dt;
        g_flat_dirty = 1;
    }
}

void repl_state_time_reset_to_zero(void) {
    repl_state_time_set(0.0f);
}

void repl_state_time_set(float value) {
    ensure_t_var_idx();
    /* Keep the free-running accumulator in step with the visible clock so
     * later pause/resume math stays consistent. */
    g_anim_time = value;
    if (g_t_var_idx < 0)
        return;

    g_predef_vars_mut[g_t_var_idx].value = value;
    g_flat_dirty = 1;
}

void repl_state_time_set_transient(float value) {
    /* Override only the predef 't' binding, leaving the free-running clock
     * (g_anim_time) and the flat-dirty flag untouched — unlike
     * repl_state_time_set, this does not perturb the running animation. It
     * lets a caller evaluate the program at an alternate t for a transient
     * purpose (e.g. sampling sub-frame times) without advancing or dirtying
     * anything. The caller owns re-flattening at the new t if it needs the
     * geometry re-baked, and restoring the prior binding afterward. */
    ensure_t_var_idx();
    if (g_t_var_idx >= 0)
        g_predef_vars_mut[g_t_var_idx].value = value;
}

/* Editor-input + editor-buffer accessors moved to editor_state.c
 * (Phase 1 commits 4-5). Use editor_state_input / _mut / _reset for
 * the input slice, editor_state_buffer / _mut for the whole-buffer
 * struct, and editor_buffer_* for slice-level line text. */

/* Editor overlay snapshot list accessors (transformers / highlights /
 * virtual_lines) moved to editor_state.c (Phase 1 commit 9). Use
 * editor_state_transformers / _highlights / _virtual_lines. */

/* editor_state_input_reset and the editor_input convenience getters
 * (input_text / input_len / cursor_pos / insert_mode / pending_newline_*)
 * moved to editor_state.c (Phase 1 commit 5). The editor_state_input
 * struct accessor and the new editor_state_input_reset entry point
 * live there too. */

/* Selection + clipboard accessors moved to editor_state.c
 * (Phase 1 commit 6). Use editor_state_selection / _clipboard. */

/* code_panel / camera / status / help / variable_panel /
 * profile_panel / pointer / viewport accessors all live on
 * ui_state.c (Phase 1 commit 8 + Phase A commits 12-14). Use the
 * canonical `ui_state_*` API directly; the transitional
 * `repl_state_*` forwarder block was removed in Phase A commit 14.
 *
 * Variable-drag accessors live on the variable_panel peer
 * (variable_panel.h). Use `variable_panel_drag` /
 * `variable_panel_handle_drag_*` directly.
 *
 * Search + autocomplete accessors moved to editor_state.c (Phase 1
 * commit 7). Use editor_state_search / _autocomplete. */

/* repl_state_presentation* / _grid_*_steps / _extents and the
 * render-config toggle accessors moved to glr_state.c (step 7a). The
 * runtime-mutated render halves (`lights[]`, `clear_color[]`) keep
 * these accessors because the executor writes them. Presentation
 * reset paths are now `glr_state_presentation_reset_defaults` /
 * `_example_defaults` and `glr_state_render_reset_defaults`, called
 * from `glr_ctrl_reset_all`. The example loader's per-load reset
 * routes through the controller-installed
 * `ReplHostEffects.example_presentation_reset` callback. */

ReplRenderState repl_state_render(void) {
    return g_repl_state.render;
}

ReplRenderState *repl_state_render_mut(void) { return &g_repl_state.render; }
void repl_state_render_set_clear_color(const float rgba[4]) { memcpy(g_repl_state.render.clear_color, rgba, sizeof(float) * 4); }
void repl_state_render_set_light_enabled(int light_idx, int enabled) {
    if (light_idx < 0 || light_idx >= REPL_LIGHT_SLOT_COUNT) return;
    if (enabled) g_repl_state.render.light_enabled_mask |= (1u << light_idx);
    else         g_repl_state.render.light_enabled_mask &= ~(1u << light_idx);
}
void repl_state_render_clear_light_enabled_mask(void) { g_repl_state.render.light_enabled_mask = 0; }

void repl_state_render_reset_defaults(void) {
    g_repl_state.render = repl_state_defaults()->render;
}

/* The legacy `repl_state_replay` / `_mut` / `_reset` forwarders are
 * gone. Callers use `replay_state_view` /
 * `replay_state_mut` / `replay_state_reset` directly. The
 * `check-replay-forwarders` ratchet is at 0/0 (implemented in
 * phase J7). */

ReplSceneRuntimeState repl_state_scenes(void) {
    return g_repl_state.scenes;
}

ReplSceneRuntimeState *repl_state_scenes_mut(void) { return &g_repl_state.scenes; }
ReplSceneRuntimeState *repl_state_scenes_writable(void) { return &g_repl_state.scenes; }
int repl_state_active_example_idx(void) { return g_repl_state.scenes.active_example_idx; }
const char *repl_state_workspace_dir(void) { return g_repl_state.scenes.workspace_dir; }
void repl_state_scenes_set_active_example_idx(int idx) { g_repl_state.scenes.active_example_idx = idx; }

ReplImportExportView repl_state_import_export(void) {
    return (ReplImportExportView){
        .workspace_header_lines = g_repl_state.import_export.workspace_header_lines,
        .workspace_header_line_count = g_repl_state.import_export.workspace_header_line_count,
        .render_state_lines = g_repl_state.import_export.render_state_lines,
        .cam_lines = g_repl_state.import_export.cam_lines,
        .export_scene_name_hint = g_repl_state.import_export.export_scene_name_hint,
        .pending_scene_name = g_repl_state.import_export.pending_scene_name,
        .pending_workspace_dir = g_repl_state.import_export.pending_workspace_dir,
    };
}

ReplImportExportState *repl_state_import_export_mut(void) { return &g_repl_state.import_export; }
ReplImportExportState *repl_state_import_export_writable(void) { return &g_repl_state.import_export; }

void repl_state_capture(ReplRuntimeState *snapshot) {
    if (!snapshot)
        return;

    repl_state_bind_eval_predef_storage();
    repl_state_refresh_workspace_header_lines();
    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();
    *snapshot = g_repl_state;
}

void repl_state_restore(const ReplRuntimeState *snapshot) {
    if (!snapshot)
        return;

    g_repl_state = *snapshot;
    repl_state_bind_eval_predef_storage();
    ensure_t_var_idx();
    repl_source_scope_depth_cache_invalidate();
}

void repl_state_import_export_reset(void) {
    g_repl_state.import_export = repl_state_defaults()->import_export;
}

/* Reset REPL-owned slices to defaults. Peer/editor/UI/autocomplete
 * registration / chrome sync / derived export+camera text caches are
 * NOT included here — they live on glr_ctrl_reset_all in glr_ctrl.c
 * (see step 2 of feature/decouple-repl-from-gl-repl-alt.md).
 *
 * The derived-text refreshes (refresh_workspace_header_lines /
 * update_render_state_strings / update_cam_lines) read app-side state
 * through glr_config_get and the camera. They cannot run here without
 * pulling app/UI/peer state into the demo link set, AND if they ran
 * here they would pre-fire before glr_ctrl_reset_all has finished
 * resetting peers — the cached strings would briefly reflect
 * pre-reset peer state. The frame loop refreshes them every frame
 * anyway; tests that need them populated immediately go through
 * glr_ctrl_reset_all.
 *
 * Do NOT call any non-REPL reset entry from this function. The
 * separation is what lets tools/repl_demo build without stubbing
 * editor / UI / peer reset symbols. */
void repl_state_reset_program(void) {
    g_repl_state = *repl_state_defaults();
    repl_state_ensure_sentinels();
    repl_state_bind_eval_predef_storage();
    repl_scenes_reset();
    reset_time_state();
    repl_source_scope_depth_cache_invalidate();
    repl_state_mark_flat_dirty();
    repl_state_mark_source_dirty();
}
