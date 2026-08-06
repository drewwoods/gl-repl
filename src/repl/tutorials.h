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
 * Five step kinds exist (TutorialStepKind):
 *   COMMAND      the original "type the expected GL call" step.
 *                `comment` is OPTIONAL: NULL (or empty) commits the
 *                expected command with no locked instruction row above
 *                it - the autocomplete ghost and status hint still
 *                teach the command, so runs of related commands don't
 *                need a narration comment each. The expected text may
 *                also be a block header (`for(i, 0, 8) {`), an if
 *                branch (`} else {`), or a close brace (`}`) - see
 *                TutorialExpectedShape below; block bodies are typed
 *                as ordinary COMMAND steps between the open and close.
 *   NOTE         a comment-only step: reveal the instruction comment,
 *                wait for an ack key (Enter/Tab/Space), advance. The
 *                showcase flow of SET without a cfg write. `expected`
 *                MUST be NULL and `comment` MUST be non-empty.
 *   SET          apply `cfg_slug = cfg_value` on entry so the user sees
 *                the result; advance on Enter/Tab/Space. `expected` MUST
 *                be NULL.
 *   REQUIRE      advance when the user themselves makes
 *                `cfg_slug == cfg_value` (via F-key/menu/etc.); auto-
 *                advances if already satisfied on entry. `expected` MUST
 *                be NULL.
 *   REQUIRE_VAR  advance when the named predefined variable's live
 *                value matches `var_target` within TUTORIAL_VAR_EPS.
 *                Either path satisfies it: a typed `name = expr;`
 *                commit or a variable-panel slider drag. Document
 *                mutation stays allowed (unlike SET/REQUIRE) so the
 *                user can type the assignment. `expected` MUST be NULL
 *                and `var_name` MUST be non-empty.
 *
 * The sentinel that terminates a step array has `comment`, `expected`
 * AND `cfg_slug` all NULL (see repl_tutorial_step_is_sentinel). Every
 * real step carries at least one of the three: COMMAND requires
 * `expected`; SET_QUIET requires `cfg_slug` and forbids `comment`;
 * every other kind requires `comment`. The definition has widened twice,
 * each time because a new comment-less shape would otherwise have read
 * as the terminator - first `comment == NULL` alone (until COMMAND
 * comments became optional), then the comment/expected pair (until
 * SET_QUIET, which has neither).
 *
 * Catalog validation runs once at tutorial_start before any state
 * mutation; see repl_tutorial_validate(). Slug validity for
 * SET/REQUIRE is checked at tutorial_start (not here) because it
 * depends on the controller-installed config bridge.
 */
#ifndef REPL_TUTORIALS_H
#define REPL_TUTORIALS_H

#include <limits.h>

#include "c_compat.h"           /* STATIC_ASSERT */
#include "repl/catalog_tags.h"  /* repl_catalog_tag_bit_for_count */

/* TUTORIAL_MAX_STEPS bounds the step count per tutorial: the catalog
 * validator rejects entries above this, and the per-step state arrays
 * (e.g. `instruction_line_for_step[]`) size off it.
 *
 * TUTORIAL_LOCKED_LINE_MAX bounds how many source lines the runtime
 * can hold "locked" against edits at once.
 *
 * The two used to be one constant. The lock table is now larger than
 * the step ceiling because preloaded `setup` scaffold rows and block
 * structure rows are locked too. The catalog validator budgets setup
 * rows + one baseline lock per step + two extra locks per block OPEN +
 * one extra per BRANCH. The static-assert below pins the >= relationship
 * without fusing the constants. */
#define TUTORIAL_MAX_STEPS         64
#define TUTORIAL_LOCKED_LINE_MAX   128
STATIC_ASSERT(TUTORIAL_LOCKED_LINE_MAX >= TUTORIAL_MAX_STEPS,
              "Locked-line capacity must hold every possible per-step lock");

/* The tutorial runner injects a fixed scene prelude ahead of every
 * tutorial's steps and setup scaffold: a one-line explanatory comment and
 * the glClear it describes (g_tutorial_scene_prelude in
 * tutorial_runner.c). Both rows are locked, so the locked-line budget in
 * repl_tutorial_validate() reserves them. Kept here so the runner (which
 * loads the rows) and the validator (which budgets for them) can't drift. */
