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

/* Optional per-tutorial view-mode (2D/3D) preference. INHERIT (the
 * zero value, so existing catalog entries keep it by default) means
 * "leave the current view mode untouched" — view mode is sticky, like
 * the camera. A tutorial that names 2D/3D applies it once at start via
 * the app layer (glr_actions.c reads repl_tutorial_view_mode and calls
 * glr_config_set on GLR_CONFIG_ORTHO_MODE); it is not restored on exit. */
typedef enum {
    TUTORIAL_VIEW_INHERIT = 0,
    TUTORIAL_VIEW_3D,
    TUTORIAL_VIEW_2D,
} TutorialViewMode;

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
    TutorialViewMode    view_mode;  /* INHERIT = keep current view mode */
} TutorialEntry;

int                       repl_tutorial_count(void);
const char               *repl_tutorial_name(int idx);
int                       repl_tutorial_step_count(int idx);
const char               *repl_tutorial_step_comment(int idx, int step_idx);
const char               *repl_tutorial_step_expected(int idx, int step_idx);
TutorialStepPlacementKind repl_tutorial_step_placement(int idx, int step_idx);
const char               *repl_tutorial_step_label(int idx, int step_idx);
const char               *repl_tutorial_step_target_label(int idx, int step_idx);

/* The tutorial's declared view-mode preference, or TUTORIAL_VIEW_INHERIT
 * (the default) when it expresses none. Out-of-range idx → INHERIT. */
TutorialViewMode          repl_tutorial_view_mode(int idx);

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
