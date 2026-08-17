#ifndef GL_STATE_DUMP_H
#define GL_STATE_DUMP_H

#include <stdio.h>  /* FILE */

/* Live OpenGL 1.1 state dump - the *queried* twin of
 * src/repl/gl_state_inspector.c.
 *
 * The inspector folds the program's own commands to predict what state a
 * source position sits in, without touching GL. This module asks the driver
 * what the state actually is, right now, on the current context. Two answers
 * to the same question from opposite directions: when a scene renders
 * differently than its exported C twin (or than the inspector claims), the
 * diff between a dump taken here and a dump taken there is the evidence.
 *
 * Output is one `NAME=value` row per state variable, in a fixed order, so
 * two dumps are directly comparable with diff(1):
 *
 *     # gl-state-dump v1 label=app/user-pass-begin
 *     # --- capabilities ---
 *     GL_BLEND=1
 *     GL_BLEND_DST=GL_ONE_MINUS_SRC_ALPHA
 *     ...
 *
 * Comment rows (`#`) carry the label and the section headings; every other
 * row is `NAME=value` with no spaces around `=`. Enum-valued state prints
 * the symbolic name resolved within that state's own value domain (GL enum
 * numbers collide across domains - GL_ZERO and GL_POINTS are both 0 - so
 * there is deliberately no global number-to-name lookup here). Vectors are
 * comma-separated; masks print as hex; floats use a fixed 4-decimal form so
 * two runs of the same frame produce byte-identical rows.
 *
 * Coverage is the GL 1.1 state tables plus a short, explicitly-marked
 * context section (renderer strings, buffer sizes, sample counts) that is
 * not 1.1 state but is the first thing you want when two machines disagree.
 * Deliberately excluded: evaluator control points (glGetMap*), pixel maps,
 * selection/feedback buffer contents, and texture image data - unbounded
 * payloads rather than state variables. The evaluator *enables* are dumped.
 *
 * Cost: ~250 glGet* round trips, each a pipeline sync on a modern driver.
 * This is a debugging instrument - call it from a diagnostic path, never
 * per frame in a rendering path.
 *
 * Errors: the dump clears the GL error flag on entry (reporting what it
 * found as `GL_ERROR_ON_ENTRY`) and again on exit, so a query for state an
 * implementation does not have cannot leak an error into the caller.
 *
 * Under -DGL_STUBS (the no-op GL stub headers) every query would answer
 * zero, so the walk emits a single row saying so rather than a page of
 * fiction. */

#define GL_STATE_DUMP_NAME_MAX  48
#define GL_STATE_DUMP_VALUE_MAX 224

/* Row sink. `name` is NULL for a section heading, whose text is in `value`;
 * the FILE writers render those as `# ...` comments. */
typedef void (*GlStateDumpEmitFn)(void *user_data,
                                  const char *name,
                                  const char *value);

/* Query the current context and hand every row to `emit` in dump order. */
void gl_state_dump_walk(GlStateDumpEmitFn emit, void *user_data);

/* Write a dump to an open stream. `label` (may be NULL) names the probe
 * point in the header comment - "app/user-pass-begin", "export/display", a
 * frame number, whatever tells the two dumps apart in a diff. */
void gl_state_dump_write(FILE *out, const char *label);

/* Same, to a path. Returns 1 on success, 0 if the file could not be opened. */
int gl_state_dump_write_path(const char *path, const char *label);

#endif /* GL_STATE_DUMP_H */
