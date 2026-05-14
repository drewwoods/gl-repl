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

/* In-flight expected-commit attempt. step_idx == -1 means inactive
 * (no commit in flight); the guard-exception predicate is derived
 * from step_idx >= 0. commit_line is the source-document row the
 * matched expected command is about to land at; doc_count_before is
 * captured so the success bookkeeping can compute how many rows the
 * commit produced. */
typedef struct {
    int step_idx;
    int commit_line;
    int doc_count_before;
} TutorialPendingCommit;

typedef struct {
    int                   active;
    int                   tutorial_idx;
    int                   step;
    int                   locked_line_count;
    int                   locked_lines[TUTORIAL_LOCKED_LINE_MAX];
    int                   fade_line_idx;
    float                 fade_start_t;
    float                 fade_duration;
    /* expected_commit_line is the source row the next user commit
     * should land on. -1 when no step is waiting. For append steps
     * this is document_count; for label-targeted steps this is the
     * row immediately below the newly-inserted instruction
     * comment. */
    int                   expected_commit_line;
    /* Single in-flight commit-attempt record. Populated by the
     * precheck immediately after the matcher passes; consumed by the
     * success or rejection paths. */
    TutorialPendingCommit pending;
    /* Per-step source line of each step's committed command, used to
     * resolve target_label at the moment a later step starts. -1
     * until the step commits. Sized to TUTORIAL_LOCKED_LINE_MAX to
     * match the existing tracked-line cap. */
    int                   committed_line_for_step[TUTORIAL_LOCKED_LINE_MAX];
    TutorialMatchResult   last_result;
} TutorialRuntimeState;

TutorialRuntimeState  tutorial_state_view(void);
TutorialRuntimeState *tutorial_state_mut(void);
void                  tutorial_state_reset(void);
int                   tutorial_active(void);

#endif /* TUTORIAL_STATE_H */
