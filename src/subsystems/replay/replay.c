/*
 * replay.c - Replay state machine, fade batches, and replay input.
 */
#include "subsystems/replay/replay.h"
#include "repl/core_internal.h"
#include "repl/eval.h"
#include "repl/pipeline.h"
#include "keys.h"
#include "repl/core.h"
#include "repl/state_owners.h"
#include "subsystems/replay/replay_state.h"

/* Batch lifetime cap (seconds). Pairs with the replay peer's
 * fade_speed (replay_state.c, alpha/sec): alpha reaches 1.0 at
 * age 1/fade_speed but the batch is culled at REPLAY_FADE_DURATION,
 * so batches are intentionally removed before fully opaque. */
#define REPLAY_FADE_DURATION   0.40f
/* A fresh fade batch starts one ~60 Hz frame "aged" rather than at 0
 * so it fades in smoothly instead of popping at full opacity. */
#define REPLAY_FADE_INITIAL_AGE GLR_FRAME_DT_SECS
/* Replay step-rate clamp (steps/sec) and the per-keystroke multipliers
 * (0.67 ~= 1/1.5, so +/- are inverse steps). */
#define REPLAY_SPEED_MIN       0.5f
#define REPLAY_SPEED_MAX       200.0f
#define REPLAY_SPEED_STEP_UP   1.5f
#define REPLAY_SPEED_STEP_DOWN 0.67f

/* Advance tessellation depth for a tess command. Depth model:
 * 0 = outside polygon, 1 = inside polygon between contours,
 * 2 = inside contour body. Returns the updated depth. */
static int replay_advance_tess_depth(CmdType type, int depth) {
    switch (type) {
    case CMD_TESS_BEGIN_POLYGON: return 1;
    case CMD_TESS_BEGIN_CONTOUR: return (depth == 1) ? 2 : depth;
    case CMD_TESS_END:
        if (depth == 2) return 1;
        if (depth == 1) return 0;
        return depth;
    default: return depth;
    }
}


static int replay_has_meaningful_cmds(void) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;
    int num_flat_cmds = flat_program.cmd_count;

    for (int flat_idx = 0; flat_idx < num_flat_cmds; flat_idx++) {
        if (!flat_cmds[flat_idx].valid) continue;
        if (flat_cmds[flat_idx].type == CMD_COMMENT) continue;
        return 1;
    }
    return 0;
}

static int replay_find_open_begin_before(int limit) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;
    int num_flat_cmds = flat_program.cmd_count;
    int open_begin = -1;
    int in_begin = 0;

    for (int flat_idx = 0; flat_idx < limit && flat_idx < num_flat_cmds; flat_idx++) {
        if (!flat_cmds[flat_idx].valid) continue;
        if (flat_cmds[flat_idx].type == CMD_BEGIN) {
            open_begin = flat_idx;
            in_begin = 1;
        } else if (flat_cmds[flat_idx].type == CMD_END && in_begin) {
            open_begin = -1;
            in_begin = 0;
        }
    }

    return open_begin;
}

static int replay_find_open_tess_polygon_before(int limit, int *out_depth) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;
    int num_flat_cmds = flat_program.cmd_count;
    int poly_start = -1;
    int tess_depth = 0;

    for (int flat_idx = 0; flat_idx < limit && flat_idx < num_flat_cmds; flat_idx++) {
        if (!flat_cmds[flat_idx].valid) continue;
        CmdType type = flat_cmds[flat_idx].type;
        if (type == CMD_TESS_BEGIN_POLYGON) {
            poly_start = flat_idx;
            tess_depth = 1;
        } else {
            int old_depth = tess_depth;
            tess_depth = replay_advance_tess_depth(type, tess_depth);
            if (type == CMD_TESS_END && old_depth == 1 && tess_depth == 0) {
                poly_start = -1;
            }
        }
    }

    if (out_depth) *out_depth = tess_depth;
    return poly_start;
}

static int replay_find_matching_gl_end(int begin_idx) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;
    int num_flat_cmds = flat_program.cmd_count;

    for (int flat_idx = begin_idx + 1; flat_idx < num_flat_cmds; flat_idx++) {
        if (!flat_cmds[flat_idx].valid) continue;
        if (flat_cmds[flat_idx].type == CMD_END)
            return flat_idx;
    }
    return num_flat_cmds > 0 ? num_flat_cmds - 1 : begin_idx;
}

