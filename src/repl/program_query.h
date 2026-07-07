/*
 * src/repl/program_query.h - Read-only queries over the current REPL program.
 */
#ifndef REPL_PROGRAM_QUERY_H
#define REPL_PROGRAM_QUERY_H

#include "repl/command.h"
#include "source_document.h"

const char *repl_mode_name(GLenum mode);
GLenum      repl_current_begin_mode(void);
int         repl_count_vertices(void);

/* Maximum number of exported keyboard knobs — one QWERTY column-pair each
 * (q/a w/s e/d r/f t/g y/h u/j i/k o/l). */
#define REPL_TUNE_MAX_KNOBS 9

/* Walk `cmds[0..count)` and collect, in declaration order, the names of every
 * variable on a CMD_VAR_DECLARE line whose source text (read from `text`)
 * carries a whole-token `// @tune` tag. Writes up to `max` name pointers into
 * `out` (each points into a GLCmd decl payload, valid as long as `cmds` is),
 * sets `*total_out` (when non-NULL) to the total number of tagged names found
 * so callers can report the knob cap, and returns the number written
 * (min(total, max)). Neutral: cmds + text are parameters, no global reach. */
int repl_collect_tuned_vars(const GLCmd *cmds, int count, SourceTextView text,
                            const char **out, int max, int *total_out);

/* Mark predef-variable slots that are written by committed `name = expr;`
 * commands. `written_slots[0..written_count)` is cleared first; each
 * CMD_VAR_ASSIGN with a valid current var_idx sets one slot. Returns the
 * number of unique slots marked. Neutral: cmds are parameters, no globals. */
int repl_mark_written_var_slots(const GLCmd *cmds, int count,
                                int *written_slots, int written_count);

#endif /* REPL_PROGRAM_QUERY_H */
