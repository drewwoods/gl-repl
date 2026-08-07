/*
 * src/repl/gl_state_inspector.h - Source-position OpenGL state inspection.
 *
 * Folds the generated init()/display() setup plus the current flat user
 * program up to a source checkpoint without issuing GL calls. The result
 * contains only state variables that those phases explicitly touched. Current
 * values are paired with a comparison basis - the OpenGL 2.1 initial values,
 * or another probe point's report via repl_gl_state_report_rebase() - and
 * equality is kept separate from touched-ness so an explicit write of a
 * basis value remains visible.
 */
#ifndef REPL_GL_STATE_INSPECTOR_H
#define REPL_GL_STATE_INSPECTOR_H

#include "repl/flatten.h"

#define REPL_GL_STATE_REPORT_MAX_ROWS 112
#define REPL_GL_STATE_NAME_MAX         64
#define REPL_GL_STATE_VALUE_MAX        192

typedef enum {
    REPL_GL_STATE_SOURCE_NONE = 0,
    REPL_GL_STATE_SOURCE_INIT,
    REPL_GL_STATE_SOURCE_DISPLAY
} ReplGlStateSourceKind;

typedef struct {
    ReplGlStateSourceKind kind;
    int source_line_idx;  /* user display row; -1 for a generated phase */
} ReplGlStateChangeSource;

/* `basis_value` is whatever this row's `current` is being compared against:
 * the OpenGL 2.1 initial value as built, or the same state's value at another
 * probe point after repl_gl_state_report_rebase(). `differs_from_basis` is the
 * comparison of the two formatted strings, which is what the popup accents. */
typedef struct {
    char name[REPL_GL_STATE_NAME_MAX];
    char current[REPL_GL_STATE_VALUE_MAX];
    char basis_value[REPL_GL_STATE_VALUE_MAX];
    int  differs_from_basis;
    ReplGlStateChangeSource source;
} ReplGlStateReportRow;

/* Rows are partitioned by authorship: every row whose latest change came from
 * a user source line (source.source_line_idx >= 0) comes first, followed by
 * the rows the generated init()/display() setup owns. `user_row_count` is the
 * boundary, so rows[0, user_row_count) is "what this program did" and
 * rows[user_row_count, count) is "what the harness set up around it" - the
 * split the popup collapses on, because the generated group routinely
 * outnumbers the authored one by an order of magnitude.
 *
 * The partition is stable: within each group rows keep the emission order of
 * gl_state_append_report(), which follows ReplGlTrackedState field order. */
typedef struct {
    ReplGlStateReportRow rows[REPL_GL_STATE_REPORT_MAX_ROWS];
    int                  count;
    int                  user_row_count;
    int                  source_line_idx;
    /* Source line the basis values were taken at, or -1 when they are the
     * OpenGL 2.1 defaults. Set only by repl_gl_state_report_rebase(). */
    int                  basis_line_idx;
} ReplGlStateReport;

/* Build the effective generated init() + display() + REPL-authored display
 * state immediately before source_line_idx. Flat-command provenance is used
 * so selected if branches, unrolled loops, and function calls reflect the
 * current flattened frame. */
void repl_gl_state_report_at_line(FlatProgramView program,
                                  int source_line_idx,
                                  ReplGlStateReport *out);

/* Re-point `out`'s comparison basis from the OpenGL 2.1 defaults to the same
 * program folded to `basis_line_idx` - so the popup reads as the differential
 * between two probe points rather than as a distance from the GL defaults.
 * Rows are matched by state name.
 *
 * The basis fold is built here rather than supplied, because it is not the
 * same fold the popup shows: it keeps the rows that are filtered out of a
 * displayed report (a disabled light's parameters, which cannot reach the
 * frame). That is what preserves the rule below - see the note on the
 * definition for the case that made it necessary.
 *
 * A row of `out` with no counterpart in the basis keeps the default it was
 * built with, and that is the right answer rather than a fallback: the report
 * omits untouched state, so absence from the basis means the fold had not
 * written that state by the basis line, which is exactly when its value there
 * was still the GL initial one. (Touched-ness is monotone along a fold -
 * glPopAttrib restores a value but leaves the flag set - so this holds for any
 * basis line.)
 *
 * The reverse case is not represented: a row the basis has and `out` lacks
 * means the basis line comes after `out`'s, and such a row simply has no cell
 * in `out` to occupy. Comparing backwards therefore shows the intersection,
 * not a negative diff. */
void repl_gl_state_report_rebase(FlatProgramView program,
                                 int basis_line_idx,
                                 ReplGlStateReport *out);

#endif /* REPL_GL_STATE_INSPECTOR_H */