static int replay_cmd_is_focus_candidate(CmdType type) {
    switch (type) {
    case CMD_COMMENT:
    case CMD_BEGIN:
    case CMD_END:
    case CMD_TESS_BEGIN_POLYGON:
    case CMD_TESS_BEGIN_CONTOUR:
    case CMD_TESS_END:
    case CMD_FOR_BEGIN:
    case CMD_FOR_END:
    case CMD_FUNC_DEF:
    case CMD_FUNC_END:
    case CMD_IF_BEGIN:
    case CMD_IF_END:
    case CMD_GOTO_LABEL:
        return 0;
    default:
        return 1;
    }
}

static int replay_last_meaningful_src(int begin, int end_exclusive) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;

    for (int flat_idx = end_exclusive - 1; flat_idx >= begin && flat_idx >= 0; flat_idx--) {
        if (!flat_cmds[flat_idx].valid) continue;
        if (!replay_cmd_is_focus_candidate(flat_cmds[flat_idx].type)) continue;
        if (flat_cmds[flat_idx].src_cmd_idx >= 0)
            return flat_cmds[flat_idx].src_cmd_idx;
    }
    return -1;
}

static void replay_set_src_line(int src_line) {
    ReplReplayRuntimeState *state = replay_state_mut();
    state->src_line_idx = src_line;
    if (src_line != state->last_src_line) {
        state->last_src_line = src_line;
        if (src_line >= 0)
            repl_dispatch_follow_cursor(1);
    }
}

float replay_batch_alpha(const ReplayFadeBatch *batch) {
    float alpha = batch->age * replay_state_view().fade_speed;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return alpha;
}

ReplayFadeBatchView replay_fade_batches_view(void) {
    ReplReplayRuntimeState *state = replay_state_mut();
    ReplayFadeBatchView view = {
        .batches = state->fade_batches,
        .count = state->fade_batch_count,
    };
    return view;
}

int replay_compute_fade_skip_limits(int *out_limits, int max_count) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *g_flat_cmds = flat_program.cmds;
    int g_num_flat_cmds = flat_program.cmd_count;
    ReplayFadeBatchView fade_batches = replay_fade_batches_view();
    int batch_limit;
    int batch_idx = 0;
    int open_begin = -1;
    int open_tess = -1;
    int tess_depth = 0;

    if (!out_limits || max_count <= 0)
        return 0;
    if (!replay_has_active_fades())
        return 0;

    batch_limit = fade_batches.count;
    if (batch_limit > max_count)
        batch_limit = max_count;
    if (batch_limit <= 0)
        return 0;

    int max_old_pc = fade_batches.batches[batch_limit - 1].old_pc;
    if (max_old_pc > g_num_flat_cmds) max_old_pc = g_num_flat_cmds;

    /* Each skip limit must rewind to the opener of any primitive that
     * still spans old_pc, so the fade redraw can replay a balanced
     * glBegin/glEnd or tess polygon block rather than starting in the
     * middle of one. */
    for (int pc = 0; pc <= max_old_pc; pc++) {
        while (batch_idx < batch_limit &&
               fade_batches.batches[batch_idx].old_pc <= pc) {
            int sl = fade_batches.batches[batch_idx].old_pc;
            if (open_begin >= 0 && open_begin < sl) sl = open_begin;
            if (open_tess  >= 0 && open_tess  < sl) sl = open_tess;
            out_limits[batch_idx++] = sl;
        }
        if (pc >= g_num_flat_cmds || !g_flat_cmds[pc].valid)
            continue;
        switch (g_flat_cmds[pc].type) {
        case CMD_BEGIN: open_begin = pc; break;
        case CMD_END:   open_begin = -1; break;
        case CMD_TESS_BEGIN_POLYGON:
            open_tess = pc;
            tess_depth = replay_advance_tess_depth(g_flat_cmds[pc].type, tess_depth);
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            tess_depth = replay_advance_tess_depth(g_flat_cmds[pc].type, tess_depth);
            break;
        case CMD_TESS_END: {
            int old_depth = tess_depth;
            tess_depth = replay_advance_tess_depth(g_flat_cmds[pc].type, tess_depth);
            if (old_depth == 1 && tess_depth == 0) {
                open_tess = -1;
            }
            break;
        }
        default: break;
        }
    }
    while (batch_idx < batch_limit) {
        int sl = fade_batches.batches[batch_idx].old_pc;
        if (open_begin >= 0 && open_begin < sl) sl = open_begin;
        if (open_tess  >= 0 && open_tess  < sl) sl = open_tess;
        out_limits[batch_idx++] = sl;
    }

    return batch_limit;
}

