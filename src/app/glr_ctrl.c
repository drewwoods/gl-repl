#include "app/glr_ctrl.h"
#include "app/glr_ctrl_export.h"
#include "app/glr_ctrl_replay_annotations.h"

#include "subsystems/replay/replay_render.h"
#include "subsystems/edit_overlays/edit_overlays.h"

#include "c_compat.h"  /* STATIC_ASSERT (C99/C11 portable) */
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include "gl_includes.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>

#include "app/glr_audio.h"
#include "subsystems/color_picker/color_picker_state.h"
#include "ui/subsystems/color_picker.h"   /* UI_HIT_COLOR_SWATCH routing */
#include "editor/clipboard.h"
#include "app/glr_completion.h"
#include "app/glr_compositor.h"      /* whole-frame post-process hook */
#include "app/glr_defaults.h"        /* CFG_DEFAULT_* */
#include "app/glr_state.h"
#include "editor/commit.h"
#include "editor/completion.h"
#include "editor/help_session.h"
#include "editor/inline_file_prompt.h"
#include "editor/inline_rename.h"
#include "editor/input.h"
#include "editor/search.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "scene/guides/geometry_guides.h" /* scene_geometry_guides_render_for_cursor */
#include "scene/lights.h"                 /* scene_lights_apply_theme */
#include "app/glr_actions.h"
#include "app/glr_config.h"
#include "app/glr_camera.h"
#include "app/glr_camera_export.h"
#include "app/glr_debug.h"
#include "keys.h"
#include "support/memprof.h"
#include "support/cpuprof.h"
#include "repl/core.h"
#include "repl/examples.h"          /* REPL_EXAMPLE_TAG_* */
#include "repl/eval.h"
#include "repl/parser.h"
#include "repl/executor.h"
#include "repl/export.h"
#include "repl/help_text.h"
#include "repl/pipeline.h"
#include "subsystems/replay/replay_annotations.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#include "repl/tutorials.h"
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "ui/subsystems/replay_hud.h"
#include "scene/overlays.h" /* scene_draw_vertex_label_text / _arrow primitives */
#include "scene/palette.h" /* scene_clr / scene_clr_a scene-space colors */
#include "scene/postprocess_filter.h" /* ScenePostFilterMode, mode_name */
#include "scene/render.h"
#include "ui/app/autocomplete_panel.h"
#include "ui/app/editor.h"
#include "ui/app/layout.h"
#include "ui/app/menu_bar.h"
#include "ui/core/metrics.h"
#include "ui/support/memprof.h"
#include "ui/app/numeric_swatch.h"
#include "ui/app/panels.h"
#include "ui/app/repl_code_panel.h"
#include "ui/support/cpuprof.h"
#include "ui/app/snapshot.h"
#include "ui/app/state.h"
#include "ui/app/state_types.h" /* UI-chrome typedefs (CodePanel/Camera/Help/etc.) */
#include "ui/core/tabbed_overlay.h"
#include "ui/subsystems/variable_panel.h"
#include "ui/app/variable_panel_view.h"
#include "app/glr_color_picker_bridge.h"
#include "app/glr_ctrl_internal.h"
#include "subsystems/variable_panel/variable_panel_drag.h"
#include "subsystems/variable_panel/variable_panel_state.h"

/* The REPL pipeline tracks light-enable state for REPL_LIGHT_SLOT_COUNT
 * slots (state_views.h, scene-include-free); the scene render contract sizes
 * its light table by MAX_LIGHTS. The controller is the one TU that sees both,
 * so it pins the two counts together — the per-frame merge in
 * glr_ctrl_build_scene_config indexes both by the same i. */
STATIC_ASSERT(REPL_LIGHT_SLOT_COUNT == MAX_LIGHTS,
              "REPL light-slot count must match scene MAX_LIGHTS");

static int glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(
    const UiRenderSnapshot *snap,
    int *out_follow_doc_line,
    int *out_visible_lines);

#define GLR_CTRL_REPLAY_PANEL_CLEARANCE_PX 10
#define GLR_CTRL_REPLAY_PANEL_BASE_Y_PX    8
#define GLR_CTRL_REPLAY_PANEL_LIFT_EASE    0.22f
#define GLR_CTRL_REPLAY_PANEL_LIFT_SNAP_PX 0.25f

static UiRenderSnapshot g_last_ui_snapshot;
static int g_last_ui_snapshot_valid = 0;
static int g_last_replay_follow_src_line = -1;

/* --- Accumulation motion-blur sub-frame driver ---
 * When accum_effect == BLUR the scene's accum loop calls back per sample to
 * vary the camera (interpolated prev<->cur pose) or the animation time (a
 * sub-step re-bake), accumulating the samples into one blurred frame. The
 * mode is resolved once per frame in glr_ctrl_resolve_blur_subframe(); the
 * baseline snapshot lets each sample start from identical REPL state so
 * accumulating programs don't compound across samples. */
typedef enum { GLR_BLUR_NONE = 0, GLR_BLUR_CAMERA, GLR_BLUR_TIME } GlrBlurMode;
typedef struct {
    GlrBlurMode     mode;
    GlrCameraPose   prev, cur;   /* GLR_BLUR_CAMERA endpoints */
    float           t_end, dt;   /* GLR_BLUR_TIME window [t_end - dt, t_end] */
    int             edit_line;   /* reflatten anchor for time blur */
    float           base_predef[MAX_PREDEF_VARS];
    float           base_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    ReplRenderState base_render;
} GlrSubframeCtx;
static GlrSubframeCtx g_subframe_ctx;
static GlrCameraPose  g_prev_frame_pose;
static int            g_prev_frame_pose_valid = 0;
static GlrCameraPose  g_cur_frame_pose;
typedef ReplExecutorPointParameterProc
(*GlrCtrlPointParameterProcLoaderFn)(const char *proc_name);

static ReplExecutorPointParameterProc
glr_ctrl_default_point_parameter_loader(const char *proc_name) {
#if defined(__APPLE__) && defined(USE_GLUT)
    (void)proc_name;
    return &glPointParameterfv;
#else
    return (ReplExecutorPointParameterProc)glutGetProcAddress(proc_name);
#endif
}

static GlrCtrlPointParameterProcLoaderFn g_glr_ctrl_point_parameter_loader =
    glr_ctrl_default_point_parameter_loader;

static ReplExecutorPointParameterProc
glr_ctrl_load_point_parameter_proc(int has_core, int has_arb, int has_ext) {
    if (!g_glr_ctrl_point_parameter_loader)
        return NULL;
    if (has_core) {
        ReplExecutorPointParameterProc proc = g_glr_ctrl_point_parameter_loader("glPointParameterfv");
        if (proc)
            return proc;
    }
    if (has_arb) {
        ReplExecutorPointParameterProc proc = g_glr_ctrl_point_parameter_loader("glPointParameterfvARB");
        if (proc)
            return proc;
    }
    if (has_ext) {
        ReplExecutorPointParameterProc proc = g_glr_ctrl_point_parameter_loader("glPointParameterfvEXT");
        if (proc)
            return proc;
    }
    /* Some extension-only stacks still expose the unsuffixed symbol
     * through the loader; take it as a final compatibility fallback
     * after the suffixes the extension contract actually names. */
    if (!has_core && (has_arb || has_ext))
        return g_glr_ctrl_point_parameter_loader("glPointParameterfv");
    return NULL;
}

/* Non-static: the input router (src/app/glr_ctrl_router.c) reads this cached
 * snapshot for drag hit-testing. Declared in glr_ctrl_internal.h. */
const UiRenderSnapshot *glr_ctrl_drag_hit_test_snapshot(void) {
    if (!g_last_ui_snapshot_valid) {
        glr_ctrl_build_ui_snapshot(&g_last_ui_snapshot);
        g_last_ui_snapshot_valid = 1;
    }
    return &g_last_ui_snapshot;
}

static int glr_ctrl_cmd_is_focus_vertex(const GLCmd *cmd) {
    /* glVertex2f counts too: args[2] is zero on a well-formed 2D vertex
     * (parser leaves the slot zero-initialised), so building focus.pos
     * from args[0..2] gives (x, y, 0) — the right point to focus on. */
    return cmd->valid && repl_cmd_emits_vertex(cmd->type);
}

static SceneFocusVertex glr_ctrl_build_focus_vertex(void) {
    SceneFocusVertex focus = { .valid = 0 };
    int edit_line = editor_state_edit_line();
    const GLCmd *cmds = repl_state_document_cmds();

    if (edit_line >= 0 && edit_line < repl_state_document_count() &&
        glr_ctrl_cmd_is_focus_vertex(&cmds[edit_line])) {
        focus.pos[0] = cmds[edit_line].args[0];
        focus.pos[1] = cmds[edit_line].args[1];
        focus.pos[2] = cmds[edit_line].args[2];
        focus.valid = 1;
    } else {
        for (int i = edit_line - 1; i >= 0; i--) {
            if (glr_ctrl_cmd_is_focus_vertex(&cmds[i])) {
                focus.pos[0] = cmds[i].args[0];
                focus.pos[1] = cmds[i].args[1];
                focus.pos[2] = cmds[i].args[2];
                focus.valid = 1;
                break;
            }
        }
    }

    return focus;
}

/* Parse up to 3 comma-separated arg slots out of `src`, recording which
 * positions are actually present. repl_eval_parse_exprs() skips leading
 * commas so ",1," and "1," look identical to it; this version tracks
 * slot indices. Empty slots leave filled[i]=0, out[i] unchanged. */
static int parse_vertex_arg_slots(const char *src,
                                  const ExprVar *predef_vars, int predef_var_count,
                                  float out[3], int filled[3]) {
    const char *s = src;
    int n_filled = 0;
    filled[0] = filled[1] = filled[2] = 0;

    for (int slot = 0; slot < 3; slot++) {
        while (*s == ' ' || *s == '\t') s++;
        const char *start = s;
        s = repl_scan_next_arg_delim(s);
        const char *end = s;
        const char *c = start;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        if (c < end) {
            ExprCtx ctx = { .p = start, .vars = predef_vars,
                            .num_vars = predef_var_count,
                            .err = NULL, .err_sz = 0 };
            float val = repl_eval_expr(&ctx);
            if (ctx.p > start) {
                out[slot] = val;
                filled[slot] = 1;
                n_filled++;
            }
        }
        if (*s == ',') s++;
        else break;
    }

    return n_filled;
}

/* Resolve the cursor-line vertex / normal args into floats so the scene
 * module can render its guides without depending on repl_eval. Sets the
 * pre-parsed fields on `snapshot` based on `input`. */
static void fill_guide_arg_slots(SceneGuideSnapshot *snapshot,
                                 const char *input, int input_len) {
    snapshot->vertex_n_filled = 0;
    snapshot->normal_n_filled = 0;
    snapshot->vertex_filled[0] = snapshot->vertex_filled[1] =
        snapshot->vertex_filled[2] = 0;

    if (!input) return;

    ReplPredefView predef = repl_eval_predef_view();

    const char *vertex_args = NULL;
    if (strncmp(input, "glVertex3f(", 11) == 0 && input_len > 11) {
        vertex_args = input + 11;
    } else if (strncmp(input, "glVertex2f(", 11) == 0 && input_len > 11) {
        vertex_args = input + 11;
    } else if (strncmp(input, "gluVertex(", 10) == 0 && input_len > 10) {
        vertex_args = input + 10;
    }
    if (vertex_args) {
        snapshot->vertex_n_filled = parse_vertex_arg_slots(
            vertex_args, predef.vars, predef.count,
            snapshot->vertex_args, snapshot->vertex_filled);
    }

    const char *normal_args = NULL;
    if (strncmp(input, "glNormal3f(", 11) == 0 && input_len > 11) {
        normal_args = input + 11;
    } else if (strncmp(input, "gluNormal(", 10) == 0 && input_len > 10) {
        normal_args = input + 10;
    }
    if (normal_args) {
        snapshot->normal_n_filled = repl_eval_parse_exprs(
            normal_args, snapshot->normal_args, 3, (ExprVar *)predef.vars, predef.count);
    }
}

/* Build a guide-render snapshot from live REPL + editor state. The
 * SceneRenderConfig argument is consulted only for fields the
 * controller already supplies (alpha_scale, user_lighting_enabled);
 * everything else comes from REPL/editor accessors so this can run
 * without a per-frame config pointer if needed. */
static SceneGuideSnapshot glr_ctrl_build_guide_snapshot(const SceneRenderConfig *config) {
    GlrPresentationState presentation = glr_state_presentation();
    ReplVariableView vars = repl_state_variables();
    EditorInputView input = editor_state_input();
    int edit_line = editor_state_edit_line();

    SceneGuideSnapshot snapshot = {
        .show_guides = (presentation.xform_guide_mode != SCENE_XFORM_GUIDE_OFF),
        .replaying = replay_active(),
        .replay_focus_anchor_flat_idx = replay_focus_anchor_flat_idx(),
        .xform_guide_mode = presentation.xform_guide_mode,
        .user_lighting_enabled = config ? config->user_lighting_enabled : 0,
        .anim_time = vars.anim_time,
        .input = input.input,
        .input_len = input.input_len,
        .cursor_pos = input.cursor_pos,
        .edit_line_idx = edit_line,
        .inserting = editor_insert_mode(),
        .edit_line_committed_text = editor_buffer_line(edit_line),
        .source_cmds = repl_state_document_cmds(),
        .source_cmd_count = repl_state_document_count(),
        .flat_program = repl_state_flat_program_view(),
        .alpha_scale = config ? config->alpha_scale : 1.0f,
    };
    fill_guide_arg_slots(&snapshot, input.input, input.input_len);
    return snapshot;
}

/* Per-frame replay-fade plan, owned by the controller. Used by the main
 * fill (to clamp the flat-cmd count to the pre-fade base limit) and by
 * the post_fill_fn hook (to render the fading-batch overlay). */
static ReplayFadePlan g_replay_fade_plan;

