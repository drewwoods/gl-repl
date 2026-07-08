#include "repl/tutorials.h"
#include "repl/catalog_tags.h"
#include "repl/eval.h"   /* repl_eval_is_reserved_ident for REQUIRE_VAR validation */
#include "gl_includes.h"
#include "keymap.h"
#include "keys.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "c_compat.h"   /* STATIC_ASSERT for the tag-label table */

#define STEP_APPEND(label, c, e) \
    { (label), (c), (e), TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f }

/* Comment-less append COMMAND step: commits `e` with no locked
 * instruction row above it — the autocomplete ghost and status hint
 * still teach the command. Use for runs of related commands where a
 * narration comment per line would just be noise. */
#define STEP_CMD(label, e) \
    { (label), NULL, (e), TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f }

/* Comment-only step: reveal the instruction comment, wait for an ack
 * key (Enter/Tab/Space), advance. SET's showcase flow without the cfg
 * write — narration between commands with no GL call to type. */
#define STEP_NOTE(c) \
    { NULL, (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_NOTE, NULL, 0, NULL, NULL, 0.0f }

#define STEP_AT(label, c, e, target) \
    { (label), (c), (e), TUTORIAL_STEP_LABEL, (target), \
      TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f }

/* Showcase step: on entry apply cfg_slug=cfg_value so the user sees the
 * effect, show a "press Enter to continue" prompt, advance on ack key.
 * `expected` is NULL — there is no command to type. */
#define STEP_SET(label, c, slug, val) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_SET, (slug), (val), NULL, NULL, 0.0f }

/* Symbolic-value variant of STEP_SET. The runner passes `val_name`
 * (e.g. "GRID_THEME_RADAR") through the controller-installed bridge's
 * resolve_text at apply time so the catalog reads the enum constant
 * rather than encoding a magic number. `cfg_value` stays 0 here as
 * the back-compat fallback in case a future caller resolves the name
 * and writes the int back. */
#define STEP_SET_SYM(label, c, slug, val_name) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_SET, (slug), 0, (val_name), NULL, 0.0f }

/* Check step: advance when the user themselves makes cfg_slug == cfg_value
 * (via F-key/menu/etc.). Auto-advances if already satisfied on entry.
 * `expected` is NULL. */
#define STEP_REQUIRE(label, c, slug, val) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_REQUIRE, (slug), (val), NULL, NULL, 0.0f }

#define STEP_REQUIRE_KEY(label, c, slug, val, key_code, mods, is_special) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_REQUIRE, (slug), (val), NULL, NULL, 0.0f, \
      (key_code), (mods), (is_special) }

/* Symbolic-value variant of STEP_REQUIRE. The runner resolves
 * `val_name` to int via the bridge's resolve_text at compare time. */
#define STEP_REQUIRE_SYM(label, c, slug, val_name) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_REQUIRE, (slug), 0, (val_name), NULL, 0.0f }

/* Predef-var check step: advance when the named predefined variable's
 * live value matches `target` within TUTORIAL_VAR_EPS. Either a typed
 * `name = expr;` commit or a variable-panel slider drag satisfies it.
 * Auto-advances if already satisfied on entry. `expected` is NULL. */
#define STEP_REQUIRE_VAR(label, c, var, target) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_REQUIRE_VAR, NULL, 0, NULL, (var), (target) }

/* Sentinel: comment AND expected both NULL (repl_tutorial_step_is_sentinel).
 * Every real step carries at least one of the two — SET/REQUIRE/NOTE have
 * NULL `expected` but a comment; comment-less COMMAND steps have NULL
 * `comment` but an expected. */
#define STEP_SENTINEL { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL, \
                        TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f }

static const TutorialStep g_tutorial_first_triangle_steps[] = {
    STEP_APPEND(NULL,
        "// Build the smallest filled shape: open a GL_TRIANGLES batch.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Place the first vertex near the top; this becomes the triangle tip.",
        "glVertex3f(0, 0.8, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-left corner so the triangle has width.",
        "glVertex3f(-0.8, -0.6, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-right corner; three vertices complete one triangle.",
        "glVertex3f(0.8, -0.6, 0)"),
    STEP_APPEND(NULL,
        "// Close the batch and the filled triangle appears in the scene.",
        "glEnd()"),
    STEP_SENTINEL,
};

