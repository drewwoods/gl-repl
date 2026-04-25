#ifndef REPL_EXECUTOR_H
#define REPL_EXECUTOR_H

#include "repl_flatten.h"

typedef struct {
    int             flat_cmd_count;
    FlatProgramView program;
} ReplExecutionOptions;

void apply_transform_cmd(const GLCmd *cmd);
void apply_tracked_transform_cmd(const GLCmd *cmd, int *matrix_depth);
void unwind_tracked_transform_stack(int *matrix_depth);

void repl_executor_init_resources(void);
void repl_executor_destroy_resources(void);
void repl_execute_program(const ReplExecutionOptions *options);

#endif /* REPL_EXECUTOR_H */