static void replay_clear_fade_batches(void) {
    replay_state_mut()->fade_batch_count = 0;
}

static void replay_push_fade_batch(int old_pc, int new_pc) {
    ReplReplayRuntimeState *state = replay_state_mut();
    ReplayFadeBatch *batch;

    if (new_pc <= old_pc)
        return;

    if (state->fade_batch_count >= REPLAY_FADE_BATCH_MAX) {
        memmove(&state->fade_batches[0], &state->fade_batches[1],
                (size_t)(REPLAY_FADE_BATCH_MAX - 1) * sizeof(state->fade_batches[0]));
        state->fade_batch_count = REPLAY_FADE_BATCH_MAX - 1;
    }

    batch = &state->fade_batches[state->fade_batch_count++];
    batch->old_pc = old_pc;
    batch->new_pc = new_pc;
    batch->age = REPLAY_FADE_INITIAL_AGE;
}

static void replay_clamp_fade_batches(int max_pc) {
    ReplReplayRuntimeState *state = replay_state_mut();
    int dst = 0;

    for (int idx = 0; idx < state->fade_batch_count; idx++) {
        ReplayFadeBatch batch = state->fade_batches[idx];

        if (batch.old_pc > max_pc)
            continue;
        if (batch.new_pc > max_pc)
            batch.new_pc = max_pc;
        if (batch.new_pc <= batch.old_pc)
            continue;
        state->fade_batches[dst++] = batch;
    }

    state->fade_batch_count = dst;
}

void replay_tick_fade_batches(float dt) {
    ReplReplayRuntimeState *state = replay_state_mut();
    int dst = 0;

    for (int idx = 0; idx < state->fade_batch_count; idx++) {
        ReplayFadeBatch batch = state->fade_batches[idx];
        batch.age += dt;
        if (batch.age >= REPLAY_FADE_DURATION)
            continue;
        state->fade_batches[dst++] = batch;
    }

    state->fade_batch_count = dst;
}

int replay_has_active_fades(void) {
    ReplReplayRuntimeState state = replay_state_view();
    return state.active && state.fade_batch_count > 0;
}

int replay_fill_base_limit(void) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    int num_flat_cmds = flat_program.cmd_count;

    if (!replay_has_active_fades())
        return num_flat_cmds;
    ReplReplayRuntimeState state = replay_state_view();
    if (state.fade_batches[0].old_pc < 0)
        return 0;
    if (state.fade_batches[0].old_pc > num_flat_cmds)
        return num_flat_cmds;
    return state.fade_batches[0].old_pc;
}

/* Apply a single transform CmdType to the current GL modelview, tracking
 * push/pop balance. Used by replay_walk_tess_preview below — REPL is
 * the home of GLCmd→GL translation, so the scene module never has to do
 * its own command iteration. */
