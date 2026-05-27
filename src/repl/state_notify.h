#ifndef REPL_STATE_NOTIFY_H
#define REPL_STATE_NOTIFY_H

void repl_state_mark_flat_dirty(void);
void repl_state_mark_source_dirty(void);
int  repl_state_parse_workspace_header_line(const char *line);

#endif /* REPL_STATE_NOTIFY_H */
