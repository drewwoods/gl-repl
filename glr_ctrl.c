#include "glr_ctrl.h"

#include <errno.h>
#include <gl_includes.h>
#include <stdio.h>
#include <stdlib.h>

#include "color_picker.h"
#include "editor/input.h"
#include "editor/completion.h"
#include "editor/state.h"
#include "editor/help_session.h"
#include "editor/commit.h"
#include "editor/search.h"
#include "glr_actions.h"
#include "audio.h"
#include "repl_camera_controls.h"
#include "repl_core.h"
#include "repl_help_text.h"
#include "repl_debug.h"
#include "repl_eval.h"
#include "repl_executor.h"
#include "repl_export.h"
#include "keys.h"
#include "repl_pipeline.h"
#include "repl_replay_annotations.h"
#include "repl_source_scope.h"
#include "repl_state_owners.h"
#include "replay.h"
#include "replay_state.h"
#include "scene/render.h"
#include "scene/overlays.h"          /* scene_draw_vertex_number_label / _arrow primitives */
#include "geometry_guides.h"   /* geometry_guides_render_for_cursor */
#include "transform_guides.h"  /* transform_guides_prepare / _render_if_due */
#include "transform_utils.h"   /* apply_tracked_transform / unwind_transform_stack */
#include "outline_offset.h"    /* REPL_OUTLINE_POLYGON_OFFSET_{FACTOR,UNITS} */
#include "ui/autocomplete_panel.h"
#include "ui/editor.h"
#include "ui/tabbed_overlay.h"
#include "ui/layout.h"
#include "ui/menu_bar.h"
#include "ui/panels.h"
#include "ui/profile_panel.h"
#include "replay_ui_hud.h"
#include "ui/snapshot.h"
#include "ui/state.h"
#include "ui/variable_panel.h"
#include "variable_panel_state.h"
#include "variable_panel_drag.h"
#include "editor/clipboard.h"
#include "editor/code_panel_document.h"
#include "prof.h"
#include "ui/editor.h"
#include "ui/state_types.h"  /* UI-chrome typedefs (CodePanel/Camera/Help/etc.) */

static int glr_ctrl_cmd_is_focus_vertex(const GLCmd *cmd) {
    return cmd->valid &&
           (cmd->type == CMD_VERTEX3F || cmd->type == CMD_TESS_VERTEX);
}

