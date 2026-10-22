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

void replay_walk_tess_preview(const ReplayTessPreviewCallbacks *cb,
                                   void *user_data) {
    if (!cb) return;

    FlatProgramView program = repl_state_flat_program_view();
    int in_contour = 0;
    int matrix_depth = 0;

    glPushMatrix();
    for (int i = 0; i < program.cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        if (!cmd->valid) continue;

        if (repl_cmd_is_transform(cmd->type)) {
            /* Contour vertices are stored in polygon-local coordinates;
             * transform commands between BEGIN_CONTOUR and END still
             * belong to the surrounding polygon frame, so only apply
             * them while not walking a live contour payload. */
            if (!in_contour)
                apply_tracked_transform(cmd, &matrix_depth);
            continue;
        }

        switch (cmd->type) {
        case CMD_TESS_BEGIN_CONTOUR:
            if (in_contour && cb->end_contour)
                cb->end_contour(user_data);
            if (cb->begin_contour)
                cb->begin_contour(user_data);
            in_contour = 1;
            break;
        case CMD_TESS_VERTEX:
            if (in_contour && cb->vertex)
                cb->vertex(cmd->args[0], cmd->args[1], cmd->args[2], user_data);
            break;
        case CMD_TESS_END:
            if (in_contour && cb->end_contour)
                cb->end_contour(user_data);
            in_contour = 0;
            break;
        default:
            break;
        }
    }
    if (in_contour && cb->end_contour)
        cb->end_contour(user_data);

    unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}

/* Walk authored vertices using the shared flat/source cursor predicates.
 * Immediate blocks temporarily own the display run inside a GLU polygon. */
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
        .vertex_idx_in_block = 0,
        .immediate_idx_in_block = 0,
        .tess_idx_in_polygon = 0,
        .lighting_enabled    = 0,
        .normal              = { 0.0f, 0.0f, 1.0f },
        .tess_normal         = { 0.0f, 0.0f, 1.0f },
    };
    int matrix_depth = 0;
    int tess_depth   = 0;
    int in_begin_block = 0;
    int tess_poly_selected = selected_block_only ? 0 : 1;
    int tess_poly_vertex_idx = 0;

    glPushMatrix();
    for (int i = 0; i < program.cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        if (!cmd->valid) continue;

        state.flat_cmd_idx = i;
        state.src_cmd_idx  = cmd->src_cmd_idx;

        if (cb->on_each_cmd)
            cb->on_each_cmd(&state, user_data);
        if (stop_flag && *stop_flag) break;

        if (!in_begin_block && repl_cmd_is_transform(cmd->type)) {
            apply_tracked_transform(cmd, &matrix_depth);
            continue;
        }

        switch (cmd->type) {
        case CMD_BEGIN:
            if (tess_depth > 0 && !in_begin_block)
                tess_poly_vertex_idx = state.vertex_idx_in_block;
            in_begin_block = 1;
            state.in_block = 1;
            state.primitive_mode = (GLenum)cmd->args[0];
            state.block_selected = selected_block_only
                ? repl_cursor_flat_block_matches(program, &ctx->cursor, i, 0) : 1;
            state.vertex_idx_in_block = 0;
            state.immediate_idx_in_block = 0;
            state.normal[0] = 0.0f; state.normal[1] = 0.0f; state.normal[2] = 1.0f;
            break;
        case CMD_END:
            if (tess_depth > 0) {
                state.in_block = 1;
                state.block_selected = tess_poly_selected;
                if (in_begin_block) state.vertex_idx_in_block = tess_poly_vertex_idx;
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
            tess_poly_vertex_idx = 0;
            state.vertex_idx_in_block = 0;
            state.immediate_idx_in_block = 0;
            state.tess_idx_in_polygon = 0;
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
                tess_poly_selected = tess_poly_vertex_idx = 0;
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
                int saved_tess_idx = state.tess_idx_in_polygon;
                state.tess_idx_in_polygon = -1;
                cb->on_vertex(&state,
                              cmd->args[0], cmd->args[1], cmd->args[2],
                              user_data);
                state.tess_idx_in_polygon = saved_tess_idx;
            }
            state.vertex_idx_in_block++;
            state.immediate_idx_in_block++;
            break;
        }
        case CMD_TESS_VERTEX: {
            int visit = selected_block_only
                ? (state.in_block && state.block_selected)
                : 1;
            if (visit && cb->on_vertex) {
                float saved_normal[3] = { state.normal[0], state.normal[1], state.normal[2] };
                int saved_immediate_idx = state.immediate_idx_in_block;
                state.normal[0] = state.tess_normal[0];
                state.normal[1] = state.tess_normal[1];
                state.normal[2] = state.tess_normal[2];
                state.immediate_idx_in_block = -1;

                cb->on_vertex(&state,
                              cmd->args[0], cmd->args[1], cmd->args[2],
                              user_data);

                state.normal[0] = saved_normal[0];
                state.normal[1] = saved_normal[1];
                state.normal[2] = saved_normal[2];
                state.immediate_idx_in_block = saved_immediate_idx;
            }
            state.vertex_idx_in_block++;
            state.tess_idx_in_polygon++;
            break;
        }
        default:
            break;
        }
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
