#include "subsystems/replay/replay_internal.h"
#include <stdio.h>
#include <string.h>

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

static int replay_last_meaningful_flat(int begin, int end_exclusive) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;

    for (int flat_idx = end_exclusive - 1; flat_idx >= begin && flat_idx >= 0; flat_idx--) {
        if (!flat_cmds[flat_idx].valid) continue;
        if (!replay_cmd_is_focus_candidate(flat_cmds[flat_idx].type)) continue;
        if (flat_cmds[flat_idx].src_cmd_idx >= 0)
            return flat_idx;
    }
    return -1;
}

static int replay_last_meaningful_src(int begin, int end_exclusive) {
    int flat_idx = replay_last_meaningful_flat(begin, end_exclusive);
    if (flat_idx < 0)
        return -1;
    return repl_state_flat_program_view().cmds[flat_idx].src_cmd_idx;
}

void replay_set_src_line(int src_line) {
    ReplayRuntimeState *state = replay_state_mut();
    state->src_line_idx = src_line;
    if (src_line != state->last_src_line) {
        state->last_src_line = src_line;
    }
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

int replay_focus_vertex_flat_idx(void) {
    const ReplayRuntimeState *state = replay_state_const();
    /* Vertex replay only: the frame-axes overlay anchors on the vertex the
     * step just emitted, so it is meaningless for polygon-batch stepping. */
    if (!state->active || state->mode != REPLAY_MODE_VERTEX)
        return -1;
    int begin = replay_prev_limit(state->pc);
    FlatProgramView flat = repl_state_flat_program_view();
    const GLCmd *cmds = flat.cmds;
    for (int i = state->pc - 1; i >= begin && i >= 0; i--) {
        if (!cmds[i].valid) continue;
        if (repl_cmd_emits_vertex(cmds[i].type))
            return i;
    }
    return -1;
}

int replay_focus_flat_idx(void) {
    const ReplayRuntimeState *state = replay_state_const();
    if (!state->active)
        return -1;
    /* Derive the active step's begin the same way step-back does, so the
     * focus is identical under advance, seek, step-back, and pause — and
     * never relies on the transient old_pc local or fade-batch state. */
    int begin = replay_prev_limit(state->pc);
    return replay_last_meaningful_flat(begin, state->pc);
}

static void replay_update_after_pc_change(ReplayRuntimeState *state, int num_flat_cmds, int search_begin) {
    if (state->pc < 0)
        state->pc = 0;
    if (state->pc > num_flat_cmds)
        state->pc = num_flat_cmds;

    /* Resolve the focused command once and read both its source line and its
     * funcN call-frame depth, so the HUD "depth N" readout and the highlighted
     * line always describe the same command. */
    int focus = replay_last_meaningful_flat(search_begin, state->pc);
    FlatProgramView focus_flat = repl_state_flat_program_view();
    replay_set_src_line(focus >= 0 ? focus_flat.cmds[focus].src_cmd_idx : -1);
    state->focus_call_depth = focus >= 0 ? focus_flat.cmds[focus].call_depth : 0;

    if (state->pc >= num_flat_cmds && num_flat_cmds > 0) {
        if (state->state == REPLAY_PLAYING) {
            state->state = REPLAY_DONE;
            repl_set_status("Replay: done");
        } else {
            state->state = REPLAY_DONE;
        }
    }
}

void replay_seek(int new_pc) {
    FlatProgramView flat_program = repl_state_flat_program_view();
    int num_flat_cmds = flat_program.cmd_count;
    ReplayRuntimeState *state = replay_state_mut();

    state->pc = new_pc;
    state->accum = 0.0f;
    replay_clear_fade_batches();
    state->state = REPLAY_PAUSED;
    replay_update_after_pc_change(state, num_flat_cmds, 0);
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
    ReplayRuntimeState *state = replay_state_mut();
    state->pc = 0;
    state->accum = 0.0f;
    replay_clear_fade_batches();
    state->state = REPLAY_PLAYING;
    state->src_line_idx = -1;
    state->focus_call_depth = 0;
    state->last_src_line = -1;
}

static void replay_snapshot_baseline(ReplayRuntimeState *state) {
    repl_eval_copy_predef_vars(state->baseline_predef_vals,
                               state->baseline_predef_names,
                               &state->baseline_predef_count);
    repl_eval_copy_scratch_arrays(state->baseline_scratch_arrays);
    state->saved_t_playing = repl_state_variables().time_playing;
    repl_dispatch_set_time_playing(0);
}

static void replay_init_playback_state(ReplayRuntimeState *state, int num_flat_cmds) {
    state->active = 1;
    state->state = REPLAY_PLAYING;
    state->pc = 0;
    state->accum = 0.0f;
    replay_clear_fade_batches();
    state->src_line_idx = -1;
    state->focus_call_depth = 0;
    state->total_flat_cmds = num_flat_cmds;
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

    ReplayRuntimeState *state = replay_state_mut();
    replay_snapshot_baseline(state);
    replay_init_playback_state(state, num_flat_cmds);
    repl_set_status("Replay: playing");
}

void replay_stop(void) {
    ReplayRuntimeState *state = replay_state_mut();
    repl_dispatch_set_time_playing(state->saved_t_playing);
    state->active = 0;
    state->state = REPLAY_OFF;
    state->focus_call_depth = 0;
    replay_clear_fade_batches();
}

void replay_advance(FlatProgramView flat_program) {
    int num_flat_cmds = flat_program.cmd_count;
    ReplayRuntimeState *state = replay_state_mut();
    int old_pc;
    int next_pc;

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

    state->pc = next_pc;
    replay_update_after_pc_change(state, num_flat_cmds, old_pc);
    replay_push_fade_batch(old_pc, state->pc);
}

void replay_step_back(void) {
    ReplayRuntimeState state = replay_state_view();
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
    ReplayRuntimeState *state = replay_state_mut();
    char msg[REPLAY_STATUS_MSG_LEN];
    state->speed *= factor;
    if (state->speed < REPLAY_SPEED_MIN) state->speed = REPLAY_SPEED_MIN;
    if (state->speed > REPLAY_SPEED_MAX) state->speed = REPLAY_SPEED_MAX;
    snprintf(msg, sizeof(msg), "Replay: %.1f step/s", state->speed);
    repl_set_status(msg);
}

void replay_toggle_play_pause(void) {
    ReplayRuntimeState *state = replay_state_mut();
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

int replay_prepare_frame(FlatProgramView flat_program, int full_flat_count) {
    int num_flat_cmds = flat_program.cmd_count;
    ReplayRuntimeState *state = replay_state_mut();
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
    ReplayRuntimeState *state = replay_state_mut();
    repl_eval_restore_predef_values_by_name(state->baseline_predef_vals,
                                            state->baseline_predef_names,
                                            state->baseline_predef_count);
}

void replay_copy_baseline_predef_snapshot(
    float dst_vals[MAX_PREDEF_VARS],
    char  dst_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX],
    int  *dst_count) {
    const ReplayRuntimeState *state = replay_state_const();
    if (dst_vals)
        memcpy(dst_vals, state->baseline_predef_vals,
               sizeof(state->baseline_predef_vals));
    if (dst_names)
        memcpy(dst_names, state->baseline_predef_names,
               sizeof(state->baseline_predef_names));
    if (dst_count)
        *dst_count = state->baseline_predef_count;
}

void replay_copy_baseline_predef_values(float *dst, int max_vals) {
    int n;

    if (!dst || max_vals <= 0)
        return;

    n = max_vals < MAX_PREDEF_VARS ? max_vals : MAX_PREDEF_VARS;
    memcpy(dst, replay_state_mut()->baseline_predef_vals,
           (size_t)n * sizeof(float));
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
