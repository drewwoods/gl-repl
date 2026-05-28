/*
 * tutorial_state.h - Tutorial peer runtime state and match results.
 *
 * Holds the runtime state for the guided tutorial subsystem: which tutorial and
 * step are active, which source lines are locked or fading, where the next
 * expected commit should land, and the status/result of the last attempted
 * tutorial match. The runner in `tutorial.c` mutates this state; controller,
 * editor, and UI-adjacent code read it through the narrow accessors below.
 */
#ifndef TUTORIAL_STATE_H
#define TUTORIAL_STATE_H

#include "repl/tutorials.h"  /* TUTORIAL_LOCKED_LINE_MAX */
#include "repl/cfg_baseline.h"

#define TUTORIAL_STATUS_MAX 128

typedef enum {
    TUT_MATCH_OK = 0,
    TUT_MISMATCH_EMPTY,
    TUT_MISMATCH_SHAPE,
} TutorialMatchKind;

/* Result of matching the user's current input against the tutorial's expected
 * command for the active step. */
typedef struct {
    TutorialMatchKind kind;
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

/* Full runtime state for an active tutorial session. */
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
    /* Per-step source line of each step's INSTRUCTION COMMENT (not
     * the committed command), used to resolve target_label at the
     * moment a later step starts. Anchoring on the instruction
     * comment lets a label-targeted step splice ABOVE the original
     * (instruction, command) pair so the comment stays adjacent to
     * the command it describes. -1 until the step's instruction
     * has been emitted. Shifted alongside locked_lines and
     * fade_line_idx by tutorial_shift_tracked_lines_from. Sized to
     * TUTORIAL_MAX_STEPS (one slot per catalog step). */
    int                   instruction_line_for_step[TUTORIAL_MAX_STEPS];
    ReplConfigBag         baseline_bag;
    int                   baseline_valid;
} TutorialRuntimeState;

/* Read-only/by-pointer accessors plus reset helpers. */
TutorialRuntimeState  tutorial_state_view(void);
TutorialRuntimeState *tutorial_state_mut(void);
void                  tutorial_state_reset(void);
void                  tutorial_state_reset_except_baseline(void);
void                  tutorial_state_init_explicit(void);
int                   tutorial_active(void);

#endif /* TUTORIAL_STATE_H */
