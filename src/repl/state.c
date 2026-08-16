#define REPL_STATE_IMPLEMENTATION
#include <stdio.h>
#include "repl/export.h"
#include "repl/flatten.h"
#include "repl/state.h"
#include "repl/command_store.h"
#include "source_document.h" /* source_document_clear */
#include "repl/eval.h"
#include "repl/expr_program.h"
#include "repl/pipeline.h"
#include "repl/scenes.h"
#include "repl/source_scope.h"
#include "repl/state_notify.h"
#include "repl/state_owners.h"
#undef REPL_STATE_IMPLEMENTATION

#include <string.h>  /* snprintf for cam_lines init */

static ReplRuntimeState g_repl_state;  /* BSS zero-initialised */

/* Writes the non-zero default values that distinguish a valid default
 * ReplRuntimeState from raw zero-fill. */
static void repl_state_apply_sentinels(ReplRuntimeState *s) {
    /* --- document --- */
    s->document.capacity       = MAX_EDITOR_COMMANDS;
    s->document.normals_dirty  = 1;
    s->document.source_uses_time_dirty = 1;

    /* --- flat_program --- */
    s->flat_program.capacity                    = MAX_FLAT_COMMANDS;
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
     * the zero-init above - no light is enabled until the program says so).
     * Positions, colors, eye-space, and the active lighting theme are
     * *presentation* concerns owned by the app shell (GlrRenderState.lights,
     * seeded by the controller via render3d_lights_apply_theme). */
    s->render.light_enabled_mask = 0;

    /* --- scene_runtime --- */
    s->scene_runtime.active_example_idx  = -1;
    s->scene_runtime.example_place_idx   = -1;
    s->scene_runtime.tutorial_origin_idx = -1;

    /* --- import_export: default render-state + cam lines --- */
    snprintf(s->import_export.render_state_lines[0], RENDER_STATE_LINE_LEN,
             "  glEnable(GL_MULTISAMPLE);");
    snprintf(s->import_export.render_state_lines[1], RENDER_STATE_LINE_LEN,
             "  glDisable(GL_LINE_SMOOTH);");
    snprintf(s->import_export.cam_lines[0], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(0.0000f, 0.0000f, -5.0000f);");
    snprintf(s->import_export.cam_lines[1], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(20.0000f, 1.0f, 0.0f, 0.0f);");
    snprintf(s->import_export.cam_lines[2], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(30.0000f, 0.0f, 1.0f, 0.0f);");
    snprintf(s->import_export.cam_lines[3], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(0.0000f, 0.0000f, 0.0000f);");
}

/* Lazily-initialised defaults - avoids embedding a 4.6 MB const copy
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
 * Idempotent - the second-and-later calls are cheap no-ops. */
void repl_state_ensure_sentinels(void) {
    static int patched;
    if (patched) return;
    repl_state_apply_sentinels(&g_repl_state);
    patched = 1;
}

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
    if (g_repl_state.variables.time_var_idx >= 0 &&
        g_repl_state.variables.time_var_idx < g_num_predef_vars &&
        strcmp(g_predef_vars[g_repl_state.variables.time_var_idx].name,
               "t") == 0)
        return;
    g_repl_state.variables.time_var_idx = repl_eval_find_predef_var_idx("t");
}

static int scan_source_uses_time(void) {
    SourceTextView text = source_document_view();
    int line_count = text.line_count;

    if (g_repl_state.document.cmd_count > 0 &&
        (!text.lines || line_count < g_repl_state.document.cmd_count))
        return 1;
    if (!text.lines || line_count <= 0)
        return 0;
    if (line_count > MAX_EDITOR_COMMANDS)
        line_count = MAX_EDITOR_COMMANDS;

    for (int line_idx = 0; line_idx < line_count; line_idx++) {
        if (repl_eval_source_uses_ident(source_text_line(text, line_idx), "t"))
            return 1;
    }
    return 0;
}

int repl_state_source_uses_time(void) {
    repl_state_ensure_sentinels();
    if (g_repl_state.document.source_uses_time_dirty) {
        g_repl_state.document.source_uses_time = scan_source_uses_time();
        g_repl_state.document.source_uses_time_dirty = 0;
    }
    return g_repl_state.document.source_uses_time;
}

static void reset_time_state(void) {
    g_repl_state.variables.anim_time = 0.0f;
    repl_eval_init_predef_vars();
    ensure_t_var_idx();
}

ReplDocumentState *repl_state_document_mut(void) {
    return &g_repl_state.document;
}

const GLCmd *repl_state_document_cmds(void) {
    return g_repl_state.document.cmds;
}

const GLCmd *repl_state_document_cmd_at(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= g_repl_state.document.cmd_count)
        return NULL;
    return &g_repl_state.document.cmds[cmd_idx];
}

int repl_state_document_count(void) {
    return g_repl_state.document.cmd_count;
}

void repl_state_document_count_set(int cmd_count) {
    /* Clamp like repl_state_flat_program_set_count: every reader walks
     * cmds[0, cmd_count) with no second bound, so an out-of-range count
     * reads past the array. */
    if (cmd_count < 0)
        cmd_count = 0;
    if (cmd_count > MAX_EDITOR_COMMANDS)
        cmd_count = MAX_EDITOR_COMMANDS;
    g_repl_state.document.cmd_count = cmd_count;
}

int repl_state_normals_dirty(void) {
    return g_repl_state.document.normals_dirty;
}

void repl_state_normals_dirty_clear(void) {
    g_repl_state.document.normals_dirty = 0;
}

/* Clears source-command storage and source text. The edit-line cursor is
 * editor-owned, so reset callers position it at their own boundary. */
void repl_state_document_reset(void) {
    ReplCommandStore store = repl_command_store_live();
    repl_command_store_load(&store, NULL, 0);
    source_document_clear();
}

ReplFlatProgramState *repl_state_flat_program_writable(void) { return &g_repl_state.flat_program; }

const GLCmd *repl_state_flat_program_cmds(void) {
    return g_repl_state.flat_program.cmds;
}

const FlatCmdLocalVars *repl_state_flat_program_local_vars(void) {
    return g_repl_state.flat_program.local_vars;
}

int repl_state_flat_program_count(void) {
    return g_repl_state.flat_program.cmd_count;
}

void repl_state_flat_program_set_count(int cmd_count) {
    if (cmd_count < 0)
        cmd_count = 0;
    if (cmd_count > MAX_FLAT_COMMANDS)
        cmd_count = MAX_FLAT_COMMANDS;
    g_repl_state.flat_program.cmd_count = cmd_count;
}

int repl_state_flat_program_dirty(void) {
    return g_repl_state.flat_program.dirty;
}

void repl_state_flat_program_clear_dirty(void) { g_repl_state.flat_program.dirty = 0; }
void repl_state_flat_program_clear_args_dirty(void) { g_repl_state.flat_program.args_dirty_mask = 0; }

int repl_state_flat_program_user_lighting_enabled(void) {
    return g_repl_state.flat_program.user_lighting_enabled;
}

int repl_state_flat_program_current_block_begin(void) {
    return g_repl_state.flat_program.current_block_begin_idx;
}

int repl_state_flat_program_current_block_end(void) {
    return g_repl_state.flat_program.current_block_end_idx;
}

int repl_state_flat_program_current_block_source_line(void) {
    return g_repl_state.flat_program.current_block_source_line_idx;
}

void repl_state_flat_program_set_user_lighting_enabled(int enabled) {
    g_repl_state.flat_program.user_lighting_enabled = enabled ? 1 : 0;
}

ReplExprDepMask repl_state_flat_program_structural_dep_mask(void) { return g_repl_state.flat_program.structural_dep_mask; }
ReplExprDepMask repl_state_flat_program_value_dep_mask(void) { return g_repl_state.flat_program.value_dep_mask; }
ReplExprDepMask repl_state_flat_program_args_dirty_mask(void) { return g_repl_state.flat_program.args_dirty_mask; }
int repl_state_flat_program_rebake_ok(void) { return g_repl_state.flat_program.rebake_ok; }

void repl_state_flat_program_set_dep_state(ReplExprDepMask structural_dep_mask,
                                           ReplExprDepMask value_dep_mask,
                                           int rebake_ok) {
    /* A full flatten just re-derived the masks; whatever args-only dirt
     * was pending is subsumed by that rebuild. */
    g_repl_state.flat_program.structural_dep_mask = structural_dep_mask;
    g_repl_state.flat_program.value_dep_mask = value_dep_mask;
    g_repl_state.flat_program.args_dirty_mask = 0;
    g_repl_state.flat_program.rebake_ok = rebake_ok ? 1 : 0;
}

void repl_state_notify_predef_value_changed(int slot) {
    ReplFlatProgramState *fp = &g_repl_state.flat_program;

    if (fp->dirty)
        return; /* full rebuild already pending; it subsumes everything */
    if (slot < 0 || slot >= MAX_PREDEF_VARS) {
        fp->dirty = 1;
        return;
    }
    ReplExprDepMask bit = (ReplExprDepMask)1u << slot;
    if (fp->structural_dep_mask & bit) {
        fp->dirty = 1;
        fp->args_dirty_mask = 0;
    } else if (fp->value_dep_mask & bit) {
        if (fp->rebake_ok)
            fp->args_dirty_mask |= bit;
        else
            fp->dirty = 1;
    }
    /* else: root unused by the current flat program - nothing to do. */
}

void repl_state_flat_program_set_current_block(int begin_idx, int end_idx,
                                               int source_line_idx) {
    g_repl_state.flat_program.current_block_begin_idx = begin_idx;
    g_repl_state.flat_program.current_block_end_idx = end_idx;
    g_repl_state.flat_program.current_block_source_line_idx = source_line_idx;
}

void repl_state_flat_program_clear_current_block(void) {
    repl_state_flat_program_set_current_block(-1, -1, -1);
}

void repl_state_mark_flat_dirty(void) {
    g_repl_state.flat_program.dirty = 1;
}

void repl_state_mark_source_dirty(void) {
    /* Source changed: every cache derived from it must be invalidated.
     * - The auto-normal pass rebuilds on demand.
     * - The flat program rebuilds on demand.
     * - The source-scope depth cache (block depths, indent) rebuilds
     *   on demand.
     * - The compiled-expression cache rebuilds lazily per line on the
     *   next full flatten.
     *
     * Don't try to invalidate caches independently - every source
     * mutation goes through here, and that's the contract callers rely
     * on. */
    g_repl_state.document.normals_dirty = 1;
    g_repl_state.document.source_uses_time_dirty = 1;
    g_repl_state.flat_program.dirty = 1;
    repl_source_scope_depth_cache_invalidate();
    repl_expr_cache_invalidate(repl_expr_cache_live());
}

void repl_mark_source_dirty(void) {
    repl_state_mark_source_dirty();
}

FlatProgramView repl_state_flat_program_view(void) {
    return (FlatProgramView){
        .cmds = g_repl_state.flat_program.cmds,
        .local_vars = g_repl_state.flat_program.local_vars,
        .cmd_count = g_repl_state.flat_program.cmd_count,
        .overflow_cmd_count = g_repl_state.flat_program.overflow_cmd_count,
        .call_frame_idx = g_repl_state.flat_program.call_frame_idx,
        .call_frames = g_repl_state.flat_program.call_frames,
        .call_frame_count = g_repl_state.flat_program.call_frame_count,
        .call_frame_args = g_repl_state.flat_program.call_frame_args,
        .call_frame_arg_count = g_repl_state.flat_program.call_frame_arg_count,
        .call_frame_overflow = g_repl_state.flat_program.call_frame_overflow,
    };
}

ReplVariableView repl_state_variables(void) {
    return (ReplVariableView){
        .vars = g_predef_vars,
        .var_count = g_num_predef_vars,
        .scratch_arrays = g_repl_state.variables.scratch_arrays,
        .scratch_array_count = REPL_SCRATCH_ARRAY_COUNT,
        .scratch_array_len = REPL_SCRATCH_ARRAY_LEN,
        .time_var_idx = g_repl_state.variables.time_var_idx,
        .time_playing = g_repl_state.variables.time_playing,
        .anim_time = g_repl_state.variables.anim_time,
    };
}

ReplVariableState *repl_state_variables_mut(void) {
    return &g_repl_state.variables;
}

void repl_state_variables_reset_predefs(void) { reset_time_state(); }

void repl_state_time_advance(float dt) {
    if (dt <= 0.0f)
        return;

    ensure_t_var_idx();
    g_repl_state.variables.anim_time += dt;
    if (g_repl_state.variables.time_playing &&
        g_repl_state.variables.time_var_idx >= 0) {
        g_predef_vars_mut[g_repl_state.variables.time_var_idx].value += dt;
        /* Route by the flat program's dependency masks, not the textual
         * source_uses_time scan: t may be value-only, structural, or unused. */
        repl_state_notify_predef_value_changed(
            g_repl_state.variables.time_var_idx);
    }
}

void repl_state_time_reset_to_zero(void) {
    repl_state_time_set(0.0f);
}

void repl_state_time_set(float value) {
    ensure_t_var_idx();
    /* Keep the free-running accumulator in step with the visible clock so
     * later pause/resume math stays consistent. */
    g_repl_state.variables.anim_time = value;
    if (g_repl_state.variables.time_var_idx < 0)
        return;

    g_predef_vars_mut[g_repl_state.variables.time_var_idx].value = value;
    repl_state_notify_predef_value_changed(g_repl_state.variables.time_var_idx);
}

void repl_state_time_set_playing(int playing) {
    g_repl_state.variables.time_playing = playing ? 1 : 0;
}

void repl_state_time_set_transient(float value) {
    /* Override only the predef 't' binding, leaving the free-running clock
     * (anim_time) and the flat-dirty flag untouched - unlike
     * repl_state_time_set, this does not perturb the running animation. It
     * lets a caller evaluate the program at an alternate t for a transient
     * purpose (e.g. sampling sub-frame times) without advancing or dirtying
     * anything. The caller owns re-flattening at the new t if it needs the
     * geometry re-baked, and restoring the prior binding afterward. */
    ensure_t_var_idx();
    if (g_repl_state.variables.time_var_idx >= 0)
        g_predef_vars_mut[g_repl_state.variables.time_var_idx].value = value;
}

ReplRenderState repl_state_render(void) {
    return g_repl_state.render;
}
ReplRenderState *repl_state_render_mut(void) { return &g_repl_state.render; }
void repl_state_render_set(const ReplRenderState *render) { if (render) g_repl_state.render = *render; }
void repl_state_render_set_light_enabled(int light_idx, int enabled) {
    if (light_idx < 0 || light_idx >= REPL_LIGHT_SLOT_COUNT) return;
    if (enabled) g_repl_state.render.light_enabled_mask |= (1u << light_idx);
    else         g_repl_state.render.light_enabled_mask &= ~(1u << light_idx);
}
void repl_state_render_clear_light_enabled_mask(void) { g_repl_state.render.light_enabled_mask = 0; }
void repl_state_render_set_light_enabled_mask(unsigned mask) { g_repl_state.render.light_enabled_mask = mask; }

void repl_state_render_reset_defaults(void) {
    g_repl_state.render = repl_state_defaults()->render;
}

ReplSceneRuntimeState repl_state_scenes(void) {
    return g_repl_state.scene_runtime;
}

ReplSceneRuntimeState *repl_state_scenes_writable(void) {
    return &g_repl_state.scene_runtime;
}

int repl_state_active_example_idx(void) {
    return g_repl_state.scene_runtime.active_example_idx;
}

int repl_state_tutorial_origin_idx(void) {
    return g_repl_state.scene_runtime.tutorial_origin_idx;
}

const char *repl_state_workspace_dir(void) {
    return g_repl_state.scene_runtime.workspace_dir;
}

void repl_state_scenes_set_active_example_idx(int idx) {
    g_repl_state.scene_runtime.active_example_idx = idx;
}

void repl_state_scenes_set_example_place_idx(int idx) {
    g_repl_state.scene_runtime.example_place_idx = idx;
}

void repl_state_scenes_set_tutorial_origin_idx(int idx) {
    g_repl_state.scene_runtime.tutorial_origin_idx = idx;
}

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

/* DEFERRED (repl-clarity-review.md finding 6, in docs/plans/partial/): the
 * post-restore invalidation tail below is spelled twice - here and in
 * repl_state_checkpoint_restore - with near-identical comments. The only
 * real difference is that the checkpoint path additionally zeroes the flat
 * counts and clears the current block, because it did not copy a flat
 * program in. **Add a line to one copy and you must add it to the other**;
 * the fix (extract the shared tail, keep the checkpoint-only clearing
 * adjacent to its call, name the restore policy rather than passing a
 * `keep_flat_count` bool) is written up in the finding. */
void repl_state_restore(const ReplRuntimeState *snapshot) {
    if (!snapshot)
        return;

    g_repl_state = *snapshot;
    repl_state_bind_eval_predef_storage();
    ensure_t_var_idx();
    g_repl_state.document.source_uses_time_dirty = 1;
    repl_source_scope_depth_cache_invalidate();
    /* The restored snapshot carries a different document (and possibly a
     * reshaped predef table), so compiled expressions - line indices and
     * compile-time-resolved predef slots - are stale wholesale. */
    repl_expr_cache_invalidate(repl_expr_cache_live());
    /* The snapshot's dep-routing state describes its own (now-discarded)
     * flat program and cache. Force a full flatten and drop any inherited
     * rebake eligibility / pending args dirt, so no rebake runs against the
     * just-invalidated cache. */
    g_repl_state.flat_program.dirty = 1;
    g_repl_state.flat_program.args_dirty_mask = 0;
    g_repl_state.flat_program.rebake_ok = 0;
}

void repl_state_checkpoint_capture(ReplCheckpointState *out) {
    if (!out)
        return;

    repl_state_bind_eval_predef_storage();
    repl_state_refresh_workspace_header_lines();
    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();

    /* Everything except the derived flat program. The flat program is
     * rebuilt from the document on the next frame (see the restore path). */
    out->document      = g_repl_state.document;
    out->variables     = g_repl_state.variables;
    out->render        = g_repl_state.render;
    out->scene_runtime = g_repl_state.scene_runtime;
    out->import_export = g_repl_state.import_export;
}

/* The second copy of the post-restore invalidation tail - see the note above
 * repl_state_restore (repl-clarity-review.md finding 6). */
void repl_state_checkpoint_restore(const ReplCheckpointState *snapshot) {
    if (!snapshot)
        return;

    g_repl_state.document      = snapshot->document;
    g_repl_state.variables     = snapshot->variables;
    g_repl_state.render        = snapshot->render;
    g_repl_state.scene_runtime = snapshot->scene_runtime;
    g_repl_state.import_export = snapshot->import_export;

    repl_state_bind_eval_predef_storage();
    ensure_t_var_idx();
    g_repl_state.document.source_uses_time_dirty = 1;
    repl_source_scope_depth_cache_invalidate();
    /* The restored document (and possibly a reshaped predef table) makes
     * every compiled expression - line indices, compile-time predef slots -
     * stale wholesale. */
    repl_expr_cache_invalidate(repl_expr_cache_live());
    /* The flat program is derived and NOT part of this checkpoint. Force a
     * full flatten and drop any inherited rebake eligibility / pending args
     * dirt so no rebake runs against the just-invalidated cache. */
    g_repl_state.flat_program.cmd_count = 0;
    g_repl_state.flat_program.overflow_cmd_count = 0;
    g_repl_state.flat_program.dirty = 1;
    g_repl_state.flat_program.args_dirty_mask = 0;
    g_repl_state.flat_program.rebake_ok = 0;
    repl_state_flat_program_clear_current_block();
}

/* Reset REPL-owned slices only. Peer/editor/UI state, chrome sync, and
 * derived app-facing export/cache refreshes are owned by glr_ctrl_reset_all.
 * Keeping this reset pure-REPL lets tools/repl_demo link without app/editor/UI
 * reset stubs. */
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
