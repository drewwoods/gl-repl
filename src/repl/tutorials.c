#include "repl/tutorials.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define STEP_APPEND(label, c, e) \
    { (label), (c), (e), TUTORIAL_STEP_APPEND, NULL }

#define STEP_AT(label, c, e, target) \
    { (label), (c), (e), TUTORIAL_STEP_LABEL, (target) }

#define STEP_SENTINEL { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL }

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

/* Phase 2 worked label-targeted tutorial: the user first draws a
 * triangle (five append steps), then a sixth step splices
 * glEnable(GL_DEPTH_TEST) above the original glBegin so the
 * batch renders with depth testing already enabled. The label
 * "triangle_begin" anchors the insertion. */
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

static const TutorialEntry g_tutorials[] = {
    {
        .name  = "First Triangle",
        .steps = g_tutorial_first_triangle_steps,
    },
    {
        .name  = "Color & Transform",
        .steps = g_tutorial_color_transform_steps,
    },
    {
        .name  = "Depth Test Triangle",
        .steps = g_tutorial_depth_triangle_steps,
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

    /* Walk to step_idx, treating the first step with NULL comment AND
     * NULL expected as the terminator. */
    for (int i = 0; i <= step_idx; i++) {
        const TutorialStep *step = &entry->steps[i];
        if (!step->comment && !step->expected)
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
    while (entry->steps[count].comment && entry->steps[count].expected)
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

static int label_is_empty(const char *s) {
    return !s || s[0] == '\0';
}

/* v1 catalog rule: each `expected` must parse to exactly one source
 * command. Detected syntactically without dragging the parser into
 * the catalog: reject empty/whitespace-only text, embedded newlines,
 * statement separators (`;`), and block punctuation (`{`/`}`). Also
 * reject multi-name float decls (`float a, b`) since those expand
 * into multiple source rows. */
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

    /* Reject multi-name float decls like "float a, b" which expand into
     * one CMD_VAR_DECLARE per name. */
    const char *s = expected;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (strncmp(s, "float", 5) == 0 &&
        (s[5] == '\0' || isspace((unsigned char)s[5]))) {
        for (const char *p = s + 5; *p; p++) {
            if (*p == ',') {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "expected float decl must be single-name: %s",
                             expected);
                return 0;
            }
        }
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
     * we have already seen. */
    int step_count = 0;
    for (int i = 0;; i++) {
        const TutorialStep *step = &entry->steps[i];
        if (!step->comment && !step->expected) {
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
        if (!step->comment || !step->expected) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' step %d missing comment or expected",
                         entry->name ? entry->name : "?", i);
            return 0;
        }
        if (!expected_is_single_command(step->expected, err, err_size))
            return 0;

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