static void replay_walk_apply_transform(const GLCmd *cmd, int *depth) {
    if (!cmd) return;
    switch (cmd->type) {
    case CMD_PUSH_MATRIX:
        glPushMatrix();
        if (depth) (*depth)++;
        break;
    case CMD_POP_MATRIX:
        if (!depth || *depth > 0) {
            glPopMatrix();
            if (depth) (*depth)--;
        }
        break;
    case CMD_LOAD_IDENTITY:
        glLoadIdentity();
        break;
    case CMD_TRANSLATE3F:
        glTranslatef(cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_SCALEF:
        glScalef(cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_ROTATEF:
        glRotatef(cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
        break;
    default: break;
    }
}

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
                replay_walk_apply_transform(cmd, &matrix_depth);
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

    while (matrix_depth > 0) { glPopMatrix(); matrix_depth--; }
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
    int          edit_line_idx       = ctx->edit_line_idx;
    int          cursor_block_begin  = ctx->cursor_block_begin;
    int          cursor_block_end    = ctx->cursor_block_end;
    unsigned int cursor_func_scope_mask = ctx->cursor_func_scope_mask;
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
            replay_walk_apply_transform(cmd, &matrix_depth);
            continue;
        }

        switch (cmd->type) {
        case CMD_BEGIN:
            state.in_block = 1;
            state.primitive_mode = (GLenum)cmd->args[0];
            state.block_selected = selected_block_only
                ? replay_walk_block_matches_cursor(i, 0, edit_line_idx,
                                                   cursor_block_begin,
                                                   cursor_block_end,
                                                   cursor_func_scope_mask,
                                                   program)
                : 1;
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
    while (matrix_depth > 0) { glPopMatrix(); matrix_depth--; }
    glPopMatrix();
}

static int replay_next_polygon_limit(int start, int *fade_begin, int *fade_end) {
    int saw_meaningful = 0;
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;
    int num_flat_cmds = flat_program.cmd_count;

    if (fade_begin) *fade_begin = -1;
    if (fade_end) *fade_end = -1;

    for (int flat_idx = start; flat_idx < num_flat_cmds; flat_idx++) {
        CmdType t;

        if (!flat_cmds[flat_idx].valid || flat_cmds[flat_idx].type == CMD_COMMENT)
            continue;

        t = flat_cmds[flat_idx].type;
        saw_meaningful = 1;

        switch (t) {
        case CMD_BEGIN: {
            int end = replay_find_matching_gl_end(flat_idx);
            if (fade_begin) *fade_begin = start;
            if (fade_end) *fade_end = end;
            return end + 1;
        }
        case CMD_GLUT_TORUS:
        case CMD_GLUT_CUBE:
        case CMD_GLUT_SPHERE:
        case CMD_GLUT_TEAPOT:
        case CMD_GLUT_CONE:
            if (fade_begin) *fade_begin = start;
            if (fade_end) *fade_end = flat_idx;
            return flat_idx + 1;
        case CMD_TESS_BEGIN_POLYGON: {
            /* Depth model: 1 = inside polygon but between contours,
             * 2 = inside the active contour payload. The polygon-mode
             * fade unit is the full polygon block, so return at the
             * matching END that closes depth 1. */
            int tess_depth = 1;
            for (int tess_idx = flat_idx + 1; tess_idx < num_flat_cmds; tess_idx++) {
                if (!flat_cmds[tess_idx].valid) continue;
                CmdType type = flat_cmds[tess_idx].type;
                if (type == CMD_TESS_BEGIN_POLYGON) {
                    tess_depth = 1;
                } else if (type == CMD_TESS_BEGIN_CONTOUR) {
                    tess_depth = replay_advance_tess_depth(type, tess_depth);
                } else if (type == CMD_TESS_END) {
                    int old_depth = tess_depth;
                    tess_depth = replay_advance_tess_depth(type, tess_depth);
                    if (old_depth == 1 && tess_depth == 0) {
                        if (fade_begin) *fade_begin = start;
                        if (fade_end) *fade_end = tess_idx;
                        return tess_idx + 1;
                    }
                }
            }
            if (fade_begin) *fade_begin = start;
            if (fade_end) *fade_end = num_flat_cmds > 0 ? num_flat_cmds - 1 : flat_idx;
            return num_flat_cmds;
        }
        default:
            break;
        }
    }

    if (saw_meaningful)
        return num_flat_cmds;
    return start;
}

static int replay_next_vertex_limit(int start, int *fade_begin, int *fade_end) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;
    int num_flat_cmds = flat_program.cmd_count;
    int open_begin = replay_find_open_begin_before(start);
    /* Depth model matches replay_next_polygon_limit:
     * 0 = outside tess polygon, 1 = inside polygon between contours,
     * 2 = inside a contour body. */
    int tess_depth = 0;
    int open_tess_poly = replay_find_open_tess_polygon_before(start, &tess_depth);
    int saw_meaningful = 0;

    if (fade_begin) *fade_begin = -1;
    if (fade_end) *fade_end = -1;

    for (int flat_idx = start; flat_idx < num_flat_cmds; flat_idx++) {
        CmdType t;

        if (!flat_cmds[flat_idx].valid || flat_cmds[flat_idx].type == CMD_COMMENT)
            continue;

        t = flat_cmds[flat_idx].type;
        saw_meaningful = 1;

        switch (t) {
        case CMD_BEGIN:
            open_begin = flat_idx;
            break;
        case CMD_END:
            open_begin = -1;
            break;
        case CMD_TESS_BEGIN_POLYGON:
            open_tess_poly = flat_idx;
            tess_depth = 1;
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            tess_depth = replay_advance_tess_depth(t, tess_depth);
            break;
        case CMD_TESS_END: {
            int old_depth = tess_depth;
            tess_depth = replay_advance_tess_depth(t, tess_depth);
            if (old_depth == 1 && tess_depth == 0) {
                /* Vertex replay reveals tessellation one contour vertex
                 * at a time, but the fade begin still has to rewind to
                 * the polygon opener so replayed state sees the full
                 * enclosing tess block. */
                if (fade_begin) *fade_begin = (open_tess_poly >= 0) ? open_tess_poly : start;
                if (fade_end) *fade_end = flat_idx;
                return flat_idx + 1;
            }
            break;
        }
        case CMD_VERTEX3F:
        case CMD_VERTEX2F:
            if (fade_begin) *fade_begin = (open_begin >= 0) ? open_begin : start;
            if (fade_end) *fade_end = flat_idx;
            return flat_idx + 1;
        case CMD_TESS_VERTEX:
            /* Same rewind rule as CMD_TESS_END above: emit one vertex,
             * but keep the fade range anchored at the polygon opener. */
            if (fade_begin) *fade_begin = (open_tess_poly >= 0) ? open_tess_poly : start;
            if (fade_end) *fade_end = flat_idx;
            return flat_idx + 1;
        case CMD_GLUT_TORUS:
        case CMD_GLUT_CUBE:
        case CMD_GLUT_SPHERE:
        case CMD_GLUT_TEAPOT:
        case CMD_GLUT_CONE:
            if (fade_begin) *fade_begin = start;
            if (fade_end) *fade_end = flat_idx;
            return flat_idx + 1;
        default:
            break;
        }
    }

    if (saw_meaningful)
        return num_flat_cmds;
    return start;
}

static int replay_prev_limit(int current_pc) {
    int pc = 0;
    int prev_pc = 0;

    if (current_pc <= 0)
        return 0;

    while (pc < current_pc) {
        int fade_begin = -1;
        int fade_end = -1;
        int next_pc = (replay_state_view().mode == REPLAY_MODE_POLYGON)
                    ? replay_next_polygon_limit(pc, &fade_begin, &fade_end)
                    : replay_next_vertex_limit(pc, &fade_begin, &fade_end);

        if (next_pc <= pc)
            break;

        prev_pc = pc;
        pc = next_pc;
    }

    return prev_pc;
}

void replay_seek(int new_pc) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    int num_flat_cmds = flat_program.cmd_count;
    ReplReplayRuntimeState *state = replay_state_mut();
    if (new_pc < 0)
        new_pc = 0;
    if (new_pc > num_flat_cmds)
        new_pc = num_flat_cmds;

    state->pc = new_pc;
    state->accum = 0.0f;
    replay_clear_fade_batches();
    replay_set_src_line(replay_last_meaningful_src(0, new_pc));
    state->state = (new_pc >= num_flat_cmds && num_flat_cmds > 0)
                 ? REPLAY_DONE
                 : REPLAY_PAUSED;
}