static const TutorialStep g_tutorial_color_transform_steps[] = {
    STEP_APPEND(NULL,
        "// Save the current matrix so this example can clean up after itself.",
        "glPushMatrix()"),
    STEP_APPEND(NULL,
        "// Set the drawing color; OpenGL keeps using it for later vertices.",
        "glColor3f(0.2, 0.8, 1)"),
    STEP_APPEND(NULL,
        "// Move the local coordinate system to the right before drawing.",
        "glTranslatef(0.4, 0, 0)"),
    STEP_APPEND(NULL,
        "// Rotate those local axes 30 degrees around Z, the screen-facing axis.",
        "glRotatef(30, 0, 0, 1)"),
    STEP_APPEND(NULL,
        "// Open a GL_QUADS batch; the next four corners make one filled square.",
        "glBegin(GL_QUADS)"),
    STEP_APPEND(NULL,
        "// Give the square its lower-left corner in the transformed space.",
        "glVertex3f(-0.2, -0.2, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-right corner at the same height.",
        "glVertex3f(0.2, -0.2, 0)"),
    STEP_APPEND(NULL,
        "// Add the upper-right corner so the quad has height.",
        "glVertex3f(0.2, 0.2, 0)"),
    STEP_APPEND(NULL,
        "// Add the upper-left corner; vertex order walks around the square.",
        "glVertex3f(-0.2, 0.2, 0)"),
    STEP_APPEND(NULL,
        "// Close the quad batch to draw the rotated cyan square.",
        "glEnd()"),
    STEP_APPEND(NULL,
        "// Restore the saved matrix so future commands are not moved or rotated.",
        "glPopMatrix()"),
    STEP_SENTINEL,
};

/* This label-targeted tutorial first has the user draw a
 * triangle (five append steps), then a sixth step splices
 * glEnable(GL_DEPTH_TEST) above the original glBegin so the
 * batch renders with depth testing already enabled. The label
 * "triangle_begin" anchors the insertion (implemented in Phase 2). */
static const TutorialStep g_tutorial_depth_triangle_steps[] = {
    STEP_APPEND("triangle_begin",
        "// Start the triangle batch; the label here anchors a later insert.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Add the top vertex.",
        "glVertex3f(0, 0.8, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-left vertex.",
        "glVertex3f(-0.8, -0.6, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-right vertex.",
        "glVertex3f(0.8, -0.6, 0)"),
    STEP_APPEND(NULL,
        "// Close the triangle batch.",
        "glEnd()"),
    STEP_AT(NULL,
        "// Enable depth testing before the triangle is submitted.",
        "glEnable(GL_DEPTH_TEST)",
        "triangle_begin"),
    STEP_SENTINEL,
};

/* A flat triangle in the z=0 plane: present it in true 2D so a
 * first-time user sees the shape head-on without perspective
 * foreshortening or accidental orbit. */
static const char *const g_tutorial_first_triangle_cfg[] = {
    "// @cfg view_mode = RENDER3D_VIEW_2D",
    NULL,
};

/* "Feature Tour" — exercises the non-COMMAND step kinds and the
 * relaxed step shapes:
 *   1) A NOTE step opens the tour (comment-only; ack key advances).
 *   2) Five COMMAND steps draw a triangle in 3D. The two lower-corner
 *      vertex steps are comment-less (STEP_CMD) — the autocomplete
 *      ghost and status hint carry the instruction, demonstrating
 *      that a narration comment per command is not required.
 *   3) One REQUIRE step asks the user to enable vertex outlines (using
 *      the GLR_VERTEX_OUTLINES binding) so they see how the feature
 *      changes the rendering.
 *   4) Two SET steps showcase Radar and Aurora grid themes;
 *      the user presses Enter/Tab/Space to advance through them.
 *
 * The entry-level `@cfg` block guarantees a known baseline (3D view,
 * grid off, vertex outlines off) so the REQUIRE step has the intended
 * teaching effect rather than auto-advancing immediately. The SET
 * steps use symbolic cfg values so enum reordering in src/scene/themes.h
 * does not silently retarget the showcase. */
