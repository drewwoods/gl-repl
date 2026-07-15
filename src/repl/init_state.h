/*
 * src/repl/init_state.h - Read-only view of init()'s REPL GL-state commands.
 */
#ifndef REPL_INIT_STATE_H
#define REPL_INIT_STATE_H

#include "repl/command.h"

/* Read the effective state-setting commands applied by init(). Toggle-disabled
 * and runtime-unsupported entries are omitted; neutralized entries return the
 * same replacement command repl_apply_init_bootstrap executes. */
int repl_init_bootstrap_state_command_count(void);
int repl_init_bootstrap_state_command_at(int state_command_idx,
                                         GLCmd *out_cmd);

#endif /* REPL_INIT_STATE_H */