static void glr_ctrl_build_replay_fade_plan(FlatProgramView flat_program, int replaying) {
    ReplayFadeBatchView fade_batches;
    int batch_count;

    memset(&g_replay_fade_plan, 0, sizeof(g_replay_fade_plan));
    g_replay_fade_plan.active = 0;
    g_replay_fade_plan.base_limit = 0;

    if (!replaying)
        return;

    replay_copy_baseline_predef_snapshot(
        g_replay_fade_plan.baseline_predef_vals,
        g_replay_fade_plan.baseline_predef_names,
        &g_replay_fade_plan.baseline_predef_count);
    replay_copy_baseline_scratch_arrays(
        g_replay_fade_plan.baseline_scratch_arrays);

    if (!replay_has_active_fades())
        return;

    g_replay_fade_plan.base_limit = replay_fill_base_limit(flat_program);
    fade_batches = replay_fade_batches_view();
    batch_count = replay_compute_fade_skip_limits(flat_program, g_replay_fade_plan.skip_limits,
                                                       REPLAY_FADE_BATCH_MAX);
    if (batch_count > REPLAY_FADE_BATCH_MAX)
        batch_count = REPLAY_FADE_BATCH_MAX;

    g_replay_fade_plan.batch_count = batch_count;
    for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
        const ReplayFadeBatch *batch = &fade_batches.batches[batch_idx];
        g_replay_fade_plan.batches[batch_idx] = *batch;
        g_replay_fade_plan.batch_alpha[batch_idx] = replay_batch_alpha(batch);
    }
    g_replay_fade_plan.active = 1;
}

static int glr_ctrl_current_begin_block_source_extent(int edit_line,
                                                      int *out_start,
                                                      int *out_end);

static OverlaySnapshotPack g_overlay_pack;

static void glr_ctrl_build_overlay_pack(OverlaySnapshotPack *pack, const SceneRenderConfig *cfg) {
    GlrPresentationState presentation = glr_state_presentation();
    int replaying = replay_active();
    int replay_mode_vertex = replaying && (replay_mode() == REPLAY_MODE_VERTEX);

    pack->walk.program = repl_state_flat_program_view();
    pack->walk.cursor.edit_line_idx = editor_state_edit_line();
    pack->walk.cursor.cursor_block_begin = repl_state_flat_program_current_block_begin();
    pack->walk.cursor.cursor_block_end = repl_state_flat_program_current_block_end();
    pack->walk.cursor.cursor_source_block_valid =
        glr_ctrl_current_begin_block_source_extent(
            pack->walk.cursor.edit_line_idx,
            &pack->walk.cursor.cursor_source_block_begin,
            &pack->walk.cursor.cursor_source_block_end);
    pack->walk.cursor.cursor_func_scope_mask = 0;

    pack->walk.show_vertex_outlines = presentation.show_vertex_outlines;
    pack->walk.highlight_current_poly = presentation.highlight_current_poly && !replaying;
    pack->walk.replay_tess_preview = replay_mode_vertex;
    pack->walk.show_vertex_points = presentation.show_vertex_points;
    pack->walk.replay_vertex_points = replay_mode_vertex;

    pack->snapshot = glr_ctrl_build_guide_snapshot(cfg);
    pack->vertex_label_mode = (OverlayVertexLabelMode)presentation.show_vertex_labels;
    pack->ortho_mode = presentation.ortho_mode;
    pack->show_normal_vectors = presentation.show_normal_vectors;
    pack->multisample_enabled = cfg ? cfg->multisample_enabled : 0;
    pack->line_smooth_enabled = cfg ? cfg->line_smooth_enabled : 0;
}

static void glr_ctrl_push_highlights(void) {
    editor_state_highlights_clear();

    int doc_count = repl_state_document_count();
    int edit_line = editor_state_edit_line();
    int insert_mode = editor_insert_mode();

    if (!insert_mode && edit_line >= 0 && edit_line < doc_count) {
        const GLCmd *cmd = repl_state_document_cmd_at(edit_line);
        if (cmd && cmd->valid) {
            int norm_idx = repl_find_feeding_normal_cmd(edit_line);
            int color_idx = repl_find_feeding_color_cmd(edit_line);
            if (norm_idx >= 0)
                editor_state_highlights_append(norm_idx, -1, -1,
                                                    HIGHLIGHT_FEEDING_NORMAL);
            if (color_idx >= 0)
                editor_state_highlights_append(color_idx, -1, -1,
                                                    HIGHLIGHT_FEEDING_COLOR);

            if (cmd->type == CMD_POP_MATRIX) {
                int push_idx = repl_find_matching_push_matrix(edit_line);
                if (push_idx >= 0)
                    editor_state_highlights_append(push_idx, -1, -1,
                                                        HIGHLIGHT_MATCHING_PUSH_MATRIX);
            } else if (cmd->type == CMD_PUSH_MATRIX) {
                int pop_idx = repl_find_matching_pop_matrix(edit_line);
                if (pop_idx >= 0)
                    editor_state_highlights_append(pop_idx, -1, -1,
                                                        HIGHLIGHT_MATCHING_PUSH_MATRIX);
            }

            /* Affecting-transform highlight for the edit cursor. During
             * replay the replay-focus vertex owns this marker instead (the
             * cursor may be parked elsewhere), so skip the cursor set and let
             * the replay block below push it. Prefer the flat-accurate
             * resolver so a vertex inside a funcN body picks up the
             * calling-scope transforms too; fall back to the source walk only
             * when the flat program is empty/unbuilt (e.g. a flatten error) —
             * by this point in the frame flatten has already run and cleared
             * its dirty flag. */
            if (!replay_active()) {
                int xform_lines[MAX_AFFECTING_TRANSFORMS];
                int xform_count = repl_find_affecting_transforms_flat(
                    edit_line, xform_lines, MAX_AFFECTING_TRANSFORMS);
                if (xform_count == 0 && repl_state_flat_program_view().cmd_count == 0)
                    xform_count = repl_find_affecting_transforms(
                        edit_line, xform_lines, MAX_AFFECTING_TRANSFORMS);
                for (int i = 0; i < xform_count; i++)
                    editor_state_highlights_append(xform_lines[i], -1, -1,
                                                        HIGHLIGHT_AFFECTING_TRANSFORM);
            }
        }
    }

    /* Always-on: flag structurally unbalanced bracket commands
     * (unmatched glPushMatrix/glBegin, orphan glPopMatrix/glEnd). Not
     * cursor-gated — the relaxed REPL tolerates these but export
     * auto-balances them, so make them visible in the gutter. */
    {
        int unbalanced[MAX_HIGHLIGHTS];
        int nb = repl_source_scope_collect_unbalanced(unbalanced, MAX_HIGHLIGHTS);
        for (int i = 0; i < nb; i++)
            editor_state_highlights_append(unbalanced[i], -1, -1,
                                                HIGHLIGHT_UNBALANCED);
    }

    int src_line = replay_src_line();
    if (replay_active() && src_line >= 0)
        editor_state_highlights_append(src_line, -1, -1,
                                            HIGHLIGHT_REPLAY_PC);

    /* When the focused replay command was expanded from a funcN(...) call,
     * also light up the call site(s) so a reused or recursive function shows
     * which invocation is live (the PC line above is the body line inside the
     * function). call_src_cmd_idx is the immediate caller; root_call_src_cmd_idx
     * the outermost caller of a nested chain — push it only when distinct. */
    if (replay_active()) {
        int focus = replay_focus_flat_idx();
        FlatProgramView flat = repl_state_flat_program_view();
        if (focus >= 0 && focus < flat.cmd_count) {
            int call_site = flat.cmds[focus].call_src_cmd_idx;
            int root_site = flat.cmds[focus].root_call_src_cmd_idx;
            if (call_site >= 0)
                editor_state_highlights_append(call_site, -1, -1,
                                                    HIGHLIGHT_REPLAY_CALL_SITE);
            if (root_site >= 0 && root_site != call_site)
                editor_state_highlights_append(root_site, -1, -1,
                                                    HIGHLIGHT_REPLAY_ROOT_CALL_SITE);
        }
    }

    /* Affecting-transform highlight anchored on the replay-focused draw
     * (req 5). replay_focus_anchor_flat_idx() is a flat index (vertex mode
     * only; -1 otherwise) of the draw the step emitted — a glVertex/gluVertex
     * or a glutSolid* — so feed it straight to the req-4 exact-flat resolver,
     * which already accepts any repl_cmd_consumes_current_color target. This
     * shows the transforms shaping the draw currently being replayed,
     * independent of where the edit cursor sits, and composes with the replay
     * PC / call-site markers. */
    if (replay_active()) {
        int focus_anchor = replay_focus_anchor_flat_idx();
        if (focus_anchor >= 0) {
            int xform_lines[MAX_AFFECTING_TRANSFORMS];
            int xform_count = repl_find_affecting_transforms_for_flat_vertex(
                focus_anchor, xform_lines, MAX_AFFECTING_TRANSFORMS);
            for (int i = 0; i < xform_count; i++)
                editor_state_highlights_append(xform_lines[i], -1, -1,
                                                    HIGHLIGHT_AFFECTING_TRANSFORM);
        }
    }

    if (tutorial_active()) {
        int insertion_line = tutorial_expected_commit_line();
        if (insertion_line >= 0)
            editor_state_highlights_append(insertion_line, -1, -1,
                                                HIGHLIGHT_TUTORIAL_INSERTION);
    }
}

/* Push per-line text overrides for source lines whose displayed text
 * should differ from the buffer text. Today only replay's expand_args
 * annotations produce overrides (e.g. `x = 0.4` → `x = 0.4 // = 0.4`
 * with the evaluated form appended). The list stays empty outside
 * active replay, and is sparse during it (only has_vars cmds of the
 * applicable kinds get entries). */
static void glr_ctrl_push_line_overrides(void) {
    editor_state_line_overrides_clear();
    ReplayRuntimeState replay = replay_state_view();
    if (!replay.active || !replay.expand_args)
        return;

    SourceTextView view = source_document_view();
    int n = repl_state_document_count();
    for (int i = 0; i < n; i++) {
        char display[MAX_LINE_OVERRIDE_TEXT];
        if (!replay_code_panel_get_command_display_text(view, i, display,
                                                             sizeof(display)))
            continue;
        const char *base = source_text_line(view, i);
        if (base && strcmp(display, base) == 0)
            continue;
        editor_state_line_overrides_append(i, display);
    }
}

static void glr_ctrl_push_color_transformers(void) {
    editor_state_transformers_clear();
    int doc_count = repl_state_document_count();
    for (int i = 0; i < doc_count; i++) {
        if (!color_picker_can_edit_cmd(i))
            continue;
        const GLCmd *cmd = repl_state_document_cmd_at(i);
        if (!cmd)
            continue;
        int has_alpha = (cmd->type == CMD_COLOR4F ||
                         cmd->type == CMD_TESS_COLOR ||
                         cmd->type == CMD_CLEAR_COLOR);
        UiTransformer t = {
            .line_idx = i,
            .char_start = -1,
            .char_end = -1,
            .kind = TRANSFORMER_COLOR_PICKER,
            .state.color = {
                .r = cmd->args[0],
                .g = cmd->args[1],
                .b = cmd->args[2],
                .a = has_alpha ? cmd->args[3] : 1.0f,
                .has_alpha = has_alpha,
            },
        };
        if (!editor_state_transformers_append(&t))
            break;
    }
}

/* Browser autoplay policy: the Web Audio context stays suspended until
 * a user gesture. The very first key / mouse / special event after
 * startup fires glr_audio_on_user_gesture; native builds make this a
 * no-op. */

/* Layout provider installed on the editor at glr_ctrl_init_gl. The
 * editor reads through this so src/editor/ does not need to include
 * src/app/glr_state.h. */
int glr_ctrl_code_panel_layout_provider(void) {
    return glr_state_presentation().code_panel_layout;
}

/* Hoisted out of src/editor/input.c per audit #8: the action
 * writes glr_state_presentation_mut(), runs glr_ctrl_sync_ui_chrome,
 * and closes the menu / picker — all controller / UI concerns the
 * editor module should not be reaching into. The editor signals
 * the intent via the restore_hidden_code_panel effect flag, which
 * apply_input_effects below actualizes by calling this. */
int glr_ctrl_restore_hidden_code_panel(void) {
    if (glr_state_presentation().code_panel_layout != CODE_PANEL_LAYOUT_HIDDEN)
        return 0;
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_menu_bar_close();
    color_picker_stop();
    return 1;
}

/* Hoisted out of src/editor/input.c per audit #8: the body reaches
 * the camera, menu bar, color picker, and the controller's own
 * code-panel-drag reset — none of which are editor-text concerns.
 * The editor still owns the commit-side state reset via
 * editor_commit_reset_transients(); this wraps that with the
 * cross-subsystem cleanup the app-frame paths actually want. */
void glr_ctrl_reset_transients(void) {
    editor_commit_reset_transients();
    glr_camera_controls_reset();
    glr_camera_clear_scene_default();
    ui_menu_bar_close();
    color_picker_stop();
    glr_ctrl_router_reset_code_panel_drag();
}

/* Non-static: the input router (src/app/glr_ctrl_router.c) applies dispatch
 * effects after each routing helper. Declared in glr_ctrl_internal.h. */
void glr_ctrl_apply_input_effects(EditorInputDispatchEffects effects) {
    if (effects.set_cursor)
        glutSetCursor(effects.cursor);
    if (effects.request_redraw)
        glutPostRedisplay();
    if (effects.restore_hidden_code_panel)
        glr_ctrl_restore_hidden_code_panel();
    if (effects.close_help_overlay)
        glr_ctrl_close_help();
}

/* ========================================================================= */
/* Scene config builder (push model)                                          */
/* ========================================================================= */

/* Scene's geometry callback for the main-fill and depth-probe passes.
 * The signature is intentionally opaque to scene — the controller
 * pulls live program / count / text from REPL state here, and clamps
 * the count to the pre-fade base limit when replay-fade overlays are
 * active so the fade pass can layer on top of an unmodified prefix.
 *
 * For non-MAIN_FILL purposes (today: SCENE_EXEC_DEPTH_PROBE — the
 * GL_FEEDBACK pass that measures geometry depth for the ortho-mode
 * scale reference), snapshot REPL mutable state before the executor
 * runs and restore it after. The executor's CMD_VAR_ASSIGN /
 * CMD_SCRATCH_ASSIGN apply precomputed args[0] directly; CMD_ENABLE
 * (for GL_LIGHT*) and CMD_CLEAR_COLOR write into repl_state_render().
 * Without this bracket the probe would advance the user's
 * `t = t + 1` style state alongside the main fill (within-frame
 * doubling) and its glEnable / glClearColor would leak across frames
 * (the frame-level snapshot in glr_ctrl_display_frame restores predef
 * + scratch only, not the persistent render-state struct).
 *
 * SceneExecutePurpose is a forward-compatible enum; future probe-like
 * purposes (fade-overlay, picking pass, etc.) take the same
 * snapshot/restore path automatically. */