static const TutorialStep g_tutorial_feature_tour_steps[] = {
    STEP_NOTE(
        "// A quick tour of the REPL's scene features."),
    STEP_APPEND(NULL,
        "// Open a triangle batch.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Add the three corners, starting at the top.",
        "glVertex3f(0, 0.7, 0)"),
    STEP_CMD(NULL, "glVertex3f(-0.7, -0.5, 0)"),
    STEP_CMD(NULL, "glVertex3f(0.7, -0.5, 0)"),
    STEP_APPEND(NULL,
        "// Close the batch - the filled triangle appears.",
        "glEnd()"),
    STEP_REQUIRE_KEY(NULL,
        "// Press %s to turn on vertex outlines; they trace each edge.",
        "vertex_outlines", 1,
        KM_KEY(GLR_VERTEX_OUTLINES), KM_MODS(GLR_VERTEX_OUTLINES), 0),
    STEP_SET_SYM(NULL,
        "// The Radar grid backdrop looks like this.",
        "grid", "GRID_THEME_RADAR"),
    STEP_SET_SYM(NULL,
        "// And the Aurora grid backdrop looks like this.",
        "grid", "GRID_THEME_AURORA"),
    STEP_SENTINEL,
};

static const char *const g_tutorial_feature_tour_cfg[] = {
    "// @cfg view_mode = RENDER3D_VIEW_3D",  /* depth gives the grid themes context */
    "// @cfg vertex_outlines = 0",  /* baseline: REQUIRE will ask the user to turn this on */
    "// @cfg grid = GRID_THEME_OFF", /* baseline: SET steps will showcase Radar then Aurora */
    NULL,
};

/* "Variable Slider" — teaches the REQUIRE_VAR step kind by driving a
 * variable that controls a drawn shape. The user declares `n` (the
 * triangle's size), draws a triangle whose vertices use `n`, then grows
 * `n` to 10 with the variable-panel slider and watches the triangle
 * scale. Both a typed commit and a slider drag flow through
 * repl_apply_predef_ops, which the editor commit path notifies after.
 * No `@cfg` block — presentation defaults are fine.
 *
 * Step 0 is a DECLARATION step: `n` does not exist when the tutorial
 * starts, so the runner detects the undeclared var (see
 * tutorial_enter_step) and treats it specially. The satisfying
 * `float n = 1;` is a declaration, which the compiler relocates to the
 * TOP of the document, so a separate locked instruction comment line
 * above it would be stranded (and would desync locked-line tracking).
 * Instead the instruction rides the autocomplete ghost as
 * `float n = 1; <this comment>` (synthesized in tutorial_shadow_suffix):
 * the comment below commits as a TRAILING comment on the decl line and
 * travels with it to the top. So this catalog string is worded to read
 * as a trailing description of `n`, not as a standalone instruction.
 *
 * The middle COMMAND steps draw the triangle (its vertices reference
 * `n`). The final step's `n` already exists, so its slider is live and
 * the instruction is an ordinary locked comment; a slider drag or a
 * typed `n = 10;` advances it. */
static const TutorialStep g_tutorial_variable_slider_steps[] = {
    STEP_REQUIRE_VAR(NULL,
        "// the triangle's size; the slider will grow it",
        "n", 1.0f),
    STEP_APPEND(NULL,
        "// Open a triangle batch.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Top vertex.",
        "glVertex3f(0, n, 0)"),
    STEP_APPEND(NULL,
        "// Lower-left vertex.",
        "glVertex3f(-n, -0.5, 0)"),
    STEP_APPEND(NULL,
        "// Lower-right vertex.",
        "glVertex3f(n, -0.5, 0)"),
    STEP_APPEND(NULL,
        "// Close the batch - the filled triangle appears.",
        "glEnd()"),
    STEP_REQUIRE_VAR(NULL,
        "// Now drag the n slider in the variable panel to bring n to 10.",
        "n", 10.0f),
    STEP_SENTINEL,
};

/* "Lighting Basics" — the first lit-surface tutorial. Six COMMAND steps
 * stand up the minimal lighting pipeline (depth test, lighting, one
 * light, color-material) and then draw a GLUT sphere so the shading
 * gradient across a curved surface is obvious. No entry `@cfg` is needed:
 * tutorials reset presentation to defaults on start, and the default
 * view is 3D, so the sphere's falloff reads correctly. Each expected
 * command is a single call with no braces or declarations, satisfying
 * the COMMAND placement rule. */
