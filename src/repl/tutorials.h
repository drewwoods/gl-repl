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
 * Three step kinds exist (TutorialStepKind):
 *   COMMAND   the original "type the expected GL call" step.
 *   SET       apply `cfg_slug = cfg_value` on entry so the user sees
 *             the result; advance on Enter/Tab/Space. `expected` MUST
 *             be NULL.
 *   REQUIRE   advance when the user themselves makes
 *             `cfg_slug == cfg_value` (via F-key/menu/etc.); auto-
 *             advances if already satisfied on entry. `expected` MUST
 *             be NULL.
 *
 * The sentinel that terminates a step array has `comment == NULL`
 * (any kind; expected may be NULL too for COMMAND-only sentinels).
 *
 * Catalog validation runs once at tutorial_start before any state
 * mutation; see repl_tutorial_validate(). Slug validity for
 * SET/REQUIRE is checked at tutorial_start (not here) because it
 * depends on the controller-installed config bridge.
 */
#ifndef REPL_TUTORIALS_H
#define REPL_TUTORIALS_H

#include <limits.h>

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

typedef enum {
    /* Default — type the expected GL command. */
    TUTORIAL_STEP_KIND_COMMAND = 0,
    /* Showcase — apply cfg_slug=cfg_value, wait for ack key, advance. */
    TUTORIAL_STEP_KIND_SET,
    /* Check — wait until user sets cfg_slug to cfg_value, then advance. */
    TUTORIAL_STEP_KIND_REQUIRE,
} TutorialStepKind;

typedef struct {
    const char               *label;
    const char               *comment;
    const char               *expected;
    TutorialStepPlacementKind placement;
    const char               *target_label;
    /* Kind + cfg target (used only by SET/REQUIRE). Trailing so existing
     * positional initializers keep zero-initialising to COMMAND/NULL/0. */
    TutorialStepKind          kind;
    const char               *cfg_slug;
    int                       cfg_value;
} TutorialStep;

/* Curated metadata tags used by the Tutorials menu. Tutorials keep their
 * flat identity; tags are only a secondary discovery index. Tag index 0
 * is the synthetic "All" group — every tutorial is a member (folded into
 * the mask by repl_tutorial_tag_mask), so the menu's first flyout lists
 * every tutorial once.
 *
 * The enum is in the header so app-layer code (e.g. an eventual tutorial
 * tag-default bridge) can name tags symbolically. The bit-shifted
 * TUTORIAL_TAG_* macros used inside g_tutorials[] stay private to
 * tutorials.c. Taxonomy is topic-based: GEOMETRY / COLOR_TRANSFORMS /
 * DEPTH_LIGHTING covers the shipped catalog; reserved ANIMATION is
 * filtered out by repl_tutorial_visible_tag_count() until something
 * carries it. */
enum {
    REPL_TUTORIAL_TAG_ALL = 0,
    REPL_TUTORIAL_TAG_GEOMETRY,
    REPL_TUTORIAL_TAG_COLOR_TRANSFORMS,
    REPL_TUTORIAL_TAG_DEPTH_LIGHTING,
    REPL_TUTORIAL_TAG_ANIMATION,
    REPL_TUTORIAL_TAG_COUNT
};

typedef unsigned int ReplTutorialTagMask;

typedef struct {
    const char         *name;
    const TutorialStep *steps;
    /* Optional NULL-terminated array of `// @cfg slug = value` strings
     * applied at tutorial start, after the same presentation reset
     * examples perform. Reuses the example `@cfg` slug vocabulary and
     * bridge (see CLAUDE.md). NULL = no scene-presentation overrides
     * (the default; existing catalog entries get this implicitly). */
    const char *const  *cfg;
    /* Tag bitmask for menu grouping (REPL_TUTORIAL_TAG_* bits OR-ed
     * via the private TUTORIAL_TAG_* macros in tutorials.c). The
     * synthetic ALL bit is folded in by repl_tutorial_tag_mask() so
     * entry literals stay free of it. Zero mask = visible only under
     * "All" — flagged by the metadata test. */
    ReplTutorialTagMask tags;
} TutorialEntry;