static void scene_execute_adapter(const SceneExecuteContext *ctx,
                                  void *user_data) {
    (void)user_data;

    SceneExecutePurpose purpose =
        ctx ? ctx->purpose : SCENE_EXEC_MAIN_FILL;
    int suppress_side_effects = (purpose != SCENE_EXEC_MAIN_FILL);

    int count = repl_state_flat_program_count();
    if (g_replay_fade_plan.active)
        count = g_replay_fade_plan.base_limit;

    float saved_predef[MAX_PREDEF_VARS];
    float saved_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    ReplRenderState saved_render = {0};
    if (suppress_side_effects) {
        repl_copy_predef_values(saved_predef, MAX_PREDEF_VARS);
        repl_eval_copy_scratch_arrays(saved_scratch);
        saved_render = repl_state_render();
    }

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    char exec_status[REPL_DIAG_TEXT_MAX] = "";
    repl_execute_program(&(ReplExecutionOptions){
        .flat_cmd_count = count,
        .program        = repl_state_flat_program_view(),
        .text           = source_document_view(),
        .status_out     = exec_status,
        .status_out_sz  = (int)sizeof(exec_status),
    });
    glPopAttrib();
    if (exec_status[0])
        repl_set_status_error(exec_status);

    if (suppress_side_effects) {
        repl_restore_predef_values(saved_predef, MAX_PREDEF_VARS);
        repl_eval_restore_scratch_arrays(saved_scratch);
        *repl_state_render_mut() = saved_render;
    }
}

/* Grid/axes in-out fade machines (plans/.../grid-axes-transitions.md).
 * The config path is untouched: toggling still just flips
 * presentation.grid_theme/axes_theme. Each frame the diff feeds these
 * machines and the renderer draws the effective {theme, opacity}. */
static SceneXnState g_grid_xn;
static SceneXnState g_axes_xn;

/* Per-renderer scene state (formerly file-static in src/scene/render.c).
 * The single-renderer assumption is now an instance, not a global —
 * glr_ctrl is one renderer, scene_demo is another, and tests own
 * theirs. Initialized in glr_ctrl_init_gl. */
static SceneRendererState g_scene_renderer;

/* Runtime GL_NV_fog_distance capability, probed once in glr_ctrl_init_gl
 * (the GL context is current there) and mirrored into each frame's
 * SceneRenderConfig. Lets the city backdrop and ocean/radar grid themes
 * opt into radial fog. 0 until detection runs — the safe default. */
static int g_nv_fog_distance_supported = 0;

/* The 2D/3D view-mode transition state machine lives in
 * src/app/glr_ctrl_view_transition.c (carved out of this file). The frame
 * loop ticks it via glr_ctrl_tick_view_transition; the scene-config builder
 * reads the blend via glr_ctrl_view_projection_mix; reset_all calls
 * glr_ctrl_view_reset — all declared in glr_ctrl_internal.h. */

static void glr_ctrl_build_scene_config(FlatProgramView flat_program, SceneRenderConfig *config) {
    GlrRenderState render = glr_state_render();
    ReplRenderState repl_render = repl_state_render();
    GlrPresentationState presentation = glr_state_presentation();
    GlrCameraState cam = glr_camera();
    const float *grid_major_steps = glr_state_grid_major_steps();
    const float *grid_extents = glr_state_grid_extents();
    float bg_lum;
    float as_val;

    /* Refresh cursor block highlight before reading cursor state */
    repl_flatten_refresh_current_block_highlight(editor_state_edit_line());

    /* --- Execute hook --- */
    config->execute_fn = scene_execute_adapter;
    config->execute_user_data = NULL;

    /* --- Post-fill hook (replay-fade overlay) ---
     * Wired up further below after the fade plan is built; left NULL when
     * there are no fades to overlay. */
    config->post_fill_fn = NULL;
    config->post_fill_user_data = NULL;

    /* --- Post-overlays hook ---
     * Always wired; the body short-circuits per-overlay based on REPL
     * presentation flags. user_data carries the per-frame config so the
     * hook can read guide_snapshot for the cursor edit guides. The
     * pack itself is built at the END of this function (see
     * "Build overlay snapshot pack" below) so it reads the fully-
     * populated config fields (multisample_enabled, line_smooth_enabled,
     * user_lighting_enabled, alpha_scale) — those are assigned later in
     * this function, so building the pack here would read stack garbage. */
    config->post_overlays_fn        = edit_overlays_post_overlays;
    config->post_overlays_user_data = &g_overlay_pack;

    /* --- Background clear color ---
     * Resolve from the user's last CMD_CLEAR_COLOR (or the editor
     * default if none). Scene takes the pre-resolved float[4] and
     * doesn't touch the flat program. */
    {
        float cr = CFG_DEFAULT_CLEAR_R;
        float cg = CFG_DEFAULT_CLEAR_G;
        float cb = CFG_DEFAULT_CLEAR_B;
        float ca = CFG_DEFAULT_CLEAR_A;
        FlatProgramView fp = repl_state_flat_program_view();
        for (int ci = 0; ci < fp.cmd_count; ci++) {
            if (fp.cmds[ci].valid && fp.cmds[ci].type == CMD_CLEAR_COLOR) {
                cr = fp.cmds[ci].args[0];
                cg = fp.cmds[ci].args[1];
                cb = fp.cmds[ci].args[2];
                ca = fp.cmds[ci].args[3];
            }
        }
        config->clear_color[0] = cr;
        config->clear_color[1] = cg;
        config->clear_color[2] = cb;
        config->clear_color[3] = ca;
    }

    /* --- Animation --- */
    config->anim_time = repl_state_variables().anim_time;

    /* --- Viewport and scene rectangle --- */
    ui_layout_scene_rect(&config->scene_x, &config->scene_y,
                           &config->scene_w, &config->scene_h);
    if (config->scene_w < 1) config->scene_w = 1;
    if (config->scene_h < 1) config->scene_h = 1;

    /* --- Camera state --- (modelview transform is applied separately by
     * the controller via glr_camera_load_modelview; these fields are
     * still needed for grid/axes orientation and the orbit-target
     * gizmo). */
    config->cam_dist = cam.dist;
    config->cam_rx = cam.rx;
    config->cam_ry = cam.ry;
    config->cam_tx = cam.tx;
    config->cam_ty = cam.ty;
    config->cam_tz = cam.tz;
    config->cam_motion_glow = cam.motion_glow;
    config->projection_mix = glr_ctrl_view_projection_mix();

    /* --- Rendering quality --- */
    config->multisample_enabled = render.multisample_enabled;
    config->line_smooth_enabled = render.line_smooth_enabled;
    config->use_accum = render.use_accum;
    config->accum_effect = render.accum_effect;
    config->accum_passes = render.accum_passes;
    /* Blur sub-frame hook is resolved per frame in glr_ctrl_display_frame
     * (it needs the captured current camera pose); default to no hook so
     * effect==BLUR degrades to the AA jitter path until wired. */
    config->setup_subframe_fn = NULL;
    config->setup_subframe_user_data = NULL;

    /* --- Lighting ---
     * Merge the app-owned dimensional light data (theme-seeded positions /
     * colors / eye-space, in glr_state) with the REPL-owned enable bitmask
     * (light_enabled_mask, written by the executor as the program runs).
     * The light-indicator overlay reads the `.enabled` flags from here. */
    config->user_lighting_enabled = repl_state_flat_program_user_lighting_enabled();
    for (int i = 0; i < MAX_LIGHTS; i++) {
        config->lights[i] = render.lights[i];
        config->lights[i].enabled = repl_light_enabled(repl_render.light_enabled_mask, i);
    }
    config->show_light_indicators = presentation.show_light_indicators;

    /* --- Environment --- */
    config->backdrop_mode = presentation.backdrop_mode;
    config->post_filter_mode = presentation.post_filter_mode;
    config->wireframe = presentation.wireframe;
    /* Single source of truth: the executor's capability flag + loaded
     * proc set in glr_ctrl_init_gl. Lets the star backdrop reset point
     * attenuation through the same callable entry point the executor
     * uses for CMD_POINT_PARAMETER_FV. */
    config->point_parameter_supported = repl_executor_point_parameter_supported();
    config->point_parameter_proc = config->point_parameter_supported
        ? repl_executor_point_parameter_proc()
        : NULL;
    /* Scoped radial fog: probed once in glr_ctrl_init_gl. Only the city
     * backdrop and ocean/radar grid passes act on it. */
    config->nv_fog_distance_supported = g_nv_fog_distance_supported;

    /* --- Grid and axes ---
     * Effective theme/opacity come from the transition machines (ticked
     * in glr_ctrl_tick), NOT directly from presentation: `current` is
     * the theme to draw while `next` may already point elsewhere mid
     * fade-out. */
    config->grid_theme = g_grid_xn.current;
    config->grid_opacity = g_grid_xn.opacity;
    config->grid_xn_phase = g_grid_xn.phase;
    config->grid_extent_idx = presentation.grid_extent_idx;
    config->grid_major_idx = presentation.grid_major_idx;
    config->axes_theme = g_axes_xn.current;
    config->axes_opacity = g_axes_xn.opacity;
    config->axes_xn_phase = g_axes_xn.phase;
    memcpy(config->grid_major_steps, grid_major_steps,
           sizeof(config->grid_major_steps));
    memcpy(config->grid_extents, grid_extents,
           sizeof(config->grid_extents));

    /* --- Replay-mode overlay state ---
     * post_fill_fn / post_overlays_fn read these directly from REPL
     * state; scene config no longer carries them. */
    int replaying = replay_active();

    /* --- Focus marker --- */
    config->focus = glr_ctrl_build_focus_vertex();

    /* Boost overlay alpha on dark backgrounds: bg_lum is the Rec. 709
     * relative luminance of the clear color (0.2126/0.7152/0.0722 R/G/B
     * weights); the reciprocal raises alpha as the background darkens,
     * with +0.02 as a black-background guard and the result clamped to
     * 1..3 below. */
    bg_lum = 0.2126f * config->clear_color[0]
        + 0.7152f * config->clear_color[1]
        + 0.0722f * config->clear_color[2];
    as_val = (CFG_DEFAULT_CLEAR_LUMA + 0.02f) /
             fmaxf(bg_lum + 0.02f, 1e-4f);
    config->alpha_scale = as_val < 1.0f ? 1.0f : (as_val > 3.0f ? 3.0f : as_val);

    /* --- Replay overlays ---
     * Build the controller-private fade plan from REPL replay state, and
     * if there's anything to overlay on the main fill (fading batches or
     * the polygon-mode tess-preview wireframe) install our post_fill_fn
     * so the scene calls back between the user-geometry fill and the
     * grid/axes/backdrop helpers. */
    glr_ctrl_build_replay_fade_plan(flat_program, replaying);
    g_replay_fade_plan.tess_preview_active = replaying &&
                                             replay_mode() == REPLAY_MODE_VERTEX;
    if (g_replay_fade_plan.active || g_replay_fade_plan.tess_preview_active) {
        config->post_fill_fn        = replay_render_post_fill;
        config->post_fill_user_data = &g_replay_fade_plan;
    }

    /* --- Build overlay snapshot pack ---
     * Done at the end so the pack reads the fully-populated config
     * fields (multisample_enabled, line_smooth_enabled,
     * user_lighting_enabled, alpha_scale). Moving this earlier in
     * the function would copy stack garbage through
     * glr_ctrl_build_guide_snapshot(). */
    glr_ctrl_build_overlay_pack(&g_overlay_pack, config);
}

/* ========================================================================= */
/* UI snapshot builder (push model)                                           */
/* ========================================================================= */

static void glr_ctrl_fill_ui_variable_panel_vars(UiRenderSnapshot *snap,
                                                    ReplPredefView predef) {
    int count = predef.count;

    if (count < 0 || !predef.vars)
        count = 0;
    if (count > MAX_PREDEF_VARS)
        count = MAX_PREDEF_VARS;

    /* Which predef vars carry a `// @tune` tag (badged in the panel as
     * exported keyboard knobs). Query all matches (not just the knob cap) so
     * every tagged row is badged. */
    const char *tuned_names[MAX_PREDEF_VARS];
    int tuned_count = repl_collect_tuned_vars(
        repl_state_document_cmds(), repl_state_document_count(),
        source_document_view(), tuned_names, MAX_PREDEF_VARS, NULL);

    snap->variable_panel_vars.vars = snap->variable_panel_var_storage;
    snap->variable_panel_vars.count = count;
    for (int i = 0; i < count; i++) {
        snprintf(snap->variable_panel_var_storage[i].name,
                 sizeof(snap->variable_panel_var_storage[i].name),
                 "%s", predef.vars[i].name);
        snap->variable_panel_var_storage[i].value = &predef.vars[i].value;
        int tuned = 0;
        for (int t = 0; t < tuned_count; t++) {
            if (strcmp(tuned_names[t], predef.vars[i].name) == 0) {
                tuned = 1;
                break;
            }
        }
        snap->variable_panel_var_storage[i].tuned = tuned;
    }
}

static int glr_ctrl_leading_ws_chars(const char *text) {
    int count = 0;

    while (text && text[count] && isspace((unsigned char)text[count]))
        count++;
    return count;
}

static int glr_ctrl_active_indent_chars(void) {
    int edit_line = editor_state_edit_line();
    int document_count = repl_state_document_count();

    if (editor_insert_mode())
        return repl_source_scope_cmd_indent_chars(edit_line);
    if (edit_line >= 0 && edit_line < document_count) {
        const char *line_text = editor_buffer_line(edit_line);
        if (!line_text || line_text[0] == '\0')
            return repl_source_scope_cmd_indent_chars(edit_line);
        return glr_ctrl_leading_ws_chars(line_text);
    }
    return repl_source_scope_cmd_indent_chars(document_count);
}

/* Constants in snapshot.h are hardcoded to keep the UI layer free of
 * repl-layer includes; assert equivalence here where repl/core.h is in
 * scope. */
STATIC_ASSERT(UI_SCENE_TAB_CAP >= MAX_USER_SCENES + 1,
              "scene-tab cap must fit every user slot plus the example tab");
