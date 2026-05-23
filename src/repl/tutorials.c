#include "repl/tutorials.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define STEP_APPEND(label, c, e) \
    { (label), (c), (e), TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_COMMAND, NULL, 0 }

#define STEP_AT(label, c, e, target) \
    { (label), (c), (e), TUTORIAL_STEP_LABEL, (target), \
      TUTORIAL_STEP_KIND_COMMAND, NULL, 0 }

/* Showcase step: on entry apply cfg_slug=cfg_value so the user sees the
 * effect, show a "press Enter to continue" prompt, advance on ack key.
 * `expected` is NULL — there is no command to type. */
#define STEP_SET(label, c, slug, val) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_SET, (slug), (val) }

/* Check step: advance when the user themselves makes cfg_slug == cfg_value
 * (via F-key/menu/etc.). Auto-advances if already satisfied on entry.
 * `expected` is NULL. */
#define STEP_REQUIRE(label, c, slug, val) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_REQUIRE, (slug), (val) }

/* Sentinel: comment == NULL is the only field the terminator scan reads
 * (SET/REQUIRE legitimately have NULL `expected`, so the old
 * comment&&expected check would misread the first SET as a sentinel). */
#define STEP_SENTINEL { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL, \
                        TUTORIAL_STEP_KIND_COMMAND, NULL, 0 }

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
    "// @cfg view_mode = 1",
    NULL,
};

/* "Feature Tour" — exercises the new step kinds:
 *   1) Five COMMAND steps draw a triangle in 3D.
 *   2) One REQUIRE step asks the user to enable vertex outlines (F7) so
 *      they see how the feature changes the rendering.
 *   3) Two SET steps showcase Radar (10) and Focus (6) grid themes;
 *      the user presses Enter/Tab/Space to advance through them.
 *
 * The entry-level `@cfg` block guarantees a known baseline (3D view,
 * grid off, vertex outlines off) so the REQUIRE step has the intended
 * teaching effect rather than auto-advancing immediately. Integer grid
 * values used directly (the catalog is repl-layer and must not include
 * src/scene/themes.h); see grid_theme_names in src/app/glr_actions.c. */
static const TutorialStep g_tutorial_feature_tour_steps[] = {
    STEP_APPEND(NULL,
        "// Open a triangle batch.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Top vertex.",
        "glVertex3f(0, 0.7, 0)"),
    STEP_APPEND(NULL,
        "// Lower-left vertex.",
        "glVertex3f(-0.7, -0.5, 0)"),
    STEP_APPEND(NULL,
        "// Lower-right vertex.",
        "glVertex3f(0.7, -0.5, 0)"),
    STEP_APPEND(NULL,
        "// Close the batch — the filled triangle appears.",
        "glEnd()"),
    STEP_REQUIRE(NULL,
        "// Press F7 to turn on vertex outlines; they trace each edge.",
        "vertex_outlines", 1),
    STEP_SET(NULL,
        "// The Radar grid backdrop looks like this.",
        "grid", 10 /* GRID_THEME_RADAR */),
    STEP_SET(NULL,
        "// And the Focus grid backdrop looks like this.",
        "grid", 6 /* GRID_THEME_FOCUS */),
    STEP_SENTINEL,
};

static const char *const g_tutorial_feature_tour_cfg[] = {
    "// @cfg view_mode = 0",        /* 3D — depth gives the grid themes context */
    "// @cfg vertex_outlines = 0",  /* baseline: REQUIRE will ask the user to turn this on */
    "// @cfg grid = 0",             /* baseline: SET steps will showcase Radar then Focus */
    NULL,
};