static SceneFocusVertex glr_ctrl_build_focus_vertex(void) {
    SceneFocusVertex focus = { .valid = 0 };
    int edit_line = repl_state_edit_line();

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
 * commas so ",1," and "1," look identical to it; this version tracks slot
 * indices. Empty slots leave filled[i]=0, out[i] unchanged. */
static int parse_vertex_arg_slots(const char *src,
                                  const ExprVar *predef_vars, int predef_var_count,
                                  float out[3], int filled[3]) {
    const char *s = src;
    int n_filled = 0;
    filled[0] = filled[1] = filled[2] = 0;

    for (int slot = 0; slot < 3; slot++) {
        while (*s == ' ' || *s == '\t') s++;
        const char *start = s;
        while (*s && *s != ',' && *s != ')') s++;
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
    ReplPresentationState presentation = repl_state_presentation();
    ReplVariableView vars = repl_state_variables();
    ReplEditorInputView input = editor_state_input();
    int edit_line = repl_state_edit_line();

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

static const ReplTessPreviewCallbacks g_tess_preview_cb = {
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

    repl_replay_copy_baseline_predef_values(g_replay_fade_plan.baseline_predef_vals,
                                            MAX_PREDEF_VARS);
    repl_replay_copy_baseline_scratch_arrays(
        g_replay_fade_plan.baseline_scratch_arrays);

    if (!repl_replay_has_active_fades())
        return;

    g_replay_fade_plan_base_limit = repl_replay_fill_base_limit();
    fade_batches = repl_replay_fade_batches_view();
    batch_count = repl_replay_compute_fade_skip_limits(g_replay_fade_plan.skip_limits,
                                                       REPLAY_FADE_BATCH_MAX);
    if (batch_count > REPLAY_FADE_BATCH_MAX)
        batch_count = REPLAY_FADE_BATCH_MAX;

    g_replay_fade_plan.batch_count = batch_count;
    for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
        const ReplayFadeBatch *batch = &fade_batches.batches[batch_idx];
        g_replay_fade_plan.batches[batch_idx] = *batch;
        g_replay_fade_plan.batch_alpha[batch_idx] = repl_replay_batch_alpha(batch);
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
    GLfloat mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };
    GLfloat mshin[] = { 30.0f };
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    FlatProgramView program = repl_state_flat_program_view();
    EditorBufferView text = editor_buffer_view();

    for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
        const ReplayFadeBatch *batch = &plan->batches[batch_idx];
        float alpha = plan->batch_alpha[batch_idx];
        if (alpha <= 0.0f) continue;

        prof_begin(PROF_SCENE_3D_FADE_BATCH_PREP);
        glr_ctrl_replay_restore_baseline(plan);
        glColor4f(0.70f, 0.70f, 0.80f, alpha);
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
 * program through repl_replay_walk_tess_preview() and emitting line
 * strips at each transformed contour. The walker handles iteration and
 * matrix tracking; we own the visual GL state. */
static void glr_ctrl_render_replay_tess_preview(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.30f, 0.95f, 0.75f, 0.80f);
    glLineWidth(2.0f);

    repl_replay_walk_tess_preview(&g_tess_preview_cb, NULL);

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

static void on_vertex_number_label(const ReplVertexWalkState *state,
                                   float vx, float vy, float vz,
                                   void *user) {
    (void)user;
    scene_draw_vertex_number_label(state->vertex_idx_in_block, vx, vy, vz);
}

static void on_normal_vector_arrow(const ReplVertexWalkState *state,
                                   float vx, float vy, float vz,
                                   void *user) {
    float scale = *(const float *)user;
    scene_draw_normal_vector_arrow(vx, vy, vz,
                                   state->normal[0],
                                   state->normal[1],
                                   state->normal[2],
                                   scale);
}

static ReplVertexWalkContext glr_ctrl_build_vertex_walk_context(int selected_block_only) {
    ReplPresentationState presentation = repl_state_presentation();
    (void)presentation;  /* might be useful for future overlay variants */

    ReplVertexWalkContext ctx = {
        .program                = repl_state_flat_program_view(),
        .edit_line_idx          = repl_state_edit_line(),
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
    glColor3f(1.0f, 1.0f, 0.30f);

    static const ReplVertexWalkCallbacks cb = {
        .on_vertex = on_vertex_number_label,
    };
    ReplVertexWalkContext ctx = glr_ctrl_build_vertex_walk_context(1);
    repl_walk_user_vertices(&ctx, &cb, NULL);

    glPopAttrib();
}

static void glr_ctrl_render_normal_vectors(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glColor3f(0.80f, 0.80f, 0.30f);

    static const ReplVertexWalkCallbacks cb = {
        .on_vertex = on_normal_vector_arrow,
    };
    float scale = 0.35f;
    ReplVertexWalkContext ctx = glr_ctrl_build_vertex_walk_context(0);
    repl_walk_user_vertices(&ctx, &cb, &scale);

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
 * — tiny inline GLCmd→GL helpers in scene/transform_utils.h. Pending
 * follow-up: relocate that header to REPL territory so this dependency
 * goes away. */

typedef struct OverlayWalkCtx {
    FlatProgramView program;
    int             edit_line_idx;
    int             cursor_block_begin;
    int             cursor_block_end;
    unsigned int    cursor_func_scope_mask;
    int             show_vertex_outlines;
    int             show_current_poly;
    int             replay_tess_preview;
    int             show_vpoints;
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

    if (ctx->show_vertex_outlines || ctx->show_current_poly) {
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
                        glColor3f(0.0f, 0.9f, 0.9f);
                    else
                        glColor3f(0.55f, 0.20f, 0.70f);
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
                int draw_outline = outline_begin_mode_has_overlay(cmds[i].mode);
                if (in_begin) glEnd();
                block_is_current = ctx->show_current_poly &&
                                   outline_block_matches_cursor(i, 0, ctx);
                if (block_is_current) {
                    glLineWidth(3.0f);
                    glColor3f(0.0f, 0.9f, 0.9f);
                } else if (ctx->show_vertex_outlines && draw_outline) {
                    glLineWidth(1.2f);
                    glColor3f(0.0f, 0.0f, 0.0f);
                } else {
                    in_begin = 0;
                    break;
                }
                glBegin(cmds[i].mode);
                in_begin = 1;
                break;
            }
            case CMD_END:
                if (in_begin) {
                    glEnd();
                    in_begin = 0;
                    glLineWidth(1.0f);
                    glColor3f(0.0f, 0.0f, 0.0f);
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
                tess_poly_is_current = ctx->show_current_poly &&
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
    if (!ctx->show_vpoints && !ctx->replay_vertex_points) return;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);

    if (ctx->replay_vertex_points)
        glColor4f(1.0f, 0.88f, 0.20f, 0.75f);
    else
        glColor4f(0.05f, 0.05f, 0.10f, 0.80f);

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
                primitive_mode = flat_cmds[i].mode;
            } else if (flat_cmds[i].type == CMD_END) {
                primitive_mode = 0;
            } else if (flat_cmds[i].type == CMD_VERTEX3F ||
                       flat_cmds[i].type == CMD_VERTEX2F ||
                       flat_cmds[i].type == CMD_TESS_VERTEX) {
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
} CursorGuideRenderCtx;

static void on_cmd_render_cursor_guides(const ReplVertexWalkState *state,
                                         void *user) {
    CursorGuideRenderCtx *ctx = (CursorGuideRenderCtx *)user;
    int is_cursor = (state->src_cmd_idx == ctx->snapshot->edit_line_idx);

    if (is_cursor && !ctx->snapshot->replaying)
        geometry_guides_render_for_cursor(ctx->snapshot);

    transform_guides_render_if_due(ctx->snapshot, &ctx->xform_plan,
                                         state->flat_cmd_idx, ctx->cam_view);
}

static void glr_ctrl_render_cursor_guides(const SceneGuideSnapshot *snapshot) {
    if (!snapshot || !snapshot->show_guides) return;

    CursorGuideRenderCtx ctx;
    ctx.snapshot = snapshot;
    transform_guides_prepare(snapshot, &ctx.xform_plan);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glPushMatrix();
    glGetFloatv(GL_MODELVIEW_MATRIX, ctx.cam_view);

    static const ReplVertexWalkCallbacks cb = {
        .on_each_cmd = on_cmd_render_cursor_guides,
    };
    ReplVertexWalkContext walk = glr_ctrl_build_vertex_walk_context(0);
    repl_walk_user_vertices(&walk, &cb, &ctx);

    glPopMatrix();
    glPopAttrib();
}

static void glr_ctrl_post_overlays(void *user_data) {
    const SceneRenderConfig *cfg = (const SceneRenderConfig *)user_data;
    ReplPresentationState presentation = repl_state_presentation();
    int replaying = replay_active();
    int replay_mode_vertex = replaying && (replay_mode() == REPLAY_MODE_VERTEX);

    OverlayWalkCtx walk = {
        .program                = repl_state_flat_program_view(),
        .edit_line_idx          = repl_state_edit_line(),
        .cursor_block_begin     = repl_state_flat_program_current_block_begin(),
        .cursor_block_end       = repl_state_flat_program_current_block_end(),
        .cursor_func_scope_mask = 0,  /* not currently surfaced via repl_state */
        .show_vertex_outlines   = presentation.show_vertex_outlines,
        .show_current_poly      = presentation.highlight_current_poly && !replaying,
        .replay_tess_preview    = replay_mode_vertex,
        .show_vpoints           = presentation.show_vertex_points,
        .replay_vertex_points   = replay_mode_vertex,
    };

    int multisample = cfg ? cfg->multisample_enabled : 0;
    int line_smooth = cfg ? cfg->line_smooth_enabled : 0;
    glr_ctrl_render_outlines(&walk, multisample, line_smooth);
    glr_ctrl_render_vertex_points(&walk);

    /* Cursor edit guides need the full snapshot (input string, predef
     * vars, source cmds). Build it once here and feed the walker. */
    SceneGuideSnapshot snapshot = glr_ctrl_build_guide_snapshot(cfg);
    glr_ctrl_render_cursor_guides(&snapshot);

    if (presentation.show_vertex_labels)
        glr_ctrl_render_vertex_numbers();
    if (presentation.show_normal_vectors)
        glr_ctrl_render_normal_vectors();
}

static void glr_ctrl_push_highlights(void) {
    editor_state_highlights_clear();

    int doc_count = repl_state_document_count();
    int edit_line = repl_state_edit_line();
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

    EditorBufferView view = editor_buffer_view();
    int n = repl_state_document_count();
    for (int i = 0; i < n; i++) {
        char display[MAX_LINE_OVERRIDE_TEXT];
        if (!repl_replay_code_panel_get_command_display_text(view, i, display,
                                                             sizeof(display)))
            continue;
        const char *base = editor_buffer_view_line(view, i);
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
        EditorTransformer t = {
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
                .is_clear = (cmd->type == CMD_CLEAR_COLOR),
            },
        };
        if (!editor_state_transformers_append(&t))
            break;
    }
}

/* Browser autoplay policy: the Web Audio context stays suspended until
 * a user gesture. The very first key / mouse / special event after
 * startup fires audio_on_user_gesture; native builds make this a
 * no-op. Phase J1 commit 48a relocated this from editor_input.c. */
static int g_audio_gesture_sent = 0;

static void glr_ctrl_notify_audio_gesture_once(void) {
    if (g_audio_gesture_sent) return;
    g_audio_gesture_sent = 1;
    audio_on_user_gesture();
}

static void glr_ctrl_apply_input_effects(ReplInputDispatchEffects effects) {
    if (effects.set_cursor)
        glutSetCursor(effects.cursor);
    if (effects.request_redraw)
        glutPostRedisplay();
    if (effects.schedule_timer)
        glutTimerFunc(effects.timer_millis, glr_ctrl_timer, effects.timer_value);
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
        .text           = editor_buffer_view(),
    });
    glPopAttrib();
}

static void glr_ctrl_build_scene_config(SceneRenderConfig *config) {
    ReplRenderState render = repl_state_render();
    ReplPresentationState presentation = repl_state_presentation();
    ReplCameraState cam = ui_state_camera();
    const float *grid_major_steps = repl_state_grid_major_steps();
    const float *grid_extents = repl_state_grid_extents();
    float bg_lum;
    float as_val;

    /* Refresh cursor block highlight before reading cursor state */
    repl_flatten_refresh_current_block_highlight();

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
        float cr = 0.10f, cg = 0.10f, cb = 0.13f, ca = 1.0f;
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

    /* --- Rendering quality --- */
    config->multisample_enabled = render.multisample_enabled;
    config->line_smooth_enabled = render.line_smooth_enabled;
    config->use_accum = render.use_accum;
    config->accum_aa_enabled = render.accum_aa_enabled;
    config->accum_samples = render.accum_samples;

    /* --- Lighting --- */
    config->user_lighting_enabled = repl_state_flat_program_user_lighting_enabled();
    memcpy(config->lights, render.lights, sizeof(config->lights));
    config->show_light_indicators = presentation.show_light_indicators;

    /* --- Environment --- */
    config->backdrop_mode = presentation.backdrop_mode;
    config->wireframe = presentation.wireframe;

    /* --- Grid and axes --- */
    config->grid_theme = presentation.grid_theme;
    config->grid_extent_idx = presentation.grid_extent_idx;
    config->grid_major_idx = presentation.grid_major_idx;
    config->axes_theme = presentation.axes_theme;
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

    /* Alpha scale boost for dark backgrounds */
        bg_lum = 0.2126f * render.clear_color[0]
            + 0.7152f * render.clear_color[1]
            + 0.0722f * render.clear_color[2];
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

static void glr_ctrl_build_ui_snapshot(UiRenderSnapshot *snap) {
    FlatProgramView flat_program;
    ReplPredefView predef;
    memset(snap, 0, sizeof(*snap));

    /* Refresh derived workspace header lines so the import/export view
     * reflects the current frame before rendering reads it. */
    repl_state_refresh_workspace_header_lines();

    /* Mirror chrome-relevant presentation fields into ui_state.code_panel
     * so ui_*.c renderers and hit-tests can read them via ui_state_*()
     * without crossing the repl_state_*() boundary. */
    repl_state_sync_ui_chrome();

    snap->viewport       = ui_state_viewport();
    snap->code_panel     = ui_state_code_panel();
    snap->help           = ui_state_help();
    snap->help_session   = editor_help_session_view();
    snap->variable_panel = variable_panel_view();
    snap->profile_panel  = ui_state_profile_panel();
    snap->status         = ui_state_status();
    snap->search         = editor_state_search();
    snap->autocomplete   = editor_state_autocomplete();
    snap->pointer        = ui_state_pointer();
    snap->render         = repl_state_render();
    snap->replay         = replay_state_view();
    snap->scenes         = repl_state_scenes();
    snap->scroll         = editor_state_scroll();
    snap->color_picker   = color_picker_view();

    snap->editor_input   = editor_state_input();
    snap->import_export  = repl_state_import_export();
    flat_program         = repl_state_flat_program_view();
    predef = repl_eval_predef_view();
    glr_ctrl_fill_ui_variable_panel_vars(snap, predef);

    snap->document_cmds       = repl_state_document_cmds();
    snap->document_count      = repl_state_document_count();
    snap->edit_line           = repl_state_edit_line();

    snap->flat_program_count  = flat_program.cmd_count;
    snap->anim_time           = repl_state_variables().anim_time;

    snap->user_scene_active_idx   = repl_active_user_scene();

    snap->help_content = repl_help_text_build();
    snap->editor_transformers = editor_state_transformers();
    snap->editor_highlights = editor_state_highlights();
    snap->editor_virtual_lines = editor_state_virtual_lines();

    /* Selection range materialized once for the per-row code-panel branch. */
    snap->selection_active = editor_clipboard_sel_active();
    snap->selection_lo     = snap->selection_active ? editor_clipboard_sel_lo() : -1;
    snap->selection_hi     = snap->selection_active ? editor_clipboard_sel_hi() : -1;

    /* Indent + statusbar metadata so the render path does not call back
     * into repl_source_scope_* / repl_code_panel_document_* per row. */
    snap->active_indent_chars   = repl_code_panel_document_active_indent_chars();
    snap->trailing_indent_chars = repl_source_scope_cmd_indent_chars(snap->document_count);
    snap->in_begin_block        = repl_source_scope_in_begin_block();
    snap->current_begin_mode    = current_begin_mode();
}

void glr_ctrl_display_frame(void) {
    int saved_flat_count;
    float live_predef_vals[MAX_PREDEF_VARS] = { 0 };
    float live_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN] = { { 0.0f } };
    FlatProgramView flat_program = repl_state_flat_program_view();
    int g_num_flat_cmds = flat_program.cmd_count;
    /* Capture replay state once before repl_replay_prepare_frame so the
     * HUD shows the per-frame "before-prepare" view (the contract that
     * test_imrepl_ctrl pins). Per-field narrow accessors elsewhere in
     * the frame are reading post-prepare state, which is what they want. */
    ReplReplayRuntimeState frame_replay = replay_state_view();
    SceneRenderConfig scene_config;
    UiRenderSnapshot ui_snap;

    prof_frame_tick();
    prof_begin(PROF_FRAME_TOTAL);

    if (repl_state_normals_dirty()) {
        prof_begin(PROF_AUTONORMAL);
        recompute_autonormals();
        repl_state_normals_dirty_clear();
        prof_end(PROF_AUTONORMAL);
    }
    if (repl_state_flat_program_dirty()) {
        prof_begin(PROF_FLATTEN);
        flatten_commands();
        repl_state_flat_program_clear_dirty();
        prof_end(PROF_FLATTEN);
        flat_program = repl_state_flat_program_view();
        g_num_flat_cmds = flat_program.cmd_count;
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
     * data; the editor is agnostic to which feature pushed them. */
    prof_begin(PROF_SNAPSHOT_VIRTUAL_LINES);
    repl_replay_annotations_prepare(editor_buffer_view());
    glr_ctrl_push_line_overrides();
    prof_end(PROF_SNAPSHOT_VIRTUAL_LINES);

    /* Per-frame prep that sits between virtual-line refresh and
     * scene-config build: replay state-machine prepare_frame plus
     * the import/export render-state and camera string refresh.
     * Wrapped in its own subsection so the SNAPSHOT_* subsections
     * sum to PROF_SNAPSHOT exactly. */
    prof_begin(PROF_SNAPSHOT_PREP);
    saved_flat_count = g_num_flat_cmds;
    repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(live_scratch_arrays);
    if (replay_active())
        repl_state_flat_program_set_count(repl_replay_prepare_frame(saved_flat_count));

    update_render_state_strings();
    update_cam_lines();
    prof_end(PROF_SNAPSHOT_PREP);

    prof_begin(PROF_SNAPSHOT_SCENE_CONFIG);
    glr_ctrl_build_scene_config(&scene_config);
    prof_end(PROF_SNAPSHOT_SCENE_CONFIG);

    prof_begin(PROF_SNAPSHOT_UI);
    glr_ctrl_build_ui_snapshot(&ui_snap);
    prof_end(PROF_SNAPSHOT_UI);

    prof_end(PROF_SNAPSHOT);

    /* 3D scene - scene_render_3d_scene() handles optional accumulation-buffer AA */
    /* Reset subsection accumulators so timings across all AA samples sum up
     * correctly before the first (or only) scene_render_3d_scene() call. */
    for (ProfSection section_idx = PROF_SCENE_3D_SETUP; section_idx <= PROF_SCENE_3D_HUD; section_idx++)
        prof_accum_reset(section_idx);
    prof_begin(PROF_SCENE_3D);
    {
        ReplCameraState cam = ui_state_camera();
        scene_apply_camera(cam.rx, cam.ry, cam.dist, cam.tx, cam.ty, cam.tz);
    }
    if (scene_render_3d_scene(&scene_config) != 0) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                    "imrepl_ctrl: scene_render_3d_scene rejected config (errno=%d)\n",
                    errno);
            warned = 1;
        }
    }
    prof_end(PROF_SCENE_3D);

    int frame_replaying = replay_active();
    if (frame_replaying) {
        prof_begin(PROF_REPLAY_HUD);
        ReplPresentationState presentation = repl_state_presentation();
        UiReplayHudState replay_hud_state = {
            .scene_x = scene_config.scene_x,
            .scene_y = scene_config.scene_y,
            .scene_w = scene_config.scene_w,
            .scene_h = scene_config.scene_h,
            .viewport_w = scene_config.viewport_w,
            .viewport_h = scene_config.viewport_h,
            .code_panel_layout = presentation.code_panel_layout,
            .replay_mode = replay_mode(),
            .replay_pc = frame_replay.pc,
            .replay_total_cmds = frame_replay.total_flat_cmds,
            .replay_state_val = frame_replay.state,
            .replay_speed = frame_replay.speed,
            .replay_expand_args = frame_replay.expand_args,
            .replaying = frame_replaying,
        };
        replay_ui_hud_render(&replay_hud_state);
        prof_end(PROF_REPLAY_HUD);
    }

    /* Commit the accumulated subsection totals now that all AA samples are done. */
    for (ProfSection section_idx = PROF_SCENE_3D_SETUP; section_idx <= PROF_SCENE_3D_HUD; section_idx++)
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

void glr_ctrl_init_gl(void) {
    repl_state_init_defaults();
    ensure_init_bootstrap_ready();
    scene_render_init_gl();
    repl_executor_init_resources();
    apply_init_bootstrap();
    /* glutInit has run by the time glr_ctrl_init_gl is called.
     * Unlock glutGetModifiers() reads in editor_input so Cmd / Ctrl /
     * Shift modifier checks land. Tests skip this hook so modifier
     * reads default to "no modifiers held" instead of aborting
     * freeglut for being called pre-init. */
    editor_input_enable_glut_modifier_reads();
    /* App-level config: tell the editor what comment prefix to use
     * for Ctrl+/ toggle. Editor itself has no default — tests that
     * exercise the toggle key path set the prefix explicitly. */
    editor_set_line_comment_prefix("// ");
}

void glr_ctrl_bootstrap_repl(const char *input_file) {
    repl_eval_init_predef_vars();
    for (int i = 0; i < g_num_predef_vars; i++) {
        if (strcmp(g_predef_vars[i].name, "t") == 0) {
            repl_state_variables_mut()->time_var_idx = i;
            break;
        }
    }
    repl_load_initial_commands(input_file);
}

void glr_ctrl_set_accum(int enabled) {
    repl_state_render_mut()->use_accum = enabled ? 1 : 0;
}

/* ===========================================================================
 * Router helpers: non-editor input concerns
 *
 * imrepl_ctrl is the controller — it owns routing of raw GLUT input to
 * the subsystem that owns each concern (replay, audio, config, save,
 * scene cycle, variable panel, scene press, camera, scroll wheel,
 * help). The editor's keyboard_func / special_func / mouse_func /
 * motion_func / mousewheel_func dispatchers see only editor-text
 * concerns: every helper below is run before the editor handler.
 *
 * Helpers are exported (declared in imrepl_ctrl.h) so test fixtures
 * can drive a single routing concern without applying GLUT effects.
 * Helpers fill the editor_input ReplInputDispatchEffects via
 * editor_request_redraw etc.; glutPostRedisplay / glutSetCursor /
 * glutTimerFunc fire only from glr_ctrl_apply_input_effects, which
 * the dispatch entry points call after the helpers. Test fixtures
 * bypass apply_input_effects entirely.
 * ===========================================================================
 */

/* ---- Keyboard router helpers ------------------------------------------ */

int glr_ctrl_router_handle_save_key(unsigned char key) {
    if (key == KEY_CTRL_S) {
        repl_save_default_output();
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_debug_dump_key(unsigned char key) {
    if (key == KEY_CTRL_P) {
        repl_debug_dump_editor(stdout, editor_buffer_view());
        repl_debug_dump_flat_commands(stdout, editor_buffer_view());
        set_status("Dumped editor + flat commands to stdout");
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_quit_key(unsigned char key) {
    if (key == KEY_CTRL_Q) {
        repl_export_save_output("/tmp/temp-output.c", editor_buffer_view());
        printf("Saved to %s\n", "/tmp/temp-output.c");
        exit(0);
    }
    return 0;
}

int glr_ctrl_router_handle_config_menu_key(unsigned char key) {
    if (!editor_state_search().active && key == '`') {
        if (replay_active())
            repl_replay_stop();
        editor_input_restore_hidden_code_panel();
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

int glr_ctrl_router_handle_accum_samples_key(unsigned char key) {
    static const int g_accum_steps[] = { 1, 2, 4, 8, 16 };
    ReplRenderState *rs = repl_state_render_mut();
    if (key == '=' || key == '+') {
        if (!(editor_input_active_modifiers() & GLUT_ACTIVE_CTRL))
            return 0;
        if (rs->use_accum) {
            for (int i = 0; i < ACCUM_STEP_COUNT - 1; i++) {
                if (rs->accum_samples <= g_accum_steps[i]) {
                    rs->accum_samples = g_accum_steps[i + 1];
                    break;
                }
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "Accum samples: %d", rs->accum_samples);
            set_status(msg);
        }
        return 1;
    }

    if (key == KEY_CTRL_DASH ||
        (key == '-' && (editor_input_active_modifiers() & GLUT_ACTIVE_CTRL))) {
        if (rs->use_accum) {
            for (int i = ACCUM_STEP_COUNT - 1; i > 0; i--) {
                if (rs->accum_samples >= g_accum_steps[i]) {
                    rs->accum_samples = g_accum_steps[i - 1];
                    break;
                }
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "Accum samples: %d", rs->accum_samples);
            set_status(msg);
        }
        return 1;
    }
    return 0;
}

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
        audio_prev_track();
    else
        audio_next_track();
    return 1;
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
    case GLUT_KEY_UP:        editor_help_session_scroll_by(-1); return 1;
    case GLUT_KEY_DOWN:      editor_help_session_scroll_by(1);  return 1;
    case GLUT_KEY_PAGE_UP:   editor_help_session_scroll_by(-5); return 1;
    case GLUT_KEY_PAGE_DOWN: editor_help_session_scroll_by(5);  return 1;
    default: return 0;
    }
}

int glr_ctrl_router_handle_help_toggle_special(int key) {
    if (key == GLUT_KEY_F1) {
        ReplHelpState *help = ui_state_help_mut();
        help->visible = !help->visible;
        editor_help_session_set_tab(0);
        editor_help_session_set_scroll(0);
        return 1;
    }
    return 0;
}

static void cycle_example_or_user_scene(void) {
    /* F12 cycles: examples[0..N-1] -> user scenes (in slot order) -> back.
     * Active example moves to the next example, then first user scene.
     * Active user scene moves to the next occupied user slot, then example 0. */
    int count = repl_example_count();
    int active_scene = repl_active_user_scene();

    if (active_scene >= 0) {
        for (int scene_idx = active_scene + 1; scene_idx < MAX_USER_SCENES; scene_idx++) {
            if (repl_user_scene_slot_used(scene_idx)) {
                repl_load_user_scene_idx(scene_idx);
                return;
            }
        }
        if (count > 0)
            repl_load_example(0);
        return;
    }

    if (count > 0) {
        int next = repl_state_scenes().active_example_idx + 1;
        if (next < count) {
            repl_load_example(next);
            return;
        }
    }

    for (int scene_idx = 0; scene_idx < MAX_USER_SCENES; scene_idx++) {
        if (repl_user_scene_slot_used(scene_idx)) {
            repl_load_user_scene_idx(scene_idx);
            return;
        }
    }
    if (count > 0)
        repl_load_example(0);
}

int glr_ctrl_router_handle_scene_cycle_special(int key) {
    if (key == GLUT_KEY_F12) {
        cycle_example_or_user_scene();
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
    if (!ui_variable_panel_hit(x, y, &row_idx))
        return 0;
    if (replay_active())
        repl_replay_stop();
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
    if (ui_panels_handle_right_press(x, y)) {
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
    repl_camera_mouse_event(button, state, x, y, editor_input_active_modifiers());
    return 1;
}

static int glr_ctrl_apply_variable_panel_value_change(
        const VariablePanelValueChange *value_change) {
    ReplCompiledChange compiled;
    ReplCompileContext ctx;
    ReplVariableDragState drag;
    char err[REPL_STATUS_TEXT_MAX] = "";
    int var_idx;
    int capture_undo;

    if (!value_change || !value_change->name[0])
        return 1;

    drag = variable_panel_drag();
    var_idx = drag.var_idx;
    if (var_idx < 0 || var_idx >= g_num_predef_vars)
        return 1;
    if (strcmp(g_predef_vars[var_idx].name, value_change->name) != 0) {
        var_idx = repl_eval_find_predef_var_idx(value_change->name);
        if (var_idx < 0)
            return 1;
    }
    if (g_predef_vars[var_idx].value == value_change->value)
        return 1;

    ctx = repl_compile_context_from_live();
    if (repl_compile_set_predef_value(value_change->name, value_change->value,
                                      &ctx, &compiled,
                                      err, sizeof(err)) != REPL_COMPILE_OK) {
        set_status(err[0] ? err : "Variable update failed");
        return 1;
    }

    capture_undo = !variable_panel_drag_undo_snapshot_pushed();
    if (!editor_commit_apply_external_change(&compiled, capture_undo)) {
        set_status("Command buffer full!");
        return 1;
    }
    if (capture_undo)
        variable_panel_drag_mark_undo_snapshot_pushed();
    return 1;
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
    repl_camera_drag_motion(x, y);
    return 1;
}

int glr_ctrl_router_handle_camera_pointer_set(int x, int y) {
    repl_camera_pointer_set(x, y);
    return 1;
}

int glr_ctrl_router_handle_glut_scroll_wheel_button(int button, int state, int x, int y) {
#ifdef USE_GLUT
    if ((button != 3 && button != 4) || state != GLUT_DOWN)
        return 0;
    int direction = (button == 3) ? -1 : 1;
    if (ui_state_help().visible) {
        editor_help_session_scroll_by(direction);
    } else if (editor_input_point_in_code_panel(x, y)) {
        editor_input_code_panel_scroll(direction);
    } else {
        repl_camera_add_zoom_velocity(direction == -1 ? -0.3f : 0.3f);
    }
    editor_request_redraw();
    return 1;
#else
    (void)button; (void)state; (void)x; (void)y;
    return 0;
#endif
}

/* ---- Code-panel UiHit dispatch (Phase J2.2) -------------------------- */

/* Code-panel selection drag tracking. Press handlers set the anchor
 * to the clicked source-cmd row; motion re-runs ui_panels_hit_test
 * to derive the drag target and extends the editor selection.
 * Release on UP clears the active flag. The state lives here (not in
 * ui_panels.c) because UI input files report hit-test data only. */
static int g_code_panel_drag_active = 0;
static int g_code_panel_drag_anchor = -1;
static int g_code_panel_drag_moved  = 0;

void glr_ctrl_router_reset_code_panel_drag(void) {
    g_code_panel_drag_active = 0;
    g_code_panel_drag_anchor = -1;
    g_code_panel_drag_moved  = 0;
}

/* Common epilog for clicks that move the editor cursor: blink reset,
 * autocomplete clear, selection clear, redraw. Mirrors the legacy
 * ui_panels_handle_code_panel_click tail. */
static void route_code_click_epilog(void) {
    glr_action_cursor_blink_reset();
    editor_completion_clear();
    editor_clipboard_clear_selection();
    editor_request_redraw();
}

/* UI_HIT_CODE_TEXT: navigate to clicked line, set cursor column, arm
 * the selection drag anchor. */
static int route_code_text_hit(const UiHit *hit) {
    /* A non-swatch click on the code panel closes any open color picker
     * (matches legacy ui_panels_handle_code_panel_press behaviour). */
    color_picker_close();

    if (hit->line_idx >= 0)
        navigate_to_line(hit->line_idx);
    if (hit->char_idx >= 0)
        editor_cursor_pos_set(hit->char_idx);
    route_code_click_epilog();

    /* Arm drag anchor for committed lines only. */
    if (hit->line_idx >= 0 && hit->line_idx < repl_state_document_count()) {
        g_code_panel_drag_active = 1;
        g_code_panel_drag_anchor = hit->line_idx;
        g_code_panel_drag_moved  = 0;
    } else {
        glr_ctrl_router_reset_code_panel_drag();
    }
    return 1;
}

/* UI_HIT_CODE_INSERT_LINE: insertion virtual row in insert mode. Set
 * cursor column but do not navigate (matches legacy on_insert_line=1
 * behaviour). No drag anchor — the insert row is virtual. */
static int route_code_insert_line_hit(const UiHit *hit) {
    color_picker_close();
    if (hit->char_idx >= 0)
        editor_cursor_pos_set(hit->char_idx);
    route_code_click_epilog();
    glr_ctrl_router_reset_code_panel_drag();
    return 1;
}

/* UI_HIT_CODE_GUTTER: clicking the line-number column selects the
 * row. Same dispatch as CODE_TEXT minus the cursor-column move. */
static int route_code_gutter_hit(const UiHit *hit) {
    color_picker_close();
    if (hit->line_idx >= 0)
        navigate_to_line(hit->line_idx);
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
        color_picker_close();
    } else {
        color_picker_open(hit->line_idx, my);
    }
    editor_request_redraw();
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
    case REPL_MENU_BAR_PIN_REPLAY:
        replay_handle_pin_clicked();
        break;
    case REPL_MENU_BAR_PIN_SEARCH:
        handle_search_key(KEY_CTRL_F);
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
        int target = hit.line_idx; /* set to repl_state_edit_line() in J2.1 */
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
    /* A click outside the menu bar (anywhere that isn't UI_HIT_MENU_BUTTON
     * / UI_HIT_MENU_ITEM) dismisses an open dropdown — matches the legacy
     * "click outside dropdown closes it" behaviour from
     * ui_panels_handle_code_panel_press. */
    int dismissed_dropdown = 0;
    if (hit.kind != UI_HIT_MENU_BUTTON &&
        hit.kind != UI_HIT_MENU_ITEM &&
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
    case UI_HIT_PIN_BUTTON:
        consumed = route_pin_button_hit(&hit); break;
    case UI_HIT_MENU_BUTTON:
        consumed = route_menu_button_hit(&hit); break;
    case UI_HIT_MENU_ITEM:
        consumed = route_menu_item_hit(&hit); break;
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

    UiHit hit = ui_panels_hit_test(x, y);
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
            int cy = y;
            if (cx < cp_x + 1) cx = cp_x + 1;
            if (cx > cp_x + cp_w - 1) cx = cp_x + cp_w - 1;
            if (gl_y < cp_y + 1) cy = win_h - (cp_y + 1);
            if (gl_y > cp_y + cp_h - 1) cy = win_h - (cp_y + cp_h - 1);
            UiHit clamped = ui_panels_hit_test(cx, cy);
            target = code_panel_target_from_hit(clamped);
        }
    }
    if (target < 0)
        return 1;

    if (target != g_code_panel_drag_anchor || g_code_panel_drag_moved) {
        g_code_panel_drag_moved = 1;
        editor_selection_start(g_code_panel_drag_anchor);
        editor_selection_set_end(target);
        navigate_to_line(target);
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
        glr_ctrl_router_handle_quit_key(key)) {
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }

    glr_ctrl_apply_input_effects(editor_handle_key(key, x, y));
}

void glr_ctrl_special(int key, int x, int y) {
    glr_ctrl_notify_audio_gesture_once();

    if (editor_input_rename_capture_special(key)) {
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
        /* J2.2: classify the click via the canonical hit-test, then
         * route by UiHit.kind to the owning subsystem. The hit-test
         * covers variable panel, color picker, menu bar, code panel
         * (including divider + inline swatch + insert line) and pin
         * buttons. Only kinds that don't apply (UI_HIT_SCENE,
         * UI_HIT_NONE, UI_HIT_HELP_PANEL) fall through to scene
         * press / camera. */
        UiHit hit = ui_panels_hit_test(x, y);
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
 * ui_state_pointer via repl_camera_pointer_set(x, y) AFTER its own
 * work so the camera's view of the pointer stays current. The camera
 * branch deliberately does NOT pre-set the pointer because
 * repl_camera_drag_motion reads the previous (px, py) to compute
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
        ReplInputDispatchEffects pre_editor = editor_take_input_effects();
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
    editor_reset_input_effects();
    /* Passive motion (no button held) just updates the pointer
     * position — there's no drag delta to preserve. */
    glr_ctrl_router_handle_camera_pointer_set(x, y);
    ReplInputDispatchEffects editor_effects = editor_handle_passive_motion(x, y);
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
        editor_help_session_scroll_by(-direction);
        editor_request_redraw();
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }
    if (editor_input_point_in_code_panel(x, y)) {
        glr_ctrl_apply_input_effects(editor_handle_mousewheel(wheel, direction, x, y));
        return;
    }
    repl_camera_add_zoom_velocity(-(float)direction * 0.1f);
    editor_request_redraw();
    glr_ctrl_apply_input_effects(editor_take_input_effects());
#else
    (void)wheel; (void)direction; (void)x; (void)y;
#endif
}

/* Phase J1 commit 48b — timer dispatch inlined.
 *
 * Per-frame tick (16 ms): advance audio playlist, surface track-change
 * status, advance time variable, advance replay state, decay camera
 * momentum, blink the cursor, decay the status TTL. The body migrated
 * verbatim from repl_editor.c's timer_func.
 *
 * The work is split from the GLUT scheduling so test fixtures (which
 * don't initialize GLUT) can drive a single tick by calling
 * glr_ctrl_tick directly. The public timer entry adds
 * glutPostRedisplay + glutTimerFunc reschedule on top. */
void glr_ctrl_tick(void) {
    /* Advance the audio playlist if the current song reached its end
     * (no-op under loop=Song; see audio_tick). */
    audio_tick();

    /* When the playing track changes (either auto-advance from tick
     * or manual next/prev), surface the song name in the status bar.
     * Tracking by generation avoids needing a callback hook into
     * the audio module. */
    {
        static unsigned int last_track_gen = 0;
        unsigned int gen = audio_track_generation();
        if (gen != last_track_gen) {
            last_track_gen = gen;
            const char *path = audio_get_current_track();
            if (path && *path) {
                const char *base = strrchr(path, '/');
                base = base ? base + 1 : path;
                char msg[128];
                snprintf(msg, sizeof(msg), "Now playing: %s", base);
                set_status(msg);
            }
        }
    }

    repl_advance_time(0.016f);

    {
        ReplReplayRuntimeState *replay = replay_state_mut();

        if (replay->active)
            repl_replay_tick_fade_batches(0.016f);

        if (replay->active && replay->state == REPLAY_PLAYING) {
            replay->accum += replay->speed * 0.016f;
            while (replay->accum >= 1.0f &&
                   replay->state == REPLAY_PLAYING) {
                replay->accum -= 1.0f;
                repl_replay_advance();
            }
        }
    }

    repl_camera_tick();

    {
        ReplCodePanelRuntimeState *code_panel_state = ui_state_code_panel_mut();
        (code_panel_state->blink_tick)++;
        if (code_panel_state->blink_tick >= 30) {
            code_panel_state->blink_tick = 0;
            code_panel_state->cursor_visible = !code_panel_state->cursor_visible;
        }
    }

    {
        ReplStatusState *status = ui_state_status_mut();
        if (status->ttl > 0)
            status->ttl--;
    }
}

void glr_ctrl_timer(int value) {
    (void)value;
    glr_ctrl_tick();
    glutPostRedisplay();
    glutTimerFunc(16, glr_ctrl_timer, 0);
}
