#include "app/glr_ctrl.h"

#include "c_compat.h"  /* STATIC_ASSERT (C99/C11 portable) */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include "gl_includes.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>

#include "app/glr_audio.h"
#include "subsystems/color_picker/color_picker_state.h"
#include "editor/clipboard.h"
#include "app/glr_completion.h"
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
#include "app/glr_actions.h"
#include "app/glr_config.h"
#include "app/glr_camera.h"
#include "app/glr_debug.h"
#include "keys.h"
#include "support/prof.h"
#include "repl/core.h"
#include "repl/examples.h"          /* REPL_EXAMPLE_TAG_* */
#include "repl/eval.h"
#include "repl/parser.h"
#include "repl/executor.h"
#include "repl/export.h"
#include "repl/help_text.h"
#include "repl/pipeline.h"
#include "repl/replay_annotations.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#include "repl/tutorials.h"
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "ui/app/replay_hud.h"
#include "scene/overlays.h" /* scene_draw_vertex_number_label / _arrow primitives */
#include "scene/palette.h" /* scene_clr / scene_clr_a scene-space colors */
#include "scene/postprocess_filter.h" /* ScenePostFilterMode, mode_name */
#include "scene/render.h"
#include "scene/guides/transform_guides.h" /* scene_transform_guides_prepare / _render_if_due */
#include "scene/guides/transform_utils.h"  /* apply_tracked_transform / unwind_transform_stack */
#include "ui/app/autocomplete_panel.h"
#include "ui/app/editor.h"
#include "ui/app/layout.h"
#include "ui/app/menu_bar.h"
#include "ui/core/metrics.h"
#include "ui/app/numeric_swatch.h"
#include "ui/app/panels.h"
#include "ui/app/repl_code_panel.h"
#include "ui/app/profile_panel.h"
#include "ui/app/snapshot.h"
#include "ui/app/state.h"
#include "ui/app/state_types.h" /* UI-chrome typedefs (CodePanel/Camera/Help/etc.) */
#include "ui/core/tabbed_overlay.h"
#include "ui/app/variable_panel.h"
#include "subsystems/variable_panel/variable_panel_drag.h"
#include "subsystems/variable_panel/variable_panel_state.h"

static int glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(
    const UiRenderSnapshot *snap,
    int *out_follow_doc_line,
    int *out_visible_lines);

static int glr_ctrl_cmd_is_focus_vertex(const GLCmd *cmd) {
    /* glVertex2f counts too: args[2] is zero on a well-formed 2D vertex
     * (parser leaves the slot zero-initialised), so building focus.pos
     * from args[0..2] gives (x, y, 0) — the right point to focus on. */
    return cmd->valid && repl_cmd_emits_vertex(cmd->type);
}

static SceneFocusVertex glr_ctrl_build_focus_vertex(void) {
    SceneFocusVertex focus = { .valid = 0 };
    int edit_line = editor_state_edit_line();

    if (edit_line >= 0 && edit_line < repl_state_document_count() &&
        glr_ctrl_cmd_is_focus_vertex(&repl_state_document_cmds_mut()[edit_line])) {
        focus.pos[0] = repl_state_document_cmds_mut()[edit_line].args[0];
        focus.pos[1] = repl_state_document_cmds_mut()[edit_line].args[1];
        focus.pos[2] = repl_state_document_cmds_mut()[edit_line].args[2];
        focus.valid = 1;
    } else {
        for (int i = edit_line - 1; i >= 0; i--) {
            if (glr_ctrl_cmd_is_focus_vertex(&repl_state_document_cmds_mut()[i])) {
                focus.pos[0] = repl_state_document_cmds_mut()[i].args[0];
                focus.pos[1] = repl_state_document_cmds_mut()[i].args[1];
                focus.pos[2] = repl_state_document_cmds_mut()[i].args[2];
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
            normal_args, snapshot->normal_args, 3, NULL, 0);
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
        .show_guides = presentation.show_vertex_guides,
        .replaying = replay_active(),
        .xform_guide_mode = presentation.xform_guide_mode,
        .user_lighting_enabled = config ? config->user_lighting_enabled : 0,
        .anim_time = vars.anim_time,
        .input = input.input,
        .input_len = input.input_len,
        .cursor_pos = input.cursor_pos,
        .edit_line_idx = edit_line,
        .inserting = editor_insert_mode(),
        .edit_line_committed_text = editor_buffer_line(edit_line),
        .source_cmds = repl_state_document_cmds_mut(),
        .source_cmd_count = repl_state_document_count(),
        .flat_program = repl_state_flat_program_view(),
        .alpha_scale = config ? config->alpha_scale : 1.0f,
    };
    fill_guide_arg_slots(&snapshot, input.input, input.input_len);
    return snapshot;
}

/* Re-establish the REPL's predef-var / scratch-array baseline before
 * each fade-batch render so each batch starts from the same world state. */
static void glr_ctrl_replay_restore_baseline(const ReplayFadePlan *fade_plan) {
    if (!fade_plan) return;
    repl_restore_predef_values(fade_plan->baseline_predef_vals, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(fade_plan->baseline_scratch_arrays);
}

/* Per-frame replay-fade plan, owned by the controller. Used by the main
 * fill (to clamp the flat-cmd count to the pre-fade base limit) and by
 * the post_fill_fn hook (to render the fading-batch overlay). */
static ReplayFadePlan g_replay_fade_plan;
static int            g_replay_fade_plan_active;     /* 1 = post_fill_fn should render fades */
static int            g_replay_fade_plan_base_limit; /* clamp for the main fill */

/* Whether the current frame should render the tess-preview wireframe
 * (replay's polygon-mode overlay). Set by build_scene_config; read by
 * the post_fill_fn body. */
static int            g_replay_tess_preview_active;

/* Tiny GL primitives the REPL tess-preview walker calls back into. They
 * exist as static functions here because the walker takes a callback
 * struct rather than driving GL itself — that keeps the walker (REPL
 * data traversal) and the rendering (GL primitives) on the right sides
 * of the layering boundary. */
static void tess_preview_begin_contour(void *ud) { (void)ud; glBegin(GL_LINE_STRIP); }
static void tess_preview_vertex(float x, float y, float z, void *ud) {
    (void)ud;
    glVertex3f(x, y, z);
}
static void tess_preview_end_contour(void *ud) { (void)ud; glEnd(); }

static const ReplayTessPreviewCallbacks g_tess_preview_cb = {
    .begin_contour = tess_preview_begin_contour,
    .vertex        = tess_preview_vertex,
    .end_contour   = tess_preview_end_contour,
};

static void glr_ctrl_build_replay_fade_plan(int replaying) {
    ReplayFadeBatchView fade_batches;
    int batch_count;

    memset(&g_replay_fade_plan, 0, sizeof(g_replay_fade_plan));
    g_replay_fade_plan_active = 0;
    g_replay_fade_plan_base_limit = 0;

    if (!replaying)
        return;

    replay_copy_baseline_predef_values(g_replay_fade_plan.baseline_predef_vals,
                                            MAX_PREDEF_VARS);
    replay_copy_baseline_scratch_arrays(
        g_replay_fade_plan.baseline_scratch_arrays);

    if (!replay_has_active_fades())
        return;

    g_replay_fade_plan_base_limit = replay_fill_base_limit();
    fade_batches = replay_fade_batches_view();
    batch_count = replay_compute_fade_skip_limits(g_replay_fade_plan.skip_limits,
                                                       REPLAY_FADE_BATCH_MAX);
    if (batch_count > REPLAY_FADE_BATCH_MAX)
        batch_count = REPLAY_FADE_BATCH_MAX;

    g_replay_fade_plan.batch_count = batch_count;
    for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
        const ReplayFadeBatch *batch = &fade_batches.batches[batch_idx];
        g_replay_fade_plan.batches[batch_idx] = *batch;
        g_replay_fade_plan.batch_alpha[batch_idx] = replay_batch_alpha(batch);
    }
    g_replay_fade_plan_active = 1;
}

/* Render the fading-batch overlays prepared in g_replay_fade_plan. */
static void glr_ctrl_render_replay_fade_batches(void) {
    const ReplayFadePlan *plan = &g_replay_fade_plan;
    int batch_count = plan->batch_count;
    if (batch_count <= 0) return;

    prof_begin(PROF_SCENE_3D_FADE);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    /* Material reflectance *coefficients* (glMaterialfv) — not glColor*
     * draw colors — so they stay named local consts and are NOT
     * scene/palette.h tokens. */
    static const GLfloat mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };
    static const GLfloat mshin[] = { 30.0f };
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    FlatProgramView program = repl_state_flat_program_view();
    SourceTextView text = source_document_view();

    for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
        const ReplayFadeBatch *batch = &plan->batches[batch_idx];
        float alpha = plan->batch_alpha[batch_idx];
        if (alpha <= 0.0f) continue;

        prof_begin(PROF_SCENE_3D_FADE_BATCH_PREP);
        glr_ctrl_replay_restore_baseline(plan);
        scene_clr_a(SCENE_CLR_REPLAY_FADE, alpha);
        glPushMatrix();
        prof_accum_end(PROF_SCENE_3D_FADE_BATCH_PREP);

        prof_begin(PROF_SCENE_3D_FADE_BATCH_EXEC);
        repl_execute_set_fade_context(alpha, plan->skip_limits[batch_idx]);
        repl_execute_program(&(ReplExecutionOptions){
            .flat_cmd_count = batch->new_pc,
            .program        = program,
            .text           = text,
        });
        prof_accum_end(PROF_SCENE_3D_FADE_BATCH_EXEC);

        glPopMatrix();
    }

    prof_begin(PROF_SCENE_3D_FADE_BATCH_POST);
    repl_execute_set_fade_context(1.0f, 0);
    glPopAttrib();
    prof_accum_end(PROF_SCENE_3D_FADE_BATCH_POST);

    prof_accum_end(PROF_SCENE_3D_FADE);
}

/* Render the polygon-mode tess-preview wireframe by walking the flat
 * program through replay_walk_tess_preview() and emitting line
 * strips at each transformed contour. The walker handles iteration and
 * matrix tracking; we own the visual GL state. */
static void glr_ctrl_render_replay_tess_preview(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    scene_clr_a(SCENE_CLR_TESS_PREVIEW, 0.80f);
    glLineWidth(2.0f);

    replay_walk_tess_preview(&g_tess_preview_cb, NULL);

    glLineWidth(1.0f);
    glPopAttrib();
}

/* Replay overlays installed on SceneRenderConfig.post_fill_fn — runs
 * between the scene's user-geometry fill and its grid/axes/backdrop
 * helpers. Composes the fading-batch overlay and the polygon-mode
 * tess-preview wireframe; both are pure REPL-state visualizations so
 * the scene module never sees them. */
static void glr_ctrl_post_fill_replay_overlay(void *user_data) {
    (void)user_data;
    if (g_replay_fade_plan_active)
        glr_ctrl_render_replay_fade_batches();
    if (g_replay_tess_preview_active)
        glr_ctrl_render_replay_tess_preview();
}

/* --- post_overlays_fn body: vertex_numbers + normal_vectors ------------
 *
 * Both overlays walk the user's flat program through the REPL walker and
 * emit one scene primitive per visited vertex. The orchestration lives
 * here in the controller — scene/overlays.c just exposes
 * scene_draw_vertex_number_label / scene_draw_normal_vector_arrow. */

static void on_vertex_number_label(const ReplayVertexWalkState *state,
                                   float vx, float vy, float vz,
                                   void *user) {
    (void)user;
    scene_draw_vertex_number_label(state->vertex_idx_in_block, vx, vy, vz);
}

static void on_normal_vector_arrow(const ReplayVertexWalkState *state,
                                   float vx, float vy, float vz,
                                   void *user) {
    float scale = *(const float *)user;
    scene_draw_normal_vector_arrow(vx, vy, vz,
                                   state->normal[0],
                                   state->normal[1],
                                   state->normal[2],
                                   scale);
}

static ReplayVertexWalkContext glr_ctrl_build_vertex_walk_context(int selected_block_only) {

    ReplayVertexWalkContext ctx = {
        .program                = repl_state_flat_program_view(),
        .edit_line_idx          = editor_state_edit_line(),
        .cursor_block_begin     = repl_state_flat_program_current_block_begin(),
        .cursor_block_end       = repl_state_flat_program_current_block_end(),
        .cursor_func_scope_mask = 0,  /* not currently exposed via repl_state */
        .selected_block_only    = selected_block_only,
    };
    return ctx;
}

static void glr_ctrl_render_vertex_numbers(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    scene_clr(SCENE_CLR_VERTEX_LABEL);

    static const ReplayVertexWalkCallbacks cb = {
        .on_vertex = on_vertex_number_label,
    };
    ReplayVertexWalkContext ctx = glr_ctrl_build_vertex_walk_context(1);
    replay_walk_user_vertices(&ctx, &cb, NULL);

    glPopAttrib();
}

/* World-space length of the per-vertex normal-vector overlay arrows. */
#define GLR_NORMAL_ARROW_SCALE 0.35f

static void glr_ctrl_render_normal_vectors(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    scene_clr(SCENE_CLR_NORMAL_LABEL);

    static const ReplayVertexWalkCallbacks cb = {
        .on_vertex = on_normal_vector_arrow,
    };
    float scale = GLR_NORMAL_ARROW_SCALE;
    ReplayVertexWalkContext ctx = glr_ctrl_build_vertex_walk_context(0);
    replay_walk_user_vertices(&ctx, &cb, &scale);

    glPopAttrib();
}

/* --- Outline + vertex-point overlays (walking-based) --------------------
 *
 * Lifted from the old scene/overlays.c so the visual matches commit-
 * before-the-polygon-mode-experiment exactly. The walks live entirely
 * in the controller now; scene only exposes per-vertex primitives.
 *
 * Inputs (flat program, cursor block, presentation flags) come in via
 * a small OverlayWalkCtx the controller fills once at the top of
 * post_overlays. The renders take that context plus the per-frame
 * SceneRenderConfig (for multisample / line_smooth quality flags).
 *
 * The walks call apply_tracked_transform / unwind_transform_stack
 * — tiny inline GLCmd→GL helpers in scene/guides/transform_utils.h.
 * Pending follow-up: relocate that header to REPL territory so this
 * dependency goes away. */

typedef struct OverlayWalkCtx {
    FlatProgramView program;
    int             edit_line_idx;
    int             cursor_block_begin;
    int             cursor_block_end;
    unsigned int    cursor_func_scope_mask;
    int             show_vertex_outlines;
    int             highlight_current_poly;
    int             replay_tess_preview;
    int             show_vertex_points;
    int             replay_vertex_points;
} OverlayWalkCtx;

static int outline_begin_mode_has_overlay(GLenum mode) {
    switch (mode) {
    case GL_POINTS:
    case GL_LINES:
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        return 0;
    default:
        return 1;
    }
}

/* Cursor-block match predicates. Identical to the predicates that used
 * to live in scene/overlays.c, but reading from the controller-side
 * OverlayWalkCtx rather than SceneRenderConfig. */
static int outline_cmd_matches_cursor(int flat_idx,
                                      const OverlayWalkCtx *ctx) {
    if (flat_idx < 0 || flat_idx >= ctx->program.cmd_count) return 0;
    const GLCmd *cmd = &ctx->program.cmds[flat_idx];
    if (!cmd->valid) return 0;
    if (ctx->edit_line_idx < 0) return 0;

    if (cmd->call_src_cmd_idx == ctx->edit_line_idx ||
        cmd->root_call_src_cmd_idx == ctx->edit_line_idx)
        return 1;
    if (ctx->cursor_func_scope_mask != 0 &&
        (cmd->func_scope_mask & ctx->cursor_func_scope_mask) != 0)
        return 1;
    if (ctx->cursor_block_begin >= 0 &&
        flat_idx >= ctx->cursor_block_begin &&
        flat_idx <= ctx->cursor_block_end)
        return 1;
    return cmd->src_cmd_idx == ctx->edit_line_idx;
}