static const TutorialStep g_tutorial_lighting_basics_steps[] = {
    STEP_APPEND(NULL,
        "// Turn on depth testing so nearer surfaces hide the ones behind them.",
        "glEnable(GL_DEPTH_TEST)"),
    STEP_APPEND(NULL,
        "// Enable lighting; OpenGL now shades surfaces instead of using flat color.",
        "glEnable(GL_LIGHTING)"),
    STEP_APPEND(NULL,
        "// Switch on light 0, the default light positioned near the camera.",
        "glEnable(GL_LIGHT0)"),
    STEP_APPEND(NULL,
        "// Let glColor drive the surface material so the next color tints the shape.",
        "glEnable(GL_COLOR_MATERIAL)"),
    STEP_APPEND(NULL,
        "// Choose a warm amber material color for the sphere.",
        "glColor3f(0.9, 0.6, 0.2)"),
    STEP_APPEND(NULL,
        "// Draw a lit sphere; watch the shading fall off from the lit side to the dark side.",
        "glutSolidSphere(0.7, 32, 24)"),
    STEP_SENTINEL,
};

/* REPL_TUTORIAL_TAG_ALL is a synthetic tag: every tutorial is a member.
 * It is not listed in any g_tutorials[] mask literal; instead
 * repl_tutorial_tag_mask() ORs its bit into every entry's mask, so the
 * whole tag query API picks it up with no per-entry bookkeeping. Kept at
 * index 0 so "All" sorts first in the Tutorials menu. Enum lives in
 * tutorials.h. */

#define TUTORIAL_TAG_BIT(tag)         (1u << (tag))
#define TUTORIAL_TAG_ALL              TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_ALL)
#define TUTORIAL_TAG_GEOMETRY         TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_GEOMETRY)
#define TUTORIAL_TAG_COLOR_TRANSFORMS TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_COLOR_TRANSFORMS)
#define TUTORIAL_TAG_DEPTH_LIGHTING   TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_DEPTH_LIGHTING)
#define TUTORIAL_TAG_ANIMATION        TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_ANIMATION)

static const char *const g_tutorial_tag_labels[] = {
    "All",
    "Geometry",
    "Color & Transforms",
    "Depth & Lighting",
    "Animation",
};
/* Must stay 1:1 with the REPL_TUTORIAL_TAG_* enum: repl_tutorial_tag_count()
 * returns REPL_TUTORIAL_TAG_COUNT but repl_tutorial_tag_label() indexes this
 * table, so any drift is an out-of-bounds read. */
STATIC_ASSERT((int)(sizeof(g_tutorial_tag_labels) /
                    sizeof(g_tutorial_tag_labels[0])) == REPL_TUTORIAL_TAG_COUNT,
              "g_tutorial_tag_labels[] out of sync with REPL_TUTORIAL_TAG_COUNT");

/* Subheading labels here are catalog-author choices, not a fixed
 * vocabulary — the menu just emits `### subheading` chrome rows when
 * the subheading changes. "Beginner" / "Intermediate" suit the current
 * 4-tutorial catalog; future catalogs (e.g. a REPL-vs-OpenGL split)
 * can use any labels that group sensibly within each tag flyout.
 *
 * Catalog order matters: entries sharing a subheading should be
 * contiguous within every tag they share, so the per-tag flyout walker
 * emits each header exactly once. test_catalog_subheading_metadata
 * enforces this. The Beginner run is placed before the Intermediate
 * entry so all three tag flyouts (Geometry, Color & Transforms, All)
 * see the Beginner group first. */