#define TUTORIAL_SCENE_PRELUDE_ROWS 2

/* Maximum length of a setup-scaffold anchor name, including the NUL. */
#define TUTORIAL_ANCHOR_NAME_MAX   64

/* Match the setup-scaffold anchor directive: a line whose entire text is
 * `// @anchor <name>`. On a match `name` receives the anchor name and 1 is
 * returned; otherwise `name` is emptied and 0 is returned.
 *
 * The directive is an ordinary CMD_COMMENT row - flatten drops it, export
 * writes it back verbatim, and it needs no CmdType, parser grammar, or spec
 * entry of its own. It marks a position in a tutorial's preloaded scaffold
 * that TUTORIAL_STEP_LABEL steps can splice above (see target_label).
 *
 * Both consumers share this one matcher: the catalog validator applies it to
 * TutorialEntry.setup strings, and the runner applies it to the live
 * document's comment rows at step-entry time. Tutorial-private - anchors are
 * not a user-facing document feature. */
int repl_tutorial_anchor_name(const char *line, char *name, int name_sz);

typedef enum {
    TUTORIAL_STEP_APPEND = 0,
    TUTORIAL_STEP_LABEL,
} TutorialStepPlacementKind;

typedef enum {
    /* Default - type the expected GL command. `comment` optional:
     * NULL/empty commits with no locked instruction row (the ghost and
     * status hint still teach the command). */
    TUTORIAL_STEP_KIND_COMMAND = 0,
    /* Showcase - apply cfg_slug=cfg_value, wait for ack key, advance. */
    TUTORIAL_STEP_KIND_SET,
    /* Check - wait until user sets cfg_slug to cfg_value, then advance. */
    TUTORIAL_STEP_KIND_REQUIRE,
    /* Check - wait until the named predef variable's live value matches
     * var_target within TUTORIAL_VAR_EPS. Either a typed `name = expr;`
     * commit or a variable-panel slider drag satisfies the step;
     * document mutation stays allowed. */
    TUTORIAL_STEP_KIND_REQUIRE_VAR,
    /* Comment-only - reveal the instruction comment, wait for an ack
     * key (Enter/Tab/Space), advance. SET's showcase flow without the
     * cfg write; the document is frozen while it waits. */
    TUTORIAL_STEP_KIND_NOTE,
    /* Staging - apply cfg_slug=cfg_value and advance IMMEDIATELY: no
     * instruction row, no ack, no pause. SET without the showcase, for
     * settings that are scaffolding rather than lesson content, so a run
     * of them stacks without a comment and an Enter apiece. `comment`
     * MUST be NULL (there is no row to put one on) and `cfg_slug` MUST
     * be non-empty - that slug is also what distinguishes the step from
     * the array sentinel. Prefer TutorialEntry.cfg when the settings can
     * be applied at tutorial start; this kind is for staging that has to
     * happen partway through a lesson. */
    TUTORIAL_STEP_KIND_SET_QUIET,
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
    /* Optional symbolic value name (e.g. "GRID_THEME_RADAR"). When
     * non-NULL the runner resolves the name through the
     * controller-installed bridge at apply time (SET) or compare time
     * (REQUIRE) instead of using the raw `cfg_value` integer. Lets the
     * catalog name the enum constant rather than encoding magic
     * numbers. */
    const char               *cfg_value_name;
    /* Predef-variable target (REQUIRE_VAR only). `var_name` names a
     * predefined variable that must exist when the step is entered;
     * `var_target` is the float value the step advances on. Trailing
     * so existing positional initializers (COMMAND/SET/REQUIRE steps)
     * keep zero-initialising these to NULL/0.0f. */
    const char               *var_name;
    float                     var_target;
    /* Optional shortcut interpolation for `comment`. When
     * comment_binding_key is non-zero, repl_tutorial_step_comment()
     * treats comment as a printf-style format string with one `%s`
     * placeholder and fills it with keymap_binding_to_string(...).
     * Kept trailing so older positional initializers remain valid. */
    int                       comment_binding_key;
    int                       comment_binding_mods;
    int                       comment_binding_is_special;
} TutorialStep;