int replay_seek_to_src_line(int target_line) {
    int pc = 0;
    int landed_pc = -1;
    int landed_src = -1;
    FlatProgramView flat_program;
    int num_flat_cmds;

    repl_ensure_flat_program_with_live_vars(repl_dispatch_edit_line_get());
    flat_program = repl_state_flat_program_view();
    num_flat_cmds = flat_program.cmd_count;

    while (pc < num_flat_cmds) {
        int fade_begin = -1;
        int fade_end = -1;
        int next_pc = (replay_state_view().mode == REPLAY_MODE_POLYGON)
                    ? replay_next_polygon_limit(pc, &fade_begin, &fade_end)
                    : replay_next_vertex_limit(pc, &fade_begin, &fade_end);

        if (next_pc <= pc)
            break;

        int step_src = replay_last_meaningful_src(pc, next_pc);
        if (step_src >= target_line) {
            landed_pc = next_pc;
            landed_src = step_src;
            break;
        }
        pc = next_pc;
    }

    if (landed_pc < 0)
        return -1;

    replay_seek(landed_pc);
    return landed_src;
}

void replay_restart_from_beginning(void) {
    ReplReplayRuntimeState *state = replay_state_mut();
    state->pc = 0;
    state->accum = 0.0f;
    replay_clear_fade_batches();
    state->state = REPLAY_PLAYING;
    state->src_line_idx = -1;
    state->last_src_line = -1;
}