static const TutorialEntry g_tutorials[] = {
    {
        .name       = "First Triangle",
        .steps      = g_tutorial_first_triangle_steps,
        .cfg        = g_tutorial_first_triangle_cfg,
        .tags       = TUTORIAL_TAG_GEOMETRY,
        .subheading = "Beginner",
    },
    {
        .name       = "Color & Transform",
        .steps      = g_tutorial_color_transform_steps,
        .tags       = TUTORIAL_TAG_COLOR_TRANSFORMS,
        .subheading = "Beginner",
    },
    {
        .name       = "Feature Tour",
        .steps      = g_tutorial_feature_tour_steps,
        .cfg        = g_tutorial_feature_tour_cfg,
        .tags       = TUTORIAL_TAG_GEOMETRY,
        .subheading = "Beginner",
    },
    {
        /* Placed before "Depth Test Triangle" (Intermediate) so the ALL
         * flyout walks Beginner-only entries first, then transitions
         * once to Intermediate at the tail — the subheading contiguity
         * test (test_catalog_subheading_metadata) requires a single
         * Beginner→Intermediate transition across the whole catalog. */
        .name       = "Variable Slider",
        .steps      = g_tutorial_variable_slider_steps,
        .tags       = TUTORIAL_TAG_COLOR_TRANSFORMS,
        .subheading = "Beginner",
    },
    {
        .name       = "Depth Test Triangle",
        .steps      = g_tutorial_depth_triangle_steps,
        .tags       = TUTORIAL_TAG_GEOMETRY | TUTORIAL_TAG_DEPTH_LIGHTING,
        /* Intermediate: introduces depth-testing as a new GL concept
         * (the previous tutorials were pure geometry/color/transform). */
        .subheading = "Intermediate",
    },
    {
        /* Appended after Depth Test Triangle so both Intermediate entries
         * stay contiguous (Beginner run, then Intermediate run) in the ALL
         * flyout, and the two DEPTH_LIGHTING entries (Depth Test Triangle,
         * Lighting Basics) form a single Intermediate run in that flyout —
         * test_catalog_subheading_metadata enforces both. */
        .name       = "Lighting Basics",
        .steps      = g_tutorial_lighting_basics_steps,
        .tags       = TUTORIAL_TAG_DEPTH_LIGHTING,
        .subheading = "Intermediate",
    },
};

static int tutorial_catalog_entry_count(void) {
    return repl_tutorial_count();
}

static int tutorial_catalog_tag_count(void) {
    return REPL_TUTORIAL_TAG_COUNT;
}

static unsigned int tutorial_catalog_tag_bit(int tag_idx) {
    return repl_tutorial_tag_bit(tag_idx);
}

static const ReplCatalogTagOps g_tutorial_tag_ops = {
    tutorial_catalog_entry_count,
    tutorial_catalog_tag_count,
    g_tutorial_tag_labels,
    repl_tutorial_tag_mask,
    tutorial_catalog_tag_bit,
};

static const TutorialEntry *tutorial_entry_at(int idx) {
    int count = (int)(sizeof(g_tutorials) / sizeof(g_tutorials[0]));
    if (idx < 0 || idx >= count)
        return NULL;
    return &g_tutorials[idx];
}

const TutorialEntry *repl_tutorial_entry(int idx) {
    return tutorial_entry_at(idx);
}

const TutorialStep *repl_tutorial_step_get(int idx, int step_idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    if (!entry || !entry->steps || step_idx < 0)
        return NULL;

    for (int i = 0; i <= step_idx; i++) {
        const TutorialStep *step = &entry->steps[i];
        if (repl_tutorial_step_is_sentinel(step))
            return NULL;
        if (i == step_idx)
            return step;
    }
    return NULL;
}

const char *repl_tutorial_step_comment(int idx, int step_idx) {
    static char comment_buf[256];
    char shortcut[KEYMAP_SHORTCUT_LABEL_MAX];
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    if (!step)
        return NULL;
    if (!step->comment_binding_key)
        return step->comment;

    keymap_binding_to_string(shortcut, (int)sizeof(shortcut),
                             step->comment_binding_key,
                             step->comment_binding_mods,
                             step->comment_binding_is_special);
    snprintf(comment_buf, sizeof(comment_buf), step->comment, shortcut);
    return comment_buf;
}

int repl_tutorial_count(void) {
    return (int)(sizeof(g_tutorials) / sizeof(g_tutorials[0]));
}

const char *repl_tutorial_name(int idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    return entry ? entry->name : NULL;
}

int repl_tutorial_step_count(int idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    if (!entry || !entry->steps)
        return 0;

    int count = 0;
    while (!repl_tutorial_step_is_sentinel(&entry->steps[count]))
        count++;
    return count;
}

/* The per-field step accessors mostly live as `static inline` helpers in
 * tutorials.h and call repl_tutorial_step_get() once each. Callers
 * that need more than one field per step (e.g. the tutorial-menu
 * row renderer) should call repl_tutorial_step_get directly and
 * walk fields off the returned pointer to avoid O(N) walks per
 * field. */

const char *const *repl_tutorial_cfg_lines(int idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    return entry ? entry->cfg : NULL;
}

int repl_tutorial_tag_count(void) {
    return REPL_TUTORIAL_TAG_COUNT;
}