STATIC_ASSERT(UI_SCENE_TAB_NAME_MAX == USER_SCENE_NAME_MAX,
              "scene-tab name buffer must match the user-scene name max");
/* Same rationale for the snapshot's resolved reshape-projection block:
 * snapshot.h hardcodes the dimensions (UI-layer purity), repl/export.h
 * owns the source-of-truth, equivalence is asserted here. */
STATIC_ASSERT(UI_RESHAPE_PROJ_LINES == REPL_EXPORT_PROJ_LINES,
              "snapshot reshape-projection line count must match repl/export");
STATIC_ASSERT(UI_RESHAPE_PROJ_LINE_MAX == REPL_EXPORT_PROJ_LINE_MAX,
              "snapshot reshape-projection line width must match repl/export");

/* Derive the scene tab strip each frame from existing state — no persistent
 * model. One tab per occupied user-scene slot (dense slot order, matching
 * the Scene menu / F12), plus one example tab iff an example is active.
 * Transient/"neither active" (Scene→New, tutorial) leaves active_idx == -1
 * with no synthetic tab (plan decision #6). */
static void glr_ctrl_build_scene_tabs(UiSceneTabList *out) {
    int active_slot = repl_active_user_scene();
    int example_idx = repl_state_scenes().active_example_idx;
    int n = 0;

    memset(out, 0, sizeof(*out));
    out->active_idx = -1;

    for (int slot = 0; slot < MAX_USER_SCENES && n < UI_SCENE_TAB_CAP; slot++) {
        if (!repl_user_scene_slot_used(slot))
            continue;
        snprintf(out->tabs[n].name, UI_SCENE_TAB_NAME_MAX, "%s",
                 repl_user_scene_name(slot));
        out->tabs[n].kind = UI_SCENE_TAB_USER;
        out->tabs[n].slot = slot;
        out->tabs[n].active = (slot == active_slot);
        if (out->tabs[n].active)
            out->active_idx = n;
        n++;
    }

    if (example_idx >= 0 && n < UI_SCENE_TAB_CAP) {
        snprintf(out->tabs[n].name, UI_SCENE_TAB_NAME_MAX, "%s",
                 repl_example_name(example_idx));
        out->tabs[n].kind = UI_SCENE_TAB_EXAMPLE;
        out->tabs[n].slot = -1;
        out->tabs[n].active = (active_slot < 0);
        if (out->tabs[n].active)
            out->active_idx = n;
        n++;
    }

    out->count = n;
}

static void glr_ctrl_populate_numeric_swatch(UiRenderSnapshot *snap) {
    EditorInputView in;
    int edit_line;
    ReplNumericArgAtCursor d;
    float anchor_y;
    int cp_x, cp_w;
    char parse_err[REPL_DIAG_TEXT_MAX] = "";
    ReplParsedLine pl;

    snap->numeric_swatch.visible = 0;

    if (editor_insert_mode()) return;
    edit_line = editor_state_edit_line();
    if (edit_line < 0 || edit_line >= repl_state_document_count()) return;
    if (repl_state_document_cmds()[edit_line].type == CMD_COMMENT) return;
    if (ui_repl_code_panel_input_row_has_color_swatch(snap)) return;
    in = editor_state_input();
    if (in.cursor_pos < 0 || !in.input || !in.input[0]) return;
    if (editor_state_autocomplete()->match_count > 0) return;
    if (editor_inline_rename_active()) return;
    if (tutorial_active() && tutorial_reject_noncommand_commit_with_hint()) return;

    d = repl_eval_numeric_arg_at_cursor(in.input, in.cursor_pos);
    if (!d.found) return;

    {
        ReplParseContext parse_ctx = {
            .source_line_idx = edit_line,
            .err_buf = parse_err,
            .err_sz = (int)sizeof parse_err,
        };
        if (!repl_parser_parse_command_ctx(in.input, &pl, &parse_ctx)) return;
    }
    if (pl.cmd.type == CMD_COMMENT) return;

    ui_layout_code_panel_rect(&cp_x, NULL, &cp_w, NULL);
    if (!ui_repl_code_panel_input_row_y(snap, &anchor_y)) return;

    snap->numeric_swatch.visible   = 1;
    snap->numeric_swatch.arg_start = d.arg_start;
    snap->numeric_swatch.arg_end   = d.arg_end;
    snap->numeric_swatch.value     = d.value;
    snap->numeric_swatch.step      = repl_eval_swatch_step(d.value);
    snap->numeric_swatch.anchor_x  = (float)(cp_x + cp_w -
                                     UI_NUMERIC_SWATCH_BTN_W - 4);
    snap->numeric_swatch.anchor_y  = anchor_y;
}

static int glr_ctrl_current_begin_block_source_extent(int edit_line,
                                                      int *out_start,
                                                      int *out_end) {
    const GLCmd *cmds = repl_state_document_cmds();
    int count = repl_state_document_count();
    int open_begin = -1;

    if (out_start) *out_start = -1;
    if (out_end) *out_end = -1;
    if (edit_line < 0 || edit_line >= count)
        return 0;

    for (int i = 0; i < count; i++) {
        if (!cmds[i].valid)
            continue;

        if (cmds[i].type == CMD_BEGIN) {
            open_begin = i;
        } else if (cmds[i].type == CMD_END && open_begin >= 0) {
            if (edit_line >= open_begin && edit_line <= i) {
                if (out_start) *out_start = open_begin;
                if (out_end) *out_end = i;
                return 1;
            }
            open_begin = -1;
        }
    }

    if (open_begin >= 0 && edit_line >= open_begin) {
        if (out_start) *out_start = open_begin;
        if (out_end) *out_end = count - 1;
        return 1;
    }
    return 0;
}

void glr_ctrl_build_ui_snapshot(UiRenderSnapshot *snap) {
    FlatProgramView flat_program;
    ReplPredefView predef;
    memset(snap, 0, sizeof(*snap));

    /* Refresh derived workspace header lines so the import/export view
     * reflects the current frame before rendering reads it. */
    repl_state_refresh_workspace_header_lines();

    /* Mirror chrome-relevant presentation fields into ui_state.code_panel
     * so ui_*.c renderers and hit-tests can read them via ui_state_*()
     * without crossing the repl_state_*() boundary. */
    glr_ctrl_sync_ui_chrome();

    snap->viewport       = ui_state_viewport();
    snap->code_panel     = ui_state_code_panel();
    snap->help           = ui_state_help();
    snap->help_session   = editor_help_session_view();
    snap->variable_panel = variable_panel_view();
    snap->variable_drag.active_var = variable_panel_drag_active_var();
    snap->variable_drag.log_mode   = variable_panel_drag_log_mode();
    snap->profile_panel  = ui_state_profile_panel();
    snap->memory_panel   = ui_state_memory_panel();
    snap->status         = ui_state_status();
    snap->status_history = ui_state_status_history();
    /* Count of structurally unbalanced bracket commands, surfaced as a
     * persistent segment in the editor statusbar (the gutter markers,
     * pushed in glr_ctrl_push_highlights, pinpoint the lines). */
    {
        int unbalanced[MAX_HIGHLIGHTS];
        snap->unbalanced_count =
            repl_source_scope_collect_unbalanced(unbalanced, MAX_HIGHLIGHTS);
    }
    snap->search         = *editor_state_search();
    snap->autocomplete   = *editor_state_autocomplete();
    snap->pointer        = ui_state_pointer();
    snap->render         = glr_state_render();
    snap->replay         = replay_state_view();
    snap->scenes         = repl_state_scenes();
    snap->scroll         = editor_state_scroll();
    snap->cursor_blink   = editor_state_cursor_blink();
    snap->color_picker   = color_picker_view();

    snap->editor_input   = editor_state_input();
    snap->import_export  = repl_state_import_export();
    flat_program         = repl_state_flat_program_view();
    predef = repl_eval_predef_view();
    glr_ctrl_fill_ui_variable_panel_vars(snap, predef);

    snap->document_cmds       = repl_state_document_cmds();
    snap->document_count      = repl_state_document_count();
    snap->edit_line           = editor_state_edit_line();

    snap->flat_program_count  = flat_program.cmd_count;
    snap->anim_time           = repl_state_variables().anim_time;

    snap->user_scene_active_idx   = repl_active_user_scene();
    glr_ctrl_build_scene_tabs(&snap->scene_tabs);

    snap->rename_active = editor_inline_rename_active();
    snprintf(snap->rename_text, sizeof(snap->rename_text), "%s",
             editor_inline_rename_buffer());

    snap->file_prompt_active = editor_inline_file_prompt_active();
    snprintf(snap->file_prompt_text, sizeof(snap->file_prompt_text), "%s",
             editor_inline_file_prompt_buffer());
    snprintf(snap->file_prompt_error, sizeof(snap->file_prompt_error), "%s",
             editor_inline_file_prompt_error());

    snap->help_content = glr_ctrl_help_overlay_content();
    snap->editor_transformers = editor_state_transformers();
    snap->editor_highlights = editor_state_highlights();
    snap->editor_virtual_lines = editor_state_virtual_lines();

    /* Selection range materialized once for the per-row code-panel branch. */
    snap->selection_active = editor_clipboard_sel_active();
    snap->selection_lo     = snap->selection_active ? editor_clipboard_sel_lo() : -1;
    snap->selection_hi     = snap->selection_active ? editor_clipboard_sel_hi() : -1;

    /* Indent + statusbar metadata so the render path does not call back
     * into repl_source_scope_* / ui_repl_code_panel_* per row. */
    snap->active_indent_chars   = glr_ctrl_active_indent_chars();
    snap->trailing_indent_chars = repl_source_scope_cmd_indent_chars(snap->document_count);
    snap->in_begin_block        = repl_source_scope_in_begin_block();
    snap->current_begin_mode    = repl_current_begin_mode();
    snap->current_begin_block_valid =
        glr_ctrl_current_begin_block_source_extent(
            snap->edit_line,
            &snap->current_begin_block_start,
            &snap->current_begin_block_end);

    /* Resolve the dynamic reshape() projection ONCE here so the panel's
     * row-count and render passes (which straddle scene_render_3d_scene)
     * read one frozen value. This is the previous frame's scene
     * projection — build_ui_snapshot runs before scene render — which is
     * exactly the consistency the panel needs; a 1-frame text lag during
     * a 2D/3D transition is invisible. */
    {
        const char *proj[REPL_EXPORT_PROJ_LINES];
        int pn = repl_export_reshape_projection_lines(proj);
        if (pn < 0) pn = 0;
        if (pn > UI_RESHAPE_PROJ_LINES) pn = UI_RESHAPE_PROJ_LINES;
        for (int i = 0; i < pn; i++)
            snprintf(snap->reshape_proj_lines[i],
                     UI_RESHAPE_PROJ_LINE_MAX, "%s", proj[i]);
        snap->reshape_proj_count = pn;
    }

    /* One-week pass: snapshot purity extensions */
    snap->editor_buffer = editor_buffer_view();
    snap->line_overrides = *editor_state_line_overrides();

    {
        TutorialRuntimeState tut = tutorial_state_view();
        snap->tutorial_fade.active = tut.active;
        snap->tutorial_fade.fade_line_idx = tut.fade_line_idx;
        snap->tutorial_fade.fade_start_t = tut.fade_start_t;
        snap->tutorial_fade.fade_duration = tut.fade_duration;

        snap->tutorial.active = tut.active;
        snap->tutorial.tutorial_idx = tut.tutorial_idx;
        snap->tutorial.visible_tag_count = repl_tutorial_visible_tag_count();
    }

    for (int i = 0; i < GLR_CONFIG_COUNT; i++) {
        snap->config_values[i] = glr_config_get((GlrConfigKey)i);
    }
    snap->example_visible_tag_count = repl_example_visible_tag_count();
    snap->user_scene_count = repl_user_scene_count();

    {
        int lc = repl_export_lights_display_line_count();
        if (lc < 0) lc = 0;
        if (lc > UI_LIGHTS_DISPLAY_MAX) lc = UI_LIGHTS_DISPLAY_MAX;
        snap->lights_display_count = lc;
        for (int i = 0; i < lc; i++) {
            repl_export_lights_display_line(i, snap->lights_display_lines[i], MAX_LINE_LEN);
        }
    }

    {
        int ic = repl_export_init_section_line_count();
        if (ic < 0) ic = 0;
        if (ic > UI_INIT_SECTION_MAX) ic = UI_INIT_SECTION_MAX;
        snap->init_section_count = ic;
        for (int i = 0; i < ic; i++) {
            repl_export_init_section_line(i, snap->init_section_lines[i], MAX_LINE_LEN);
        }
    }

    glr_ctrl_populate_numeric_swatch(snap);
}

/* Memory-panel stacking layout. The controller owns the anchor (the
 * renderer is snapshot-free), so these scene-relative margins live here —
 * distinct from the panel's own internal padding. They mirror the CPU
 * profile panel's spacing. */
enum {
    MEM_PANEL_SCENE_MARGIN_PX = 12, /* gap from scene edge when the variable panel is visible */
    MEM_PANEL_SIDE_GAP_PX     = 8,  /* gap when shifted left of the CPU profile panel */
    MEM_PANEL_EDGE_PAD_PX     = 4,  /* min inset from the scene edge after clamping */
};

/* Resolve the memory panel's stacked anchor and pack it into the narrow
 * view the renderer consumes. Mirrors the CPU profile panel's logic:
 * right-edge of the variable panel (which itself carries the replay lift),
 * shifted left when the CPU profile panel is visible, clamped into the
 * scene rect. The renderer is snapshot-free (links against {support,
 * ui/core} alone), so the layout-policy reads live here in the controller. */
