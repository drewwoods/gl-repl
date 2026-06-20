#include "repl/flatten.h"
#include "subsystems/replay/replay_internal.h"
#include <string.h>

float replay_batch_alpha(const ReplayFadeBatch *batch) {
    float alpha = batch->age * replay_state_view().fade_speed;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return alpha;
}

ReplayFadeBatchView replay_fade_batches_view(void) {
    const ReplayRuntimeState *state = replay_state_const();
    ReplayFadeBatchView view = {
        .batches = state->fade_batches,
        .count = state->fade_batch_count,
    };
    return view;
}

int replay_compute_fade_skip_limits(FlatProgramView flat_program, int *out_limits, int max_count) {
    const GLCmd *flat_cmds = flat_program.cmds;
    int num_flat_cmds = flat_program.cmd_count;
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
    if (max_old_pc > num_flat_cmds) max_old_pc = num_flat_cmds;

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
        if (pc >= num_flat_cmds || !flat_cmds[pc].valid)
            continue;
        switch (flat_cmds[pc].type) {
        case CMD_BEGIN: open_begin = pc; break;
        case CMD_END:   open_begin = -1; break;
        case CMD_TESS_BEGIN_POLYGON:
            open_tess = pc;
            tess_depth = replay_advance_tess_depth(flat_cmds[pc].type, tess_depth);
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            tess_depth = replay_advance_tess_depth(flat_cmds[pc].type, tess_depth);
            break;
        case CMD_TESS_END: {
            int old_depth = tess_depth;
            tess_depth = replay_advance_tess_depth(flat_cmds[pc].type, tess_depth);
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

void replay_clear_fade_batches(void) {
    replay_state_mut()->fade_batch_count = 0;
}

void replay_push_fade_batch(int old_pc, int new_pc) {
    ReplayRuntimeState *state = replay_state_mut();
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

void replay_clamp_fade_batches(int max_pc) {
    ReplayRuntimeState *state = replay_state_mut();
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
    ReplayRuntimeState *state = replay_state_mut();
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
    ReplayRuntimeState state = replay_state_view();
    return state.active && state.fade_batch_count > 0;
}

int replay_fill_base_limit(FlatProgramView flat_program) {
    int num_flat_cmds = flat_program.cmd_count;

    if (!replay_has_active_fades())
        return num_flat_cmds;
    ReplayRuntimeState state = replay_state_view();
    if (state.fade_batches[0].old_pc < 0)
        return 0;
    if (state.fade_batches[0].old_pc > num_flat_cmds)
        return num_flat_cmds;
    return state.fade_batches[0].old_pc;
}

int replay_bench_fade_install(const int *old_pcs, const int *new_pcs,
                            int count, float age) {
    ReplayRuntimeState *state = replay_state_mut();
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

    /* Full snapshot — same rationale as replay_start: the fade-baseline
     * spans multiple frames and must survive predef-table reshape. */
    repl_eval_capture_predef_snapshot(&state->baseline_predef);
    repl_eval_copy_scratch_arrays(state->baseline_scratch_arrays);
    state->active = (installed > 0);
    return installed;
}

void replay_bench_fade_clear(void) {
    ReplayRuntimeState *state = replay_state_mut();
    state->fade_batch_count = 0;
    state->active = 0;
}