unsigned int repl_tutorial_tag_mask(int tutorial_idx) {
    if (tutorial_idx < 0 || tutorial_idx >= repl_tutorial_count())
        return 0u;
    /* Fold in the synthetic "All" membership so every tag query derives
     * it uniformly; entry literals stay free of an explicit ALL bit. */
    return g_tutorials[tutorial_idx].tags | TUTORIAL_TAG_ALL;
}

REPL_DEFINE_CATALOG_TAG_WRAPPERS(tutorial, &g_tutorial_tag_ops)

const char *repl_tutorial_subheading(int tutorial_idx) {
    const TutorialEntry *entry = tutorial_entry_at(tutorial_idx);
    return entry ? entry->subheading : NULL;
}

static int label_is_empty(const char *s) {
    return !s || s[0] == '\0';
}

/* v1 catalog rule: each `expected` must parse to exactly one
 * source command AND land at the runner's chosen
 * expected_commit_line on commit. The full guarantee would require
 * driving the live parser/compile seam against a temp document
 * snapshot, which the catalog validator can't easily do without
 * dragging REPL state into Phase 1. Until that lands, this checker
 * is a best-effort syntactic filter focused on the patterns that
 * actively break label-line bookkeeping:
 *
 *   - empty / whitespace-only text;
 *   - embedded newlines (`\n`, `\r`);
 *   - statement separators (`;`) — would commit two source rows;
 *   - block punctuation (`{`/`}`) — opens a structured block;
 *   - any `float ` declaration (single- or multi-name) — the
 *     CMD_VAR_DECLARE placement rule relocates the new decl to the
 *     top of non-decl code regardless of edit_line, so
 *     pending.commit_line would not match the actual landing row
 *     and committed_line_for_step would point at the wrong source
 *     line for any later label-targeted step.
 *
 * Known-shallow gaps (commit-time failures, not catastrophic): an
 * unknown GL call or one with wrong arity validates here but fails
 * at commit time, leaving the tutorial unrunnable at that step.
 * Catalog authors notice immediately. Promoting this to a real
 * parser-driven check is tracked as future work. */
static int expected_is_single_command(const char *expected,
                                      char *err, int err_size) {
    if (!expected) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size, "expected is NULL");
        return 0;
    }

    int saw_non_ws = 0;
    for (const char *p = expected; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\n' || ch == '\r') {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "expected has embedded newline: %s", expected);
            return 0;
        }
        if (ch == ';') {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "expected has statement separator ';': %s",
                         expected);
            return 0;
        }
        if (ch == '{' || ch == '}') {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "expected has block punctuation: %s", expected);
            return 0;
        }
        if (!isspace(ch))
            saw_non_ws = 1;
    }
    if (!saw_non_ws) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size, "expected is empty");
        return 0;
    }

    /* Reject every `float ` declaration. Single-name decls like
     * `float x` parse fine but the commit path relocates them to
     * the top of non-decl code (CLAUDE.md: "new CMD_VAR_DECLARE
     * lines are inserted at the top of non-decl code [...]
     * regardless of cursor position"), which means
     * pending.commit_line drifts away from the actual landing row
     * and any later label-targeted step that targets this decl
     * would resolve to the wrong source line. Multi-name decls
     * additionally expand into one CMD_VAR_DECLARE per name. */
    const char *s = expected;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (strncmp(s, "float", 5) == 0 &&
        (s[5] == '\0' || isspace((unsigned char)s[5]))) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size,
                     "expected `float` declarations are not allowed in "
                     "tutorial steps (placement rule relocates them): %s",
                     expected);
        return 0;
    }
    return 1;
}

int repl_tutorial_validate(int idx, char *err, int err_size) {
    if (err && err_size > 0) err[0] = '\0';
    const TutorialEntry *entry = tutorial_entry_at(idx);
    if (!entry) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size, "tutorial idx %d out of range", idx);
        return 0;
    }
    return repl_tutorial_validate_entry(entry, err, err_size);
}