void replay_start(void) {
    FlatProgramView flat_program;
    int num_flat_cmds;

    repl_ensure_flat_program_with_live_vars(repl_dispatch_edit_line_get());
    flat_program = repl_state_flat_program_view();
    num_flat_cmds = flat_program.cmd_count;

    if (!replay_has_meaningful_cmds()) {
        repl_set_status("Replay: nothing to play");
        return;
    }

    ReplReplayRuntimeState *state = replay_state_mut();
    repl_copy_predef_values(state->baseline_predef_vals, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(state->baseline_scratch_arrays);
    state->saved_t_playing = repl_state_variables().time_playing;
    repl_state_variables_mut()->time_playing = 0;

    state->active = 1;
    state->state = REPLAY_PLAYING;
    state->pc = 0;
    state->accum = 0.0f;
    replay_clear_fade_batches();
    state->src_line_idx = -1;
    state->total_flat_cmds = num_flat_cmds;
    state->last_src_line = -1;
    repl_set_status("Replay: playing");
}

void replay_stop(void) {
    ReplReplayRuntimeState *state = replay_state_mut();
    repl_state_variables_mut()->time_playing = state->saved_t_playing;
    state->active = 0;
    state->state = REPLAY_OFF;
    state->pc = 0;
    state->accum = 0.0f;
    replay_clear_fade_batches();
    state->src_line_idx = -1;
    state->total_flat_cmds = 0;
    state->last_src_line = -1;
}

void replay_advance(void) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    int num_flat_cmds = flat_program.cmd_count;
    ReplReplayRuntimeState *state = replay_state_mut();
    int old_pc;
    int next_pc;
    int src_line = -1;

    if (!replay_active())
        return;

    if (state->pc >= num_flat_cmds) {
        state->state = REPLAY_DONE;
        return;
    }

    old_pc = state->pc;
    next_pc = (state->mode == REPLAY_MODE_POLYGON)
            ? replay_next_polygon_limit(old_pc, NULL, NULL)
            : replay_next_vertex_limit(old_pc, NULL, NULL);

    if (next_pc <= old_pc)
        next_pc = num_flat_cmds;
    if (next_pc > num_flat_cmds)
        next_pc = num_flat_cmds;

    state->pc = next_pc;
    replay_push_fade_batch(old_pc, next_pc);

    src_line = replay_last_meaningful_src(old_pc, next_pc);
    replay_set_src_line(src_line);

    if (state->pc >= num_flat_cmds) {
        state->state = REPLAY_DONE;
        repl_set_status("Replay: done");
    }
}

void replay_step_back(void) {
    ReplReplayRuntimeState state = replay_state_view();
    if (!state.active)
        return;

    if (state.pc <= 0) {
        replay_seek(0);
        repl_set_status("Replay: at start");
        return;
    }

    replay_seek(replay_prev_limit(state.pc));
}

int replay_exec_limit(void) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    int num_flat_cmds = flat_program.cmd_count;
    if (replay_active())
        return replay_state_view().pc;
    return num_flat_cmds;
}

void replay_speed_adjust(float factor) {
    ReplReplayRuntimeState *state = replay_state_mut();
    char msg[REPLAY_STATUS_MSG_LEN];
    state->speed *= factor;
    if (state->speed < REPLAY_SPEED_MIN) state->speed = REPLAY_SPEED_MIN;
    if (state->speed > REPLAY_SPEED_MAX) state->speed = REPLAY_SPEED_MAX;
    snprintf(msg, sizeof(msg), "Replay: %.1f step/s", state->speed);
    repl_set_status(msg);
}

