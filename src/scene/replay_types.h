/*
 * src/scene/replay_types.h - small POD types the scene's replay-fade
 * pass consumes. These describe geometry-fade rendering, so they live
 * with the scene module. replay.h includes this header to share the
 * struct with the REPL replay state machine that produces batches.
 */
#ifndef SCENE_REPLAY_TYPES_H
#define SCENE_REPLAY_TYPES_H

#define REPLAY_FADE_BATCH_MAX 24

/* A snapshot of geometry from [old_pc, new_pc) that fades out as new
 * geometry appears. age is the fade timestamp (incremented by the REPL
 * replay state machine). Multiple batches can be active simultaneously
 * via a ring buffer. */
typedef struct {
    int   old_pc;
    int   new_pc;
    float age;
} ReplayFadeBatch;

/* Read-only view over the active fade batches; valid for one frame. */
typedef struct {
    const ReplayFadeBatch *batches;
    int                    count;
} ReplayFadeBatchView;

#endif /* SCENE_REPLAY_TYPES_H */