static UiMemoryPanelView glr_ctrl_build_memory_panel_view(const UiRenderSnapshot *snap) {
    UiMemoryPanelView v;
    v.window_w = snap->viewport.window_w;
    v.window_h = snap->viewport.window_h;
    v.mode     = (UiMemoryPanelMode)snap->memory_panel.mode;

    int panel_w = ui_memory_panel_width();
    int panel_h = ui_memory_panel_height();

    int scene_x, scene_y, scene_w, scene_h;
    ui_layout_scene_rect(&scene_x, &scene_y, &scene_w, &scene_h);

    int panel_x, panel_y;
    if (snap->variable_panel.visible) {
        panel_x = scene_x + scene_w - panel_w - MEM_PANEL_SCENE_MARGIN_PX;
        panel_y = scene_y + scene_h - panel_h - MEM_PANEL_SCENE_MARGIN_PX;
    } else {
        UiVariablePanelView var_view = ui_app_variable_panel_view(snap);
        int var_x, var_y, var_w, var_h;
        ui_variable_panel_rect(&var_view, &var_x, &var_y, &var_w, &var_h);
        panel_x = var_x + var_w - panel_w;
        panel_y = var_y;
    }

    if (snap->profile_panel.mode != PROFILE_PANEL_OFF)
        panel_x -= (PROFILE_PANEL_W + MEM_PANEL_SIDE_GAP_PX);

    int min_x = scene_x + MEM_PANEL_EDGE_PAD_PX;
    int max_x = scene_x + scene_w - panel_w - MEM_PANEL_EDGE_PAD_PX;
    if (panel_x < min_x) panel_x = min_x;
    if (panel_x > max_x) panel_x = max_x;

    int min_y = scene_y + STATUSBAR_H + MEM_PANEL_EDGE_PAD_PX;
    int max_y = scene_y + scene_h     - panel_h - MEM_PANEL_EDGE_PAD_PX;
    if (max_y >= min_y) {
        if (panel_y < min_y) panel_y = min_y;
        if (panel_y > max_y) panel_y = max_y;
    } else {
        panel_y = min_y;
    }

    v.panel_x = panel_x;
    v.panel_y = panel_y;
    return v;
}

/* CPU profile panel stacking layout (controller-owned anchor, snapshot-free
 * renderer). Same scene-relative spacing the panel used internally before it
 * was narrowed. */
enum {
    PROF_PANEL_SCENE_MARGIN_PX = 12, /* gap from scene edge when the variable panel is visible */
    PROF_PANEL_EDGE_PAD_PX     = 4,  /* min inset from the scene edge after clamping */
};

/* Resolve the CPU profile panel's stacked anchor (right edge of the scene, or
 * the variable panel's right edge when it's visible) into the narrow view the
 * renderer consumes — mirrors glr_ctrl_build_memory_panel_view. */
static UiProfilePanelView glr_ctrl_build_profile_panel_view(const UiRenderSnapshot *snap) {
    UiProfilePanelView v;
    v.window_w = snap->viewport.window_w;
    v.window_h = snap->viewport.window_h;
    v.mode     = (UiProfilePanelMode)snap->profile_panel.mode;

    int panel_w = ui_profile_panel_width();
    int panel_h = ui_profile_panel_height(v.mode);

    int scene_x, scene_y, scene_w, scene_h;
    ui_layout_scene_rect(&scene_x, &scene_y, &scene_w, &scene_h);

    int panel_x, panel_y;
    if (snap->variable_panel.visible) {
        panel_x = scene_x + scene_w - panel_w - PROF_PANEL_SCENE_MARGIN_PX;
        panel_y = scene_y + scene_h - panel_h - PROF_PANEL_SCENE_MARGIN_PX;
    } else {
        UiVariablePanelView var_view = ui_app_variable_panel_view(snap);
        int var_x, var_y, var_w, var_h;
        ui_variable_panel_rect(&var_view, &var_x, &var_y, &var_w, &var_h);
        panel_x = var_x + var_w - panel_w;
        panel_y = var_y;
    }

    int min_x = scene_x + PROF_PANEL_EDGE_PAD_PX;
    int max_x = scene_x + scene_w - panel_w - PROF_PANEL_EDGE_PAD_PX;
    if (panel_x < min_x) panel_x = min_x;
    if (panel_x > max_x) panel_x = max_x;

    int min_y = scene_y + STATUSBAR_H + PROF_PANEL_EDGE_PAD_PX;
    int max_y = scene_y + scene_h     - panel_h - PROF_PANEL_EDGE_PAD_PX;
    if (max_y >= min_y) {
        if (panel_y < min_y) panel_y = min_y;
        if (panel_y > max_y) panel_y = max_y;
    } else {
        panel_y = min_y;
    }

    v.panel_x = panel_x;
    v.panel_y = panel_y;
    return v;
}

/* Per-sample hook for accum motion blur (installed only when the frame's
 * blur mode is CAMERA or TIME). Resets to the frame baseline so each sample
 * is independent, then applies the sample's camera pose or time sub-step. */
static void glr_ctrl_setup_subframe(void *ud, int pass_idx, int pass_count,
                                    SceneRenderConfig *pass_cfg) {
    GlrSubframeCtx *c = (GlrSubframeCtx *)ud;
    float f = (pass_count > 1) ? (float)pass_idx / (float)(pass_count - 1) : 0.0f;

    /* Per-sample isolation: start from the frame baseline so accumulating
     * programs (A[0]=A[0]+1, t=t+1) don't compound across samples. */
    repl_restore_predef_values(c->base_predef, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(c->base_scratch);
    *repl_state_render_mut() = c->base_render;

    if (c->mode == GLR_BLUR_CAMERA) {
        GlrCameraPose p = glr_camera_pose_lerp(&c->prev, &c->cur, f);
        glr_camera_load_modelview(&p);
        /* Expose the interpolated pose so grid/axes/orbit-target/lights and
         * the ortho projection (recomputed per sample by the scene) blur with
         * the camera, not just the user geometry's modelview. */
        pass_cfg->cam_rx = p.rx;
        pass_cfg->cam_ry = p.ry;
        pass_cfg->cam_dist = p.dist;
        pass_cfg->cam_tx = p.tx;
        pass_cfg->cam_ty = p.ty;
        pass_cfg->cam_tz = p.tz;
    } else if (c->mode == GLR_BLUR_TIME) {
        /* Trailing shutter [t_end - dt, t_end]: re-bake geometry at the
         * sub-step t (the modelview stays at the current camera pose). The
         * last sample (f==1) bakes at exactly t_end, so the flat program is
         * left at the true frame time. */
        repl_state_time_set_transient(c->t_end - c->dt * (1.0f - f));
        repl_flatten_commands(c->edit_line);
    }
}

/* Decide this frame's blur mode and, when active, install the per-sample
 * hook + capture the baseline the hook resets to. Leaves setup_subframe_fn
 * NULL when blur is off/inapplicable so the scene takes the AA jitter path
 * (the paused + still-camera fallback, and the replay degradation). */
static void glr_ctrl_resolve_blur_subframe(SceneRenderConfig *config) {
    config->setup_subframe_fn = NULL;
    config->setup_subframe_user_data = NULL;

    if (!SCENE_ACCUM_EFFECT_IS_BLUR(config->accum_effect) ||
        !config->use_accum || config->accum_passes <= 1 ||
        replay_active())
        return;

    int camera_moved = g_prev_frame_pose_valid &&
        glr_camera_pose_changed(&g_prev_frame_pose, &g_cur_frame_pose);

    GlrBlurMode mode = GLR_BLUR_NONE;
    if (camera_moved)
        mode = GLR_BLUR_CAMERA;
    else if (config->accum_effect == SCENE_ACCUM_EFFECT_BLUR &&
             repl_state_variables().time_playing)
        mode = GLR_BLUR_TIME;   /* Blur (full) also blurs animation time;
                                 * Blur Camera does not -> AA fallback. */

    if (mode == GLR_BLUR_NONE)
        return;  /* still camera (and not time-blurring) -> AA jitter fallback */

    g_subframe_ctx.mode = mode;
    g_subframe_ctx.prev = g_prev_frame_pose;
    g_subframe_ctx.cur  = g_cur_frame_pose;
    g_subframe_ctx.dt   = GLR_FRAME_DT_SECS;
    g_subframe_ctx.edit_line = editor_state_edit_line();
    {
        ReplVariableView vv = repl_state_variables();
        g_subframe_ctx.t_end =
            (vv.time_var_idx >= 0 && vv.time_var_idx < vv.var_count)
                ? vv.vars[vv.time_var_idx].value : 0.0f;
    }
    repl_copy_predef_values(g_subframe_ctx.base_predef, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(g_subframe_ctx.base_scratch);
    g_subframe_ctx.base_render = repl_state_render();

    config->setup_subframe_fn = glr_ctrl_setup_subframe;
    config->setup_subframe_user_data = &g_subframe_ctx;
}

void glr_ctrl_display_frame(void) {
    int saved_flat_count;
    float live_predef_vals[MAX_PREDEF_VARS];
    float live_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    FlatProgramView flat_program = repl_state_flat_program_view();
    int num_flat_cmds = flat_program.cmd_count;
    /* Capture replay state once before replay_prepare_frame so the
     * HUD shows the per-frame "before-prepare" view (the contract that
     * test_glr_ctrl pins). Per-field narrow accessors elsewhere in
     * the frame are reading post-prepare state, which is what they want. */
    ReplayRuntimeState frame_replay = replay_state_view();
    SceneRenderConfig scene_config;
    UiRenderSnapshot ui_snap;

    prof_frame_tick();
    memprof_frame_tick();
    prof_begin(PROF_FRAME_TOTAL);

    if (repl_state_normals_dirty()) {
        prof_begin(PROF_AUTONORMAL);
        /* Caller-owned cursor: read edit-line into a local int, pass
         * &local so the pipeline never reaches into editor_state_*. */
        int edit_line = editor_state_edit_line();
        repl_recompute_autonormals(glr_state_presentation().autonormal,
                                   &edit_line);
        editor_state_edit_line_set(edit_line);
        repl_state_normals_dirty_clear();
        prof_end(PROF_AUTONORMAL);
    }
    if (repl_state_flat_program_dirty()) {
        prof_begin(PROF_FLATTEN);
        repl_flatten_commands(editor_state_edit_line());
        repl_state_flat_program_clear_dirty();
        prof_end(PROF_FLATTEN);
        flat_program = repl_state_flat_program_view();
        num_flat_cmds = flat_program.cmd_count;
    }

    /* Snapshot production: every per-frame list/snapshot the UI consumes
     * is built here so timing of the producer side is visible in profile.
     * The aggregate PROF_SNAPSHOT section sums the four sub-phases plus
     * the scene-config and ui-snapshot builders below. */
    prof_begin(PROF_SNAPSHOT);

    prof_begin(PROF_SNAPSHOT_TRANSFORMERS);
    glr_ctrl_push_color_transformers();
    prof_end(PROF_SNAPSHOT_TRANSFORMERS);

    prof_begin(PROF_SNAPSHOT_HIGHLIGHTS);
    glr_ctrl_push_highlights();
    prof_end(PROF_SNAPSHOT_HIGHLIGHTS);

    /* Prepare replay annotations + push the virtual-line list and
     * the per-line text-override list. Layout reads both as snapshot
     * data; the editor is agnostic to which feature pushed them.
     * prepare() fills an output struct and the controller publishes
     * via glr_publish_replay_annotations so REPL code doesn't reach
     * directly into editor_state_virtual_lines. */
    prof_begin(PROF_SNAPSHOT_VIRTUAL_LINES);
    ReplReplayAnnotationOutput replay_annotations;
    replay_annotations_prepare(source_document_view(),
                                    &replay_annotations);
    glr_publish_replay_annotations(&replay_annotations);
    glr_ctrl_push_line_overrides();
    prof_end(PROF_SNAPSHOT_VIRTUAL_LINES);

    /* Per-frame prep that sits between virtual-line refresh and
     * scene-config build: replay state-machine prepare_frame plus
     * the import/export render-state and camera string refresh.
     * Wrapped in its own subsection so the SNAPSHOT_* subsections
     * sum to PROF_SNAPSHOT exactly. */
    prof_begin(PROF_SNAPSHOT_PREP);
    saved_flat_count = num_flat_cmds;
    repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(live_scratch_arrays);
    if (replay_active()) {
        repl_state_flat_program_set_count(replay_prepare_frame(flat_program, saved_flat_count));
        int post_prep_src_line = replay_src_line();
        if (post_prep_src_line >= 0 &&
            post_prep_src_line != g_last_replay_follow_src_line) {
            editor_scroll_follow_cursor_set(1);
        }
        g_last_replay_follow_src_line = post_prep_src_line;
    } else {
        g_last_replay_follow_src_line = -1;
    }

    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();
    prof_end(PROF_SNAPSHOT_PREP);

    prof_begin(PROF_SNAPSHOT_SCENE_CONFIG);
    glr_ctrl_build_scene_config(flat_program, &scene_config);
    prof_end(PROF_SNAPSHOT_SCENE_CONFIG);

    prof_begin(PROF_SNAPSHOT_UI);
    /* Build, let follow-scroll adjust editor scroll, then update scroll-dependent
     * fields in the snapshot selectively (instead of rebuilding the heavy snapshot):
     * the second pass is to reflect post-follow-scroll offset in the published snapshot. */
    glr_ctrl_build_ui_snapshot(&ui_snap);
    {
        int old_scroll = editor_state_scroll().scroll;
        glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(&ui_snap, NULL, NULL);
        if (editor_state_scroll().scroll != old_scroll) {
            ui_snap.scroll = editor_state_scroll();
            glr_ctrl_populate_numeric_swatch(&ui_snap);
        }
    }
    g_last_ui_snapshot = ui_snap;
    g_last_ui_snapshot_valid = 1;
    prof_end(PROF_SNAPSHOT_UI);

    prof_end(PROF_SNAPSHOT);

    /* 3D scene - scene_render_3d_scene() handles optional accumulation-buffer AA */
    /* Reset subsection accumulators so timings across all AA samples sum up
     * correctly before the first (or only) scene_render_3d_scene() call. */
    for (ProfSection section_idx = PROF_SCENE_3D_SETUP; section_idx <= PROF_SCENE_3D_LAST; section_idx++)
        prof_accum_reset(section_idx);
    prof_begin(PROF_SCENE_3D);
    {
        GlrCameraState cam = glr_camera();
        g_cur_frame_pose = glr_camera_pose_from_state(&cam);
        glr_camera_load_modelview(&g_cur_frame_pose);
    }
    /* Resolve motion-blur mode for this frame now that the current pose is
     * captured; installs the per-sample hook on scene_config when active. */
    glr_ctrl_resolve_blur_subframe(&scene_config);
    if (scene_render_3d_scene(&g_scene_renderer, &scene_config) != 0) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                    "glr_ctrl: scene_render_3d_scene rejected config (errno=%d)\n",
                    errno);
            warned = 1;
        }
    }
    prof_end(PROF_SCENE_3D);

    int frame_replaying = replay_active();
    if (frame_replaying) {
        prof_begin(PROF_REPLAY_HUD);
        /* HUD reads replay/scene/layout state from the per-frame snapshot;
         * the explicit UiReplayHudState struct it used to receive was just
         * a copy of fields already on snap.
         *
         * Override the replay slice with the pre-prepare capture taken at
         * frame top, mirroring the historic "HUD shows the per-frame
         * before-prepare view" contract pinned by test_glr_ctrl. Other
         * snap->replay readers only touch fields replay_prepare_frame
         * doesn't write (src_line_idx, active), so the override is HUD-
         * specific and ephemeral. */
        ReplayRuntimeState saved_snap_replay = ui_snap.replay;
        ui_snap.replay = frame_replay;
        replay_ui_hud_render(&ui_snap);
        ui_snap.replay = saved_snap_replay;
        prof_end(PROF_REPLAY_HUD);
    }

    /* Commit the accumulated subsection totals now that all AA samples are done. */
    for (ProfSection section_idx = PROF_SCENE_3D_SETUP; section_idx <= PROF_SCENE_3D_LAST; section_idx++)
        prof_accum_commit(section_idx);

    prof_begin(PROF_CODE_PANEL);
    UiCodePanelOutput cp_out = { 0, 0, 0 };
    ui_panels_render_code_panel(&ui_snap, &cp_out);
    prof_end(PROF_CODE_PANEL);

    prof_begin(PROF_UI_PANELS);
    /* Autocomplete popup anchors under the editor cursor. When the
     * active input row didn't render this frame (code panel hidden,
     * row scrolled offscreen) cp_out.cursor_valid is 0 and we skip
     * the popup — there's no visible cursor to anchor to. Same
     * semantic as the legacy mid-render publish, which simply didn't
     * fire on those frames. */
    if (cp_out.cursor_valid)
        ui_autocomplete_panel_render(&ui_snap, cp_out.cursor_px, cp_out.cursor_py);
    ui_menu_bar_render_example_dropdown(&ui_snap);
    {
        UiVariablePanelView var_view = ui_app_variable_panel_view(&ui_snap);
        ui_variable_panel_render(&var_view);
    }
    ui_panels_render_scene_status(&ui_snap);
    {
        UiOverlayState help_overlay = {
            .visible    = ui_snap.help.visible,
            .tab_idx    = ui_snap.help_session.tab_idx,
            .scroll     = ui_snap.help_session.scroll,
            .viewport_w = ui_snap.viewport.window_w,
            .viewport_h = ui_snap.viewport.window_h,
            .content    = ui_snap.help_content,
        };
        ui_tabbed_overlay_render(&help_overlay);
    }
    prof_end(PROF_UI_PANELS);

    prof_begin(PROF_PROFILE_PANEL);
    {
        UiProfilePanelView prof_view = glr_ctrl_build_profile_panel_view(&ui_snap);
        ui_profile_panel_render(&prof_view);
    }
    prof_end(PROF_PROFILE_PANEL);

    prof_begin(PROF_MEMORY_PANEL);
    {
        UiMemoryPanelView mem_view = glr_ctrl_build_memory_panel_view(&ui_snap);
        ui_memory_panel_render(&mem_view);
    }
    prof_end(PROF_MEMORY_PANEL);

    /* Compositor post-process: the whole-frame filter runs over the
     * entire composited image (3D scene + every 2D UI layer) now that
     * all drawing for the frame is done, before the buffer swap in
     * display_func(). The scene-viewport filter (PROF_SCENE_3D_POST_PROCESS)
     * is the separate scene-layer pass; this is the compositor stage. */
    prof_begin(PROF_COMPOSITOR);
    glr_compositor_postprocess_frame(
        glr_state_presentation().compositor_filter_mode,
        ui_snap.viewport.window_w, ui_snap.viewport.window_h);
    prof_end(PROF_COMPOSITOR);

    prof_begin(PROF_FRAME_RESTORE);
    repl_state_flat_program_set_count(saved_flat_count);
    repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(live_scratch_arrays);
    prof_end(PROF_FRAME_RESTORE);

    /* Remember this frame's camera pose so the next frame can detect camera
     * motion and interpolate prev<->cur for camera blur. */
    g_prev_frame_pose = g_cur_frame_pose;
    g_prev_frame_pose_valid = 1;

    prof_end(PROF_FRAME_TOTAL);
}