void replay_toggle_play_pause(void) {
    ReplReplayRuntimeState *state = replay_state_mut();
    if (!state->active || state->state == REPLAY_DONE) {
        replay_start();
        return;
    }

    if (state->state == REPLAY_PLAYING) {
        state->state = REPLAY_PAUSED;
        repl_set_status("Replay: paused");
    } else if (state->state == REPLAY_PAUSED) {
        state->state = REPLAY_PLAYING;
        repl_set_status("Replay: playing");
    } else {
        replay_start();
    }
}

int replay_prepare_frame(int full_flat_count) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    int num_flat_cmds = flat_program.cmd_count;
    ReplReplayRuntimeState *state = replay_state_mut();
    if (!state->active)
        return num_flat_cmds;

    state->total_flat_cmds = full_flat_count;
    if (state->pc > num_flat_cmds)
        state->pc = num_flat_cmds;
    if (state->pc >= num_flat_cmds && num_flat_cmds > 0 &&
        state->state == REPLAY_PLAYING)
        state->state = REPLAY_DONE;
    replay_clamp_fade_batches(state->pc);
    return replay_exec_limit();
}

void replay_restore_baseline_predef_values(void) {
    repl_restore_predef_values(replay_state_mut()->baseline_predef_vals, MAX_PREDEF_VARS);
}

void replay_copy_baseline_predef_values(float *dst, int max_vals) {
    int n;

    if (!dst || max_vals <= 0)
        return;

    n = max_vals < MAX_PREDEF_VARS ? max_vals : MAX_PREDEF_VARS;
    memcpy(dst, replay_state_mut()->baseline_predef_vals, (size_t)n * sizeof(float));
}

void replay_restore_baseline_scratch_arrays(void) {
    repl_eval_restore_scratch_arrays(replay_state_mut()->baseline_scratch_arrays);
}

