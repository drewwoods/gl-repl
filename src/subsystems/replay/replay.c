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
#include "repl/core_internal.h"
#include "repl/eval.h"
#include "repl/pipeline.h"
#include "keys.h"
#include "repl/flatten.h"
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

/* Cursor-block-match logic: does a flat cmd belong to the user's currently
 * focused source line / block / function call? Lifted out of scene/overlays.c
 * — these checks read REPL flatten metadata (call_src_cmd_idx, src_cmd_idx,
 * func_scope_mask, current-block bounds), so they're squarely REPL state. */
static int replay_walk_flat_cmd_matches_cursor(int flat_idx,
                                               int edit_line_idx,
                                               int cursor_block_begin,
                                               int cursor_block_end,
                                               unsigned int cursor_func_scope_mask,
                                               FlatProgramView program) {
    if (edit_line_idx < 0)
        return 0;
    if (flat_idx < 0 || flat_idx >= program.cmd_count)
        return 0;

    const GLCmd *cmd = &program.cmds[flat_idx];
    if (!cmd->valid)
        return 0;

    if (cmd->call_src_cmd_idx == edit_line_idx ||
        cmd->root_call_src_cmd_idx == edit_line_idx)
        return 1;
    if (cursor_func_scope_mask != 0 &&
        (cmd->func_scope_mask & cursor_func_scope_mask) != 0)
        return 1;
    if (cursor_block_begin >= 0 &&
        flat_idx >= cursor_block_begin &&
        flat_idx <= cursor_block_end)
        return 1;
    return cmd->src_cmd_idx == edit_line_idx;
}

static int replay_walk_source_block_matches_cursor(int begin_idx, int is_tess,
                                                   const CursorBlockState *cursor,
                                                   FlatProgramView program) {
    const GLCmd *begin_cmd;

    if (!cursor || !cursor->cursor_source_block_valid)
        return 0;
    if (is_tess)
        return 0;
    if (begin_idx < 0 || begin_idx >= program.cmd_count)
        return 0;

    begin_cmd = &program.cmds[begin_idx];
    if (!begin_cmd->valid || begin_cmd->type != CMD_BEGIN)
        return 0;
    if (begin_cmd->src_cmd_idx != cursor->cursor_source_block_begin)
        return 0;

    for (int i = begin_idx + 1; i < program.cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        if (!cmd->valid)
            continue;
        if (cmd->type == CMD_END)
            return cmd->src_cmd_idx == cursor->cursor_source_block_end;
        if (cmd->type == CMD_BEGIN)
            break;
    }

    return cursor->cursor_source_block_end >= cursor->cursor_source_block_begin;
}

static int replay_walk_block_matches_cursor(int begin_idx, int is_tess,
                                            int edit_line_idx,
                                            int cursor_block_begin,
                                            int cursor_block_end,
                                            unsigned int cursor_func_scope_mask,
                                            FlatProgramView program) {
    int depth = is_tess ? 1 : 0;

    for (int i = begin_idx; i < program.cmd_count; i++) {
        if (!program.cmds[i].valid) continue;
        if (replay_walk_flat_cmd_matches_cursor(i, edit_line_idx,
                                                cursor_block_begin,
                                                cursor_block_end,
                                                cursor_func_scope_mask,
                                                program))
            return 1;
        if (!is_tess && i > begin_idx && program.cmds[i].type == CMD_END)
            break;
        if (is_tess && i > begin_idx) {
            if (program.cmds[i].type == CMD_TESS_BEGIN_POLYGON) depth++;
            else if (program.cmds[i].type == CMD_TESS_END) {
                depth--;
                if (depth == 0) break;
            }
        }
    }

    return 0;
}

