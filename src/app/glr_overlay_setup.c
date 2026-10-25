/*
 * glr_overlay_setup.c - per-frame setup for the 3D edit overlays.
 *
 * Turns live REPL + editor + presentation state into the inputs the overlay
 * passes in src/subsystems/edit_overlays/ consume: the cursor's block state,
 * the OverlayWalkCtx the outline / vertex-point / label / normal-arrow passes
 * walk, the cursor-line guide snapshot, and the OverlaySnapshotPack the
 * controller hands the render3d post-overlay hooks as user_data. The overlay
 * subsystem itself reads no controller state - everything it sees crosses
 * this seam. See glr_overlay_setup.h.
 */
#include "app/glr_overlay_setup.h"

#include <string.h>

#include "app/glr_ctrl.h"            /* glr_ctrl_depth_snapshot_view */
#include "app/glr_state.h"
#include "editor/input.h"
#include "editor/state.h"
#include "gl_includes.h"
#include "repl/eval.h"
#include "repl/flatten_query.h"
#include "repl/source_scope.h"
#include "repl/state.h"
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"

/* Parse up to `max_slots` comma-separated arg slots out of `src`, recording
 * which positions are actually present. repl_eval_parse_exprs() skips leading
 * commas so ",1," and "1," look identical to it; this version tracks
 * slot indices. Empty slots leave filled[i]=0, out[i] unchanged. Callers
 * pass 3 (glVertex / glTranslatef / glScalef) or 4 (glRotatef angle+axis). */