/* The step-array terminator: a record with NO comment, NO expected and
 * NO cfg_slug. Every real step has at least one of the three (COMMAND
 * requires expected, SET_QUIET requires cfg_slug and forbids a comment,
 * all other kinds require a comment), so the triple-NULL record stays
 * unambiguous. Single definition - every walker (accessors, validators,
 * the runner) must use this rather than open-coding a `!comment` test,
 * which would misread a comment-less COMMAND step as the terminator;
 * the cfg_slug term is what keeps a SET_QUIET step from doing the same. */
static inline int repl_tutorial_step_is_sentinel(const TutorialStep *step) {
    return step->comment == NULL && step->expected == NULL &&
           step->cfg_slug == NULL;
}

/* Curated metadata tags used by the Tutorials menu. Tutorials keep their
 * flat identity; tags are only a secondary discovery index. Tag index 0
 * is the synthetic "All" group - every tutorial is a member (returned
 * unconditionally by tutorial_catalog_entry_has_tag), so the menu's first
 * flyout lists every tutorial once.
 *
 * The enum is in the header so app-layer code can name tags symbolically;
 * g_tutorials[] entries name tags by string in their `tag_names` list
 * (tutorials.c), matched against this enum's order via g_tutorial_tag_labels.
 * Taxonomy is topic-based: GEOMETRY / COLOR_TRANSFORMS / DEPTH_LIGHTING /
 * ANIMATION / REPL_LANGUAGE / EFFECTS. Tags without a catalog entry stay
 * filtered out by repl_tutorial_visible_tag_count(). */
enum {
    REPL_TUTORIAL_TAG_ALL = 0,
    REPL_TUTORIAL_TAG_GEOMETRY,
    REPL_TUTORIAL_TAG_COLOR_TRANSFORMS,
    REPL_TUTORIAL_TAG_DEPTH_LIGHTING,
    REPL_TUTORIAL_TAG_ANIMATION,
    REPL_TUTORIAL_TAG_REPL_LANGUAGE,
    REPL_TUTORIAL_TAG_EFFECTS,
    REPL_TUTORIAL_TAG_COUNT
};

typedef struct {
    const char         *name;
    const TutorialStep *steps;
    /* Optional NULL-terminated array of `// @cfg slug = value` strings
     * applied at tutorial start, after the same presentation reset
     * examples perform. Reuses the example `@cfg` slug vocabulary and
     * bridge (see CLAUDE.md). NULL = no scene-presentation overrides
     * (the default; existing catalog entries get this implicitly). */
    const char *const  *cfg;
    /* Optional NULL-terminated array of scaffold source lines preloaded
     * into the transient scene at tutorial start, BEFORE step 0 - so a
     * tutorial can build on code an earlier tutorial taught without
     * making the learner re-type it. Honors the example header
     * vocabulary: leading `// @cfg slug = value` lines and an optional
     * `@camera`-tagged transform rows are consumed like example metadata
     * (setup @cfg slugs join the teardown-restore baseline); the
     * remaining body lines are REPL source fed through the non-editor
     * loader. Every loaded row is LOCKED (read-only for the duration
     * of the tutorial). Body lines may carry `// @anchor <name>`
     * directives; label-placement steps can target those (see
     * target_label).
     * NULL = start from an empty scene (the default). */
    const char *const  *setup;
    /* Null-terminated list of tag string labels for tutorial entry */
    const char *const  *tag_names;
    /* Optional in-flyout section subheading. Free-form display string
     * (e.g. "Beginner", "Grids", "Lighting") - the menu groups
     * consecutive tutorials sharing the same subheading under a chrome
     * `### subheading` header. NULL = no section header (the tutorial
     * appears under whichever header preceded it in catalog order, or
     * at the top with no header). Convention enforced by the metadata
     * test: per tag, each non-NULL subheading must appear in a single
     * contiguous run - interleaving causes duplicate headers. */
    const char         *subheading;
} TutorialEntry;

int                       repl_tutorial_count(void);
const char               *repl_tutorial_name(int idx);
int                       repl_tutorial_step_count(int idx);

/* Return the raw catalog entry at `idx`, or NULL for out-of-range.
 * Exposed so the bridge-based validators in
 * src/subsystems/tutorial/tutorial.c can be exercised against
 * out-of-catalog synthetic entries from tests without per-field
 * shimming. */