void glr_ctrl_reshape(int w, int h) {
    if (h < 1) h = 1;
    ui_state_viewport_set_size(w, h);
    g_last_ui_snapshot_valid = 0;
}

/* Idempotent app-service installer required for any REPL loading/export path (including CLI). */
/* Applies example tag-default overrides dynamically after a global state reset. */
static const GlrExampleTagDefault k_example_tag_defaults[] =
    GLR_EXAMPLE_TAG_DEFAULTS;

int glr_ctrl_apply_tag_defaults(unsigned int tag_mask,
                                 const GlrExampleTagDefault *table,
                                 int n) {
    /* Track GlrConfigKey values already set during this call so we can
     * surface policy collisions. The shipped table has one entry; the
     * cap covers an order-of-magnitude expansion plus any synthetic
     * test policies. Iteration order is the table's declaration order;
     * later entries that target the same key overwrite earlier ones
     * (matches glr_config_set's last-write-wins semantics) and bump
     * the returned collision count. */
    enum { SEEN_CAP = 32 };
    GlrConfigKey seen[SEEN_CAP];
    int seen_count = 0;
    int collisions = 0;

    if (!table || n <= 0) return 0;

    for (int i = 0; i < n; i++) {
        const GlrExampleTagDefault *d = &table[i];
        if (!(tag_mask & repl_example_tag_bit(d->tag_idx)))
            continue;

        int seen_idx = -1;
        for (int j = 0; j < seen_count; j++) {
            if (seen[j] == d->key) { seen_idx = j; break; }
        }
        if (seen_idx >= 0) {
            fprintf(stderr,
                    "glr_ctrl: tag-default collision on key=%d at "
                    "table index %d (later entry wins)\n",
                    (int)d->key, i);
            collisions++;
        } else if (seen_count < SEEN_CAP) {
            seen[seen_count++] = d->key;
        }

        glr_config_set(d->key, d->value);
    }
    return collisions;
}

static void glr_ctrl_reset_example_chrome(unsigned int tag_mask) {
    glr_state_presentation_reset_example_defaults();
    glr_camera_mut()->auto_rotate = CFG_DEFAULT_CAMERA_ROTATE;
    variable_panel_set_visible(CFG_DEFAULT_VARIABLE_PANEL);

    glr_ctrl_apply_tag_defaults(
        tag_mask, k_example_tag_defaults,
        (int)(sizeof(k_example_tag_defaults) /
              sizeof(k_example_tag_defaults[0])));

    /* glr_state_presentation_reset_example_defaults() writes the
     * light_theme field directly (it is a pure storage module and
     * cannot reach scene_lights_apply_theme), so the app-state lights[]
     * positions/colors/eye-space flags are left on the *previous*
     * theme. Re-seed them from whatever theme the reset + tag defaults
     * settled on — same call reset_all makes. An example's own leading
     * `@cfg light_theme = X` runs after this through glr_config_set,
     * which re-applies via the same path (idempotent). */
    scene_lights_apply_theme(glr_state_render_mut()->lights,
                             glr_state_presentation().light_theme);
}

/* Adapter for repl_executor_install_camera_distance_source. The
 * source is unconditionally installed; only the point-size fallback
 * (taken when the runtime GL lacks glPointParameterfv) consumes it,
 * so this is a small zero-cost shim when point parameters are
 * supported. */
static float glr_ctrl_camera_distance(void) {
    return glr_camera().dist;
}

/* Adapter for the export reshape-projection bridge. Translates the
 * scene's currently-applied projection (cached by scene_apply_projection)
 * into the C lines the exported reshape() / live code panel emit between
 * glLoadIdentity() and glMatrixMode(GL_MODELVIEW). Ortho uses the
 * aspect-independent half-height and recomputes the aspect at runtime
 * from w/h so the exported program stays resolution-independent. */
static void glr_ctrl_export_reshape_projection(ReplExportProjectionBlock *blk) {
    SceneProjectionDesc p;

    scene_get_active_projection(&g_scene_renderer, &p);
    blk->count = 0;
    if (p.ortho) {
        snprintf(blk->lines[blk->count++], REPL_EXPORT_PROJ_LINE_MAX,
                 "  double _t = %.6g, _r = _t * (double)w / (double)h;",
                 p.ortho_top);
        snprintf(blk->lines[blk->count++], REPL_EXPORT_PROJ_LINE_MAX,
                 "  glOrtho(-_r, _r, -_t, _t, %.6g, %.6g);",
                 p.ortho_near, p.ortho_far);
    } else {
        snprintf(blk->lines[blk->count++], REPL_EXPORT_PROJ_LINE_MAX,
                 "  gluPerspective(%.6g, (double)w/(double)h, %.6g, %.6g);",
                 p.fovy_deg, p.near_z, p.far_z);
    }
}

static const ReplExportProjectionBridge g_export_projection_bridge_impl = {
    glr_ctrl_export_reshape_projection
};

/* Adapter for the export light bridge. Copies the app-owned theme-seeded
 * light data (GlrRenderState.lights) into the exporter's neutral float
 * struct so src/repl/export.c can emit the glLightfv init/display blocks
 * without including scene/app headers. Enable state is intentionally not
 * carried — the export bootstrap disables every slot and the program's own
 * glEnable(GL_LIGHTn) re-enables in display(). */
static void glr_ctrl_export_fill_light(int slot, ReplExportLightInfo *out) {
    if (slot < 0 || slot >= MAX_LIGHTS) return;  /* out is pre-zeroed by caller */
    GlrRenderState render = glr_state_render();
    const SceneLight *l = &render.lights[slot];
    memcpy(out->pos, l->pos, sizeof(out->pos));
    memcpy(out->diffuse, l->diffuse, sizeof(out->diffuse));
    memcpy(out->ambient, l->ambient, sizeof(out->ambient));
    memcpy(out->specular, l->specular, sizeof(out->specular));
    out->pos_is_eye_space = l->pos_is_eye_space;
}

static const ReplExportLightBridge g_export_light_bridge_impl = {
    glr_ctrl_export_fill_light
};

/* Editor-input cleanup that the REPL loaders used to do inline now
 * routes through callback sinks. The two helpers below are the
 * full-app implementations the controller installs at startup; the
 * demo leaves both unset. */
static void glr_ctrl_host_input_reset(void) {
    editor_insert_mode_set(0);
    editor_input_clear();
    EditorInputState *inp = editor_state_input_mut();
    inp->pending_newline[0] = '\0';
    inp->pending_newline_len = 0;
}

static void glr_ctrl_host_insert_mode_off(void) {
    editor_insert_mode_set(0);
}

/* Route the residual editor scroll/follow writes through host-effect
 * sinks. */
static void glr_ctrl_scroll_to_line(int target) {
    editor_scroll_set(target);
    editor_scroll_follow_cursor_set(0);
}

const UiOverlayContent *glr_ctrl_help_overlay_content(void) {
    enum { GLR_HELP_OVERLAY_MAX_TABS = 4 };
    static UiOverlayTab tabs[GLR_HELP_OVERLAY_MAX_TABS];
    static UiOverlayContent content;
    static int cached = 0;

    if (cached)
        return &content;

    const ReplHelpContent *help = repl_help_text_build();
    if (!help)
        return NULL;

    int count = help->tab_count;
    if (count < 0)
        count = 0;
    if (count > GLR_HELP_OVERLAY_MAX_TABS)
        count = GLR_HELP_OVERLAY_MAX_TABS;

    for (int i = 0; i < count; i++) {
        tabs[i].label = help->tabs[i].label;
        tabs[i].lines = help->tabs[i].lines;
    }

    content.title = help->title;
    content.tabs = tabs;
    content.tab_count = count;
    cached = 1;
    return &content;
}

void glr_publish_replay_annotations(const ReplReplayAnnotationOutput *out) {
    editor_state_virtual_lines_clear();
    if (!out) return;
    for (int i = 0; i < out->count; i++) {
        const ReplReplayAnnotation *row = &out->items[i];
        UiVirtualLineStyle style;
        switch (row->kind) {
        case REPL_REPLAY_ANNOTATION_KIND_SUBST:
            style = VIRTUAL_STYLE_REPLAY_SUBST;
            break;
        case REPL_REPLAY_ANNOTATION_KIND_EVAL:
            style = VIRTUAL_STYLE_REPLAY_EVAL;
            break;
        default:
            continue;
        }
        editor_state_virtual_lines_append(row->after_line_idx, style,
                                          row->text,
                                          row->aux[0] ? row->aux : NULL);
    }
}

static void glr_ctrl_host_editor_cursor_park(int line, int insert_mode) {
    editor_state_edit_line_set(line);
    editor_insert_mode_set(insert_mode);
}

static void glr_ctrl_host_completion_clear(void) {
    editor_completion_clear();
}

static void glr_ctrl_host_completion_update(void) {
    editor_completion_update();
}

static const char *glr_ctrl_host_editor_input_get(void) {
    return editor_state_input().input;
}

static void glr_ctrl_host_set_time_playing(int playing) {
    repl_state_variables_mut()->time_playing = playing;
}

/* The host-effect bridge routing core pipeline events to the UI and editor state. */
static const ReplHostEffects g_glr_host_effects = {
    .status                     = ui_state_status_set,
    .status_error               = ui_state_status_set_error,
    .example_presentation_reset = glr_ctrl_reset_example_chrome,
    .input_reset                = glr_ctrl_host_input_reset,
    .insert_mode_off            = glr_ctrl_host_insert_mode_off,
    .scroll_to_line             = glr_ctrl_scroll_to_line,
    .tutorial_teardown          = tutorial_teardown,
    .edit_line_get              = editor_state_edit_line,
    .edit_line_set              = editor_state_edit_line_set,
    .host_cursor_park           = glr_ctrl_host_editor_cursor_park,
    .completion_clear           = glr_ctrl_host_completion_clear,
    .completion_update          = glr_ctrl_host_completion_update,
    .host_input_get             = glr_ctrl_host_editor_input_get,
    .set_time_playing           = glr_ctrl_host_set_time_playing,
};