static int outline_block_matches_cursor(int begin_idx, int is_tess,
                                        const OverlayWalkCtx *ctx) {
    const GLCmd *cmds = ctx->program.cmds;
    int cmd_count = ctx->program.cmd_count;
    int depth = is_tess ? 1 : 0;

    for (int i = begin_idx; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;
        if (outline_cmd_matches_cursor(i, ctx)) return 1;
        if (!is_tess && i > begin_idx && cmds[i].type == CMD_END) break;
        if (is_tess && i > begin_idx) {
            if (cmds[i].type == CMD_TESS_BEGIN_POLYGON) depth++;
            else if (cmds[i].type == CMD_TESS_END) {
                depth--;
                if (depth == 0) break;
            }
        }
    }
    return 0;
}

static void glr_ctrl_render_outlines(const OverlayWalkCtx *ctx,
                                        int multisample_enabled,
                                        int line_smooth_enabled) {
    const GLCmd *cmds = ctx->program.cmds;
    int cmd_count = ctx->program.cmd_count;

    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(REPL_OUTLINE_POLYGON_OFFSET_FACTOR,
                    REPL_OUTLINE_POLYGON_OFFSET_UNITS);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    if (ctx->show_vertex_outlines || ctx->highlight_current_poly) {
        glPushMatrix();
        int in_begin = 0;
        int matrix_depth = 0;
        int block_is_current = 0;
        int tess_in_contour = 0;
        int tess_poly_is_current = 0;

        for (int i = 0; i < cmd_count; i++) {
            if (!cmds[i].valid) continue;

            if (repl_cmd_is_transform(cmds[i].type)) {
                if (!in_begin && !tess_in_contour)
                    apply_tracked_transform(&cmds[i], &matrix_depth);
                continue;
            }

            switch (cmds[i].type) {
            case CMD_TESS_BEGIN_CONTOUR:
                if (ctx->replay_tess_preview) break;
                if (tess_in_contour) {
                    glEnd();
                    glLineWidth(1.0f);
                }
                if (ctx->show_vertex_outlines || tess_poly_is_current) {
                    glLineWidth(1.5f);
                    if (tess_poly_is_current)
                        scene_clr(SCENE_CLR_OUTLINE_ACTIVE);
                    else
                        scene_clr(SCENE_CLR_OUTLINE);
                    glBegin(GL_LINE_LOOP);
                    tess_in_contour = 1;
                }
                break;
            case CMD_TESS_VERTEX:
                if (ctx->replay_tess_preview) break;
                if (tess_in_contour)
                    glVertex3f(cmds[i].args[0], cmds[i].args[1], cmds[i].args[2]);
                break;
            case CMD_TESS_END:
                if (ctx->replay_tess_preview) {
                    tess_poly_is_current = 0;
                    break;
                }
                if (tess_in_contour) {
                    glEnd();
                    glLineWidth(1.0f);
                    tess_in_contour = 0;
                }
                if (!tess_in_contour) tess_poly_is_current = 0;
                break;
            case CMD_BEGIN: {
                int draw_outline = outline_begin_mode_has_overlay((GLenum)cmds[i].args[0]);
                if (in_begin) glEnd();
                block_is_current = ctx->highlight_current_poly &&
                                   outline_block_matches_cursor(i, 0, ctx);
                if (block_is_current) {
                    glLineWidth(3.0f);
                    scene_clr(SCENE_CLR_OUTLINE_ACTIVE);
                } else if (ctx->show_vertex_outlines && draw_outline) {
                    glLineWidth(1.2f);
                    scene_clr(SCENE_CLR_OUTLINE_EDGE);
                } else {
                    in_begin = 0;
                    break;
                }
                glBegin((GLenum)cmds[i].args[0]);
                in_begin = 1;
                break;
            }
            case CMD_END:
                if (in_begin) {
                    glEnd();
                    in_begin = 0;
                    glLineWidth(1.0f);
                    scene_clr(SCENE_CLR_OUTLINE_EDGE);
                }
                block_is_current = 0;
                break;
            case CMD_VERTEX3F:
                if (in_begin && (block_is_current || ctx->show_vertex_outlines))
                    glVertex3f(cmds[i].args[0], cmds[i].args[1], cmds[i].args[2]);
                break;
            case CMD_VERTEX2F:
                if (in_begin && (block_is_current || ctx->show_vertex_outlines))
                    glVertex2f(cmds[i].args[0], cmds[i].args[1]);
                break;
            case CMD_TESS_BEGIN_POLYGON:
                tess_poly_is_current = ctx->highlight_current_poly &&
                                       outline_block_matches_cursor(i, 1, ctx);
                break;
            default:
                break;
            }
        }

        if (in_begin) {
            glEnd();
            glLineWidth(1.0f);
        }
        if (tess_in_contour) {
            glEnd();
            glLineWidth(1.0f);
        }
        unwind_transform_stack(&matrix_depth);
        glPopMatrix();
    }

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

static void glr_ctrl_render_vertex_points(const OverlayWalkCtx *ctx) {
    if (!ctx->show_vertex_points && !ctx->replay_vertex_points) return;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);

    if (ctx->replay_vertex_points)
        scene_clr_a(SCENE_CLR_VERTEX_POINT_REPLAY, 0.75f);
    else
        scene_clr_a(SCENE_CLR_VERTEX_POINT, 0.80f);

    glPushMatrix();
    {
        const GLCmd *flat_cmds = ctx->program.cmds;
        int flat_cmd_count = ctx->program.cmd_count;
        int matrix_depth = 0;
        GLenum primitive_mode = 0;

        for (int i = 0; i < flat_cmd_count; i++) {
            if (!flat_cmds[i].valid) continue;
            if (repl_cmd_is_transform(flat_cmds[i].type)) {
                apply_tracked_transform(&flat_cmds[i], &matrix_depth);
            } else if (flat_cmds[i].type == CMD_BEGIN) {
                primitive_mode = (GLenum)flat_cmds[i].args[0];
            } else if (flat_cmds[i].type == CMD_END) {
                primitive_mode = 0;
            } else if (repl_cmd_emits_vertex(flat_cmds[i].type)) {
                int is_line = (primitive_mode == GL_LINES ||
                               primitive_mode == GL_LINE_STRIP ||
                               primitive_mode == GL_LINE_LOOP);
                glPointSize(is_line ? 2.0f : 7.0f);
                glBegin(GL_POINTS);
                glVertex3f(flat_cmds[i].args[0], flat_cmds[i].args[1],
                           flat_cmds[i].args[2]);
                glEnd();
            }
        }
        glPointSize(1.0f);
        unwind_transform_stack(&matrix_depth);
    }
    glPopMatrix();

    glPopAttrib();
}

/* --- Cursor edit-guide walker --------------------------------------------
 *
 * The geometry-guide planes / crosshairs and transform-guide arrows need
 * to render at the user's current modelview transform at the cursor's
 * flat-cmd position. We walk the program with the existing REPL vertex
 * walker (which tracks transforms) and invoke the scene-side guide
 * renderers at each cmd's transformed position. */

typedef struct {
    const SceneGuideSnapshot *snapshot;
    SceneTransformGuidePlan   xform_plan;
    float                     cam_view[16];
    int                       have_xform;
    int                       geometry_guide_done;
    int                       early_stop;
} CursorGuideRenderCtx;

/* Search forward in `flat` from `start_idx` looking for the next
 * vertex-emitting command, and write its evaluated args into `out`.
 * Returns 1 on success, 0 if a block boundary (CMD_BEGIN / CMD_END /
 * tess boundaries) intervenes or no vertex is found before the end.
 *
 * Mirrors draw_normal_guides's source-cmd forward search but reads
 * from the flat program — flat args are re-evaluated every frame for
 * has_vars commands, so the position tracks dynamically-assigned
 * vars (e.g. waves' `x = -b/2 + b*j/n` inside the loop body). */
static int find_next_vertex_args_in_flat(const FlatProgramView *flat,
                                         int start_idx, float out[3]) {
    if (!flat || !out) return 0;
    for (int i = start_idx + 1; i < flat->cmd_count; i++) {
        const GLCmd *c = &flat->cmds[i];
        if (!c->valid) continue;
        if (repl_cmd_emits_vertex(c->type)) {
            out[0] = c->args[0];
            out[1] = c->args[1];
            out[2] = (c->type == CMD_VERTEX2F) ? 0.0f : c->args[2];
            return 1;
        }
        if (c->type == CMD_END || c->type == CMD_BEGIN ||
            c->type == CMD_TESS_END || c->type == CMD_TESS_BEGIN_POLYGON)
            return 0;
    }
    return 0;
}

/* Return a copy of `snapshot` with vertex_args / normal_args / normal
 * base position overridden from live flat-program data.
 *
 * Why (vertex_args / normal_args): the snapshot's vertex_args /
 * normal_args are parsed from the input text via a predef-only
 * evaluator, which can't resolve function-local parameters
 * (e.g. `scale`, `phase` from `funcN(scale, phase)`). On a cursor
 * inside a funcN body those args silently evaluate to 0, so a guide
 * drawn from them lands at the local origin — visually the *center*
 * of the transformed object rather than the vertex/normal the user
 * is editing.
 *
 * Why (normal_base_pos): draw_normal_guides anchors its arrow at the
 * NEXT vertex following the cursor's normal line. Its built-in
 * forward search uses source_cmds whose args are frozen at parse
 * time; for examples like waves — where the surrounding x/y/z vars
 * are reassigned inside a for-loop body — that anchor doesn't track
 * the dynamic per-iteration position. Walking the flat program
 * (re-evaluated every frame) gives the live anchor.
 *
 * Flatten has already substituted parameters and evaluated
 * expressions, so flat->args carries the real numeric values. The
 * helper is pure — no GL state, no side effects — to keep
 * `on_cmd_render_cursor_guides` testable in isolation. */
static SceneGuideSnapshot
cursor_guide_snapshot_with_flat_args(const SceneGuideSnapshot *snapshot,
                                     const GLCmd *flat,
                                     int flat_idx) {
    SceneGuideSnapshot snap = *snapshot;
    if (!flat) return snap;
    if (repl_cmd_emits_vertex(flat->type)) {
        snap.vertex_args[0] = flat->args[0];
        snap.vertex_args[1] = flat->args[1];
        snap.vertex_args[2] = (flat->type == CMD_VERTEX2F) ? 0.0f
                                                           : flat->args[2];
    } else if (flat->type == CMD_NORMAL3F || flat->type == CMD_TESS_NORMAL) {
        snap.normal_args[0] = flat->args[0];
        snap.normal_args[1] = flat->args[1];
        snap.normal_args[2] = flat->args[2];
        if (find_next_vertex_args_in_flat(&snap.flat_program, flat_idx,
                                          snap.normal_base_pos))
            snap.normal_base_pos_valid = 1;
    }
    return snap;
}

static void on_cmd_render_cursor_guides(const ReplayVertexWalkState *state,
                                         void *user) {
    CursorGuideRenderCtx *ctx = (CursorGuideRenderCtx *)user;
    int is_cursor = (state->src_cmd_idx == ctx->snapshot->edit_line_idx);

    if (is_cursor && !ctx->geometry_guide_done && !ctx->snapshot->replaying) {
        const GLCmd *flat =
            (state->flat_cmd_idx >= 0 &&
             state->flat_cmd_idx < ctx->snapshot->flat_program.cmd_count)
              ? &ctx->snapshot->flat_program.cmds[state->flat_cmd_idx]
              : NULL;
        SceneGuideSnapshot snap =
            cursor_guide_snapshot_with_flat_args(ctx->snapshot, flat,
                                                 state->flat_cmd_idx);
        scene_geometry_guides_render_for_cursor(&snap);
        ctx->geometry_guide_done = 1;
    }

    if (ctx->have_xform && !ctx->xform_plan.consumed) {
        scene_transform_guides_render_if_due(ctx->snapshot, &ctx->xform_plan,
                                       state->flat_cmd_idx, ctx->cam_view);
    }

    /* Bail out as soon as both guides have rendered. The geometry guide
     * needs the modelview at the cursor's first flat occurrence; the
     * transform guide fires at the same flat index. After both have run,
     * the walker has no remaining work — important for big loops where
     * walking the full flat program every frame is expensive. */
    int xform_done = (!ctx->have_xform || ctx->xform_plan.consumed);
    int geometry_done = (ctx->geometry_guide_done || ctx->snapshot->replaying);
    if (geometry_done && xform_done)
        ctx->early_stop = 1;
}

static void glr_ctrl_render_cursor_guides(const SceneGuideSnapshot *snapshot) {
    if (!snapshot || !snapshot->show_guides) return;

    CursorGuideRenderCtx ctx;
    ctx.snapshot = snapshot;
    ctx.geometry_guide_done = 0;
    ctx.early_stop = 0;
    ctx.have_xform = scene_transform_guides_prepare(snapshot, &ctx.xform_plan);

    /* Previously this path took a fast exit (`return;`) when no transform
    * guide was needed, calling scene_geometry_guides_render_for_cursor with
     * the caller's modelview only. That broke the geometry guide for
     * cursors inside funcN call frames — the user's accumulated
     * transforms hadn't been applied yet, so the guide rendered at world
     * origin. We always walk now; the stop_flag below makes the walk
     * exit as soon as both guides have rendered, preserving the perf
     * intent for big loops. */
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glPushMatrix();
    glGetFloatv(GL_MODELVIEW_MATRIX, ctx.cam_view);

    static const ReplayVertexWalkCallbacks cb = {
        .on_each_cmd = on_cmd_render_cursor_guides,
    };
    ReplayVertexWalkContext walk = glr_ctrl_build_vertex_walk_context(0);
    walk.stop_flag = &ctx.early_stop;
    replay_walk_user_vertices(&walk, &cb, &ctx);

    glPopMatrix();
    glPopAttrib();
}