const TutorialEntry      *repl_tutorial_entry(int idx);

/* Return a pointer to the i-th step's struct, or NULL on out-of-range.
 * Performs the O(step_idx) sentinel walk once; callers that need
 * multiple fields off the same step should hold this pointer rather
 * than re-querying via the per-field shims below (rendering a tutorial
 * row touches 5+ fields - without this entry point each accessor would
 * re-walk, making paint O(N^2) per tutorial). */
const TutorialStep       *repl_tutorial_step_get(int idx, int step_idx);

/* Per-field shims. Each is one walk + one field read. Use the bulk
 * accessor above when more than one field is needed for the same
 * step. `comment` is a real function because some catalog comments
 * interpolate keymap.h shortcuts at read time. */
const char *repl_tutorial_step_comment(int idx, int step_idx);
static inline const char *repl_tutorial_step_expected(int idx, int step_idx) {
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    return step ? step->expected : NULL;
}
static inline TutorialStepPlacementKind repl_tutorial_step_placement(int idx, int step_idx) {
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    return step ? step->placement : TUTORIAL_STEP_APPEND;
}
static inline const char *repl_tutorial_step_label(int idx, int step_idx) {
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    return step ? step->label : NULL;
}
static inline const char *repl_tutorial_step_target_label(int idx, int step_idx) {
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    return step ? step->target_label : NULL;
}
static inline TutorialStepKind repl_tutorial_step_kind(int idx, int step_idx) {
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    return step ? step->kind : TUTORIAL_STEP_KIND_COMMAND;
}
static inline const char *repl_tutorial_step_cfg_slug(int idx, int step_idx) {
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    return step ? step->cfg_slug : NULL;
}
static inline int repl_tutorial_step_cfg_value(int idx, int step_idx) {
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    return step ? step->cfg_value : 0;
}
static inline const char *repl_tutorial_step_cfg_value_name(int idx, int step_idx) {
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    return step ? step->cfg_value_name : NULL;
}
static inline const char *repl_tutorial_step_var_name(int idx, int step_idx) {
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    return step ? step->var_name : NULL;
}
static inline float repl_tutorial_step_var_target(int idx, int step_idx) {
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    return step ? step->var_target : 0.0f;
}

/* The tutorial's leading `@cfg` strings (NULL-terminated array), or NULL
 * when it has none. Out-of-range idx -> NULL. */
const char *const        *repl_tutorial_cfg_lines(int idx);

/* The tutorial's preloaded scaffold lines (NULL-terminated array), or
 * NULL when it starts from an empty scene. Out-of-range idx -> NULL.
 * See TutorialEntry.setup. */
const char *const        *repl_tutorial_setup_lines(int idx);

/* Tag query API - mirrors the example tag API in src/repl/examples.h.
 * `tag_count` includes the synthetic ALL at index 0. */
int                       repl_tutorial_tag_count(void);
const char               *repl_tutorial_tag_label(int tag_idx);
int                       repl_tutorial_has_tag(int tutorial_idx, int tag_idx);
int                       repl_tutorial_count_for_tag(int tag_idx);
int                       repl_tutorial_index_for_tag(int tag_idx, int ordinal);
int                       repl_tutorial_visible_tag_count(void);
int                       repl_tutorial_visible_tag_at(int dense_idx);

/* Subheading API. Returns the tutorial's free-form section label or NULL
 * (no subheading / out-of-range idx). The Tutorials menu uses this to
 * group entries within a tag flyout - see TutorialEntry.subheading and
 * the contiguous-runs convention noted there. */
const char               *repl_tutorial_subheading(int tutorial_idx);

/* Shape of a COMMAND step's `expected` text. ORDINARY is the classic
 * single-row command. The three block shapes teach the REPL's
 * structured blocks (`for` / `funcN` / named-func defs / `if`):
 *   BLOCK_OPEN    "for(i, 0, 8) {" / "if(expr) {" / "name(args) {" -
 *                 commits TWO rows (header + auto-inserted `}`) and
 *                 leaves the cursor between them in insert mode.
 *   BLOCK_BRANCH  "} else {" / "} else if(expr) {" - commits one
 *                 branch-separator row inside an open if-block.
 *   BLOCK_CLOSE   "}" - matches the auto-inserted end row (no document
 *                 change; cursor advances past it, insert mode off).
 * Classified syntactically by repl_tutorial_expected_shape(); the
 * validator uses the shape to track block depth across a tutorial's
 * steps, and the runner uses it to keep locked-line / block-depth
 * bookkeeping in step with what the editor's block commits actually
 * do to the document. */