void replay_copy_baseline_scratch_arrays(
    float dst[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    if (!dst)
        return;
    memcpy(dst, replay_state_mut()->baseline_scratch_arrays,
           sizeof(replay_state_mut()->baseline_scratch_arrays));
}

int replay_handle_key(unsigned char key) {
    ReplReplayRuntimeState *state = replay_state_mut();
    if (!state->active) {
        if (key == KEY_CTRL_R) {
            replay_start();
            return 1;
        }
        if (key == KEY_CTRL_K) {
            int target_line = repl_dispatch_edit_line_get();
            replay_start();
            if (replay_active()) {
                int landed = replay_seek_to_src_line(target_line);
                if (landed < 0) {
                    repl_set_status("Jump: no geometry at or after cursor");
                } else {
                    char msg[REPLAY_STATUS_MSG_LEN];
                    replay_state_mut()->state = REPLAY_PAUSED;
                    snprintf(msg, sizeof(msg), "Jump: paused at line %d", landed + 1);
                    repl_set_status(msg);
                }
            }
            return 1;
        }
        return 0;
    }

    if (key == KEY_CTRL_R) {
        replay_stop();
        repl_set_status("Replay: off");
        return 1;
    }
    if (key == KEY_CTRL_K) {
        int landed = replay_seek_to_src_line(repl_dispatch_edit_line_get());
        if (landed < 0) {
            repl_set_status("Jump: no geometry at or after cursor");
        } else {
            char msg[REPLAY_STATUS_MSG_LEN];
            state->state = REPLAY_PAUSED;
            snprintf(msg, sizeof(msg), "Jump: paused at line %d", landed + 1);
            repl_set_status(msg);
        }
        return 1;
    }
    if (key == ' ') {
        if (state->state == REPLAY_PLAYING) {
            state->state = REPLAY_PAUSED;
            repl_set_status("Replay: paused");
        } else if (state->state == REPLAY_PAUSED) {
            state->state = REPLAY_PLAYING;
            repl_set_status("Replay: playing");
        } else if (state->state == REPLAY_DONE) {
            replay_restart_from_beginning();
            repl_set_status("Replay: restarted");
        }
        return 1;
    }
    if (key == '+' || key == '=') {
        replay_speed_adjust(REPLAY_SPEED_STEP_UP);
        return 1;
    }
    if (key == '-') {
        replay_speed_adjust(REPLAY_SPEED_STEP_DOWN);
        return 1;
    }
    if (key == 'm' || key == 'M') {
        int was_playing = (state->state == REPLAY_PLAYING);
        state->mode = (state->mode == REPLAY_MODE_VERTEX)
                    ? REPLAY_MODE_POLYGON
                    : REPLAY_MODE_VERTEX;
        replay_seek(state->pc);
        if (was_playing && state->state != REPLAY_DONE)
            state->state = REPLAY_PLAYING;
        repl_set_status(state->mode == REPLAY_MODE_VERTEX
                 ? "Replay: vertex mode"
                 : "Replay: polygon mode");
        return 1;
    }
    if (key == 'e' || key == 'E') {
        /* Route Replay expand toggle through the config bridge */
        repl_cfg_set_int("replay_expand", !repl_cfg_get_int("replay_expand", 0));
        repl_set_status(repl_cfg_get_int("replay_expand", 0)
                 ? "Replay: expand args ON"
                 : "Replay: expand args OFF");
        return 1;
    }
    if (key == KEY_ESC) {
        replay_stop();
        repl_set_status("Replay: off");
        return 1;
    }

    /* Set status and stop replay on unrecognized key press */
    repl_set_status("Replay: cancelled (key)");
    replay_stop();
    return 0;
}

static int replay_modifier_special_key(int key) {
#ifdef USE_GLUT
    return 0;
#else
    return key == GLUT_KEY_NUM_LOCK ||
           key == GLUT_KEY_SHIFT_L || key == GLUT_KEY_SHIFT_R ||
           key == GLUT_KEY_CTRL_L || key == GLUT_KEY_CTRL_R ||
           key == GLUT_KEY_ALT_L || key == GLUT_KEY_ALT_R ||
           key == GLUT_KEY_SUPER_L || key == GLUT_KEY_SUPER_R;
#endif
}

int replay_handle_special(int key) {
    if (!replay_active())
        return 0;

    if (key == GLUT_KEY_LEFT) {
        replay_step_back();
        return 1;
    }
    if (key == GLUT_KEY_RIGHT) {
        replay_advance();
        return 1;
    }
    if (key == GLUT_KEY_UP) {
        replay_speed_adjust(REPLAY_SPEED_STEP_UP);
        return 1;
    }
    if (key == GLUT_KEY_DOWN) {
        replay_speed_adjust(REPLAY_SPEED_STEP_DOWN);
        return 1;
    }

    if (!replay_modifier_special_key(key)) {
        /* Set status and stop replay on unrecognized key press */
        repl_set_status("Replay: cancelled (key)");
        replay_stop();
    }
    return 0;
}

int replay_bench_fade_install(const int *old_pcs, const int *new_pcs,
                            int count, float age) {
    ReplReplayRuntimeState *state = replay_state_mut();
    int installed = 0;
    FlatProgramView flat_program = repl_state_flat_program_view();
    int num_flat_cmds = flat_program.cmd_count;

    if (count < 0) count = 0;
    if (count > REPLAY_FADE_BATCH_MAX) count = REPLAY_FADE_BATCH_MAX;

    if (count > 0 && (old_pcs == NULL || new_pcs == NULL))
        count = 0;

    for (int idx = 0; idx < count; idx++) {
        int old_pc = old_pcs[idx];
        int new_pc = new_pcs[idx];

        if (old_pc < 0) old_pc = 0;
        if (old_pc > num_flat_cmds) old_pc = num_flat_cmds;
        if (new_pc < 0) new_pc = 0;
        if (new_pc > num_flat_cmds) new_pc = num_flat_cmds;
        if (new_pc <= old_pc)
            continue;

        state->fade_batches[installed].old_pc = old_pc;
        state->fade_batches[installed].new_pc = new_pc;
        state->fade_batches[installed].age = age;
        installed++;
    }
    state->fade_batch_count = installed;

    repl_copy_predef_values(state->baseline_predef_vals, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(state->baseline_scratch_arrays);
    state->active = (installed > 0);
    return installed;
}

void replay_bench_fade_clear(void) {
    ReplReplayRuntimeState *state = replay_state_mut();
    state->fade_batch_count = 0;
    state->active = 0;
}