static const TutorialEntry g_tutorials[] = {
    {
        .name  = "First Triangle",
        .steps = g_tutorial_first_triangle_steps,
        .cfg   = g_tutorial_first_triangle_cfg,
    },
    {
        .name  = "Color & Transform",
        .steps = g_tutorial_color_transform_steps,
    },
    {
        .name  = "Depth Test Triangle",
        .steps = g_tutorial_depth_triangle_steps,
    },
    {
        .name  = "Feature Tour",
        .steps = g_tutorial_feature_tour_steps,
        .cfg   = g_tutorial_feature_tour_cfg,
    },
};

static const TutorialEntry *tutorial_entry_at(int idx) {
    int count = (int)(sizeof(g_tutorials) / sizeof(g_tutorials[0]));
    if (idx < 0 || idx >= count)
        return NULL;
    return &g_tutorials[idx];
}

static const TutorialStep *tutorial_step_at(int idx, int step_idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    if (!entry || !entry->steps || step_idx < 0)
        return NULL;

    /* Sentinel keyed on `comment` alone: SET/REQUIRE steps legitimately
     * have NULL `expected`, so the old comment&&expected check would
     * misread them as the terminator. */
    for (int i = 0; i <= step_idx; i++) {
        const TutorialStep *step = &entry->steps[i];
        if (!step->comment)
            return NULL;
        if (i == step_idx)
            return step;
    }
    return NULL;
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
    while (entry->steps[count].comment)
        count++;
    return count;
}

const char *repl_tutorial_step_comment(int idx, int step_idx) {
    const TutorialStep *step = tutorial_step_at(idx, step_idx);
    return step ? step->comment : NULL;
}

const char *repl_tutorial_step_expected(int idx, int step_idx) {
    const TutorialStep *step = tutorial_step_at(idx, step_idx);
    return step ? step->expected : NULL;
}

TutorialStepPlacementKind repl_tutorial_step_placement(int idx, int step_idx) {
    const TutorialStep *step = tutorial_step_at(idx, step_idx);
    return step ? step->placement : TUTORIAL_STEP_APPEND;
}

const char *repl_tutorial_step_label(int idx, int step_idx) {
    const TutorialStep *step = tutorial_step_at(idx, step_idx);
    return step ? step->label : NULL;
}

const char *repl_tutorial_step_target_label(int idx, int step_idx) {
    const TutorialStep *step = tutorial_step_at(idx, step_idx);
    return step ? step->target_label : NULL;
}

TutorialStepKind repl_tutorial_step_kind(int idx, int step_idx) {
    const TutorialStep *step = tutorial_step_at(idx, step_idx);
    return step ? step->kind : TUTORIAL_STEP_KIND_COMMAND;
}

const char *repl_tutorial_step_cfg_slug(int idx, int step_idx) {
    const TutorialStep *step = tutorial_step_at(idx, step_idx);
    return step ? step->cfg_slug : NULL;
}

int repl_tutorial_step_cfg_value(int idx, int step_idx) {
    const TutorialStep *step = tutorial_step_at(idx, step_idx);
    return step ? step->cfg_value : 0;
}

const char *const *repl_tutorial_cfg_lines(int idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    return entry ? entry->cfg : NULL;
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
     * we have already seen. Sentinel is `comment == NULL` alone — SET
     * and REQUIRE steps legitimately leave `expected` NULL. */
    int step_count = 0;
    for (int i = 0;; i++) {
        const TutorialStep *step = &entry->steps[i];
        if (!step->comment) {
            /* sentinel */
            break;
        }
        if (step_count >= TUTORIAL_LOCKED_LINE_MAX) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' has more than %d steps",
                         entry->name ? entry->name : "?",
                         TUTORIAL_LOCKED_LINE_MAX);
            return 0;
        }
        /* Kind-aware shape check. Slug *validity* (is the bridge aware
         * of this slug?) is a runtime check at tutorial_start because
         * it depends on the controller-installed config bridge. */
        if (step->kind == TUTORIAL_STEP_KIND_COMMAND) {
            if (!step->expected) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d COMMAND missing expected",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            if (!expected_is_single_command(step->expected, err, err_size))
                return 0;
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
