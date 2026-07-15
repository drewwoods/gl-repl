/*
 * src/repl/gl_state_inspector.h - Source-position OpenGL state inspection.
 *
 * Folds the current flat program up to a source checkpoint without issuing
 * GL calls.  The result contains only state variables that user-authored REPL
 * commands explicitly touched.  Current values are paired with the OpenGL
 * 2.1 initial values, and equality is kept separate from touched-ness so an
 * explicit write of a default value remains visible.
 */
#ifndef REPL_GL_STATE_INSPECTOR_H
#define REPL_GL_STATE_INSPECTOR_H

#include "repl/flatten.h"

#define REPL_GL_STATE_REPORT_MAX_ROWS 72
#define REPL_GL_STATE_NAME_MAX         64
#define REPL_GL_STATE_VALUE_MAX        192

typedef struct {
    char name[REPL_GL_STATE_NAME_MAX];
    char current[REPL_GL_STATE_VALUE_MAX];
    char default_value[REPL_GL_STATE_VALUE_MAX];
    int  differs_from_default;
} ReplGlStateReportRow;

typedef struct {
    ReplGlStateReportRow rows[REPL_GL_STATE_REPORT_MAX_ROWS];
    int                  count;
    int                  source_line_idx;
} ReplGlStateReport;

/* Build the REPL-authored GL state immediately before source_line_idx.
 * Flat-command provenance is used so selected if branches, unrolled loops,
 * and function calls reflect the current flattened frame. */
void repl_gl_state_report_at_line(FlatProgramView program,
                                  int source_line_idx,
                                  ReplGlStateReport *out);

#endif /* REPL_GL_STATE_INSPECTOR_H */