/* Seed both overlay fade machines to the current presentation theme at steady full opacity. */
static void glr_ctrl_seed_overlay_xn(void) {
    GlrPresentationState p = glr_state_presentation();
    scene_xn_init(&g_grid_xn, p.grid_theme);
    scene_xn_init(&g_axes_xn, p.axes_theme);
}

/* Fixed frame timestep (~60 Hz). The animation timer reschedules every
 * GLR_FRAME_DT_MS ms; every per-frame advance (time var, replay fade,
 * camera momentum, grid/axes fade, view transition) uses the matching
 * GLR_FRAME_DT_SECS so motion speed stays decoupled from redraw rate.
 * GLR_CURSOR_BLINK_TICKS is the cursor blink half-period counted in
 * those ticks (~0.5 s). */
#define GLR_FRAME_DT_MS       16
#define GLR_CURSOR_BLINK_TICKS 30

/* Per-frame diff + advance, called from glr_ctrl_tick (the animation
 * timer) with the same fixed GLR_FRAME_DT_SECS dt as repl_advance_time /
 * replay_tick_fade_batches / glr_camera_tick. NOT the display path:
 * display fires on reshape/expose without the timer, which would
 * couple fade speed to redraw rate. Rule 6 off-source short-circuit:
 * when the overlay is currently the off index, scene_xn_show skips the
 * pointless OUT of an already-invisible overlay (controller owns the
 * off-index policy; the machine stays theme-index-agnostic). */
static void glr_ctrl_tick_overlay_xn(void) {
    GlrPresentationState p = glr_state_presentation();

    if (p.grid_theme != g_grid_xn.current &&
        g_grid_xn.current == GRID_THEME_OFF)
        scene_xn_show(&g_grid_xn, p.grid_theme);
    else
        scene_xn_set(&g_grid_xn, p.grid_theme);
    scene_xn_tick(&g_grid_xn, GLR_FRAME_DT_SECS,
                  GRID_FADE_IN_SECS, GRID_FADE_OUT_SECS);

    if (p.axes_theme != g_axes_xn.current &&
        g_axes_xn.current == AXES_THEME_OFF)
        scene_xn_show(&g_axes_xn, p.axes_theme);
    else
        scene_xn_set(&g_axes_xn, p.axes_theme);
    scene_xn_tick(&g_axes_xn, GLR_FRAME_DT_SECS,
                  AXES_FADE_IN_SECS, AXES_FADE_OUT_SECS);
}

/* Look up the label of the config row currently bound to F<fn>.
 * Used by the help overlay's F-Key Toggles section through the
 * controller-installed ReplHelpFkeyProvider — closes audit #1's
 * layering inversion where src/repl/help_text.c reached into
 * src/app/glr_config.h directly. */
static const char *glr_ctrl_help_fkey_label(int fn) {
    int cfg_count = 0;
    const GlrConfigItem *items = glr_config_items(&cfg_count);
    for (int i = 0; i < cfg_count; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE)
            continue;
        if (item->is_special && item->key_code == fn)
            return glr_config_item_display_label(item);
    }
    return NULL;
}

static const ReplHelpFkeyProvider g_glr_help_fkey_provider = {
    .fkey_label = glr_ctrl_help_fkey_label,
};

/* REPL-backed value source for the variable-panel drag handlers. The peer
 * reads the declared variable's name + value through this bridge instead of
 * touching the eval table directly, which keeps src/subsystems/variable_panel
 * linkable from the standalone variable_panel_demo (it installs its own). */
static int glr_ctrl_var_read_row(int row, char *name_out, int name_cap, float *value_out) {
    ReplPredefView predef = repl_eval_predef_view();
    if (row < 0 || row >= predef.count) return 0;
    snprintf(name_out, (size_t)name_cap, "%s", predef.vars[row].name);
    *value_out = predef.vars[row].value;
    return 1;
}
static const VariablePanelValueSource g_glr_var_value_source = {
    glr_ctrl_var_read_row,
};

static void glr_ctrl_install_app_services(void) {
    /* Install the host-effect bridge (status sink, example presentation, editor effects, tutorial). */
    repl_install_host_effects(&g_glr_host_effects);
    /* Variable-panel drag value source: name + value reads from the REPL eval table. */
    variable_panel_install_value_source(&g_glr_var_value_source);
    /* Color-picker host: document read/write + screen geometry. */
    glr_color_picker_install_host();
    /* Install the export-config bridge for @cfg headers and per-scene config snapshotting. */
    glr_actions_install_export_cfg_bridge();
    /* Install the export-camera bridge for // camera blocks serialization. */
    glr_camera_export_install_bridge();
    /* Install the executor's camera-distance source to support dynamic point-attenuation scaling fallbacks. */
    repl_executor_install_camera_distance_source(glr_ctrl_camera_distance);
    /* Reshape-projection bridge: queries the active 2D/3D projection for export and layout calculations. */
    repl_export_install_projection_bridge(&g_export_projection_bridge_impl);
    /* Light bridge: feeds the app-owned theme-seeded light data to the exporter's glLightfv blocks. */
    repl_export_install_light_bridge(&g_export_light_bridge_impl);
    /* Help-overlay F-key label provider so src/repl/help_text.c can
     * walk F2..F10 labels without including app/glr_config.h (audit #1). */
    repl_help_text_install_fkey_provider(&g_glr_help_fkey_provider);
    /* Seed the grid/axes overlay transition machines. */
    glr_ctrl_seed_overlay_xn();
}

/* Full-world reset entry point. Clears REPL, editor, UI, and all peer subsystems. */
void glr_ctrl_reset_all(void) {
    editor_undo_note_wholesale_replacement();
    repl_state_reset_program();
    /* Reset presentation, rendering, and camera defaults. */
    glr_state_presentation_reset_defaults();
    glr_state_render_reset_defaults();
    /* Seed the app-state light slots with the active theme's
     * positions / colors. The scene module owns the theme presets;
     * the controller wires them into GlrRenderState so the merge in
     * glr_ctrl_build_scene_config (dimensional data + REPL enable mask)
     * and the export light bridge see a coherent set of lights.
     * glr_state defaults only seed .id; without this call positions /
     * colors stay zero. */
    scene_lights_apply_theme(glr_state_render_mut()->lights,
                             glr_state_presentation().light_theme);
    glr_camera_reset_default();
    glr_ctrl_view_reset();
    g_last_ui_snapshot_valid = 0;
    g_last_replay_follow_src_line = -1;
    editor_state_reset();
    ui_state_reset();
    variable_panel_state_reset();
    replay_state_reset();
    color_picker_state_reset();
    /* tutorial_teardown (rather than tutorial_state_reset) so an active
     * tutorial's cfg baseline gets restored before any presentation cfg
     * that follows this reset locks the tutorial-mutated state in. */
    tutorial_teardown();
    editor_help_session_reset();
    glr_ctrl_reset_transients();
    /* Inline modals are transient editor state too: a post-reset
     * world should not still be hosting a half-typed rename or
     * file-load prompt. */
    editor_inline_rename_cancel();
    editor_inline_file_prompt_cancel();
    /* Register the default editor completion provider. Editor input
     * dispatch calls editor_completion_* without knowing about
     * glr_completion; the registration here installs the
     * REPL-aware backing. */
    glr_completion_register_provider();
    glr_ctrl_install_app_services();
    /* Refresh derived export/camera text caches AFTER peer resets
     * so the cached strings reflect post-reset state, not whatever
     * was on the peers before this call. These read app-side cfg /
     * camera state, so they cannot live on the REPL-side reset
     * (would pull glr_config / glr_camera into the demo link set
     * AND would pre-fire before peers were reset).
     * The frame loop refreshes them every frame in build_ui_snapshot
     * + display, so tests that go through glr_ctrl_reset_all see
     * coherent caches without waiting for a frame. */
    repl_state_refresh_workspace_header_lines();
    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();
    glr_ctrl_sync_ui_chrome();
}

void glr_ctrl_sync_ui_chrome(void) {
    GlrPresentationState p = glr_state_presentation();
    UiCodePanelRuntimeState *cp = ui_state_code_panel_mut();
    cp->layout_mode         = p.code_panel_layout;
    cp->show_vertex_indices = p.show_vertex_indices;
    cp->wrap_at_comma       = p.wrap_at_comma;
    cp->syntax_highlight    = p.syntax_highlight;
    cp->code_focus          = p.code_focus;
    cp->paren_match         = p.paren_match;
    cp->paren_scope         = p.paren_scope;
}

void glr_ctrl_apply_code_panel_follow_scroll(
    const UiReplCodePanelLayout *layout) {
    int max_scroll;
    EditorScrollState *scroll;

    if (!layout)
        return;

    max_scroll = layout->total_lines - layout->visible_lines;
    if (max_scroll < 0)
        max_scroll = 0;

    scroll = editor_state_scroll_mut();
    if (scroll->scroll > max_scroll)
        scroll->scroll = max_scroll;
    if (scroll->scroll < 0)
        scroll->scroll = 0;

    if (scroll->scroll_follow_cursor) {
        if (layout->follow_doc_line < scroll->scroll)
            scroll->scroll = layout->follow_doc_line;
        if (layout->follow_doc_line >= scroll->scroll + layout->visible_lines)
            scroll->scroll = layout->follow_doc_line - layout->visible_lines + 1;
        if (scroll->scroll > max_scroll)
            scroll->scroll = max_scroll;
        if (scroll->scroll < 0)
            scroll->scroll = 0;
        scroll->scroll_follow_cursor = 0;
    }
}

static int glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(
    const UiRenderSnapshot *snap,
    int *out_follow_doc_line,
    int *out_visible_lines) {
    UiReplCodePanelLayout layout;
    int cp_x;
    int cp_y;
    int cp_w;
    int cp_h;
    int text_x;
    int scroll;

    if (!snap)
        return 0;

    text_x = ui_repl_code_panel_compute_text_x(snap);

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    (void)cp_x;
    (void)cp_y;

    ui_repl_code_panel_build_layout(snap, &layout, cp_w, text_x, cp_h);
    glr_ctrl_apply_code_panel_follow_scroll(&layout);

    if (out_follow_doc_line)
        *out_follow_doc_line = layout.follow_doc_line;
    if (out_visible_lines)
        *out_visible_lines = layout.visible_lines;

    scroll = editor_scroll();
    return layout.follow_doc_line >= scroll &&
           layout.follow_doc_line < scroll + layout.visible_lines;
}

int glr_ctrl_code_panel_apply_scroll_follow_for_test(
    const UiRenderSnapshot *snap,
    int *out_follow_doc_line,
    int *out_visible_lines) {
    return glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(
        snap, out_follow_doc_line, out_visible_lines);
}

void glr_ctrl_init_gl(void) {
    tutorial_state_init_explicit();
    glr_ctrl_reset_all();
    repl_ensure_init_bootstrap_ready();
    scene_render_init_gl();
    scene_renderer_state_init(&g_scene_renderer);
    repl_executor_init_resources();

    /* Runtime point-parameter capability (replaces the old
     * compile-time NO_POINT_PARAMETER macro). glPointParameterfv is
     * core GL 1.4 but on older Linux stacks the context can advertise
     * support while the unsuffixed C symbol is not safely callable. The
     * GL context is already current here (glr_ctrl_init_gl runs
     * post-glutInit/window), so detect BOTH pieces now:
     *   (1) capability from GL_VERSION >= 1.4 or ARB/EXT extension bits
     *   (2) a callable core/ARB/EXT proc loaded after the context exists
     * This MUST run before repl_apply_init_bootstrap() below: on
     * unsupported hardware the point-attenuation bootstrap entry has
     * to be skipped entirely rather than invoking a missing entry
     * point. GLR_NO_POINT_PARAMETER (any non-empty value) still forces
     * the unsupported path for testing on capable HW. */
    int gl_major = 0, gl_minor = 0;
    const char *gl_ver = (const char *)glGetString(GL_VERSION);
    if (gl_ver) sscanf(gl_ver, "%d.%d", &gl_major, &gl_minor);
    int has_point_param_core =
        (gl_major > 1) || (gl_major == 1 && gl_minor >= 4);
    int has_point_param_arb =
        glutExtensionSupported("GL_ARB_point_parameters") ? 1 : 0;
    int has_point_param_ext =
        glutExtensionSupported("GL_EXT_point_parameters") ? 1 : 0;
    int point_param_advertised =
        has_point_param_core || has_point_param_arb || has_point_param_ext;
    ReplExecutorPointParameterProc point_param_proc =
        point_param_advertised
            ? glr_ctrl_load_point_parameter_proc(has_point_param_core,
                                                 has_point_param_arb,
                                                 has_point_param_ext)
            : NULL;
    repl_executor_install_point_parameter_proc(point_param_proc);
    const char *no_pp = getenv("GLR_NO_POINT_PARAMETER");
    int forced_off = (no_pp && no_pp[0]) ? 1 : 0;
    int point_param_ok = (point_param_proc != NULL) && !forced_off;
    /* Tell the user on the terminal when point attenuation is off, and
     * which of the two reasons applies — a deliberate env override vs.
     * a GL context that genuinely lacks the entry point. */
    if (!point_param_ok) {
        if (forced_off) {
            const char *forced_detail =
                point_param_proc
                    ? " (this GL context does support it)"
                    : point_param_advertised
                        ? " (this GL context advertises it, but no callable entry point was found)"
                        : " (this GL context does not support it either)";
            fprintf(stderr,
                "[gl-repl] glPointParameterfv disabled via "
                "GLR_NO_POINT_PARAMETER=%s; using the glPointSize "
                "distance approximation%s.\n",
                no_pp,
                forced_detail);
        } else if (!point_param_advertised) {
            fprintf(stderr,
                "[gl-repl] glPointParameterfv unsupported by this GL "
                "context (GL_VERSION \"%s\", no GL_ARB/EXT_point_parameters); "
                "using the glPointSize distance approximation. Set "
                "GLR_NO_POINT_PARAMETER=1 to force this path on capable "
                "hardware.\n",
                gl_ver ? gl_ver : "unknown");
        } else {
            fprintf(stderr,
                "[gl-repl] GL point-parameter support is advertised by "
                "this GL context (GL_VERSION \"%s\"), but no "
                "glPointParameterfv/glPointParameterfvARB/"
                "glPointParameterfvEXT entry point could be loaded; "
                "using the glPointSize distance approximation.\n",
                gl_ver ? gl_ver : "unknown");
        }
    }
    repl_executor_set_point_parameter_supported(point_param_ok);

    /* Scoped radial fog (GL_NV_fog_distance). The city backdrop and the
     * ocean/radar grid themes draw distance fog over geometry that wraps
     * around the camera, where the default eye-plane (|z|) metric makes
     * the fringes pop in and out as the camera orbits. When the NV
     * extension is present, those passes switch to true radial eye
     * distance; every other (eye-plane-tuned) theme is left alone.
     * Mirrored into SceneRenderConfig per frame and applied per-pass —
     * NOT set globally, which would fog out the tuned grid themes. */
    g_nv_fog_distance_supported =
        glutExtensionSupported("GL_NV_fog_distance") ? 1 : 0;

    repl_apply_init_bootstrap();

    /* Query actual MSAA sample count from OpenGL for the menu label. */
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    glr_actions_set_msaa_label((int)samples);

    /* glutInit has run by the time glr_ctrl_init_gl is called.
     * Unlock glutGetModifiers() reads in editor_input so Cmd / Ctrl /
     * Shift modifier checks land. Tests skip this hook so modifier
     * reads default to "no modifiers held" instead of aborting
     * freeglut for being called pre-init. */
    editor_input_enable_glut_modifier_reads();
    /* Editor reads the code-panel layout through a provider hook so
     * src/editor/ stops depending on src/app/glr_state.h. The
     * controller owns the layout field; the editor only needs the
     * read view for hit-test geometry and the hidden-panel auto-
     * restore. */
    editor_input_set_code_panel_layout_provider(glr_ctrl_code_panel_layout_provider);
    /* App-level config: tell the editor what comment prefix to use
     * for Ctrl+/ toggle. Editor itself has no default — tests that
     * exercise the toggle key path set the prefix explicitly. */
    editor_set_line_comment_prefix("// ");

    /* Capture the memory baseline after REPL bootstrap so it reflects
     * "REPL ready and idle" rather than the bare process at main()
     * entry — the more useful baseline for leak attribution. */
    memprof_init();
}

