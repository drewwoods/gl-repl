/*
 * src/repl/init_state.h - Read-only generated-program GL-state writes.
 */
#ifndef REPL_INIT_STATE_H
#define REPL_INIT_STATE_H

#include "repl/command.h"

/* Generated init()/display() contain a few fixed-function calls that are not
 * part of the user REPL language. Expose them as neutral writes so learning
 * tools can fold the same program the editor shows without issuing GL calls. */
typedef enum {
    REPL_GENERATED_STATE_COMMAND = 0,
    REPL_GENERATED_STATE_LIGHT_FV,
    REPL_GENERATED_STATE_LIGHT_MODEL_FV,
    REPL_GENERATED_STATE_PUSH_ATTRIB
} ReplGeneratedStateWriteKind;

typedef struct {
    ReplGeneratedStateWriteKind kind;
    GLCmd command;       /* REPL_GENERATED_STATE_COMMAND */
    GLenum object;       /* light enum for REPL_GENERATED_STATE_LIGHT_FV */
    GLenum pname;
    float value[4];
    int value_count;
} ReplGeneratedStateWrite;

/* Read the effective state-setting commands applied by init(). Toggle-disabled
 * and runtime-unsupported entries are omitted; neutralized entries return the
 * same replacement command repl_apply_init_bootstrap executes. */
int repl_init_bootstrap_state_command_count(void);
int repl_init_bootstrap_state_command_at(int state_command_idx,
                                         GLCmd *out_cmd);

/* Effective state writes in source/execution order. Comments and non-state
 * calls (for example glClear) are omitted. Values come from the same bridges
 * and config reads used by the code panel and C exporter. */
int repl_generated_init_state_write_count(void);
int repl_generated_init_state_write_at(int state_write_idx,
                                       ReplGeneratedStateWrite *out_write);
int repl_generated_display_state_write_count(void);
int repl_generated_display_state_write_at(int state_write_idx,
                                          ReplGeneratedStateWrite *out_write);

#endif /* REPL_INIT_STATE_H */
