/*
 * tutorials.h - Built-in tutorial catalog.
 *
 * Each tutorial is a name plus a NULL-terminated array of TutorialStep
 * records. A step has an optional label naming the source line its
 * `expected` command will commit, an instruction `comment` that the
 * runner reveals to the user, the `expected` command itself, a
 * placement kind, and (for label-targeted steps) the label of an
 * earlier step whose committed source line should anchor the new
 * instruction comment.
 *
 * Append steps reveal the next instruction at the end of the document
 * (the original tutorial behavior). Label-targeted steps splice the
 * instruction comment above the earlier labeled command line so
 * tutorials can teach "draw first, then insert setup before the batch".
 *
 * Catalog validation runs once at tutorial_start before any state
 * mutation; see repl_tutorial_validate().
 */
#ifndef REPL_TUTORIALS_H
#define REPL_TUTORIALS_H

/* Maximum tutorial step count and tracked-line cap. Lives here
 * (rather than in widgets/tutorial_state.h) so the catalog
 * validator can use it without taking a dependency on widget
 * runtime state — the catalog defines the upper bound and the
 * widget-side state arrays consume it. */
#define TUTORIAL_LOCKED_LINE_MAX 64

typedef enum {
    TUTORIAL_STEP_APPEND = 0,
    TUTORIAL_STEP_LABEL,
} TutorialStepPlacementKind;

typedef struct {
    const char               *label;
    const char               *comment;
    const char               *expected;
    TutorialStepPlacementKind placement;
    const char               *target_label;
} TutorialStep;

typedef struct {
    const char         *name;
    const TutorialStep *steps;
    /* Optional NULL-terminated array of `// @cfg slug = value` strings
     * applied at tutorial start, after the same presentation reset
     * examples perform. Reuses the example `@cfg` slug vocabulary and
     * bridge (see CLAUDE.md). NULL = no scene-presentation overrides
     * (the default; existing catalog entries get this implicitly). */
    const char *const  *cfg;
} TutorialEntry;

int                       repl_tutorial_count(void);
const char               *repl_tutorial_name(int idx);
int                       repl_tutorial_step_count(int idx);
const char               *repl_tutorial_step_comment(int idx, int step_idx);
const char               *repl_tutorial_step_expected(int idx, int step_idx);
TutorialStepPlacementKind repl_tutorial_step_placement(int idx, int step_idx);
const char               *repl_tutorial_step_label(int idx, int step_idx);
const char               *repl_tutorial_step_target_label(int idx, int step_idx);

/* The tutorial's leading `@cfg` strings (NULL-terminated array), or NULL
 * when it has none. Out-of-range idx → NULL. */
const char *const        *repl_tutorial_cfg_lines(int idx);

/* Validate a tutorial catalog entry. Returns 1 on success. On failure
 * returns 0 and writes a short diagnostic into `err` (when err_size > 0).
 *
 * Rules enforced:
 *   - Each step before the sentinel has non-null comment and expected.
 *   - Every non-empty label is unique within the tutorial.
 *   - TUTORIAL_STEP_APPEND has no non-empty target_label.
 *   - TUTORIAL_STEP_LABEL has a non-null non-empty target_label that
 *     names an earlier non-empty label in the same tutorial (forward
 *     references rejected).
 *   - Each expected parses to a single source command and lands at
 *     the runner's chosen row (v1 catalog rule, syntactic best-
 *     effort: no `;`, no `\n`, no block-open `{`/`}`, no `float`
 *     declarations of any shape — single-name float decls are
 *     relocated to the top of non-decl code on commit, which
 *     breaks the runner's pending.commit_line bookkeeping).
 *   - Step count stays within TUTORIAL_LOCKED_LINE_MAX. */
int repl_tutorial_validate(int idx, char *err, int err_size);

/* Validate an out-of-catalog TutorialEntry. Same rules as above; the
 * idx variant is a thin wrapper that looks up the entry. Exposed so
 * tests can drive the rules directly against synthetic fixtures
 * without polluting the shipped catalog. */
int repl_tutorial_validate_entry(const TutorialEntry *entry,
                                 char *err, int err_size);

#endif /* REPL_TUTORIALS_H */