void replay_walk_user_vertices(const ReplayVertexWalkContext *ctx,
                             const ReplayVertexWalkCallbacks *cb,
                             void *user_data) {
    if (!ctx || !cb) return;

    FlatProgramView program          = ctx->program;
    int          edit_line_idx       = ctx->cursor.edit_line_idx;
    int          cursor_block_begin  = ctx->cursor.cursor_block_begin;
    int          cursor_block_end    = ctx->cursor.cursor_block_end;
    unsigned int cursor_func_scope_mask = ctx->cursor.cursor_func_scope_mask;
    int          selected_block_only = ctx->selected_block_only;
    int         *stop_flag           = ctx->stop_flag;

    ReplayVertexWalkState state = {
        .flat_cmd_idx        = -1,
        .src_cmd_idx         = -1,
        .primitive_mode      = 0,
        .in_block            = 0,
        .block_selected      = selected_block_only ? 0 : 1,
        .vertex_idx_in_block = 0,
        .normal              = { 0.0f, 0.0f, 1.0f },
    };
    int matrix_depth = 0;
    int tess_depth   = 0;

    glPushMatrix();
    for (int i = 0; i < program.cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        if (!cmd->valid) continue;

        state.flat_cmd_idx = i;
        state.src_cmd_idx  = cmd->src_cmd_idx;

        if (cb->on_each_cmd)
            cb->on_each_cmd(&state, user_data);
        if (stop_flag && *stop_flag) break;

        if (!state.in_block && repl_cmd_is_transform(cmd->type)) {
            apply_tracked_transform(cmd, &matrix_depth);
            continue;
        }

        switch (cmd->type) {
        case CMD_BEGIN:
            state.in_block = 1;
            state.primitive_mode = (GLenum)cmd->args[0];
            if (selected_block_only && ctx->cursor.cursor_source_block_valid) {
                state.block_selected =
                    replay_walk_source_block_matches_cursor(i, 0, &ctx->cursor,
                                                            program);
            } else {
                state.block_selected = selected_block_only
                    ? replay_walk_block_matches_cursor(i, 0, edit_line_idx,
                                                       cursor_block_begin,
                                                       cursor_block_end,
                                                       cursor_func_scope_mask,
                                                       program)
                    : 1;
            }
            state.vertex_idx_in_block = 0;
            tess_depth = 0;
            state.normal[0] = 0.0f; state.normal[1] = 0.0f; state.normal[2] = 1.0f;
            break;
        case CMD_END:
            state.in_block = 0;
            state.primitive_mode = 0;
            state.block_selected = selected_block_only ? 0 : 1;
            tess_depth = 0;
            break;
        case CMD_TESS_BEGIN_POLYGON:
            state.in_block = 1;
            state.primitive_mode = 0;
            state.block_selected = selected_block_only
                ? replay_walk_block_matches_cursor(i, 1, edit_line_idx,
                                                   cursor_block_begin,
                                                   cursor_block_end,
                                                   cursor_func_scope_mask,
                                                   program)
                : 1;
            state.vertex_idx_in_block = 0;
            tess_depth = replay_advance_tess_depth(cmd->type, tess_depth);
            state.normal[0] = 0.0f; state.normal[1] = 0.0f; state.normal[2] = 1.0f;
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            tess_depth = replay_advance_tess_depth(cmd->type, tess_depth);
            break;
        case CMD_TESS_END: {
            int old_depth = tess_depth;
            tess_depth = replay_advance_tess_depth(cmd->type, tess_depth);
            if (old_depth == 1 && tess_depth == 0) {
                state.in_block = 0;
                state.block_selected = selected_block_only ? 0 : 1;
            }
            break;
        }
        case CMD_NORMAL3F:
        case CMD_TESS_NORMAL:
            state.normal[0] = cmd->args[0];
            state.normal[1] = cmd->args[1];
            state.normal[2] = cmd->args[2];
            break;
        case CMD_VERTEX2F:
        case CMD_VERTEX3F:
        case CMD_TESS_VERTEX: {
            int visit = selected_block_only
                ? (state.in_block && state.block_selected)
                : 1;
            if (visit && cb->on_vertex) {
                cb->on_vertex(&state,
                              cmd->args[0], cmd->args[1], cmd->args[2],
                              user_data);
            }
            state.vertex_idx_in_block++;
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
