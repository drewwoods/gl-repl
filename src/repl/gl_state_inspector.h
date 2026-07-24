/*
 * src/repl/gl_state_inspector.h - Source-position OpenGL state inspection.
 *
 * Folds the generated init()/display() setup plus the current flat user
 * program up to a source checkpoint without issuing GL calls. The result
 * contains only state variables that those phases explicitly touched. Current
 * values are paired with the OpenGL 2.1 initial values, and equality is kept
 * separate from touched-ness so an explicit write of a default remains
 * visible.
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

typedef struct {
    char name[REPL_GL_STATE_NAME_MAX];
    char current[REPL_GL_STATE_VALUE_MAX];
    char default_value[REPL_GL_STATE_VALUE_MAX];
    int  differs_from_default;
    ReplGlStateChangeSource source;
} ReplGlStateReportRow;

/* Rows are partitioned by authorship: every row whose latest change came from
 * a user source line (source.source_line_idx >= 0) comes first, followed by
 * the rows the generated init()/display() setup owns. `user_row_count` is the
 * boundary, so rows[0, user_row_count) is "what this program did" and
 * rows[user_row_count, count) is "what the harness set up around it" — the
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
} ReplGlStateReport;

/* Build the effective generated init() + display() + REPL-authored display
 * state immediately before source_line_idx. Flat-command provenance is used
 * so selected if branches, unrolled loops, and function calls reflect the
 * current flattened frame. */
void repl_gl_state_report_at_line(FlatProgramView program,
                                  int source_line_idx,
                                  ReplGlStateReport *out);

#endif /* REPL_GL_STATE_INSPECTOR_H */