static void glr_ctrl_post_overlays(void *user_data) {
    const SceneRenderConfig *cfg = (const SceneRenderConfig *)user_data;
    GlrPresentationState presentation = glr_state_presentation();
    int replaying = replay_active();
    int replay_mode_vertex = replaying && (replay_mode() == REPLAY_MODE_VERTEX);

    OverlayWalkCtx walk = {
        .program                = repl_state_flat_program_view(),
        .edit_line_idx          = editor_state_edit_line(),
        .cursor_block_begin     = repl_state_flat_program_current_block_begin(),
        .cursor_block_end       = repl_state_flat_program_current_block_end(),
        .cursor_func_scope_mask = 0,  /* not currently surfaced via repl_state */
        .show_vertex_outlines   = presentation.show_vertex_outlines,
        .highlight_current_poly = presentation.highlight_current_poly && !replaying,
        .replay_tess_preview    = replay_mode_vertex,
        .show_vertex_points     = presentation.show_vertex_points,
        .replay_vertex_points   = replay_mode_vertex,
    };

    int multisample = cfg ? cfg->multisample_enabled : 0;
    int line_smooth = cfg ? cfg->line_smooth_enabled : 0;
    prof_begin(PROF_SCENE_3D_OVERLAY_OUTLINES);
    glr_ctrl_render_outlines(&walk, multisample, line_smooth);
    glr_ctrl_render_vertex_points(&walk);
    prof_accum_end(PROF_SCENE_3D_OVERLAY_OUTLINES);

    /* Cursor edit guides need the full snapshot (input string, predef
     * vars, source cmds). Build it once here and feed the walker. */
    prof_begin(PROF_SCENE_3D_OVERLAY_BUILD_GUIDES);
    SceneGuideSnapshot snapshot = glr_ctrl_build_guide_snapshot(cfg);
    prof_accum_end(PROF_SCENE_3D_OVERLAY_BUILD_GUIDES);
    prof_begin(PROF_SCENE_3D_OVERLAY_TRANSFORM_GUIDES);
    glr_ctrl_render_cursor_guides(&snapshot);
    prof_accum_end(PROF_SCENE_3D_OVERLAY_TRANSFORM_GUIDES);

    if (presentation.show_vertex_labels){
        prof_begin(PROF_SCENE_3D_OVERLAY_VERTEX_NUMBERS);
        glr_ctrl_render_vertex_numbers();
        prof_accum_end(PROF_SCENE_3D_OVERLAY_VERTEX_NUMBERS);
    }
    if (presentation.show_normal_vectors) {
        prof_begin(PROF_SCENE_3D_OVERLAY_NORMALS);
        glr_ctrl_render_normal_vectors();
        prof_accum_end(PROF_SCENE_3D_OVERLAY_NORMALS);
    }
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
        }
    }

    int src_line = replay_src_line();
    if (replay_active() && src_line >= 0)
        editor_state_highlights_append(src_line, -1, -1,
                                            HIGHLIGHT_REPLAY_PC);

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
    ReplReplayRuntimeState replay = replay_state_view();
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
 * no-op. (Relocated from editor_input.c in Phase J1 commit 48a.) */
static int g_audio_gesture_sent = 0;

static void glr_ctrl_notify_audio_gesture_once(void) {
    if (g_audio_gesture_sent) return;
    g_audio_gesture_sent = 1;
    glr_audio_on_user_gesture();
}

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
void glr_app_reset_transients(void) {
    editor_commit_reset_transients();
    glr_camera_controls_reset();
    glr_camera_clear_scene_default();
    ui_menu_bar_close();
    color_picker_stop();
    glr_ctrl_router_reset_code_panel_drag();
}

static void glr_ctrl_apply_input_effects(EditorInputDispatchEffects effects) {
    if (effects.set_cursor)
        glutSetCursor(effects.cursor);
    if (effects.request_redraw)
        glutPostRedisplay();
    if (effects.schedule_timer)
        glutTimerFunc(effects.timer_millis, glr_ctrl_timer, effects.timer_value);
    if (effects.restore_hidden_code_panel)
        glr_ctrl_restore_hidden_code_panel();
}

/* ========================================================================= */
/* Scene config builder (push model)                                          */
/* ========================================================================= */

/* Scene's main-fill geometry callback. The signature is intentionally
 * opaque to scene — the controller pulls live program / count / text
 * from REPL state here, and clamps the count to the pre-fade base limit
 * when replay-fade overlays are active so the fade pass can layer on
 * top of an unmodified prefix. */
static void scene_execute_adapter(const SceneExecuteContext *ctx,
                                  void *user_data) {
    (void)ctx;
    (void)user_data;

    int count = repl_state_flat_program_count();
    if (g_replay_fade_plan_active)
        count = g_replay_fade_plan_base_limit;

    repl_execute_set_fade_context(1.0f, 0);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    repl_execute_program(&(ReplExecutionOptions){
        .flat_cmd_count = count,
        .program        = repl_state_flat_program_view(),
        .text           = source_document_view(),
    });
    glPopAttrib();
}

/* Grid/axes in-out fade machines (plans/.../grid-axes-transitions.md).
 * The config path is untouched: toggling still just flips
 * presentation.grid_theme/axes_theme. Each frame the diff feeds these
 * machines and the renderer draws the effective {theme, opacity}. */
static SceneXnState g_grid_xn;
static SceneXnState g_axes_xn;

/* Runtime GL_NV_fog_distance capability, probed once in glr_ctrl_init_gl
 * (the GL context is current there) and mirrored into each frame's
 * SceneRenderConfig. Lets the city backdrop and ocean/radar grid themes
 * opt into radial fog. 0 until detection runs — the safe default. */
static int g_nv_fog_distance_supported = 0;

static float g_projection_mix = 1.0f; /* 0 = ortho, 1 = perspective */
typedef enum {
    GLR_VIEW_XN_IDLE = 0,
    GLR_VIEW_XN_CAMERA_TO_2D,
    GLR_VIEW_XN_PROJECTION_TO_2D,
    GLR_VIEW_XN_PROJECTION_TO_3D,
    GLR_VIEW_XN_CAMERA_TO_3D
} GlrViewTransitionPhase;

static int g_view_mode_target_ortho = 0;
static GlrViewTransitionPhase g_view_xn_phase = GLR_VIEW_XN_IDLE;
static GlrCameraState g_saved_3d_camera;
static int g_saved_3d_camera_valid = 0;

