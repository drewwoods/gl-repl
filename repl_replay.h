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

void replay_start(void);
void replay_stop(void);
void replay_advance(void);
void replay_tick_fade_batches(float dt);
void replay_seek(int new_pc);
int  replay_seek_to_src_line(int target_line);
void replay_step_back(void);
void replay_restart_from_beginning(void);
void replay_speed_adjust(float factor);

int  replay_exec_limit(void);
int  replay_has_active_fades(void);
int  replay_fill_base_limit(void);
int  replay_compute_fade_skip_limits(int *out_limits, int max_count);
ReplayFadeBatchView replay_fade_batches_view(void);
float replay_batch_alpha(const ReplayFadeBatch *batch);
int  replay_prepare_frame(int full_flat_count);
void replay_restore_baseline_predef_values(void);
void repl_copy_replay_baseline_predef_values(float *dst, int max_vals);

int  replay_handle_key(unsigned char key);
int  replay_handle_special_key(int key);

int  repl_bench_fade_install(const int *old_pcs, const int *new_pcs,
                             int count, float age);
void repl_bench_fade_clear(void);

#endif /* REPL_REPLAY_H */
