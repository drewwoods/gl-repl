#include "repl/tutorials.h"

#include <stddef.h>

static const char *const g_tutorial_first_triangle_comments[] = {
    "// Build the smallest filled shape: open a GL_TRIANGLES batch.",
    "// Place the first vertex near the top; this becomes the triangle tip.",
    "// Add the lower-left corner so the triangle has width.",
    "// Add the lower-right corner; three vertices complete one triangle.",
    "// Close the batch and the filled triangle appears in the scene.",
    NULL,
};

static const char *const g_tutorial_first_triangle_expected[] = {
    "glBegin(GL_TRIANGLES)",
    "glVertex3f(0, 0.8, 0)",
    "glVertex3f(-0.8, -0.6, 0)",
    "glVertex3f(0.8, -0.6, 0)",
    "glEnd()",
    NULL,
};

static const char *const g_tutorial_color_transform_comments[] = {
    "// Save the current matrix so this example can clean up after itself.",
    "// Set the drawing color; OpenGL keeps using it for later vertices.",
    "// Move the local coordinate system to the right before drawing.",
    "// Rotate those local axes 30 degrees around Z, the screen-facing axis.",
    "// Open a GL_QUADS batch; the next four corners make one filled square.",
    "// Give the square its lower-left corner in the transformed space.",
    "// Add the lower-right corner at the same height.",
    "// Add the upper-right corner so the quad has height.",
    "// Add the upper-left corner; vertex order walks around the square.",
    "// Close the quad batch to draw the rotated cyan square.",
    "// Restore the saved matrix so future commands are not moved or rotated.",
    NULL,
};

static const char *const g_tutorial_color_transform_expected[] = {
    "glPushMatrix()",
    "glColor3f(0.2, 0.8, 1)",
    "glTranslatef(0.4, 0, 0)",
    "glRotatef(30, 0, 0, 1)",
    "glBegin(GL_QUADS)",
    "glVertex3f(-0.2, -0.2, 0)",
    "glVertex3f(0.2, -0.2, 0)",
    "glVertex3f(0.2, 0.2, 0)",
    "glVertex3f(-0.2, 0.2, 0)",
    "glEnd()",
    "glPopMatrix()",
    NULL,
};

static const TutorialEntry g_tutorials[] = {
    {
        .name = "First Triangle",
        .comments = g_tutorial_first_triangle_comments,
        .expected = g_tutorial_first_triangle_expected,
    },
    {
        .name = "Color & Transform",
        .comments = g_tutorial_color_transform_comments,
        .expected = g_tutorial_color_transform_expected,
    },
};

static const TutorialEntry *tutorial_entry_at(int idx) {
    int count = (int)(sizeof(g_tutorials) / sizeof(g_tutorials[0]));
    if (idx < 0 || idx >= count)
        return NULL;
    return &g_tutorials[idx];
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
    if (!entry || !entry->comments || !entry->expected)
        return 0;

    int count = 0;
    while (entry->comments[count] && entry->expected[count])
        count++;
    return count;
}

const char *repl_tutorial_step_comment(int idx, int step_idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    if (!entry || step_idx < 0)
        return NULL;
    return entry->comments[step_idx];
}

const char *repl_tutorial_step_expected(int idx, int step_idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    if (!entry || step_idx < 0)
        return NULL;
    return entry->expected[step_idx];
}
