#ifndef REPL_STATE_NOTIFY_H
#define REPL_STATE_NOTIFY_H

void repl_state_mark_flat_dirty(void);
void repl_state_mark_source_dirty(void);
/* A predef variable's VALUE changed (slider drag, t advance, apply
 * SET_VALUE) with the source text untouched. Routes the change by the flat
 * program's dependency masks: a structural root marks the whole flat
 * program dirty; a value-only root accumulates into args_dirty_mask (an
 * in-place rebake suffices, provided rebake_ok - otherwise escalates to
 * full); a root neither mask contains is a no-op. Out-of-range slots take
 * the conservative full-dirty path. */
void repl_state_notify_predef_value_changed(int slot);
/* Public invalidation wrapper for source edits outside REPL state internals. */
void repl_mark_source_dirty(void);
int  repl_state_parse_workspace_header_line(const char *line);

#endif /* REPL_STATE_NOTIFY_H */
