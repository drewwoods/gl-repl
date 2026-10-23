/*
 * replay.c - Replay vertex/tessellation walkers.
 *
 * Core replay machine logic has been split into:
 *   - replay_fade.c: Fade batch ring
 *   - replay_input.c: Key handlers
 *   - replay_playback.c: Seek, advance, and speed control
 */
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_internal.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/pipeline.h"
#include "keys.h"
#include "repl/flatten.h"
#include "repl/flatten_query.h"
#include "repl/transform_utils.h" /* apply_tracked_transform / unwind_transform_stack */
#include "subsystems/replay/replay_state.h"

/* GLU tessellates a polygon at its closing gluEnd() - or at the end of the
 * program when it is left open - so every gluVertex in it is drawn with the
 * modelview current there, not at its own row. Writes that matrix to out
 * when a transform inside the polygon makes it differ from the one current
 * at the head. Returns 0 when no transform does, when a restart abandons the
 * polygon, or when the polygon pops a matrix pushed before it (unreachable
 * without unwinding the caller's stack; vertices then keep the running
 * matrix). */
static int replay_tess_close_modelview(FlatProgramView program, int head,
                                       float out[16]) {
    int depth = 1, push_depth = 0, moved = 0, close = program.cmd_count;

    for (int j = head + 1; j < program.cmd_count; j++) {
        const GLCmd *cmd = &program.cmds[j];
        if (!cmd->valid) continue;
        if (cmd->type == CMD_TESS_BEGIN_POLYGON) return 0;
        if (repl_cmd_is_transform(cmd->type)) {
            if (cmd->type == CMD_PUSH_MATRIX) push_depth++;
            else if (cmd->type == CMD_POP_MATRIX && --push_depth < 0) return 0;
            moved = 1;
            continue;
        }
        depth = repl_tess_advance_depth(cmd->type, depth);
        if (depth == 0) { close = j; break; }
    }
    if (!moved) return 0;

    int local_depth = 0;
    glPushMatrix();
    for (int j = head + 1; j < close; j++) {
        const GLCmd *cmd = &program.cmds[j];
        if (cmd->valid && repl_cmd_is_transform(cmd->type))
            apply_tracked_transform(cmd, &local_depth);
    }
    glGetFloatv(GL_MODELVIEW_MATRIX, out);
    unwind_transform_stack(&local_depth);
    glPopMatrix();
    return 1;
}

/* Close the preview's live strip, then apply the transforms GL could not
 * take while it was open. */
static void tess_preview_end_contour(const ReplayTessPreviewCallbacks *cb,
                                     void *user_data, FlatProgramView program,
                                     int contour_head, int end_idx,
                                     int close_valid, int *matrix_depth) {
    if (cb->end_contour)
        cb->end_contour(user_data);
    if (close_valid) glPopMatrix();
    for (int k = contour_head + 1; k < end_idx; k++)
        if (program.cmds[k].valid && repl_cmd_is_transform(program.cmds[k].type))
            apply_tracked_transform(&program.cmds[k], matrix_depth);
}

void replay_walk_tess_preview(const ReplayTessPreviewCallbacks *cb,
                                   void *user_data) {
    if (!cb) return;

    FlatProgramView program = repl_state_flat_program_view();
    int in_contour = 0;
    int contour_head = -1;
    int matrix_depth = 0;
    int tess_depth = 0;
    float close_mv[16];
    int close_valid = 0;

    glPushMatrix();
    for (int i = 0; i < program.cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        if (!cmd->valid) continue;

        /* A contour is a live line strip, where GL accepts no matrix
         * change; its transforms are applied once the strip ends. */
        if (repl_cmd_is_transform(cmd->type)) {
            if (!in_contour)
                apply_tracked_transform(cmd, &matrix_depth);
            continue;
        }

        int old_depth = tess_depth;
        tess_depth = repl_tess_advance_depth(cmd->type, tess_depth);
        switch (cmd->type) {
        case CMD_TESS_BEGIN_POLYGON:
            if (in_contour)
                tess_preview_end_contour(cb, user_data, program, contour_head,
                                         i, close_valid, &matrix_depth);
            in_contour = 0;
            close_valid = replay_tess_close_modelview(program, i, close_mv);
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (old_depth != 1 || tess_depth != 2) break;
            if (close_valid) { glPushMatrix(); glLoadMatrixf(close_mv); }
            if (cb->begin_contour)
                cb->begin_contour(user_data);
            in_contour = 1;
            contour_head = i;
            break;
        case CMD_TESS_VERTEX:
            if (in_contour && cb->vertex)
                cb->vertex(cmd->args[0], cmd->args[1], cmd->args[2], user_data);
            break;
        case CMD_TESS_END:
            if (in_contour)
                tess_preview_end_contour(cb, user_data, program, contour_head,
                                         i, close_valid, &matrix_depth);
            in_contour = 0;
            if (tess_depth == 0) close_valid = 0;
            break;
        default:
            break;
        }
    }
    if (in_contour)
        tess_preview_end_contour(cb, user_data, program, contour_head,
                                 program.cmd_count, close_valid, &matrix_depth);

    unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}

