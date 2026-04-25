/*
 * repl_replay.h - Replay state machine and replay rendering helpers.
 */
#ifndef REPL_REPLAY_H
#define REPL_REPLAY_H

typedef struct {
    int   old_pc;
    int   new_pc;
    float age;
} ReplayFadeBatch;

typedef struct {
    const ReplayFadeBatch *batches;
    int                    count;
} ReplayFadeBatchView;

#define REPLAY_FADE_BATCH_MAX 24

void repl_replay_start(void);
void repl_replay_stop(void);
void repl_replay_advance(void);
void repl_replay_tick_fade_batches(float dt);
void repl_replay_seek(int new_pc);
int  repl_replay_seek_to_src_line(int target_line);
void repl_replay_step_back(void);
void repl_replay_restart_from_beginning(void);
void repl_replay_speed_adjust(float factor);

int  repl_replay_exec_limit(void);
int  repl_replay_has_active_fades(void);
int  repl_replay_fill_base_limit(void);
int  repl_replay_compute_fade_skip_limits(int *out_limits, int max_count);
ReplayFadeBatchView repl_replay_fade_batches_view(void);
float repl_replay_batch_alpha(const ReplayFadeBatch *batch);
int  repl_replay_prepare_frame(int full_flat_count);
void repl_replay_restore_baseline_predef_values(void);
void repl_replay_copy_baseline_predef_values(float *dst, int max_vals);

int  repl_replay_handle_key(unsigned char key);
int  repl_replay_handle_special_key(int key);

int  repl_bench_fade_install(const int *old_pcs, const int *new_pcs,
                             int count, float age);
void repl_bench_fade_clear(void);

#endif /* REPL_REPLAY_H */