int repl_tutorial_validate_entry(const TutorialEntry *entry,
                                 char *err, int err_size) {
    if (err && err_size > 0) err[0] = '\0';
    if (!entry) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size, "tutorial entry is NULL");
        return 0;
    }
    if (!entry->steps) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size, "tutorial '%s' has no steps",
                     entry->name ? entry->name : "?");
        return 0;
    }

    /* Walk steps, validating each as we go. Forward references are
     * rejected naturally because target_label can only match a label
     * we have already seen. Sentinel is comment AND expected both NULL
     * — SET/REQUIRE/NOTE steps legitimately leave `expected` NULL, and
     * comment-less COMMAND steps legitimately leave `comment` NULL. */
    int step_count = 0;
    for (int i = 0;; i++) {
        const TutorialStep *step = &entry->steps[i];
        if (repl_tutorial_step_is_sentinel(step)) {
            break;
        }
        if (step_count >= TUTORIAL_MAX_STEPS) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' has more than %d steps",
                         entry->name ? entry->name : "?",
                         TUTORIAL_MAX_STEPS);
            return 0;
        }
        /* Kind-aware shape check. Slug *validity* (is the bridge aware
         * of this slug?) is a runtime check at tutorial_start because
         * it depends on the controller-installed config bridge. */
        if (step->kind == TUTORIAL_STEP_KIND_COMMAND) {
            /* `comment` is optional for COMMAND — NULL/empty commits the
             * expected command with no locked instruction row. */
            if (!step->expected) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d COMMAND missing expected",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            if (!expected_is_single_command(step->expected, err, err_size))
                return 0;
        } else if (step->kind == TUTORIAL_STEP_KIND_NOTE) {
            if (step->expected) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d NOTE must leave "
                             "expected NULL",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            if (label_is_empty(step->comment)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d NOTE needs non-empty "
                             "comment",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
        } else if (step->kind == TUTORIAL_STEP_KIND_SET ||
                   step->kind == TUTORIAL_STEP_KIND_REQUIRE) {
            if (step->expected) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d %s must leave "
                             "expected NULL",
                             entry->name ? entry->name : "?", i,
                             step->kind == TUTORIAL_STEP_KIND_SET
                                 ? "SET" : "REQUIRE");
                return 0;
            }
            if (label_is_empty(step->cfg_slug)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d %s needs non-empty "
                             "cfg_slug",
                             entry->name ? entry->name : "?", i,
                             step->kind == TUTORIAL_STEP_KIND_SET
                                 ? "SET" : "REQUIRE");
                return 0;
            }
            if (label_is_empty(step->comment)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d %s needs non-empty "
                             "comment",
                             entry->name ? entry->name : "?", i,
                             step->kind == TUTORIAL_STEP_KIND_SET
                                 ? "SET" : "REQUIRE");
                return 0;
            }
        } else if (step->kind == TUTORIAL_STEP_KIND_REQUIRE_VAR) {
            if (step->expected) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d REQUIRE_VAR must leave "
                             "expected NULL",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            if (label_is_empty(step->var_name)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d REQUIRE_VAR needs "
                             "non-empty var_name",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            if (repl_eval_is_reserved_ident(step->var_name)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d REQUIRE_VAR var_name "
                             "'%s' is reserved",
                             entry->name ? entry->name : "?", i,
                             step->var_name);
                return 0;
            }
            if (label_is_empty(step->comment)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d REQUIRE_VAR needs "
                             "non-empty comment",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
        } else {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' step %d unknown kind",
                         entry->name ? entry->name : "?", i);
            return 0;
        }

        /* Unique non-empty labels. */
        if (!label_is_empty(step->label)) {
            for (int j = 0; j < i; j++) {
                if (!label_is_empty(entry->steps[j].label) &&
                    strcmp(entry->steps[j].label, step->label) == 0) {
                    if (err_size > 0)
                        snprintf(err, (size_t)err_size,
                                 "tutorial '%s' step %d duplicate label '%s'",
                                 entry->name ? entry->name : "?", i,
                                 step->label);
                    return 0;
                }
            }
        }

        if (step->placement == TUTORIAL_STEP_APPEND) {
            if (!label_is_empty(step->target_label)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d append placement must not "
                             "set target_label",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
        } else if (step->placement == TUTORIAL_STEP_LABEL) {
            if (label_is_empty(step->target_label)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d label placement needs "
                             "non-empty target_label",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            int found = 0;
            for (int j = 0; j < i; j++) {
                if (!label_is_empty(entry->steps[j].label) &&
                    strcmp(entry->steps[j].label, step->target_label) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d target_label '%s' is "
                             "missing or forward-referenced",
                             entry->name ? entry->name : "?", i,
                             step->target_label);
                return 0;
            }
        } else {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' step %d unknown placement",
                         entry->name ? entry->name : "?", i);
            return 0;
        }
        step_count++;
    }
    return 1;
}