/* The vertex's three ordinals, from the numbering the flatten made, onto the
 * state a callback sees. The walker keeps no counters of its own. */
static void walk_state_set_ordinals(ReplayVertexWalkState *state,
                                    ReplVertexOrdinal ord) {
    state->vertex_idx_in_block = ord.run;
    state->immediate_idx_in_block = ord.imm;
    state->tess_idx_in_polygon = ord.tess;
}

/* Walk authored vertices using the shared flat/source cursor predicates.
 * Vertex callbacks receive the program's own vertex numbering
 * (FlatProgramView.vertex_ord). The rule that a glBegin strip owns the display
 * run inside a GLU polygon lives in that numbering, not in the walk. */
void replay_walk_user_vertices(const ReplayVertexWalkContext *ctx,
                             const ReplayVertexWalkCallbacks *cb,
                             void *user_data) {
    if (!ctx || !cb) return;

    FlatProgramView program          = ctx->program;
    int          selected_block_only = ctx->selected_block_only;
    int         *stop_flag           = ctx->stop_flag;

    ReplayVertexWalkState state = {
        .flat_cmd_idx        = -1,
        .src_cmd_idx         = -1,
        .primitive_mode      = 0,
        .in_block            = 0,
        .block_selected      = selected_block_only ? 0 : 1,
        .vertex_idx_in_block = REPL_VERTEX_ORDINAL_NONE,
        .immediate_idx_in_block = REPL_VERTEX_ORDINAL_NONE,
        .tess_idx_in_polygon = REPL_VERTEX_ORDINAL_NONE,
        .lighting_enabled    = 0,
        .normal              = { 0.0f, 0.0f, 1.0f },
        .tess_normal         = { 0.0f, 0.0f, 1.0f },
    };
    int matrix_depth = 0;
    int tess_depth   = 0;
    int in_begin_block = 0;
    int tess_poly_selected = selected_block_only ? 0 : 1;
    float tess_close_mv[16];
    int tess_close_valid = 0;

    glPushMatrix();
    for (int i = 0; i < program.cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        if (!cmd->valid) continue;

        state.flat_cmd_idx = i;
        state.src_cmd_idx  = cmd->src_cmd_idx;

        /* Callbacks see a gluVertex where the tessellator draws it. */
        int at_tess_close = tess_close_valid && cmd->type == CMD_TESS_VERTEX;
        if (at_tess_close) {
            glPushMatrix();
            glLoadMatrixf(tess_close_mv);
        }
        if (cb->on_each_cmd)
            cb->on_each_cmd(&state, user_data);
        if (stop_flag && *stop_flag) {
            if (at_tess_close) glPopMatrix();
            break;
        }

        if (!in_begin_block && repl_cmd_is_transform(cmd->type)) {
            apply_tracked_transform(cmd, &matrix_depth);
            continue;
        }

        switch (cmd->type) {
        case CMD_BEGIN:
            in_begin_block = 1;
            state.in_block = 1;
            state.primitive_mode = (GLenum)cmd->args[0];
            state.block_selected = selected_block_only
                ? repl_cursor_flat_block_matches(program, &ctx->cursor, i, 0) : 1;
            state.normal[0] = 0.0f; state.normal[1] = 0.0f; state.normal[2] = 1.0f;
            break;
        case CMD_END:
            if (tess_depth > 0) {
                state.in_block = 1;
                state.block_selected = tess_poly_selected;
            } else {
                state.in_block = 0;
                state.block_selected = selected_block_only ? 0 : 1;
            }
            in_begin_block = 0;
            state.primitive_mode = 0;
            break;
        case CMD_TESS_BEGIN_POLYGON:
            state.in_block = 1;
            state.primitive_mode = 0;
            state.block_selected = selected_block_only
                ? repl_cursor_flat_block_matches(program, &ctx->cursor, i, 1) : 1;
            /* Save the polygon verdict before a nested immediate head can
             * replace it, including when its vertices start inside a strip. */
            tess_poly_selected = state.block_selected;
            tess_close_valid = replay_tess_close_modelview(program, i, tess_close_mv);
            tess_depth = repl_tess_advance_depth(cmd->type, tess_depth);
            state.normal[0] = 0.0f; state.normal[1] = 0.0f; state.normal[2] = 1.0f;
            state.tess_normal[0] = 0.0f; state.tess_normal[1] = 0.0f; state.tess_normal[2] = 1.0f;
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            tess_depth = repl_tess_advance_depth(cmd->type, tess_depth);
            break;
        case CMD_TESS_END: {
            int old_depth = tess_depth;
            tess_depth = repl_tess_advance_depth(cmd->type, tess_depth);
            if (old_depth == 1 && tess_depth == 0) {
                tess_poly_selected = 0;
                tess_close_valid = 0;
                if (!in_begin_block) {
                    state.in_block = 0;
                    state.block_selected = selected_block_only ? 0 : 1;
                }
            }
            break;
        }
        case CMD_NORMAL3F:
            state.normal[0] = cmd->args[0];
            state.normal[1] = cmd->args[1];
            state.normal[2] = cmd->args[2];
            break;
        case CMD_ENABLE:
            if ((GLenum)cmd->args[0] == GL_LIGHTING)
                state.lighting_enabled = 1;
            break;
        case CMD_DISABLE:
            if ((GLenum)cmd->args[0] == GL_LIGHTING)
                state.lighting_enabled = 0;
            break;
        case CMD_TESS_NORMAL:
            state.tess_normal[0] = cmd->args[0];
            state.tess_normal[1] = cmd->args[1];
            state.tess_normal[2] = cmd->args[2];
            break;
        case CMD_VERTEX2F:
        case CMD_VERTEX3F:
        case CMD_VERTEX4F: {
            int visit = selected_block_only
                ? (state.in_block && state.block_selected)
                : 1;
            if (visit && cb->on_vertex) {
                walk_state_set_ordinals(&state,
                    repl_vertex_ordinal_at(program.vertex_ord, program.cmd_count, i));
                cb->on_vertex(&state,
                              cmd->args[0], cmd->args[1], cmd->args[2],
                              user_data);
                walk_state_set_ordinals(&state, repl_vertex_ordinal_none());
            }
            break;
        }
        case CMD_TESS_VERTEX: {
            int visit = selected_block_only
                ? (state.in_block && state.block_selected)
                : 1;
            if (visit && cb->on_vertex) {
                float saved_normal[3] = { state.normal[0], state.normal[1], state.normal[2] };
                state.normal[0] = state.tess_normal[0];
                state.normal[1] = state.tess_normal[1];
                state.normal[2] = state.tess_normal[2];
                walk_state_set_ordinals(&state,
                    repl_vertex_ordinal_at(program.vertex_ord, program.cmd_count, i));

                cb->on_vertex(&state,
                              cmd->args[0], cmd->args[1], cmd->args[2],
                              user_data);

                state.normal[0] = saved_normal[0];
                state.normal[1] = saved_normal[1];
                state.normal[2] = saved_normal[2];
                walk_state_set_ordinals(&state, repl_vertex_ordinal_none());
            }
            break;
        }
        default:
            break;
        }
        if (at_tess_close) glPopMatrix();
        if (stop_flag && *stop_flag) break;
    }
    if ((!stop_flag || !*stop_flag) && cb->on_end) {
        state.flat_cmd_idx = program.cmd_count;
        state.src_cmd_idx = -1;
        cb->on_end(&state, user_data);
    }
    unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}