static int parse_arg_slots(const char *src,
                           const ExprVar *predef_vars, int predef_var_count,
                           float *out, int *filled, int max_slots) {
    const char *s = src;
    int n_filled = 0;
    for (int i = 0; i < max_slots; i++) filled[i] = 0;

    for (int slot = 0; slot < max_slots; slot++) {
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

/* Resolve the cursor-line geometry args into floats so the scene
 * module can render its guides without depending on repl_eval. Sets the
 * pre-parsed fields on `snapshot` based on `input`. */
static void fill_guide_arg_slots(Render3dGuideSnapshot *snapshot,
                                 const char *input, int input_len) {
    snapshot->vertex_n_filled = 0;
    snapshot->raster_pos_n_filled = 0;
    snapshot->normal_n_filled = 0;
    snapshot->vertex_filled[0] = snapshot->vertex_filled[1] =
        snapshot->vertex_filled[2] = 0;
    snapshot->clip_plane_idx = -1;
    snapshot->clip_plane_n_filled = 0;
    snapshot->clip_plane_cap_enabled = 0;

    if (!input) return;

    ReplPredefView predef = repl_eval_predef_view();

    const char *vertex_args = NULL;
    if (strncmp(input, "glVertex3f(", 11) == 0 && input_len > 11) {
        vertex_args = input + 11;
    } else if (strncmp(input, "glVertex4f(", 11) == 0 && input_len > 11) {
        vertex_args = input + 11;
    } else if (strncmp(input, "glVertex2f(", 11) == 0 && input_len > 11) {
        vertex_args = input + 11;
    } else if (strncmp(input, "gluVertex(", 10) == 0 && input_len > 10) {
        vertex_args = input + 10;
    }
    if (vertex_args) {
        snapshot->vertex_n_filled = parse_arg_slots(
            vertex_args, predef.vars, predef.count,
            snapshot->vertex_args, snapshot->vertex_filled, 3);
    }

    if (strncmp(input, "glRasterPos3f(", 14) == 0 && input_len > 14) {
        int raster_filled[3];
        snapshot->raster_pos_n_filled = parse_arg_slots(
            input + 14, predef.vars, predef.count,
            snapshot->raster_pos_args, raster_filled, 3);
    }

    /* Live transform args (glTranslatef / glScalef / glRotatef), parsed the
     * same way so transform guides can track the cursor line before commit.
     * Rotate carries a 4th slot (angle + axis); translate/scale use 3. */
    snapshot->xform_n_filled = 0;
    snapshot->xform_filled[0] = snapshot->xform_filled[1] =
        snapshot->xform_filled[2] = snapshot->xform_filled[3] = 0;
    const char *xform_args = NULL;
    int xform_max = 0;
    if (strncmp(input, "glTranslatef(", 13) == 0) {
        xform_args = input + 13; xform_max = 3;
    } else if (strncmp(input, "glScalef(", 9) == 0) {
        xform_args = input + 9; xform_max = 3;
    } else if (strncmp(input, "glRotatef(", 10) == 0) {
        xform_args = input + 10; xform_max = 4;
    }
    if (xform_args) {
        snapshot->xform_n_filled = parse_arg_slots(
            xform_args, predef.vars, predef.count,
            snapshot->xform_args, snapshot->xform_filled, xform_max);
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

    /* Live clip-plane args. The plane digit lands the guide as soon as
     * it's typed; coefficients follow after the first comma, with an
     * optional (GLdouble[]){ prefix (the canonical committed form). */
    if (strncmp(input, "glClipPlane(GL_CLIP_PLANE", 25) == 0 &&
        input_len > 25 && input[25] >= '0' && input[25] <= '5') {
        snapshot->clip_plane_idx = input[25] - '0';
        const char *cp_args = strchr(input + 25, ',');
        if (cp_args) {
            int cp_filled[4];
            cp_args++;
            while (*cp_args == ' ' || *cp_args == '\t') cp_args++;
            if (strncmp(cp_args, "(GLdouble[]){", 13) == 0)
                cp_args += 13;
            snapshot->clip_plane_n_filled = parse_arg_slots(
                cp_args, predef.vars, predef.count,
                snapshot->clip_plane_args, cp_filled, 4);
        }
        /* Net enable state for this plane's cap: last glEnable/glDisable
         * in document order wins. Loop/if nesting is ignored - this is a
         * hint for the guide's dimmed "cap off" styling, not execution. */
        {
            GLenum cap = (GLenum)(GL_CLIP_PLANE0 + snapshot->clip_plane_idx);
            for (int i = 0; i < snapshot->source_cmd_count; i++) {
                const GLCmd *cmd = &snapshot->source_cmds[i];
                if (!cmd->valid) continue;
                if (cmd->type == CMD_ENABLE && (GLenum)cmd->args[0] == cap)
                    snapshot->clip_plane_cap_enabled = 1;
                else if (cmd->type == CMD_DISABLE && (GLenum)cmd->args[0] == cap)
                    snapshot->clip_plane_cap_enabled = 0;
            }
        }
    }
}

/* Build a guide-render snapshot from live REPL + editor state. The
 * Render3dRenderConfig argument is consulted only for fields the
 * controller already supplies (alpha_scale);
 * everything else comes from REPL/editor accessors so this can run
 * without a per-frame config pointer if needed. */
static Render3dGuideSnapshot glr_overlay_build_guide_snapshot(const Render3dRenderConfig *config) {
    GlrPresentationState presentation = glr_state_presentation();
    ReplVariableView vars = repl_state_variables();
    EditorInputView input = editor_state_input();
    int edit_line = editor_state_edit_line();

    Render3dGuideSnapshot snapshot = {
        .show_guides = (presentation.xform_guide_mode != RENDER3D_XFORM_GUIDE_OFF),
        .replaying = replay_active(),
        .replay_focus_anchor_flat_idx = replay_focus_anchor_flat_idx(),
        .xform_guide_mode = presentation.xform_guide_mode,
        .anim_time = vars.anim_time,
        .input = input.input,
        .input_len = input.input_len,
        .cursor_pos = input.cursor_pos,
        .edit_line_idx = edit_line,
        .prefer_last_instance =
            (presentation.overlay_scope == OVERLAY_SCOPE_LAST_INSTANCE ||
             presentation.overlay_scope == OVERLAY_SCOPE_SINGLE_POLYGON),
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

/* The frame's pack: filled by glr_overlay_build_pack(), read by the render3d
 * post-overlay hooks through the pointer glr_overlay_pack() hands out. */
static OverlaySnapshotPack g_overlay_pack;

OverlaySnapshotPack *glr_overlay_pack(void) {
    return &g_overlay_pack;
}

/* The cursor half of the overlay walk context: which flat / source block the
 * 3D overlays (outline highlight, vertex labels) treat as the cursor's. One
 * fill for the frame and for tests, so the gutter/scene label parity test
 * walks with exactly what the label pass walks with. Reads the flat block
 * cache as-is; the frame path refreshes it before the pack is built. */
void glr_overlay_fill_cursor_block_state(int edit_line,
                                         CursorBlockState *out) {
    out->edit_line_idx = edit_line;
    out->cursor_block_begin = repl_state_flat_program_current_block_begin();
    out->cursor_block_end = repl_state_flat_program_current_block_end();
    ReplCursorScope scope;
    repl_source_scope_cursor_scope(edit_line, &scope);
    out->cursor_source_block_valid = scope.begin_block.valid;
    out->cursor_source_block_begin = scope.begin_block.start;
    out->cursor_source_block_end = scope.begin_block.end;
    out->cursor_source_tess_valid = scope.tess_polygon.valid;
    out->cursor_source_tess_begin = scope.tess_polygon.start;
    out->cursor_source_tess_end = scope.tess_polygon.end;
    out->cursor_func_scope_mask = 0;
}

void glr_overlay_fill_walk_ctx(int edit_line, OverlayWalkCtx *out) {
    GlrPresentationState presentation = glr_state_presentation();
    int replaying = replay_active();
    int replay_mode_vertex = replaying && (replay_mode() == REPLAY_MODE_VERTEX);

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->program = repl_state_flat_program_view();
    glr_overlay_fill_cursor_block_state(edit_line, &out->cursor);
    out->cursor_overlay_scope = presentation.overlay_scope;

    /* cursor_poly_* / cursor_contour_* stay zero (= whole block) outside
     * single-polygon scope. */
    if (presentation.overlay_scope == OVERLAY_SCOPE_SINGLE_POLYGON) {
        ReplCursorPolygon poly =
            repl_flatten_cursor_polygon(out->cursor.edit_line_idx);
        out->cursor_poly_valid = poly.valid;
        out->cursor_poly_first = poly.first;
        out->cursor_poly_last = poly.last;
        out->cursor_poly_fan_anchor = poly.fan_anchor;

        ReplCursorPolygon contour =
            repl_flatten_cursor_tess_contour(out->cursor.edit_line_idx);
        out->cursor_contour_valid = contour.valid;
        out->cursor_contour_first = contour.first;
        out->cursor_contour_last = contour.last;
    }

    /* Silhouette outlines only make sense over filled geometry; in any
     * wireframe mode the fill is replaced by edges, so suppress them. */
    out->show_vertex_outlines = presentation.show_vertex_outlines &&
                                presentation.wireframe == WIREFRAME_OFF;
    out->vertex_outline_style = presentation.vertex_outline_style;
    out->highlight_current_poly =
        presentation.highlight_current_poly != POLY_HIGHLIGHT_OFF && !replaying;
    out->highlight_clipped_culled =
        presentation.highlight_current_poly == POLY_HIGHLIGHT_CLIPPED_CULLED &&
        !replaying;
    out->highlight_glow = 1;
    out->anim_time = repl_state_variables().anim_time;
    out->replay_tess_preview = replay_mode_vertex;
    out->show_vertex_points = presentation.show_vertex_points;
    out->replay_vertex_points = replay_mode_vertex;
    out->replay_vertex_label = replay_mode_vertex && replay_vertex_label();
    out->show_normal_vectors = presentation.show_normal_vectors;
    out->replay_normal_display = replay_mode_vertex
                                 ? replay_normal_display()
                                 : REPLAY_NORMAL_DISPLAY_OFF;
    out->replay_anchor_flat_idx = replay_mode_vertex
                                  ? replay_focus_anchor_flat_idx() : -1;
    out->xform_guide_mode = presentation.xform_guide_mode;
}

void glr_overlay_build_pack(const Render3dRenderConfig *cfg) {
    OverlaySnapshotPack *pack = &g_overlay_pack;
    GlrPresentationState presentation = glr_state_presentation();

    glr_overlay_fill_walk_ctx(editor_state_edit_line(), &pack->walk);

    pack->snapshot = glr_overlay_build_guide_snapshot(cfg);
    pack->vertex_label_mode = (OverlayVertexLabelMode)presentation.show_vertex_labels;
    pack->overlay_scope = presentation.overlay_scope;
    pack->vertex_label_placement = presentation.vertex_label_placement;
    pack->depth_snapshot = glr_ctrl_depth_snapshot_view();
    pack->ortho_mode = presentation.ortho_mode;
    pack->show_normal_vectors = presentation.show_normal_vectors;
    pack->multisample_enabled = cfg ? cfg->multisample_enabled : 0;
    pack->line_smooth_enabled = cfg ? cfg->line_smooth_enabled : 0;
}