int                       repl_tutorial_count(void);
const char               *repl_tutorial_name(int idx);
int                       repl_tutorial_step_count(int idx);
const char               *repl_tutorial_step_comment(int idx, int step_idx);
const char               *repl_tutorial_step_expected(int idx, int step_idx);
TutorialStepPlacementKind repl_tutorial_step_placement(int idx, int step_idx);
const char               *repl_tutorial_step_label(int idx, int step_idx);
const char               *repl_tutorial_step_target_label(int idx, int step_idx);
TutorialStepKind          repl_tutorial_step_kind(int idx, int step_idx);
const char               *repl_tutorial_step_cfg_slug(int idx, int step_idx);
int                       repl_tutorial_step_cfg_value(int idx, int step_idx);

/* The tutorial's leading `@cfg` strings (NULL-terminated array), or NULL
 * when it has none. Out-of-range idx → NULL. */
const char *const        *repl_tutorial_cfg_lines(int idx);

/* Tag query API — mirrors the example tag API in src/repl/examples.h.
 * `tag_count` includes the synthetic ALL at index 0. `tag_mask` ORs the
 * ALL bit into the entry's stored mask so every query (has_tag,
 * count_for_tag, index_for_tag, visible_tag_*) sees the ALL membership
 * uniformly. Out-of-range / invalid inputs return 0 / NULL / -1. */
int                       repl_tutorial_tag_count(void);
const char               *repl_tutorial_tag_label(int tag_idx);
unsigned int              repl_tutorial_tag_mask(int tutorial_idx);
int                       repl_tutorial_has_tag(int tutorial_idx, int tag_idx);
int                       repl_tutorial_count_for_tag(int tag_idx);
int                       repl_tutorial_index_for_tag(int tag_idx, int ordinal);
int                       repl_tutorial_visible_tag_count(void);
int                       repl_tutorial_visible_tag_at(int dense_idx);

static inline unsigned int repl_tutorial_tag_bit(int tag_idx) {
    if (tag_idx < 0 || tag_idx >= repl_tutorial_tag_count() ||
        tag_idx >= (int)(sizeof(unsigned int) * CHAR_BIT))
        return 0u;
    return 1u << (unsigned int)tag_idx;
}

/* Validate a tutorial catalog entry. Returns 1 on success. On failure
 * returns 0 and writes a short diagnostic into `err` (when err_size > 0).
 *
 * Rules enforced:
 *   - Sentinel: first step with `comment == NULL` terminates the array.
 *   - COMMAND steps require non-null comment and expected, and each
 *     expected parses to a single source command and lands at the
 *     runner's chosen row (v1 catalog rule, syntactic best-effort: no
 *     `;`, no `\n`, no block-open `{`/`}`, no `float` declarations of
 *     any shape — single-name float decls are relocated to the top of
 *     non-decl code on commit, which breaks pending.commit_line).
 *   - SET / REQUIRE steps require non-null comment, non-empty cfg_slug,
 *     and expected == NULL. Slug *validity* is checked at runtime in
 *     tutorial_start once the config bridge is installed.
 *   - Every non-empty label is unique within the tutorial.
 *   - TUTORIAL_STEP_APPEND has no non-empty target_label.
 *   - TUTORIAL_STEP_LABEL has a non-null non-empty target_label that
 *     names an earlier non-empty label in the same tutorial (forward
 *     references rejected).
 *   - Step count stays within TUTORIAL_LOCKED_LINE_MAX. */
int repl_tutorial_validate(int idx, char *err, int err_size);

/* Validate an out-of-catalog TutorialEntry. Same rules as above; the
 * idx variant is a thin wrapper that looks up the entry. Exposed so
 * tests can drive the rules directly against synthetic fixtures
 * without polluting the shipped catalog. */
int repl_tutorial_validate_entry(const TutorialEntry *entry,
                                 char *err, int err_size);

#endif /* REPL_TUTORIALS_H */