typedef enum {
    TUTORIAL_EXPECTED_ORDINARY = 0,
    TUTORIAL_EXPECTED_BLOCK_OPEN,
    TUTORIAL_EXPECTED_BLOCK_BRANCH,
    TUTORIAL_EXPECTED_BLOCK_CLOSE,
} TutorialExpectedShape;

TutorialExpectedShape repl_tutorial_expected_shape(const char *expected);

/* True iff `expected` is a BLOCK_OPEN whose header is a function
 * definition (`name(args) {` where name is neither `for` nor `if`).
 * Func defs relocate to the top of non-decl code on commit, so the
 * validator only allows them while nothing but comments / decls /
 * completed func blocks precede (relocation is then an identity). */
int repl_tutorial_expected_is_func_open(const char *expected);

/* Validate a tutorial catalog entry. Returns 1 on success. On failure
 * returns 0 and writes a short diagnostic into `err` (when err_size > 0).
 *
 * Rules enforced:
 *   - Sentinel: first step with `comment`, `expected` and `cfg_slug`
 *     all NULL terminates the array
 *     (repl_tutorial_step_is_sentinel).
 *   - COMMAND steps require non-null expected (comment is optional),
 *     and each expected parses to a single source command and lands at
 *     the runner's chosen row (v1 catalog rule, syntactic best-effort:
 *     no `;`, no `\n`, no `float` declarations of any shape -
 *     a float decl typed below the prologue is relocated to the top of
 *     non-decl code on commit, which breaks pending.commit_line). Braces are
 *     allowed only in the block shapes classified by
 *     repl_tutorial_expected_shape(); the validator walks the steps
 *     with a depth counter: opens/branches/closes must balance
 *     (depth 0 at the sentinel), a branch needs an enclosing if-open,
 *     ORDINARY `for(`/`if(`-prefixed text without a trailing `{` is
 *     rejected (it would still commit as a two-row block), func-shaped
 *     opens are rejected at nesting depth > 0, after any non-func
 *     top-level content, or when the entry has a setup scaffold
 *     (relocation would break row bookkeeping), and while a block is
 *     open both label placement and REQUIRE_VAR steps are rejected
 *     (decl relocation / free-line commits don't mix with an open
 *     block).
 *   - NOTE steps require a non-empty comment and expected == NULL.
 *   - SET / REQUIRE steps require non-empty comment and cfg_slug,
 *     and expected == NULL. Slug *validity* is checked at runtime in
 *     tutorial_start once the config bridge is installed.
 *   - SET_QUIET steps require non-empty cfg_slug, expected == NULL and
 *     comment == NULL (a comment it would never render is an authoring
 *     mistake, not a harmless extra). Slug validity is checked at
 *     tutorial_start alongside SET / REQUIRE.
 *   - Every non-empty label is unique within the tutorial AND does not
 *     collide with a `// @anchor` defined in the setup scaffold.
 *   - TUTORIAL_STEP_APPEND has no non-empty target_label.
 *   - TUTORIAL_STEP_LABEL has a non-null non-empty target_label that
 *     names an earlier non-empty step label in the same tutorial
 *     (forward references rejected) OR a `// @anchor` defined in
 *     the setup scaffold.
 *   - Step count stays within TUTORIAL_MAX_STEPS, and setup lines +
 *     steps together fit the locked-line table
 *     (TUTORIAL_LOCKED_LINE_MAX). */
int repl_tutorial_validate(int idx, char *err, int err_size);

/* Validate an out-of-catalog TutorialEntry. Same rules as above; the
 * idx variant is a thin wrapper that looks up the entry. Exposed so
 * tests can drive the rules directly against synthetic fixtures
 * without polluting the shipped catalog. */
int repl_tutorial_validate_entry(const TutorialEntry *entry,
                                 char *err, int err_size);

#endif /* REPL_TUTORIALS_H */