static float smoothstep01(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static int glr_ctrl_view_controls_are_2d(void) {
    return g_view_mode_target_ortho ||
           g_view_xn_phase == GLR_VIEW_XN_PROJECTION_TO_3D;
}

static void glr_ctrl_sync_camera_control_mode(void) {
    glr_camera_set_control_mode(glr_ctrl_view_controls_are_2d()
                                ? GLR_CAMERA_CONTROL_2D
                                : GLR_CAMERA_CONTROL_3D);
}

static void glr_ctrl_start_camera_to_2d(void) {
    GlrCameraState cam = glr_camera();
    if (!g_saved_3d_camera_valid || g_view_xn_phase == GLR_VIEW_XN_IDLE) {
        g_saved_3d_camera = cam;
        g_saved_3d_camera_valid = 1;
    }
    glr_camera_ease_to(0.0f, 0.0f, cam.dist, cam.tx, cam.ty, 0.0f);
    /* Use a faster decay for this leg only — the orbit flattening
     * shouldn't drag the projection blend that follows it. The override
     * is reset by the next glr_camera_ease_to call (drag, scene load,
     * etc.), so non-view-mode eases keep the global default. */
    glr_camera_set_target_decay(GLR_VIEW_CAMERA_TO_2D_DECAY);
    g_view_xn_phase = GLR_VIEW_XN_CAMERA_TO_2D;
}

static void glr_ctrl_start_camera_to_3d(void) {
    GlrCameraState cam;
    GlrCameraState target;

    if (!g_saved_3d_camera_valid) {
        g_view_xn_phase = GLR_VIEW_XN_IDLE;
        return;
    }

    cam = glr_camera();
    target = g_saved_3d_camera;
    target.tx = cam.tx;
    target.ty = cam.ty;
    target.dist = cam.dist;
    glr_camera_ease_to(target.rx, target.ry, target.dist,
                       target.tx, target.ty, target.tz);
    g_view_xn_phase = GLR_VIEW_XN_CAMERA_TO_3D;
}

static void glr_ctrl_handle_view_mode_target_change(void) {
    int ortho = glr_state_presentation().ortho_mode ? 1 : 0;

    if (ortho == g_view_mode_target_ortho)
        return;

    g_view_mode_target_ortho = ortho;
    if (ortho)
        glr_ctrl_start_camera_to_2d();
    else
        g_view_xn_phase = GLR_VIEW_XN_PROJECTION_TO_3D;
    glr_ctrl_sync_camera_control_mode();
}

void glr_ctrl_view_record_external_3d_pose(float rx, float ry, float tz) {
    if (!g_view_mode_target_ortho)
        return;
    /* Seed the snapshot when we've never been in 3D this session
     * (e.g. workspace loaded with ortho_mode=1): without this, the
     * 2D->3D restoration early-returns and the camera is left wherever
     * the previous example put it. */
    if (!g_saved_3d_camera_valid) {
        g_saved_3d_camera = glr_camera();
        g_saved_3d_camera_valid = 1;
    }
    g_saved_3d_camera.rx = rx;
    g_saved_3d_camera.ry = ry;
    g_saved_3d_camera.tz = tz;
}

static int glr_ctrl_step_projection_toward(float target, float dt) {
    float step;

    if (GLR_VIEW_PROJECTION_TRANSITION_SECS <= 0.0f) {
        g_projection_mix = target;
        return 1;
    }

    step = dt / GLR_VIEW_PROJECTION_TRANSITION_SECS;
    if (g_projection_mix < target) {
        g_projection_mix += step;
        if (g_projection_mix >= target) {
            g_projection_mix = target;
            return 1;
        }
        return 0;
    }
    if (g_projection_mix > target) {
        g_projection_mix -= step;
        if (g_projection_mix <= target) {
            g_projection_mix = target;
            return 1;
        }
        return 0;
    }
    return 1;
}

static void glr_ctrl_tick_view_transition(float dt) {
    int guard;

    glr_ctrl_handle_view_mode_target_change();

    for (guard = 0; guard < 2; guard++) {
        int phase_changed_without_work = 0;

        switch (g_view_xn_phase) {
        case GLR_VIEW_XN_CAMERA_TO_2D:
            if (!glr_camera_target_active()) {
                g_view_xn_phase = GLR_VIEW_XN_PROJECTION_TO_2D;
                phase_changed_without_work = 1;
            }
            break;
        case GLR_VIEW_XN_PROJECTION_TO_2D:
            if (glr_ctrl_step_projection_toward(0.0f, dt))
                g_view_xn_phase = GLR_VIEW_XN_IDLE;
            break;
        case GLR_VIEW_XN_PROJECTION_TO_3D:
            if (glr_ctrl_step_projection_toward(1.0f, dt))
                glr_ctrl_start_camera_to_3d();
            break;
        case GLR_VIEW_XN_CAMERA_TO_3D:
            if (!glr_camera_target_active())
                g_view_xn_phase = GLR_VIEW_XN_IDLE;
            break;
        case GLR_VIEW_XN_IDLE:
        default:
            break;
        }

        if (!phase_changed_without_work)
            break;
    }

    glr_ctrl_sync_camera_control_mode();
}

static void glr_ctrl_build_scene_config(SceneRenderConfig *config) {
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
     * hook can read guide_snapshot for the cursor edit guides. */
    config->post_overlays_fn        = glr_ctrl_post_overlays;
    config->post_overlays_user_data = config;

    /* --- Background clear color ---
     * Resolve from the user's last CMD_CLEAR_COLOR (or the editor
     * default if none). Scene takes the pre-resolved float[4] and
     * doesn't touch the flat program. */
    {
        float cr = 0.10f, cg = 0.10f, cb = 0.10f, ca = 1.0f;
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
    config->viewport_w = ui_state_viewport().window_w;
    config->viewport_h = ui_state_viewport().window_h;
    ui_layout_scene_rect(&config->scene_x, &config->scene_y,
                           &config->scene_w, &config->scene_h);
    if (config->scene_w < 1) config->scene_w = 1;
    if (config->scene_h < 1) config->scene_h = 1;

    /* --- Camera state --- (modelview transform is applied separately by
     * the controller via scene_apply_camera; these fields are still needed
     * for grid/axes orientation and the orbit-target gizmo). */
    config->cam_dist = cam.dist;
    config->cam_rx = cam.rx;
    config->cam_ry = cam.ry;
    config->cam_tx = cam.tx;
    config->cam_ty = cam.ty;
    config->cam_tz = cam.tz;
    config->cam_motion_glow = cam.motion_glow;
    config->projection_mix = smoothstep01(g_projection_mix);

    /* --- Rendering quality --- */
    config->multisample_enabled = render.multisample_enabled;
    config->line_smooth_enabled = render.line_smooth_enabled;
    config->use_accum = render.use_accum;
    config->accum_aa_enabled = render.accum_aa_enabled;
    config->accum_samples = render.accum_samples;

    /* --- Lighting --- */
    config->user_lighting_enabled = repl_state_flat_program_user_lighting_enabled();
    memcpy(config->lights, repl_render.lights, sizeof(config->lights));
    config->show_light_indicators = presentation.show_light_indicators;

    /* --- Environment --- */
    config->backdrop_mode = presentation.backdrop_mode;
    config->post_filter_mode = presentation.post_filter_mode;
    config->wireframe = presentation.wireframe;
    /* Single source of truth: the executor flag set in glr_ctrl_init_gl.
     * Lets the star backdrop's direct glPointParameterfv call be gated
     * the same way the executor's is. */
    config->point_parameter_supported = repl_executor_point_parameter_supported();
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
    g_replay_tess_preview_active = replaying &&
                                   replay_mode() == REPLAY_MODE_VERTEX;

    /* --- Focus marker --- */
    config->focus = glr_ctrl_build_focus_vertex();

    /* Boost overlay alpha on dark backgrounds: bg_lum is the Rec. 709
     * relative luminance of the clear color (0.2126/0.7152/0.0722 R/G/B
     * weights); the reciprocal raises alpha as the background darkens,
     * with +0.02 as a black-background guard and the result clamped to
     * 1..3 below. */
    bg_lum = 0.2126f * repl_render.clear_color[0]
        + 0.7152f * repl_render.clear_color[1]
        + 0.0722f * repl_render.clear_color[2];
    as_val = (0.10f + 0.02f) / fmaxf(bg_lum + 0.02f, 1e-4f);
    config->alpha_scale = as_val < 1.0f ? 1.0f : (as_val > 3.0f ? 3.0f : as_val);

    /* --- Replay overlays ---
     * Build the controller-private fade plan from REPL replay state, and
     * if there's anything to overlay on the main fill (fading batches or
     * the polygon-mode tess-preview wireframe) install our post_fill_fn
     * so the scene calls back between the user-geometry fill and the
     * grid/axes/backdrop helpers. */
    glr_ctrl_build_replay_fade_plan(replaying);
    if (g_replay_fade_plan_active || g_replay_tess_preview_active) {
        config->post_fill_fn        = glr_ctrl_post_fill_replay_overlay;
        config->post_fill_user_data = NULL;
    }
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

    snap->variable_panel_vars.vars = snap->variable_panel_var_storage;
    snap->variable_panel_vars.count = count;
    for (int i = 0; i < count; i++) {
        snprintf(snap->variable_panel_var_storage[i].name,
                 sizeof(snap->variable_panel_var_storage[i].name),
                 "%s", predef.vars[i].name);
        snap->variable_panel_var_storage[i].value = &predef.vars[i].value;
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
    char parse_err[128] = "";
    ReplParsedLine pl;

    snap->numeric_swatch.visible = 0;

    if (editor_insert_mode()) return;
    edit_line = editor_state_edit_line();
    if (edit_line < 0 || edit_line >= repl_state_document_count()) return;
    if (repl_state_document_cmds()[edit_line].type == CMD_COMMENT) return;
    if (ui_repl_code_panel_input_row_has_color_swatch(snap)) return;
    in = editor_state_input();
    if (in.cursor_pos < 0 || !in.input || !in.input[0]) return;
    if (editor_state_autocomplete().match_count > 0) return;
    if (editor_inline_rename_active()) return;
    if (tutorial_active() && tutorial_block_noncommand_commit()) return;

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
    snap->status         = ui_state_status();
    snap->search         = *editor_state_search();
    snap->autocomplete   = editor_state_autocomplete();
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
    ReplReplayRuntimeState frame_replay = replay_state_view();
    SceneRenderConfig scene_config;
    UiRenderSnapshot ui_snap;

    prof_frame_tick();
    prof_begin(PROF_FRAME_TOTAL);

    if (repl_state_normals_dirty()) {
        prof_begin(PROF_AUTONORMAL);
        /* Caller-owned cursor: read edit-line into a local int, pass
         * &local so the pipeline never reaches into editor_state_*.
         * (Convention introduced in Phase 3.6.0 of
         * edit-line-ownership.md.) */
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
     * directly into editor_state_virtual_lines. (Indirection added in
     * Phase 4 of feature/source-document-port.md.) */
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
    if (replay_active())
        repl_state_flat_program_set_count(replay_prepare_frame(saved_flat_count));

    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();
    prof_end(PROF_SNAPSHOT_PREP);

    prof_begin(PROF_SNAPSHOT_SCENE_CONFIG);
    glr_ctrl_build_scene_config(&scene_config);
    prof_end(PROF_SNAPSHOT_SCENE_CONFIG);

    prof_begin(PROF_SNAPSHOT_UI);
    /* Build, let follow-scroll adjust editor scroll, then rebuild: the
     * second build is intentional so the published snapshot reflects the
     * post-follow-scroll offset (not a copy-paste). */
    glr_ctrl_build_ui_snapshot(&ui_snap);
    glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(&ui_snap, NULL, NULL);
    glr_ctrl_build_ui_snapshot(&ui_snap);
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
        scene_apply_camera(cam.rx, cam.ry, cam.dist, cam.tx, cam.ty, cam.tz);
    }
    if (scene_render_3d_scene(&scene_config) != 0) {
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
        ReplReplayRuntimeState saved_snap_replay = ui_snap.replay;
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
    ui_variable_panel_render(&ui_snap);
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
    ui_profile_panel_render(&ui_snap);
    prof_end(PROF_PROFILE_PANEL);

    prof_begin(PROF_FRAME_RESTORE);
    repl_state_flat_program_set_count(saved_flat_count);
    repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(live_scratch_arrays);
    prof_end(PROF_FRAME_RESTORE);

    prof_end(PROF_FRAME_TOTAL);
}

void glr_ctrl_reshape(int w, int h) {
    if (h < 1) h = 1;
    ui_state_viewport_set_size(w, h);
}

/* App-service installers that must be present for any code path that
 * loads/exports REPL state — including the dump-only CLI paths
 * (--dump-code / --dump-flat) that bypass glr_ctrl_init_gl. Idempotent;
 * called from glr_app_reset_all and from glr_ctrl_bootstrap_repl.
 * (Bootstrapping the dump CLI paths through this installer was the
 * Step 4 [P2] fix in feature/decouple-repl-from-gl-repl-alt.md;
 * previously these installs lived only in glr_app_reset_all, so the
 * dump CLI ran without them and dropped @cfg from imported files.) */
/* Bundles the example-defaults reset for presentation chrome plus the
 * camera auto-rotate toggle. The cfg bridge handles per-scene cfg in
 * the scene_cfg snapshot, but the example loader's pre-cfg baseline
 * reset still wants both flipped to defaults. This goes through a sink
 * because the example loader is a REPL pipeline TU. (Sink wired as
 * step 7a of feature/decouple-repl-from-gl-repl-alt.md.)
 *
 * `tag_mask` carries REPL_EXAMPLE_TAG_* bits for the example being
 * loaded. After the global reset, the controller applies typed
 * tag-default overrides via glr_config_set() — no parser syntax, no
 * slug strings, no magic enum values. The example's own `@cfg` is
 * parsed later in load_example_lines and wins because both paths
 * mutate the same backing field. */
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

static void glr_app_reset_example_chrome(unsigned int tag_mask) {
    glr_state_presentation_reset_example_defaults();
    glr_camera_mut()->auto_rotate = CFG_DEFAULT_CAMERA_ROTATE;
    variable_panel_state_mut()->visible = CFG_DEFAULT_VARIABLE_PANEL;

    glr_ctrl_apply_tag_defaults(
        tag_mask, k_example_tag_defaults,
        (int)(sizeof(k_example_tag_defaults) /
              sizeof(k_example_tag_defaults[0])));
}

/* Adapter for repl_executor_install_camera_distance_source. The
 * source is unconditionally installed; only the point-size fallback
 * (taken when the runtime GL lacks glPointParameterfv) consumes it,
 * so this is a small zero-cost shim when point parameters are
 * supported. */
static float glr_app_camera_distance(void) {
    return glr_camera().dist;
}

/* Adapter for the export reshape-projection bridge. Translates the
 * scene's currently-applied projection (cached by scene_apply_projection)
 * into the C lines the exported reshape() / live code panel emit between
 * glLoadIdentity() and glMatrixMode(GL_MODELVIEW). Ortho uses the
 * aspect-independent half-height and recomputes the aspect at runtime
 * from w/h so the exported program stays resolution-independent. */
static void glr_app_export_reshape_projection(ReplExportProjectionBlock *blk) {
    SceneProjectionDesc p;

    scene_get_active_projection(&p);
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
    glr_app_export_reshape_projection
};

/* Editor-input cleanup that the REPL loaders used to do inline now
 * routes through callback sinks. The two helpers below are the
 * full-app implementations the controller installs at startup; the
 * demo leaves both unset. (Indirection added in Phase 3 of
 * feature/source-document-port.md.) */
static void glr_app_editor_input_reset(void) {
    editor_insert_mode_set(0);
    EditorInputState *inp = editor_state_input_mut();
    inp->input[0] = '\0';
    inp->input_len = 0;
    editor_cursor_pos_set(0);
    inp->pending_newline[0] = '\0';
    inp->pending_newline_len = 0;
}

static void glr_app_editor_insert_mode_off(void) {
    editor_insert_mode_set(0);
}

/* Route the residual editor scroll/follow writes through host-effect
 * sinks. (Indirection added in Phase 6 of
 * feature/source-document-port.md.) */
static void glr_app_scroll_to_line(int target) {
    editor_scroll_set(target);
    editor_scroll_follow_cursor_set(0);
}

static void glr_app_follow_cursor(int follow) {
    editor_scroll_follow_cursor_set(follow);
}

const UiOverlayContent *glr_ctrl_help_overlay_content(void) {
    enum { GLR_HELP_OVERLAY_MAX_TABS = 4 };
    static UiOverlayTab tabs[GLR_HELP_OVERLAY_MAX_TABS];
    static UiOverlayContent content;

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

static void glr_host_editor_cursor_park(int line, int insert_mode) {
    editor_state_edit_line_set(line);
    editor_insert_mode_set(insert_mode);
}

static void glr_host_completion_clear(void) {
    editor_completion_clear();
}

static void glr_host_completion_update(void) {
    editor_completion_update();
}

static const char *glr_host_editor_input_get(void) {
    return editor_state_input().input;
}

/* The host-effect bridge the controller installs into the REPL
 * pipeline. Status routes pipeline diagnostics to UiState; the rest
 * actualize loader / scene-switch / replay effects on the editor and
 * peer subsystems.
 * This file is where editor coupling concentrates, so the editor-
 * neutral names in core.h resolve to glr_app_* here. The demo installs
 * its own edit-line-only bridge, so these app/editor/tutorial effects
 * stay out of its link set. */
static const ReplHostEffects g_glr_host_effects = {
    .status                     = ui_state_status_set,
    .status_error               = ui_state_status_set_error,
    .example_presentation_reset = glr_app_reset_example_chrome,
    .input_reset                = glr_app_editor_input_reset,
    .insert_mode_off            = glr_app_editor_insert_mode_off,
    .scroll_to_line             = glr_app_scroll_to_line,
    .follow_cursor              = glr_app_follow_cursor,
    .tutorial_teardown          = tutorial_teardown,
    .edit_line_get              = editor_state_edit_line,
    .edit_line_set              = editor_state_edit_line_set,
    .host_cursor_park           = glr_host_editor_cursor_park,
    .completion_clear           = glr_host_completion_clear,
    .completion_update          = glr_host_completion_update,
    .host_input_get             = glr_host_editor_input_get,
};

/* Seed both fade machines to the current presentation theme at full
 * opacity, STEADY — a zero-init machine would diff OFF -> the non-off
 * default grid and animate it in on frame 1, and animate stale prior
 * themes after a world reset. Folded into glr_app_install_app_services
 * (idempotent, called from both program init and glr_app_reset_all)
 * so it runs AFTER glr_state_presentation_reset_defaults().
 * (Codified as Rule 8 of plans/.../grid-axes-transitions.md.) */
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
#define GLR_FRAME_DT_SECS     0.016f
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

static void glr_app_install_app_services(void) {
    /* Install the host-effect bridge (status sink, example-
     * presentation-reset, editor effects, and tutorial teardown).
     * Single consolidated call in place of the previous individual
     * repl_install_*_sink calls. (Consolidation per item 2 of
     * plans/partial/src-repl-simplicity-review.md.) */
    repl_install_host_effects(&g_glr_host_effects);
    /* Install the export-config bridge so src/repl/export.c can emit/parse
     * @cfg headers and src/repl/scenes.c can snapshot per-scene cfg
     * without referencing glr_config_* directly. The demo does not
     * install a bridge, so the @cfg path is a no-op there (clearing
     * g_cfg_items / CFG_ITEM_COUNT / audio_* / ui_state_profile_panel_mut
     * / variable_panel_state_mut stubs). */
    glr_actions_install_export_cfg_bridge();
    /* Install the export-camera bridge so src/repl/export.c can emit
     * and parse the `// camera` block + g_angle preamble without
     * referencing glr_camera_*. Camera state still pulls glr_camera.c
     * into the demo link set via src/repl/state.c (auto_rotate reset)
     * and src/repl/example_loader.c (example camera presets). (Bridge
     * was step 4a of feature/decouple-repl-from-gl-repl-alt.md;
     * step 7 closes the remaining doors.) */
    glr_camera_export_install_bridge();
    /* Install the executor's camera-distance source. The point-size
     * fallback (taken at runtime when the GL context lacks
     * glPointParameterfv) needs the current camera distance to scale
     * glPointSize calls; routing it through a controller-installed
     * callback keeps glr_camera.c out of the REPL pipeline. The demo
     * doesn't install — the fallback then passes glPointSize through
     * unscaled. (Step 7e of feature/decouple-repl-from-gl-repl-alt.md.) */
    repl_executor_install_camera_distance_source(glr_app_camera_distance);
    /* Reshape-projection bridge: lets the GL-free exporter / code panel
     * emit a reshape() that matches the scene's live projection
     * (perspective in 3D, ortho in 2D). Demo/tests don't install, so the
     * canonical perspective default is used there. */
    repl_export_install_projection_bridge(&g_export_projection_bridge_impl);
    /* Seed the grid/axes fade machines (see glr_ctrl_seed_overlay_xn
     * comment for the steady-at-current-theme invariant). In
     * glr_app_reset_all this runs after the presentation reset (line
     * ordering above); at bootstrap it reads the static CFG_DEFAULT_*
     * presentation. Idempotent. */
    glr_ctrl_seed_overlay_xn();
}

/* Full-world reset entry point. Lives on the controller side so the
 * REPL pipeline (and tools/repl_demo) does not have to link / stub the
 * peer / editor / UI reset symbols. (Split out of src/repl/state.c as
 * step 2 of feature/decouple-repl-from-gl-repl-alt.md.) */
void glr_app_reset_all(void) {
    editor_undo_note_wholesale_replacement();
    repl_state_reset_program();
    /* GlrState owns presentation + render-config toggles. Reset them
     * alongside the REPL halves so callers see a coherent post-reset
     * world. The camera reset moved off the REPL-side presentation
     * reset (was wiring `glr_camera_mut()->auto_rotate
     * = CFG_DEFAULT_CAMERA_ROTATE`); the camera resets itself here.
     * (Ownership relocated as step 7a of
     * feature/decouple-repl-from-gl-repl-alt.md.) */
    glr_state_presentation_reset_defaults();
    glr_state_render_reset_defaults();
    glr_camera_reset_default();
    g_projection_mix = 1.0f;
    g_view_mode_target_ortho = 0;
    g_view_xn_phase = GLR_VIEW_XN_IDLE;
    g_saved_3d_camera_valid = 0;
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
    glr_app_reset_transients();
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
    glr_app_install_app_services();
    /* Refresh derived export/camera text caches AFTER peer resets
     * so the cached strings reflect post-reset state, not whatever
     * was on the peers before this call. These read app-side cfg /
     * camera state, so they cannot live on the REPL-side reset
     * (would pull glr_config / glr_camera into the demo link set
     * AND would pre-fire before peers were reset).
     * The frame loop refreshes them every frame in build_ui_snapshot
     * + display, so tests that go through glr_app_reset_all see
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
    glr_app_reset_all();
    repl_ensure_init_bootstrap_ready();
    scene_render_init_gl();
    repl_executor_init_resources();

    /* Runtime point-parameter capability (replaces the old
     * compile-time NO_POINT_PARAMETER macro). glPointParameterfv is
     * core GL 1.4 but absent on some legacy contexts — a runtime
     * property, not a build one. The GL context is already current
     * here (glr_ctrl_init_gl runs post-glutInit/window), so query it
     * now. This MUST run before repl_apply_init_bootstrap() below: on
     * unsupported hardware the point-attenuation bootstrap entry has
     * to be skipped entirely rather than invoking the missing entry
     * point. Check the GL version first — an ARB/EXT-only test
     * false-negatives on a 1.4+ core context that doesn't advertise
     * the extension string. GLR_NO_POINT_PARAMETER (any non-empty
     * value) forces the unsupported path for testing on capable HW. */
    int gl_major = 0, gl_minor = 0;
    const char *gl_ver = (const char *)glGetString(GL_VERSION);
    if (gl_ver) sscanf(gl_ver, "%d.%d", &gl_major, &gl_minor);
    int hw_point_param = (gl_major > 1) || (gl_major == 1 && gl_minor >= 4)
        || glutExtensionSupported("GL_ARB_point_parameters")
        || glutExtensionSupported("GL_EXT_point_parameters");
    const char *no_pp = getenv("GLR_NO_POINT_PARAMETER");
    int forced_off = (no_pp && no_pp[0]) ? 1 : 0;
    int point_param_ok = hw_point_param && !forced_off;
    /* Tell the user on the terminal when point attenuation is off, and
     * which of the two reasons applies — a deliberate env override vs.
     * a GL context that genuinely lacks the entry point. */
    if (!point_param_ok) {
        if (forced_off)
            fprintf(stderr,
                "[gl-repl] glPointParameterfv disabled via "
                "GLR_NO_POINT_PARAMETER=%s; using the glPointSize "
                "distance approximation%s.\n",
                no_pp,
                hw_point_param ? " (this GL context does support it)"
                               : " (this GL context does not support it either)");
        else
            fprintf(stderr,
                "[gl-repl] glPointParameterfv unsupported by this GL "
                "context (GL_VERSION \"%s\", no GL_ARB/EXT_point_parameters); "
                "using the glPointSize distance approximation. Set "
                "GLR_NO_POINT_PARAMETER=1 to force this path on capable "
                "hardware.\n",
                gl_ver ? gl_ver : "unknown");
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

    /* Query actual MSAA sample count from OpenGL */
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    glr_state_render_mut()->msaa_samples = (int)samples;

    if (samples > 1) {
        static char msaa_label[32];
        snprintf(msaa_label, sizeof(msaa_label), "MSAAx%d", (int)samples);
        for (int i = 0; i < CFG_ITEM_COUNT; i++) {
            if (g_cfg_items[i].key == GLR_CONFIG_MSAA) {
                g_cfg_items[i].label = msaa_label;
                break;
            }
        }
    } else {
        for (int i = 0; i < CFG_ITEM_COUNT; i++) {
            if (g_cfg_items[i].key == GLR_CONFIG_MSAA) {
                g_cfg_items[i].label = "MSAA";
                break;
            }
        }
    }

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
     * glr_app_install_app_services is idempotent so the windowed path
     * (which already installed via glr_app_reset_all) is unaffected.
     * (Originally the Step 4 [P2] fix in
     * feature/decouple-repl-from-gl-repl-alt.md.) */
    glr_app_install_app_services();
    repl_eval_init_predef_vars();
    for (int i = 0; i < g_num_predef_vars; i++) {
        if (strcmp(g_predef_vars[i].name, "t") == 0) {
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

/* ---- Keyboard router helpers ------------------------------------------ */

int glr_ctrl_router_handle_save_key(unsigned char key) {
    if (key == KEY_CTRL_S) {
        /* Ctrl+S == File > Save Scene: active named scene ->
         * <workspace>/<slug>.c; example/transient -> ./output.c. */
        ReplExportLayout layout;
        glr_ctrl_fill_export_layout(&layout);
        repl_save_active_scene(&layout);
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_debug_dump_key(unsigned char key) {
    if (key == KEY_CTRL_P) {
        glr_debug_dump_editor(stdout, source_document_view());
        glr_debug_dump_flat_commands(stdout, editor_buffer_view());
        repl_set_status("Dumped editor + flat commands to stdout");
        return 1;
    }
    return 0;
}

/* Quit safeguard: Ctrl+Q and SIGINT (Ctrl+C) write a recovery copy to
 * a DISTINCT, findable file — never the active scene/workspace. The
 * point is to rescue an unintended exit / forgotten save without
 * silently clobbering the user's real scene; reload it with
 * `./gl-repl quit-recovery.c`. (Not /tmp — the user would never find
 * it; not the scene file — that would defeat the safeguard.) The
 * filename lives in config.h as QUIT_RECOVERY_FILE. */

static volatile sig_atomic_t g_quit_requested = 0;

/* Async-signal-safe: only sets a sig_atomic_t flag. The actual save +
 * exit runs on the normal path in glr_ctrl_tick(). */
void glr_ctrl_request_quit(void) {
    g_quit_requested = 1;
}

static void glr_ctrl_save_quit_recovery(void) {
    ReplExportLayout layout;
    glr_ctrl_fill_export_layout(&layout);
    if (repl_export_save_output(QUIT_RECOVERY_FILE, source_document_view(),
                                &layout)) {
        printf("Saved recovery copy to %s (reload: ./%s %s)\n",
               QUIT_RECOVERY_FILE, glr_ctrl_program_name(),
               QUIT_RECOVERY_FILE);
    }
}

int glr_ctrl_router_handle_quit_key(unsigned char key) {
    if (key == KEY_CTRL_Q) {
        glr_ctrl_save_quit_recovery();
        exit(0);
    }
    return 0;
}

/* SET-step ack: while a tutorial SET (showcase) step is waiting, Enter /
 * Tab / Space advances. Scoped strictly to SET steps inside
 * tutorial_handle_ack_key so REQUIRE / COMMAND steps never have their
 * keys swallowed here. Runs AFTER the existing controller routes (so
 * Ctrl-keys, replay, cfg shortcuts, save, quit keep priority) and
 * BEFORE editor_handle_key — see the chain in glr_ctrl_keyboard. */
int glr_ctrl_router_handle_tutorial_ack_key(unsigned char key) {
    return tutorial_handle_ack_key(key);
}

int glr_ctrl_router_handle_config_menu_key(unsigned char key) {
    if (!editor_state_search()->active && key == '`') {
        if (replay_active())
            replay_stop();
        glr_ctrl_restore_hidden_code_panel();
        ui_menu_bar_open_config(repl_state_variables().anim_time);
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_active_replay_key(unsigned char key) {
    return replay_active() && replay_handle_key(key);
}

int glr_ctrl_router_handle_replay_toggle_key(unsigned char key) {
    return replay_handle_key(key);
}

int glr_ctrl_router_handle_cfg_shortcut_key(unsigned char key) {
    return glr_cfg_handle_ascii_shortcut(key);
}

/* Ctrl+= / Ctrl+- drive the *same* Off->2x->4x->8x->16x cycle the
 * Config "Accum AA" menu row uses (clamped, no wrap), so the keyboard
 * and the menu are one coherent model — no latent sample count behind
 * an "Off" state. Still gated on use_accum: with --noaccum there is no
 * accumulation buffer to enable. */
int glr_ctrl_router_handle_accum_samples_key(unsigned char key) {
    GlrRenderState *rs = glr_state_render_mut();
    int is_up = (key == '=' || key == '+');
    int is_down = (key == KEY_CTRL_DASH ||
                   (key == '-' &&
                    (editor_input_active_modifiers() & GLUT_ACTIVE_CTRL)));

    if (is_up && !(editor_input_active_modifiers() & GLUT_ACTIVE_CTRL))
        return 0;
    if (!is_up && !is_down)
        return 0;

    if (rs->use_accum) {
        int n   = glr_config_state_count(GLR_CONFIG_ACCUM_AA);
        int idx = glr_config_get(GLR_CONFIG_ACCUM_AA);
        int next = idx + (is_up ? 1 : -1);
        if (next < 0)     next = 0;
        if (next > n - 1) next = n - 1;
        if (next != idx)
            glr_config_set(GLR_CONFIG_ACCUM_AA, next);
        char msg[64];
        snprintf(msg, sizeof(msg), "Accum AA: %s",
                 glr_config_state_name(GLR_CONFIG_ACCUM_AA, next));
        repl_set_status(msg);
    }
    return 1;
}

/* Ctrl+N: cycle the experimental scene post-processing filter. Hidden
 * shortcut only — no Config row, no @cfg. Session-level state on
 * GlrPresentationState; flows into SceneRenderConfig each frame. */
int glr_ctrl_router_handle_post_filter_key(unsigned char key) {
    if (key != KEY_CTRL_N)
        return 0;

    GlrPresentationState *p = glr_state_presentation_mut();
    p->post_filter_mode =
        (p->post_filter_mode + 1) % SCENE_POST_FILTER_COUNT;

    char msg[64];
    snprintf(msg, sizeof(msg), "Post filter: %s",
             scene_postprocess_filter_mode_name(p->post_filter_mode));
    repl_set_status(msg);
    return 1;
}

/* Shared by the Ctrl+Shift+F shortcut and the status-bar keycap click.
 * Toggling collapses the chrome header rows from ~20 to 0 (and back);
 * the follow-scroll request keeps the active edit row on screen, and
 * the sync mirrors the new flag into the code-panel UI snapshot. */
void glr_ctrl_toggle_code_focus(void) {
    GlrPresentationState *p = glr_state_presentation_mut();
    p->code_focus = !p->code_focus;
    glr_ctrl_sync_ui_chrome();
    editor_scroll_follow_cursor_set(1);
    repl_set_status(p->code_focus ? "Code focus: ON" : "Code focus: OFF");
}

/* Ctrl+Shift+F: toggle the code-panel focus view. Ctrl+F (no Shift)
 * stays the search shortcut handled downstream in editor_handle_key;
 * the Shift modifier disambiguates, and this router runs ahead of the
 * editor so search never sees the shifted form. Hidden shortcut only —
 * no Config row, no @cfg (mirrors the Ctrl+N post-filter pattern). */
int glr_ctrl_router_handle_code_focus_key(unsigned char key) {
    if (key != KEY_CTRL_F)
        return 0;
    if (!(editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT))
        return 0;
    glr_ctrl_toggle_code_focus();
    return 1;
}

/* The Ctrl+Shift camera shortcuts (Ctrl+Shift+C reset / +O focus-origin
 * / +V view mode) have no dedicated router: they are ordinary Config
 * rows carrying a GLUT_ACTIVE_SHIFT modifier, dispatched by the
 * two-pass glr_cfg_handle_ascii_shortcut (see glr_actions.c). */

/* ---- Special-key router helpers --------------------------------------- */

int glr_ctrl_router_handle_replay_special(int key) {
    return replay_handle_special(key);
}

int glr_ctrl_router_handle_cfg_special_shortcut(int key) {
    return glr_cfg_handle_special_shortcut(key);
}

int glr_ctrl_router_handle_horizontal_audio_special(int key) {
    if (key != GLUT_KEY_LEFT && key != GLUT_KEY_RIGHT)
        return 0;
    if (!(editor_input_active_modifiers() & GLUT_ACTIVE_CTRL))
        return 0;
    if (key == GLUT_KEY_LEFT)
        glr_audio_prev_track();
    else
        glr_audio_next_track();
    return 1;
}

/* Snapshot of the live help overlay for pure geometry queries
 * (scroll clamp, click hit-test). Mirrors the per-frame state the
 * renderer is handed in glr_ctrl_display_frame. */
static UiOverlayState glr_ctrl_help_overlay_state(void) {
    UiViewportState vp = ui_state_viewport();
    EditorHelpSession s = editor_help_session_view();
    UiOverlayState st = {
        .visible    = ui_state_help().visible,
        .tab_idx    = s.tab_idx,
        .scroll     = s.scroll,
        .viewport_w = vp.window_w,
        .viewport_h = vp.window_h,
        .content    = glr_ctrl_help_overlay_content(),
    };
    return st;
}

/* Scroll the help session, then clamp to the active tab's real
 * bounds so the offset can't run past the end (which would otherwise
 * have to be "unwound" before an upward scroll did anything). */
void glr_ctrl_help_scroll_by(int delta) {
    editor_help_session_scroll_by(delta);
    UiOverlayState st = glr_ctrl_help_overlay_state();
    int max_scroll = ui_tabbed_overlay_max_scroll(&st);
    int scroll = editor_help_session_scroll();
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;
    editor_help_session_set_scroll(scroll);
}

/* Left-click routing while the modal help overlay is up: tab bar
 * selects a tab, anywhere outside the panel dismisses, body clicks
 * are swallowed (the overlay is modal). */
int glr_ctrl_router_handle_help_click(int button, int state, int x, int y) {
    if (button != GLUT_LEFT_BUTTON || state != GLUT_DOWN)
        return 0;
    if (!ui_state_help().visible)
        return 0;
    UiOverlayState st = glr_ctrl_help_overlay_state();
    UiOverlayHit hit = ui_tabbed_overlay_hit_test(&st, x, y);
    if (hit.kind == UI_OVERLAY_HIT_OUTSIDE) {
        glr_ctrl_toggle_help();          /* click-away dismiss */
        editor_request_redraw();
        return 1;
    }
    if (hit.kind == UI_OVERLAY_HIT_TAB) {
        editor_help_session_set_tab(hit.tab);
        editor_help_session_set_scroll(0);
        editor_request_redraw();
        return 1;
    }
    return 1;                            /* modal: swallow body clicks */
}

int glr_ctrl_router_handle_help_tab_special(int key) {
    if (!ui_state_help().visible)
        return 0;
    if (key == GLUT_KEY_LEFT) {
        glr_action_help_tab_prev();
        return 1;
    }
    if (key == GLUT_KEY_RIGHT) {
        glr_action_help_tab_next();
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_help_scroll_special(int key) {
    if (!ui_state_help().visible)
        return 0;
    switch (key) {
    case GLUT_KEY_UP:        glr_ctrl_help_scroll_by(-1); return 1;
    case GLUT_KEY_DOWN:      glr_ctrl_help_scroll_by(1);  return 1;
    case GLUT_KEY_PAGE_UP:   glr_ctrl_help_scroll_by(-5); return 1;
    case GLUT_KEY_PAGE_DOWN: glr_ctrl_help_scroll_by(5);  return 1;
    default: return 0;
    }
}

/* Shared by the F1 key and the status-bar "F1 help" keycap click. */
void glr_ctrl_toggle_help(void) {
    UiHelpState *help = ui_state_help_mut();
    help->visible = !help->visible;
    editor_help_session_set_tab(0);
    editor_help_session_set_scroll(0);
}

int glr_ctrl_router_handle_help_toggle_special(int key) {
    if (key == GLUT_KEY_F1) {
        glr_ctrl_toggle_help();
        return 1;
    }
    return 0;
}

static void cycle_example_or_user_scene(void) {
    /* F12 cycles: examples[0..N-1] -> user scenes (in slot order) -> back.
     * Active example moves to the next example, then first user scene.
     * Active user scene moves to the next occupied user slot, then example 0. */
    /* Clear editor / camera / menu / picker / code-panel-drag transients
     * so the new scene starts from a clean controller state. (Moved out
     * of src/repl/example_loader.c as step 2 of the decouple plan,
     * feature/decouple-repl-from-gl-repl-alt.md.) */
    glr_app_reset_transients();
    editor_undo_note_wholesale_replacement();
    int count = repl_example_count();
    int active_scene = repl_active_user_scene();

    if (active_scene >= 0) {
        for (int scene_idx = active_scene + 1; scene_idx < MAX_USER_SCENES; scene_idx++) {
            if (repl_user_scene_slot_used(scene_idx)) {
                if (repl_load_user_scene_idx(scene_idx))
                    editor_load_line_to_input(editor_state_edit_line());
                return;
            }
        }
        if (count > 0)
            editor_state_edit_line_set(repl_load_example(0));
        return;
    }

    if (count > 0) {
        int next = repl_state_scenes().active_example_idx + 1;
        if (next < count) {
            editor_state_edit_line_set(repl_load_example(next));
            return;
        }
    }

    for (int scene_idx = 0; scene_idx < MAX_USER_SCENES; scene_idx++) {
        if (repl_user_scene_slot_used(scene_idx)) {
            if (repl_load_user_scene_idx(scene_idx))
                editor_load_line_to_input(editor_state_edit_line());
            return;
        }
    }
    if (count > 0)
        editor_state_edit_line_set(repl_load_example(0));
}

/* Reverse counterpart to cycle_example_or_user_scene. Walks the same
 * sequence — [examples 0..N-1, occupied user slots 0..M-1] — backwards,
 * wrapping at the start to the last occupied user slot, then the last
 * example. Symmetric structure with the forward cycle: same transient
 * cleanup and undo-clear, just inverted index walks. */
static void cycle_example_or_user_scene_prev(void) {
    glr_app_reset_transients();
    editor_undo_note_wholesale_replacement();
    int count = repl_example_count();
    int active_scene = repl_active_user_scene();

    if (active_scene >= 0) {
        for (int scene_idx = active_scene - 1; scene_idx >= 0; scene_idx--) {
            if (repl_user_scene_slot_used(scene_idx)) {
                if (repl_load_user_scene_idx(scene_idx))
                    editor_load_line_to_input(editor_state_edit_line());
                return;
            }
        }
        if (count > 0)
            editor_state_edit_line_set(repl_load_example(count - 1));
        return;
    }

    if (count > 0) {
        int prev = repl_state_scenes().active_example_idx - 1;
        if (prev >= 0) {
            editor_state_edit_line_set(repl_load_example(prev));
            return;
        }
    }

    for (int scene_idx = MAX_USER_SCENES - 1; scene_idx >= 0; scene_idx--) {
        if (repl_user_scene_slot_used(scene_idx)) {
            if (repl_load_user_scene_idx(scene_idx))
                editor_load_line_to_input(editor_state_edit_line());
            return;
        }
    }
    if (count > 0)
        editor_state_edit_line_set(repl_load_example(count - 1));
}

int glr_ctrl_router_handle_scene_cycle_special(int key) {
    if (key == GLUT_KEY_F12) {
        cycle_example_or_user_scene();
        return 1;
    }
    if (key == GLUT_KEY_F11) {
        cycle_example_or_user_scene_prev();
        return 1;
    }
    return 0;
}

/* ---- Mouse / motion router helpers ------------------------------------ */

int glr_ctrl_router_handle_variable_panel_drag_begin(int button, int state, int x, int y) {
    if (state != GLUT_DOWN) return 0;
    if (!variable_panel_visible()) return 0;
    if (button != GLUT_LEFT_BUTTON && button != GLUT_RIGHT_BUTTON)
        return 0;
    int row_idx;
    if (!ui_variable_panel_hit_for_count(NULL, x, y, repl_eval_predef_view().count,
                                         &row_idx))
        return 0;
    if (replay_active())
        replay_stop();
    int log_mode = (button == GLUT_RIGHT_BUTTON) ? 1 : 0;
    variable_panel_handle_drag_begin(row_idx, log_mode, x);
    editor_request_redraw();
    return 1;
}

int glr_ctrl_router_handle_variable_panel_drag_release(int state) {
    if (state != GLUT_UP) return 0;
    if (!variable_panel_drag_active()) return 0;
    variable_panel_handle_drag_reset();
    editor_request_redraw();
    return 1;
}

int glr_ctrl_router_handle_right_config_press(int button, int state, int x, int y) {
    if (state != GLUT_DOWN || button != GLUT_RIGHT_BUTTON)
        return 0;
    UiHit hit = ui_panels_handle_right_press(x, y);
    if (hit.kind == UI_HIT_SUBMENU_ITEM && hit.cmd_idx == GLR_MENU_CONFIG && hit.item_idx >= 0) {
        glr_cfg_cycle_row(hit.item_idx, -1);
        editor_request_redraw();
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_scene_press(int button, int state, int x, int y) {
    if (state != GLUT_DOWN || button != GLUT_LEFT_BUTTON)
        return 0;
    /* The scene region is owned by the camera, but the floating color
     * picker can overlap it. Give the picker first crack on press; if
     * the click lands outside its rects the peer dismisses itself
     * (closed=1) and we redraw but fall through so the camera sees
     * the event. */
    ColorPickerInputResult r = color_picker_handle_press(x, y);
    if (r.closed || r.changed)
        editor_request_redraw();
    if (r.consumed)
        return 1;
    return 0;
}

int glr_ctrl_router_handle_camera_mouse(int button, int state, int x, int y) {
    glr_ctrl_sync_camera_control_mode();
    if (state == GLUT_DOWN)
        ui_state_pointer_set(x, y, button);
    else if (state == GLUT_UP)
        ui_state_pointer_set(x, y, -1);
    else
        ui_state_pointer_set_pos(x, y);
    glr_camera_mouse_event(button, state, x, y, editor_input_active_modifiers());
    return 1;
}

static void glr_ctrl_apply_variable_panel_value_change(
        const VariablePanelValueChange *value_change) {
    ReplCompiledChange compiled;
    ReplCompileContext ctx;
    VariablePanelDragState drag;
    char err[REPL_STATUS_TEXT_MAX] = "";
    int var_idx;
    int capture_undo;

    if (!value_change || !value_change->name[0])
        return;

    drag = variable_panel_drag();
    var_idx = drag.var_idx;
    if (var_idx < 0 || var_idx >= g_num_predef_vars)
        return;
    if (strcmp(g_predef_vars[var_idx].name, value_change->name) != 0) {
        var_idx = repl_eval_find_predef_var_idx(value_change->name);
        if (var_idx < 0)
            return;
    }
    if (g_predef_vars[var_idx].value == value_change->value)
        return;

    ctx = repl_compile_context_from_live(editor_state_edit_line());
    /* repl_compile_context_from_live(editor_state_edit_line()) defaults insert_mode to 0
     * (non-editor convention); the editor-side caller knows the live
     * value and overrides. */
    ctx.insert_mode = editor_insert_mode();
    if (repl_compile_set_predef_value(value_change->name, value_change->value,
                                      &ctx, &compiled,
                                      err, sizeof(err)) != REPL_COMPILE_OK) {
        repl_set_status_error(err[0] ? err : "Variable update failed");
        return;
    }

    capture_undo = !variable_panel_drag_undo_snapshot_pushed();
    if (!editor_commit_apply_external_change(&compiled, capture_undo)) {
        repl_set_status_error("Command buffer full!");
        return;
    }
    if (capture_undo)
        variable_panel_drag_mark_undo_snapshot_pushed();
}

int glr_ctrl_router_handle_variable_panel_motion(int x, int y) {
    VariablePanelValueChange value_change;

    (void)y;
    if (!variable_panel_drag_active())
        return 0;
    if (variable_panel_handle_drag_motion(x, &value_change))
        glr_ctrl_apply_variable_panel_value_change(&value_change);
    editor_request_redraw();
    return 1;
}

int glr_ctrl_router_handle_camera_motion(int x, int y) {
    glr_ctrl_sync_camera_control_mode();
    glr_camera_drag_motion(x, y);
    return 1;
}

int glr_ctrl_router_handle_camera_pointer_set(int x, int y) {
    glr_camera_pointer_set(x, y);
    ui_state_pointer_set_pos(x, y);
    return 1;
}

int glr_ctrl_router_handle_glut_scroll_wheel_button(int button, int state, int x, int y) {
#ifdef USE_GLUT
    if ((button != 3 && button != 4) || state != GLUT_DOWN)
        return 0;
    int direction = (button == 3) ? -1 : 1;
    if (ui_state_help().visible) {
        glr_ctrl_help_scroll_by(direction);
    } else if (editor_input_point_in_code_panel(x, y)) {
        editor_input_code_panel_scroll(direction);
    } else {
        glr_camera_add_zoom_velocity(direction == -1 ? -0.3f : 0.3f);
    }
    editor_request_redraw();
    return 1;
#else
    (void)button; (void)state; (void)x; (void)y;
    return 0;
#endif
}

/* ---- Code-panel UiHit dispatch --------------------------------------- *
 * (Introduced in Phase J2.2 of feature/decouple-repl-from-gl-repl-alt.md.) */

/* Code-panel selection drag tracking. Press handlers set the anchor
 * to the clicked source-cmd row; motion re-runs ui_panels_hit_test
 * to derive the drag target and extends the editor selection.
 * Release on UP clears the active flag. The state lives here (not in
 * ui_panels.c) because UI input files report hit-test data only. */
static int g_code_panel_drag_active = 0;
static int g_code_panel_drag_anchor = -1;
static int g_code_panel_drag_moved  = 0;
/* Press char column inside the active-edit-row drag. -1 when the
 * press wasn't on a code-text row (gutter / insert-line / non-text
 * hits don't arm an input-row drag). The drag motion handler reads
 * this to decide between per-char input-buffer selection (drag stays
 * on the active edit row) and the existing line-range path. */
static int g_code_panel_drag_char_anchor = -1;

/* Double-click detection: previous UI_HIT_CODE_TEXT press timestamp
 * and target. A second press at the same (line, char) within
 * DOUBLE_CLICK_MS reads as a double-click and selects the word under
 * the cursor. Using line/char instead of pixel coords means small
 * mouse jitter between presses doesn't break the gesture. */
#define DOUBLE_CLICK_MS 400u
static unsigned int g_last_text_press_ms     = 0;
static int          g_last_text_press_line   = -1;
static int          g_last_text_press_char   = -1;
/* Scene-tab double-click: a 2nd press on the same tab display index
 * within DOUBLE_CLICK_MS triggers inline rename (user tabs only). */
static unsigned int g_last_tab_press_ms      = 0;
static int          g_last_tab_press_idx     = -1;
/* Test clock seam mirrors editor_input_set_modifier_provider_for_test:
 * tests that drive a double-click without a live GLUT context replace
 * the clock with a deterministic source. Production code falls back to
 * glutGet(GLUT_ELAPSED_TIME). */
static unsigned int (*g_double_click_clock_ms_for_test)(void) = NULL;

void glr_ctrl_router_set_double_click_clock_for_test(
    unsigned int (*clock_ms)(void)) {
    g_double_click_clock_ms_for_test = clock_ms;
    /* Tests typically arrange a fresh clock per case — wipe the
     * stored press so prior real-time history can't leak across. */
    g_last_text_press_ms   = 0;
    g_last_text_press_line = -1;
    g_last_text_press_char = -1;
    g_last_tab_press_ms    = 0;
    g_last_tab_press_idx   = -1;
}

static unsigned int current_double_click_ms(void) {
    if (g_double_click_clock_ms_for_test)
        return g_double_click_clock_ms_for_test();
    return (unsigned int)glutGet(GLUT_ELAPSED_TIME);
}

void glr_ctrl_router_reset_code_panel_drag(void) {
    g_code_panel_drag_active = 0;
    g_code_panel_drag_anchor = -1;
    g_code_panel_drag_moved  = 0;
    g_code_panel_drag_char_anchor = -1;
}

/* Apply an input-row drag motion: if the press char-anchor is armed
 * and the drag still hits the active edit row, grow the input-buffer
 * selection toward target_char. Returns 1 if handled (caller should
 * stop processing), 0 otherwise (caller falls through to line-range).
 * Exposed in glr_ctrl.h so tests can drive the per-char logic without
 * computing pixel coordinates that match the live panel layout. */
int glr_ctrl_router_apply_input_row_drag(int target_line, int target_char) {
    if (!g_code_panel_drag_active || g_code_panel_drag_anchor < 0)
        return 0;
    if (g_code_panel_drag_char_anchor < 0)
        return 0;
    if (target_line != g_code_panel_drag_anchor)
        return 0;
    if (target_line != editor_state_edit_line())
        return 0;
    if (target_char < 0)
        return 0;
    editor_cursor_pos_extend_selection(target_char);
    g_code_panel_drag_moved = 1;
    glr_action_cursor_blink_reset();
    editor_request_redraw();
    return 1;
}

/* Common epilog for clicks that move the editor cursor: blink reset,
 * autocomplete clear, selection clear, redraw. Mirrors the legacy
 * ui_panels_handle_code_panel_click tail. */
static void route_code_click_epilog(void) {
    glr_action_cursor_blink_reset();
    /* On a click that landed the cursor on the tutorial's expected
     * commit line, refresh autocomplete so the shadow ghost
     * reappears; anywhere else, clear to drop stale completions. */
    if (tutorial_active() &&
        editor_state_edit_line() == tutorial_expected_commit_line())
        editor_completion_update();
    else
        editor_completion_clear();
    editor_selection_clear_line_range();
    editor_request_redraw();
}

/* Select the word containing (line_idx, char_idx) as an input-buffer
 * selection. Navigates to the line first (reloads input) so the word
 * walk runs against the row's actual canonical text. char_idx outside
 * a word leaves the buffer unselected. Public so tests can drive
 * double-click selection without faking GLUT_ELAPSED_TIME. */
void glr_ctrl_router_select_word_at(int line_idx, int char_idx) {
    if (line_idx < 0)
        return;
    editor_navigate_to_line(line_idx);

    const char *text = editor_input_text();
    int len = editor_input_len();
    int word_start = char_idx;
    int word_end   = char_idx;
    editor_input_word_bounds_at(text, len, char_idx, &word_start, &word_end);
    if (word_end <= word_start) {
        /* Clicked between words / on whitespace: leave cursor placed
         * but no selection. */
        editor_cursor_pos_set(char_idx);
        return;
    }
    /* Place cursor at word_start, then atomically pin and extend to
     * word_end. Doing it as two steps with editor_input_anchor_set
     * would collapse immediately (anchor == cursor) — that's the
     * footgun the extend helper was added to avoid. */
    editor_cursor_pos_set(word_start);
    editor_cursor_pos_extend_selection(word_end);
}

/* UI_HIT_CODE_TEXT: navigate to clicked line, set cursor column, arm
 * the selection drag anchor. A press that hits the same (line, char)
 * as the previous press within DOUBLE_CLICK_MS reads as a double-click
 * and selects the word at the cursor instead of placing a bare
 * cursor. */
static int route_code_text_hit(const UiHit *hit) {
    /* A non-swatch click on the code panel closes any open color picker
     * (matches legacy ui_panels_handle_code_panel_press behaviour). */
    color_picker_stop();

    unsigned int now_ms = current_double_click_ms();
    int is_double_click = (g_last_text_press_line == hit->line_idx &&
                           g_last_text_press_char == hit->char_idx &&
                           hit->line_idx >= 0 &&
                           hit->char_idx >= 0 &&
                           now_ms - g_last_text_press_ms < DOUBLE_CLICK_MS);
    g_last_text_press_ms   = now_ms;
    g_last_text_press_line = hit->line_idx;
    g_last_text_press_char = hit->char_idx;

    if (is_double_click) {
        glr_ctrl_router_select_word_at(hit->line_idx, hit->char_idx);
        route_code_click_epilog();
        /* Double-click also arms the drag in case the user starts to
         * drag-extend the word selection. */
        if (hit->line_idx >= 0 && hit->line_idx < repl_state_document_count()) {
            g_code_panel_drag_active = 1;
            g_code_panel_drag_anchor = hit->line_idx;
            g_code_panel_drag_moved  = 0;
            g_code_panel_drag_char_anchor = hit->char_idx;
        }
        return 1;
    }

    if (hit->line_idx >= 0)
        editor_navigate_to_line(hit->line_idx);
    if (hit->char_idx >= 0)
        editor_cursor_pos_set(hit->char_idx);
    route_code_click_epilog();

    /* Arm drag anchor for committed lines only. */
    if (hit->line_idx >= 0 && hit->line_idx < repl_state_document_count()) {
        g_code_panel_drag_active = 1;
        g_code_panel_drag_anchor = hit->line_idx;
        g_code_panel_drag_moved  = 0;
        /* Record the press column so a drag that stays on the active
         * edit row can build a per-character input-buffer selection.
         * The press itself moved the cursor to hit->char_idx (and
         * cleared any anchor as the Phase B default of the
         * editor-input-selection plan), so on the first drag-motion
         * the extend helper pins this column. */
        g_code_panel_drag_char_anchor = hit->char_idx;
    } else {
        glr_ctrl_router_reset_code_panel_drag();
    }
    return 1;
}

/* UI_HIT_CODE_INSERT_LINE: insertion virtual row in insert mode. Set
 * cursor column but do not navigate (matches legacy on_insert_line=1
 * behaviour). No drag anchor — the insert row is virtual. */
static int route_code_insert_line_hit(const UiHit *hit) {
    color_picker_stop();
    if (hit->char_idx >= 0)
        editor_cursor_pos_set(hit->char_idx);
    route_code_click_epilog();
    glr_ctrl_router_reset_code_panel_drag();
    return 1;
}

/* UI_HIT_CODE_GUTTER: clicking the line-number column selects the
 * row. Same dispatch as CODE_TEXT minus the cursor-column move. */
static int route_code_gutter_hit(const UiHit *hit) {
    color_picker_stop();
    if (hit->line_idx >= 0)
        editor_navigate_to_line(hit->line_idx);
    route_code_click_epilog();
    if (hit->line_idx >= 0 && hit->line_idx < repl_state_document_count()) {
        g_code_panel_drag_active = 1;
        g_code_panel_drag_anchor = hit->line_idx;
        g_code_panel_drag_moved  = 0;
    } else {
        glr_ctrl_router_reset_code_panel_drag();
    }
    return 1;
}

/* UI_HIT_CODE_PANEL_CHROME: inert code-panel chrome (e.g. statusbar).
 * Consume the press so scene/camera handlers do not see it, but do not
 * move the cursor or selection. */
static int route_code_panel_chrome_hit(void) {
    color_picker_stop();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_CODE_FOCUS_TOGGLE: the statusbar "focus" keycap. Same action
 * as the Ctrl+Shift+F shortcut — go through the shared toggle so both
 * paths sync chrome, request follow-scroll, and post the status line. */
static int route_code_focus_toggle_hit(void) {
    glr_ctrl_toggle_code_focus();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_HELP_TOGGLE: the statusbar "F1 help" keycap. Same action as
 * the F1 key — go through the shared toggle so the overlay tab/scroll
 * reset identically. */
static int route_help_toggle_hit(void) {
    glr_ctrl_toggle_help();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_INLINE_COLOR_SWATCH: toggle / open the floating color picker
 * for the swatch's source line. Undo capture is owned by the picker's
 * writeback path (color_picker_write_cmd → editor_commit_apply_external
 * _change with capture_undo on the first slider edit per session), so
 * a session that opens and closes without editing leaves the undo ring
 * untouched. */
static int route_inline_color_swatch_hit(const UiHit *hit, int my) {
    if (hit->line_idx < 0)
        return 0;
    if (color_picker_active_line() == hit->line_idx) {
        color_picker_stop();
    } else {
        color_picker_start(hit->line_idx, my);
    }
    editor_request_redraw();
    return 1;
}

static int route_numeric_swatch_hit(const UiHit *hit) {
    int edit_line = editor_state_edit_line();
    EditorInputView in = editor_state_input();
    ReplNumericArgAtCursor d;
    float step, new_value;
    char buf[32];
    char new_line[MAX_LINE_LEN];
    int n;
    char parse_err[REPL_STATUS_TEXT_MAX] = "";
    ReplParsedLine pl;
    ReplCompiledChange change;
    int text_len;

    if (editor_insert_mode() ||
        edit_line < 0 || edit_line >= repl_state_document_count())
        return 1;

    if (tutorial_active() &&
        !tutorial_guard_source_change(edit_line, 1, 1))
        return 1;

    d = repl_eval_numeric_arg_at_cursor(in.input, in.cursor_pos);
    if (!d.found) return 1;

    step = repl_eval_swatch_step(d.value);
    new_value = d.value + (hit->item_idx > 0 ? step : -step);
    repl_eval_format_swatch_number(new_value, buf, sizeof buf);

    n = snprintf(new_line, sizeof new_line, "%.*s%s%s",
                 d.arg_start, in.input, buf, in.input + d.arg_end);
    if (n < 0 || n >= (int)sizeof new_line) return 1;

    {
        ReplParseContext parse_ctx = {
            .source_line_idx = edit_line,
            .err_buf = parse_err,
            .err_sz = (int)sizeof parse_err,
        };
        if (!repl_parser_parse_command_ctx(new_line, &pl, &parse_ctx)) {
            if (parse_err[0]) repl_set_status(parse_err);
            return 1;
        }
    }
    if (pl.cmd.type == CMD_COMMENT) return 1;

    repl_compiled_change_init(&change);
    change.kind = REPL_COMPILED_REPLACE_ONE;
    change.pos = edit_line;
    change.count = 1;
    change.cmds[0] = pl.cmd;
    text_len = (int)strlen(pl.text);
    if (text_len >= MAX_LINE_LEN) text_len = MAX_LINE_LEN - 1;
    memcpy(change.text[0], pl.text, (size_t)text_len);
    change.text[0][text_len] = '\0';

    if (editor_commit_apply_external_change(&change, 1)) {
        editor_load_line_to_input(edit_line);
        {
            EditorInputView reloaded = editor_state_input();
            int pos = d.arg_start < reloaded.input_len
                          ? d.arg_start : reloaded.input_len;
            editor_cursor_pos_set(pos);
        }
        editor_completion_update();
        editor_request_redraw();
    }
    return 1;
}

/* UI_HIT_COLOR_SWATCH: floating picker slider control press. The
 * picker has its own internal hit-test for SV/hue/alpha regions and
 * starts a drag on press. */
static int route_color_picker_control_hit(int x, int y) {
    ColorPickerInputResult r = color_picker_handle_press(x, y);
    if (r.changed || r.closed)
        editor_request_redraw();
    return r.consumed;
}

/* UI_HIT_PIN_BUTTON: Search / Replay pinned right-side button. */
static int route_pin_button_hit(const UiHit *hit) {
    ui_menu_bar_close();
    switch (hit->item_idx) {
    case UI_MENU_BAR_PIN_REPLAY:
        replay_handle_pin_clicked();
        break;
    case UI_MENU_BAR_PIN_SEARCH:
        editor_search_handle_key(KEY_CTRL_F);
        ui_menu_bar_note_search_opened(repl_state_variables().anim_time);
        break;
    default:
        break;
    }
    editor_request_redraw();
    return 1;
}

/* UI_HIT_MENU_BUTTON: top-level menu-bar button. Click on the open
 * menu's button toggles it closed; click on a different button
 * switches the open dropdown. */
static int route_menu_button_hit(const UiHit *hit) {
    int menu_id = hit->cmd_idx;
    if (menu_id < 0) return 0;

    int open_menu = ui_menu_bar_open_menu_id();
    if (open_menu == menu_id)
        ui_menu_bar_close();
    else
        ui_menu_bar_set_open_menu(menu_id, repl_state_variables().anim_time);
    editor_request_redraw();
    return 1;
}

/* UI_HIT_MENU_ITEM: open dropdown row click. Activates the action
 * via glr_action_menu_item_activate using both menu_id (cmd_idx)
 * and item_idx from the hit payload. The action returns 1 if the
 * dropdown should close after activation (most action items) or 0 to
 * leave it open (cycle / toggle items). */
static int route_menu_item_hit(const UiHit *hit) {
    if (hit->cmd_idx < 0 || hit->item_idx < 0) return 0;
    int close = glr_action_menu_item_activate(hit->cmd_idx, hit->item_idx);
    if (close)
        ui_menu_bar_close();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_SUBMENU_ITEM: a flyout row. cmd_idx is the owning menu_id;
 * item_idx is the absolute target index the provider resolved.
 *  - MENU_SCENE: item_idx = global flat example index → load it and
 *    dismiss the menu (a scene pick is a one-shot action).
 *  - MENU_TUTORIALS: item_idx = global tutorial index → start it via
 *    tutorial_start (REPL-side; sidesteps glr_action_menu_item_activate,
 *    whose MENU_TUTORIALS branch only owns the top-level tag-row /
 *    Restart / Exit dispatch) and dismiss the menu.
 *  - MENU_CONFIG: item_idx = absolute g_cfg_items[] index → cycle the
 *    item forward via glr_cfg_cycle_row (NOT through
 *    glr_action_menu_item_activate, whose Config branch is the inert
 *    parent-row guard) and KEEP the dropdown + flyout open so repeated
 *    toggles work, matching the old flat Config dropdown. (Parent-row
 *    guard added in Step 5 of config-menu-submenu-sections.md.) */
static int route_submenu_item_hit(const UiHit *hit) {
    if (hit->item_idx < 0)
        return 0;
    if (hit->cmd_idx == GLR_MENU_CONFIG) {
        glr_cfg_cycle_row(hit->item_idx, 1);
        editor_request_redraw();
        return 1;
    }
    if (hit->cmd_idx == GLR_MENU_TUTORIALS) {
        tutorial_start(hit->item_idx);
        ui_menu_bar_close();
        editor_request_redraw();
        return 1;
    }
    glr_scene_load_example(hit->item_idx);
    ui_menu_bar_close();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_CODE_PANEL_TAB: scene tab strip click. Single click switches
 * scenes through the shared Scene-menu load helpers (reusing the exact
 * load sequence — no duplication); a 2nd click on the same user tab
 * within DOUBLE_CLICK_MS opens the existing status-bar inline rename.
 * The display index is resolved against the *live* scene shape so a
 * frame-stale click can't misroute. Always consumed. */
static int route_scene_tab_hit(const UiHit *hit) {
    int idx           = hit->item_idx;
    int user_tab_count = repl_user_scene_count();
    int example_idx   = repl_state_scenes().active_example_idx;
    int active_slot   = repl_active_user_scene();
    unsigned int now_ms = current_double_click_ms();
    int is_double = (g_last_tab_press_idx >= 0 &&
                     g_last_tab_press_idx == idx &&
                     now_ms - g_last_tab_press_ms < DOUBLE_CLICK_MS);

    g_last_tab_press_ms  = now_ms;
    g_last_tab_press_idx = idx;

    if (idx < 0)
        return 1;  /* stale — consumed no-op */

    if (idx < user_tab_count) {
        int slot = glr_scene_menu_slot_for_dense_index(idx);
        if (slot < 0)
            return 1;  /* stale */
        if (slot != active_slot)
            glr_scene_load_user_slot(slot);  /* no-op when already active */
        if (is_double)
            editor_inline_rename_begin(slot);
        editor_request_redraw();
        return 1;
    }

    if (idx == user_tab_count && example_idx >= 0) {
        /* The example tab is active iff no user slot is active. Only
         * reload when it is not already the active scene; never rename. */
        if (active_slot >= 0)
            glr_scene_load_example(example_idx);
        editor_request_redraw();
        return 1;
    }

    return 1;  /* out-of-range stale idx — consumed no-op */
}

/* UI_HIT_PANEL_DIVIDER: start the panel-resize drag. Motion updates
 * panel_frac via editor_handle_motion's resizing-panel branch; UP
 * clears the resizing flag. */
static int route_panel_divider_hit(const UiHit *hit) {
    (void)hit;
    ui_state_code_panel_mut()->resizing_panel = 1;
    editor_set_cursor(editor_input_code_panel_resize_cursor());
    return 1;
}

/* UI_HIT_VARIABLE_SLIDER: variable-panel left-click drag begin. The
 * J1 helper handles replay-stop + drag start. */
static int route_variable_slider_hit(int x, int y) {
    return glr_ctrl_router_handle_variable_panel_drag_begin(GLUT_LEFT_BUTTON,
                                                               GLUT_DOWN, x, y);
}

/* Derive a code-panel target line from a hit, mirroring the legacy
 * code_panel_drag_target. Insert-line drags use edit_line (the line
 * the cursor is parked on), only falling back to the last committed
 * line when edit_line is past the document end — preserving the
 * insertion-point as the drag endpoint when the user is editing
 * mid-document. Returns -1 if the hit is not a code-panel kind. */
static int code_panel_target_from_hit(UiHit hit) {
    switch (hit.kind) {
    case UI_HIT_CODE_TEXT:
    case UI_HIT_CODE_GUTTER:
        return hit.line_idx;
    case UI_HIT_CODE_INSERT_LINE: {
        int target = hit.line_idx; /* set to editor_state_edit_line() in J2.1 */
        int doc_count = repl_state_document_count();
        if (target >= doc_count)
            target = doc_count - 1;
        return target;
    }
    default:
        return -1;
    }
}

int glr_ctrl_router_handle_code_panel_hit(UiHit hit, int x, int y) {
    /* Inline rename is a hard modal for keystrokes but NOT for the
     * mouse, so a click otherwise moves the cursor / switches tabs
     * while typed keys still feed the rename buffer — misleading.
     * Clicking anywhere cancels the in-progress rename (discard, like
     * Esc), then the click proceeds normally (cursor lands where the
     * user clicked, rename mode exits). Begin-rename routes through a
     * later dispatch in this same call, so rename is not yet active at
     * entry for the click that starts it — no self-cancel. */
    if (editor_inline_rename_active())
        editor_inline_rename_cancel();
    if (editor_inline_file_prompt_active())
        editor_inline_file_prompt_cancel();

    /* A click outside the menu bar (anywhere that isn't UI_HIT_MENU_BUTTON,
     * UI_HIT_MENU_ITEM, or UI_HIT_SUBMENU_ITEM) dismisses an open
     * dropdown — matches the legacy
     * "click outside dropdown closes it" behaviour from
     * ui_panels_handle_code_panel_press. */
    int dismissed_dropdown = 0;
    if (hit.kind != UI_HIT_MENU_BUTTON &&
        hit.kind != UI_HIT_MENU_ITEM &&
        hit.kind != UI_HIT_SUBMENU_ITEM &&
        hit.kind != UI_HIT_PIN_BUTTON &&
        hit.kind != UI_HIT_COLOR_SWATCH &&
        ui_menu_bar_menu_dropdown_is_open()) {
        ui_menu_bar_close();
        dismissed_dropdown = 1;
    }

    int consumed;
    switch (hit.kind) {
    case UI_HIT_COLOR_SWATCH:
        consumed = route_color_picker_control_hit(x, y); break;
    case UI_HIT_INLINE_COLOR_SWATCH:
        consumed = route_inline_color_swatch_hit(&hit, y); break;
    case UI_HIT_NUMERIC_SWATCH:
        consumed = route_numeric_swatch_hit(&hit); break;
    case UI_HIT_PIN_BUTTON:
        consumed = route_pin_button_hit(&hit); break;
    case UI_HIT_MENU_BUTTON:
        consumed = route_menu_button_hit(&hit); break;
    case UI_HIT_MENU_ITEM:
        consumed = route_menu_item_hit(&hit); break;
    case UI_HIT_SUBMENU_ITEM:
        consumed = route_submenu_item_hit(&hit); break;
    case UI_HIT_VARIABLE_SLIDER:
        consumed = route_variable_slider_hit(x, y); break;
    case UI_HIT_PANEL_DIVIDER:
        consumed = route_panel_divider_hit(&hit); break;
    case UI_HIT_CODE_TEXT:
        consumed = route_code_text_hit(&hit); break;
    case UI_HIT_CODE_INSERT_LINE:
        consumed = route_code_insert_line_hit(&hit); break;
    case UI_HIT_CODE_GUTTER:
        consumed = route_code_gutter_hit(&hit); break;
    case UI_HIT_CODE_PANEL_CHROME:
        consumed = route_code_panel_chrome_hit(); break;
    case UI_HIT_CODE_FOCUS_TOGGLE:
        consumed = route_code_focus_toggle_hit(); break;
    case UI_HIT_HELP_TOGGLE:
        consumed = route_help_toggle_hit(); break;
    case UI_HIT_CODE_PANEL_TAB:
        consumed = route_scene_tab_hit(&hit); break;
    case UI_HIT_HELP_PANEL:
    case UI_HIT_REPLAY_BUTTON:
    case UI_HIT_SCENE:
    case UI_HIT_NONE:
    default:
        consumed = 0;
    }

    /* A click that only dismissed an open dropdown is consumed by the
     * dismiss itself; do not fall through to scene-press / camera /
     * variable-slider drag for the same press. Matches legacy
     * UI_PANEL_PRESS_CONSUMED behaviour. */
    if (!consumed && dismissed_dropdown)
        return 1;
    return consumed;
}

int glr_ctrl_router_handle_code_panel_drag(int x, int y) {
    if (!g_code_panel_drag_active || g_code_panel_drag_anchor < 0)
        return 0;

    UiRenderSnapshot ui_snap;
    glr_ctrl_build_ui_snapshot(&ui_snap);
    UiHit hit = ui_panels_hit_test(&ui_snap, x, y, repl_eval_predef_view().count);

    /* Per-character input-buffer drag: as long as the drag stays on
     * the same source row as the press AND that row is the active
     * edit line, grow the input-buffer selection toward the current
     * char column. */
    if (hit.kind == UI_HIT_CODE_TEXT && hit.char_idx >= 0 &&
        glr_ctrl_router_apply_input_row_drag(hit.line_idx, hit.char_idx))
        return 1;

    /* Input-row drag is sticky: once the press armed the char anchor
     * (g_code_panel_drag_char_anchor >= 0), motion that wanders off
     * the row is absorbed as a no-op rather than switching to
     * line-range selection. Input text selection is single-line by
     * design, so dragging downward shouldn't silently re-target the
     * selection model. The user has to release and re-press in the
     * gutter (or on a non-edit row) to start a line-range drag. */
    if (g_code_panel_drag_char_anchor >= 0)
        return 1;

    int target = code_panel_target_from_hit(hit);
    if (target < 0) {
        /* Drag wandered off the code-panel kinds — clamp the pointer
         * into the code-panel rect and re-classify so the selection
         * extends to the nearest visible row. Matches legacy
         * code_panel_drag_target's [0, visible_lines-1] vis clamp. */
        int cp_x, cp_y, cp_w, cp_h;
        ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
        int win_h = ui_state_viewport().window_h;
        if (cp_w > 0 && cp_h > 0 && win_h > 0) {
            int gl_y = win_h - y;
            int cx = x;
            UiRenderSnapshot ui_snap;
            glr_ctrl_build_ui_snapshot(&ui_snap);
            int cy = y;
            if (cx < cp_x + 1) cx = cp_x + 1;
            if (cx > cp_x + cp_w - 1) cx = cp_x + cp_w - 1;
            if (gl_y < cp_y + 1) cy = win_h - (cp_y + 1);
            UiHit clamped = ui_panels_hit_test(&ui_snap, cx, cy,
                                               repl_eval_predef_view().count);
            target = code_panel_target_from_hit(clamped);
        }
    }
    if (target < 0)
        return 1;

    if (target != g_code_panel_drag_anchor || g_code_panel_drag_moved) {
        g_code_panel_drag_moved = 1;
        editor_selection_start(g_code_panel_drag_anchor);
        editor_selection_set_end(target);
        editor_navigate_to_line(target);
        glr_action_cursor_blink_reset();
        editor_request_redraw();
    }
    return 1;
}

/* ===========================================================================
 * Dispatch entry points
 *
 * Each entry point is a fixed sandwich:
 *   1. Audio gesture once (browser autoplay policy).
 *   2. Rename modal capture FIRST (hard modal — every key/special key
 *      goes to the rename buffer).
 *   3. Controller-owned routes: routing helpers above, in the same
 *      order as the legacy editor dispatch chain.
 *   4. Editor's domain: editor_handle_* fires for clicks / keys that
 *      land in the editor's text-document or code-panel UI rect.
 *
 * Effect accumulation is shared with the editor: every helper writes
 * to the editor_input g_pending_input_effects struct via
 * editor_request_redraw etc., and apply_input_effects flushes the
 * accumulated effects through GLUT once per call.
 * ===========================================================================
 */

void glr_ctrl_keyboard(unsigned char key, int x, int y) {
    glr_ctrl_notify_audio_gesture_once();

    /* macOS Cmd+letter normalization happens before any dispatch so
     * the controller-owned cfg-shortcut chain (Cmd+B / Cmd+S / Cmd+T
     * etc.) compares against the control-character form (KEY_CTRL_*).
     * Without this, Cmd+B arrives here as 'b' (0x62), the chain looks
     * for KEY_CTRL_B (0x02) in g_cfg_items[].key_code, and the
     * shortcut silently misses. */
    key = editor_input_normalize_super_to_ctrl(key);

    /* Rename capture: hard modal. */
    if (editor_input_rename_capture_key(key)) {
        editor_reset_input_effects();
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    /* File-prompt capture: same hard-modal contract as rename. */
    if (editor_input_file_prompt_capture_key(key)) {
        editor_reset_input_effects();
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    editor_reset_input_effects();

    /* Controller-owned routes — order matches the legacy editor chain
     * so backtick / cfg shortcut / replay forwarding / Ctrl+G replay
     * toggle / Ctrl+= accum / Ctrl+S save / Ctrl+P debug / Ctrl+Q quit
     * fire exactly where they did before. */
    if (glr_ctrl_router_handle_config_menu_key(key) ||
        glr_ctrl_router_handle_active_replay_key(key) ||
        glr_ctrl_router_handle_cfg_shortcut_key(key) ||
        glr_ctrl_router_handle_replay_toggle_key(key) ||
        glr_ctrl_router_handle_save_key(key) ||
        glr_ctrl_router_handle_debug_dump_key(key) ||
        glr_ctrl_router_handle_accum_samples_key(key) ||
        glr_ctrl_router_handle_post_filter_key(key) ||
        glr_ctrl_router_handle_code_focus_key(key) ||
        glr_ctrl_router_handle_tutorial_ack_key(key) ||
        glr_ctrl_router_handle_quit_key(key)) {
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    /* Ctrl+F opens search downstream in editor_handle_key. Note the
     * rising edge here so the search-overlay fade clock is driven from
     * the controller — symmetric with the menu-pin path in
     * route_pin_button_hit; the renderer no longer mutates it. */
    int search_was_active = editor_state_search()->active;
    EditorInputDispatchEffects kb_effects = editor_handle_key(key, x, y);
    if (!search_was_active && editor_state_search()->active)
        ui_menu_bar_note_search_opened(repl_state_variables().anim_time);
    glr_ctrl_apply_input_effects(kb_effects);
}

void glr_ctrl_special(int key, int x, int y) {
    glr_ctrl_notify_audio_gesture_once();

    if (editor_input_rename_capture_special(key)) {
        editor_reset_input_effects();
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    if (editor_input_file_prompt_capture_special(key)) {
        editor_reset_input_effects();
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    editor_reset_input_effects();

    if (glr_ctrl_router_handle_replay_special(key) ||
        glr_ctrl_router_handle_cfg_special_shortcut(key) ||
        glr_ctrl_router_handle_horizontal_audio_special(key) ||
        glr_ctrl_router_handle_help_tab_special(key) ||
        glr_ctrl_router_handle_help_scroll_special(key) ||
        glr_ctrl_router_handle_help_toggle_special(key) ||
        glr_ctrl_router_handle_scene_cycle_special(key)) {
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    glr_ctrl_apply_input_effects(editor_handle_special(key, x, y));
}

/* Mouse routing: hit-test to decide owner before dispatching.
 *
 * UP cleanup: the editor first releases its own UP-side state
 * (ui_panels_handle_mouse_release, panel resize end), then the
 * variable-panel drag release fires if active.
 *
 * DOWN: variable panel hit always wins over code panel because it
 * owns its own rect that may overlap. Code-panel domain (proper /
 * divider / dropdown extension) goes to the editor. Scene region
 * tries the color picker overlay first, then camera. Right-click
 * dispatches the config dropdown, the variable panel (log mode), or
 * camera. The freeglut scroll-wheel emulation (buttons 3/4) routes
 * to help-overlay scroll, code-panel scroll (editor), or camera zoom
 * velocity. */
void glr_ctrl_mouse(int button, int state, int x, int y) {
    glr_ctrl_notify_audio_gesture_once();

    editor_reset_input_effects();
    if (button == GLUT_LEFT_BUTTON || button == GLUT_MIDDLE_BUTTON ||
        button == GLUT_RIGHT_BUTTON) {
        ui_state_pointer_set(x, y, state == GLUT_DOWN ? button : -1);
    } else {
        ui_state_pointer_set_pos(x, y);
    }

    if (state == GLUT_UP) {
        /* UP cleanup: release floating color picker drag, clear the
         * code-panel selection drag tracking, fire editor's UP-side
         * (panel resize end), then variable-panel release, then
         * camera UP so the orbit/pan/zoom interaction releases. */
        color_picker_handle_release();
        glr_ctrl_router_reset_code_panel_drag();
        glr_ctrl_apply_input_effects(editor_handle_mouse(button, state, x, y));
        editor_reset_input_effects();
        if (glr_ctrl_router_handle_variable_panel_drag_release(state)) {
            glr_ctrl_apply_input_effects(editor_take_input_effects());
            return;
        }
        glr_ctrl_router_handle_camera_mouse(button, state, x, y);
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    if (button == GLUT_LEFT_BUTTON) {
        /* Modal help overlay intercepts left clicks first: tab
         * select / click-away dismiss / swallow body. */
        if (glr_ctrl_router_handle_help_click(button, state, x, y)) {
            glr_ctrl_apply_input_effects(editor_take_input_effects());
            return;
        }
        /* Classify the click via the canonical hit-test, then route by
         * UiHit.kind to the owning subsystem. The hit-test covers
         * variable panel, color picker, menu bar, code panel (including
         * divider + inline swatch + insert line) and pin buttons. Only
         * kinds that don't apply (UI_HIT_SCENE, UI_HIT_NONE,
         * UI_HIT_HELP_PANEL) fall through to scene press / camera.
         * (Introduced as Phase J2.2 of
         * feature/decouple-repl-from-gl-repl-alt.md.) */
        UiRenderSnapshot ui_snap;
        glr_ctrl_build_ui_snapshot(&ui_snap);
        UiHit hit = ui_panels_hit_test(&ui_snap, x, y,
                           repl_eval_predef_view().count);
        if (glr_ctrl_router_handle_code_panel_hit(hit, x, y)) {
            glr_ctrl_apply_input_effects(editor_take_input_effects());
            return;
        }
        if (glr_ctrl_router_handle_scene_press(button, state, x, y)) {
            glr_ctrl_apply_input_effects(editor_take_input_effects());
            return;
        }
        glr_ctrl_router_handle_camera_mouse(button, state, x, y);
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    if (button == GLUT_RIGHT_BUTTON) {
        if (glr_ctrl_router_handle_right_config_press(button, state, x, y)) {
            glr_ctrl_apply_input_effects(editor_take_input_effects());
            return;
        }
        if (glr_ctrl_router_handle_variable_panel_drag_begin(button, state, x, y)) {
            glr_ctrl_apply_input_effects(editor_take_input_effects());
            return;
        }
        glr_ctrl_router_handle_camera_mouse(button, state, x, y);
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    if (glr_ctrl_router_handle_glut_scroll_wheel_button(button, state, x, y)) {
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    glr_ctrl_router_handle_camera_mouse(button, state, x, y);
    glr_ctrl_apply_input_effects(editor_take_input_effects());
}

/* Motion routing: UI overlay (color picker drag), variable-panel drag
 * motion, and camera drag motion are router-side; code-panel resize
 * tracking and code-panel selection drag are editor's.
 *
 * Pointer-state tracking: each non-camera handler updates
 * ui_state_pointer via glr_camera_pointer_set(x, y) AFTER its own
 * work so the camera's view of the pointer stays current. The camera
 * branch deliberately does NOT pre-set the pointer because
 * glr_camera_drag_motion reads the previous (px, py) to compute
 * delta and updates the pointer to (x, y) at the end. Pre-setting
 * here would zero the delta and freeze orbit/pan/zoom drag. */
void glr_ctrl_motion(int x, int y) {
    editor_reset_input_effects();

    /* Floating color picker drag tracking (SV / hue / alpha sliders). */
    {
        ColorPickerInputResult r = color_picker_handle_motion(x, y);
        if (r.consumed) {
            glr_ctrl_router_handle_camera_pointer_set(x, y);
            editor_request_redraw();
            glr_ctrl_apply_input_effects(editor_take_input_effects());
            return;
        }
    }

    if (glr_ctrl_router_handle_variable_panel_motion(x, y)) {
        glr_ctrl_router_handle_camera_pointer_set(x, y);
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    /* Code-panel selection drag (controller-owned state). */
    if (glr_ctrl_router_handle_code_panel_drag(x, y)) {
        glr_ctrl_router_handle_camera_pointer_set(x, y);
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    /* Editor's domain: panel resize tracking. editor_handle_motion is
     * a no-op when resizing_panel is clear. */
    if (ui_state_code_panel().resizing_panel) {
        EditorInputDispatchEffects pre_editor = editor_take_input_effects();
        glr_ctrl_apply_input_effects(editor_handle_motion(x, y));
        glr_ctrl_router_handle_camera_pointer_set(x, y);
        glr_ctrl_apply_input_effects(pre_editor);
        return;
    }

    /* Camera drag motion reads pointer = (px, py), computes delta,
     * then calls pointer_set(x, y) itself. */
    glr_ctrl_router_handle_camera_motion(x, y);
    glr_ctrl_apply_input_effects(editor_take_input_effects());
}

void glr_ctrl_passive_motion(int x, int y) {
    int menu_hover_changed;
    editor_reset_input_effects();
    /* Passive motion (no button held) just updates the pointer
     * position — there's no drag delta to preserve. */
    glr_ctrl_router_handle_camera_pointer_set(x, y);
    EditorInputDispatchEffects editor_effects = editor_handle_passive_motion(x, y);
    menu_hover_changed = ui_menu_bar_update_pointer_hover(x, y,
                                                          repl_state_variables().anim_time);
    if (menu_hover_changed)
        editor_request_redraw();
    glr_ctrl_apply_input_effects(editor_take_input_effects());
    glr_ctrl_apply_input_effects(editor_effects);
}

void glr_ctrl_mousewheel(int wheel, int direction, int x, int y) {
#ifndef USE_GLUT
    /* freeglut wheel callback: route to help scroll, code-panel scroll
     * (editor), or camera zoom velocity. */
    (void)wheel;
    editor_reset_input_effects();
    if (ui_state_help().visible) {
        glr_ctrl_help_scroll_by(-direction);
        editor_request_redraw();
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }
    if (editor_input_point_in_code_panel(x, y)) {
        glr_ctrl_apply_input_effects(editor_handle_mousewheel(wheel, direction, x, y));
        return;
    }
    glr_camera_add_zoom_velocity(-(float)direction * 0.1f);
    editor_request_redraw();
    glr_ctrl_apply_input_effects(editor_take_input_effects());
#else
    (void)wheel; (void)direction; (void)x; (void)y;
#endif
}

/* Per-frame tick (16 ms): advance audio playlist, surface track-change
 * status, advance time variable, advance replay state, decay camera
 * momentum, blink the cursor, decay the status TTL.
 *
 * The work is split from the GLUT scheduling so test fixtures (which
 * don't initialize GLUT) can drive a single tick by calling
 * glr_ctrl_tick directly. The public timer entry adds
 * glutPostRedisplay + glutTimerFunc reschedule on top.
 *
 * (Inlined here from the legacy editor's timer_func in
 * Phase J1 commit 48b.) */
void glr_ctrl_tick(void) {
    /* SIGINT (Ctrl+C) requested quit: the handler only set a flag;
     * do the recovery save + exit here on the normal path so no
     * stdio/file I/O runs inside the signal handler. */
    if (g_quit_requested) {
        glr_ctrl_save_quit_recovery();
        exit(0);
    }

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
                char msg[128];
                snprintf(msg, sizeof(msg), "Now playing: %s", base);
                repl_set_status(msg);
            }
        }
    }

    repl_advance_time(GLR_FRAME_DT_SECS);

    {
        ReplReplayRuntimeState *replay = replay_state_mut();

        if (replay->active)
            replay_tick_fade_batches(GLR_FRAME_DT_SECS);

        if (replay->active && replay->state == REPLAY_PLAYING) {
            replay->accum += replay->speed * GLR_FRAME_DT_SECS;
            while (replay->accum >= 1.0f &&
                   replay->state == REPLAY_PLAYING) {
                replay->accum -= 1.0f;
                replay_advance();
            }
        }
    }

    glr_ctrl_tick_view_transition(GLR_FRAME_DT_SECS);
    glr_camera_tick();
    glr_ctrl_tick_overlay_xn();

    {
        /* Easing for variable panel's lift above replay HUD (Smell #21/#22/#40) */
        UiVariablePanelState *vp = variable_panel_state_mut();
        float target = 0.0f;
        if (replay_active()) {
            float lift_target = (float)((REPLAY_HUD_BOTTOM_Y + 10) - 8); /* clearance = 10, BASE_Y = 8 */
            if (lift_target > 0.0f) target = lift_target;
        }
        vp->replay_lift_px += (target - vp->replay_lift_px) * 0.22f; /* LIFT_EASE = 0.22f */
        if (fabsf(target - vp->replay_lift_px) < 0.25f) { /* LIFT_SNAP_PX = 0.25f */
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
        if (status->ttl > 0)
            status->ttl--;

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