/* Program name for user-facing messages. Defaults to "gl-repl" so it is
 * sensible even if main() never forwards argv[0] (tests, demos). */
static char g_program_name[64] = "gl-repl";

void glr_ctrl_set_program_name(const char *argv0) {
    const char *base;

    if (!argv0 || !*argv0)
        return;
    base = argv0;
    for (const char *p = argv0; *p; p++)
        if (*p == '/')
            base = p + 1;
    if (*base)
        snprintf(g_program_name, sizeof(g_program_name), "%s", base);
}

const char *glr_ctrl_program_name(void) {
    return g_program_name;
}

void glr_ctrl_bootstrap_repl(const char *input_file) {
    /* Dump-only CLI paths (--dump-code / --dump-flat) call this without
     * going through glr_ctrl_init_gl, so the status sink and
     * export-config bridge would otherwise be missing here and any
     * @cfg in imported files would be silently dropped.
     * glr_ctrl_install_app_services is idempotent so the windowed path
     * (which already installed via glr_ctrl_reset_all) is unaffected.
     * (Originally the Step 4 [P2] fix in
     * feature/decouple-repl-from-gl-repl-alt.md.) */
    glr_ctrl_install_app_services();
    /* The windowed path patches REPL-state sentinels via
     * glr_ctrl_init_gl -> glr_ctrl_reset_all -> repl_state_reset_program
     * before reaching here. The dump-only paths skip init_gl, so without
     * this the document/flat capacities stay 0 (raw BSS) and every load
     * insert is rejected with a misleading "command store at capacity"
     * — i.e. --dump-code / --dump-flat would print an empty buffer for any
     * non-empty file. Idempotent, so the windowed path is unaffected. */
    repl_state_ensure_sentinels();
    repl_eval_init_predef_vars();
    ReplPredefView predef = repl_eval_predef_view();
    for (int i = 0; i < predef.count; i++) {
        if (strcmp(predef.vars[i].name, "t") == 0) {
            repl_state_variables_mut()->time_var_idx = i;
            break;
        }
    }
    editor_state_edit_line_set(repl_load_initial_commands(input_file));
    /* Startup banner. Lives on the controller side so pipeline TUs
     * don't own display-string side effects. (Moved out of
     * src/repl/core.c as step 3 of
     * feature/decouple-repl-from-gl-repl-alt.md.) */
    repl_set_status("Ready - type GL commands, press ; to execute. F1 for help. F12 for examples.");
}

void glr_ctrl_set_accum(int enabled) {
    glr_state_render_mut()->use_accum = enabled ? 1 : 0;
}

void glr_ctrl_set_time(float value) {
    repl_set_time(value);
}

void glr_ctrl_set_edit_line(int line) {
    int count = repl_state_document_count();
    if (line < 0 || count <= 0) return;
    if (line >= count) line = count - 1;
    editor_state_edit_line_set(line);
    editor_load_line_to_input(line);
}

void glr_ctrl_set_accum_passes(int count) {
    static const int steps[] = { 1, 2, 4, 8, 12, 16 };
    for (int i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); i++) {
        if (steps[i] == count) {
            glr_state_render_mut()->accum_passes = count;
            return;
        }
    }
    fprintf(stderr,
            "gl-repl: GLR_ACCUM_PASSES=%d ignored (want 1/2/4/8/12/16)\n",
            count);
}

void glr_ctrl_fill_export_layout(ReplExportLayout *out) {
    if (!out) return;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    ui_layout_scene_rect(&sx, &sy, &sw, &sh);
    *out = (ReplExportLayout){
        .scene_w = sw,
        .scene_h = sh,
    };
}

/* ===========================================================================
 * Router helpers: non-editor input concerns
 *
 * glr_ctrl is the controller — it owns routing of raw GLUT input to
 * the subsystem that owns each concern (replay, audio, config, save,
 * scene cycle, variable panel, scene press, camera, scroll wheel,
 * help). The editor's keyboard_func / special_func / mouse_func /
 * motion_func / mousewheel_func dispatchers see only editor-text
 * concerns: every helper below is run before the editor handler.
 *
 * Helpers are exported (declared in glr_ctrl.h) so test fixtures
 * can drive a single routing concern without applying GLUT effects.
 * Helpers fill the editor_input EditorInputDispatchEffects via
 * editor_request_redraw etc.; glutPostRedisplay / glutSetCursor /
 * glutTimerFunc fire only from glr_ctrl_apply_input_effects, which
 * the dispatch entry points call after the helpers. Test fixtures
 * bypass apply_input_effects entirely.
 * ===========================================================================
 */


/* Ctrl+N: cycle the experimental post-process effects. One key walks
 * every (effect, target) position plus Off, keeping the two mode fields
 * mutually exclusive so an effect never runs twice in one frame:
 *
 *   Off
 *   -> Chromatic aberration (scene)   post_filter_mode, over the scene rect
 *   -> Chromatic aberration (frame)   compositor_filter_mode, whole frame
 *   -> Vignette (scene)
 *   -> Vignette (frame)
 *   -> Off ...
 *
 * The scene slot feeds scene_render_3d_scene's per-scene pass; the frame
 * slot feeds the glr_compositor hook at frame end. Table-driven so a new
 * ScenePostFilterMode is two rows, not new branches. Hidden shortcut
 * only — no Config row, no @cfg. Session-level state on
 * GlrPresentationState. */
int glr_ctrl_router_handle_post_filter_key(unsigned char key) {
    if (!keymap_event_is(key, GLR_POST_FILTER))
        return 0;

    static const struct {
        ScenePostFilterMode scene;   /* -> post_filter_mode       */
        ScenePostFilterMode frame;   /* -> compositor_filter_mode */
        const char         *where;
    } cycle[] = {
        { SCENE_POST_FILTER_OFF,                  SCENE_POST_FILTER_OFF,                  ""        },
        { SCENE_POST_FILTER_CHROMATIC_ABERRATION, SCENE_POST_FILTER_OFF,                  " (scene)" },
        { SCENE_POST_FILTER_OFF,                  SCENE_POST_FILTER_CHROMATIC_ABERRATION, " (frame)" },
        { SCENE_POST_FILTER_VIGNETTE,             SCENE_POST_FILTER_OFF,                  " (scene)" },
        { SCENE_POST_FILTER_OFF,                  SCENE_POST_FILTER_VIGNETTE,             " (frame)" },
    };
    int n = (int)(sizeof(cycle) / sizeof(cycle[0]));

    GlrPresentationState *p = glr_state_presentation_mut();

    /* Locate the current position (default Off if state is somehow
     * off-cycle), then advance to the next. */
    int cur = 0;
    for (int i = 0; i < n; i++) {
        if (cycle[i].scene == (ScenePostFilterMode)p->post_filter_mode &&
            cycle[i].frame == (ScenePostFilterMode)p->compositor_filter_mode) {
            cur = i;
            break;
        }
    }
    int next = (cur + 1) % n;
    p->post_filter_mode       = cycle[next].scene;
    p->compositor_filter_mode = cycle[next].frame;

    ScenePostFilterMode shown = cycle[next].scene != SCENE_POST_FILTER_OFF
                              ? cycle[next].scene : cycle[next].frame;
    char msg[80];
    snprintf(msg, sizeof(msg), "Post filter: %s%s",
             scene_postprocess_filter_mode_name(shown), cycle[next].where);
    repl_set_status(msg);
    return 1;
}

/* Per-frame tick (16 ms): advance audio playlist, surface track-change
 * status, advance time variable, advance replay state, decay camera
 * momentum, blink the cursor, decay the status TTL.
 *
 * The work is split from the GLUT scheduling so test fixtures (which
 * don't initialize GLUT) can drive a single tick by calling
 * glr_ctrl_tick directly. The public timer entry adds
 * glutPostRedisplay + glutTimerFunc reschedule on top. */
void glr_ctrl_tick(void) {
    /* SIGINT (Ctrl+C) requested quit: the handler only set a flag; the
     * router owns the flag + the recovery save + exit (so no stdio/file
     * I/O runs inside the signal handler). Runs it here on the normal path. */
    glr_ctrl_router_run_pending_quit();

    /* Advance the audio playlist if the current song reached its end
     * (no-op under loop=Song; see glr_audio_tick). */
    glr_audio_tick();

    /* When the playing track changes (either auto-advance from tick
     * or manual next/prev), surface the song name in the status bar.
     * Tracking by generation avoids needing a callback hook into
     * the audio module. */
    {
        static unsigned int last_track_gen = 0;
        unsigned int gen = glr_audio_track_generation();
        if (gen != last_track_gen) {
            last_track_gen = gen;
            const char *path = glr_audio_get_current_track();
            if (path && *path) {
                const char *base = strrchr(path, '/');
                base = base ? base + 1 : path;
                char msg[REPL_DIAG_TEXT_MAX];
                snprintf(msg, sizeof(msg), "Now playing: %s", base);
                repl_set_status(msg);
            }
        }
    }

    repl_advance_time(GLR_FRAME_DT_SECS);

    {
        ReplayRuntimeState *replay = replay_state_mut();

        if (replay->active)
            replay_tick_fade_batches(GLR_FRAME_DT_SECS);

        if (replay->active && replay->state == REPLAY_PLAYING) {
            replay->accum += replay->speed * GLR_FRAME_DT_SECS;
            FlatProgramView flat_program = repl_state_flat_program_view();
            while (replay->accum >= 1.0f &&
                   replay->state == REPLAY_PLAYING) {
                replay->accum -= 1.0f;
                replay_advance(flat_program);
            }
        }
    }

    glr_ctrl_tick_view_transition(GLR_FRAME_DT_SECS);
    glr_camera_tick();
    glr_ctrl_tick_overlay_xn();

    {
        /* Easing for variable panel's lift above replay HUD (Smell #21/#22/#40) */
        VariablePanelViewState *vp = variable_panel_state_mut();
        float target = 0.0f;
        if (replay_active()) {
            float lift_target = (float)(REPLAY_HUD_BOTTOM_Y +
                                        GLR_CTRL_REPLAY_PANEL_CLEARANCE_PX -
                                        GLR_CTRL_REPLAY_PANEL_BASE_Y_PX);
            if (lift_target > 0.0f) target = lift_target;
        }
        vp->replay_lift_px +=
            (target - vp->replay_lift_px) * GLR_CTRL_REPLAY_PANEL_LIFT_EASE;
        if (fabsf(target - vp->replay_lift_px) <
            GLR_CTRL_REPLAY_PANEL_LIFT_SNAP_PX) {
            vp->replay_lift_px = target;
        }
    }

    {
        EditorCursorBlinkState *cb = editor_state_cursor_blink_mut();
        (cb->blink_tick)++;
        if (cb->blink_tick >= GLR_CURSOR_BLINK_TICKS) {
            cb->blink_tick = 0;
            cb->cursor_visible = !cb->cursor_visible;
        }
    }

    {
        /* Inline rename no longer rides the status line (it owns its
         * own display state via the snapshot rename view), so the TTL
         * just ages normally. */
        UiStatusState *status = ui_state_status_mut();
        if (status->ttl > 0) {
            status->ttl--;
            /* Age the live message so the banner's telescope-out animation
             * advances even while a per-frame re-emitter keeps renewing the
             * ttl (see ui_state_status_set_kind). Capped to avoid overflow
             * on long-lived hints. */
            if (status->age < (1 << 24))
                status->age++;
        }

        /* Tutorial COMMAND-step hint persistence: while a COMMAND step
         * is active, keep the affordance hint visible by re-emitting it
         * each frame — but only when the status slot is empty (TTL has
         * fully expired) or already shows one of our hint variants
         * (recognised by the "Tutorial: step " prefix via
         * tutorial_status_is_hint). Non-tutorial messages (parse
         * errors, save confirmations, etc.) keep their full TTL window
         * uncontested; once they fade, the next tick brings the hint
         * back. */
        char hint[REPL_STATUS_TEXT_MAX];
        if (tutorial_status_hint(hint, sizeof hint) &&
            (status->ttl <= 0 || tutorial_status_is_hint(status->text))) {
            ui_state_status_set(hint);
        }
    }
}

void glr_ctrl_timer(int value) {
    (void)value;
    glr_ctrl_tick();
    glutPostRedisplay();
    glutTimerFunc(GLR_FRAME_DT_MS, glr_ctrl_timer, 0);
}

void glr_shutdown(void) {
    glr_audio_shutdown();
    repl_executor_destroy_resources();
}
