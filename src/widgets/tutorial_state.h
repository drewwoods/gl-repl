/*
 * tutorial_state.h - Tutorial peer subsystem ownership.
 */
#ifndef TUTORIAL_STATE_H
#define TUTORIAL_STATE_H

#define TUTORIAL_LOCKED_LINE_MAX 64
#define TUTORIAL_STATUS_MAX 128

typedef enum {
    TUT_MATCH_OK = 0,
    TUT_MISMATCH_EMPTY,
    TUT_MISMATCH_SHAPE,
    TUT_MISMATCH_COMMAND,
    TUT_MISMATCH_ARG,
} TutorialMatchKind;

typedef struct {
    TutorialMatchKind kind;
    int               arg_index;
    char              message[TUTORIAL_STATUS_MAX];
} TutorialMatchResult;

typedef struct {
    int                 active;
    int                 tutorial_idx;
    int                 step;
    int                 locked_line_count;
    int                 locked_lines[TUTORIAL_LOCKED_LINE_MAX];
    int                 fade_line_idx;
    float               fade_start_t;
    float               fade_duration;
    TutorialMatchResult last_result;
} TutorialRuntimeState;

TutorialRuntimeState  tutorial_state_view(void);
TutorialRuntimeState *tutorial_state_mut(void);
void                  tutorial_state_reset(void);
int                   tutorial_active(void);

#endif /* TUTORIAL_STATE_H */