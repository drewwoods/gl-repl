#ifndef REPL_EXECUTOR_H
#define REPL_EXECUTOR_H

#include "repl_flatten.h"

typedef struct {
    int             flat_cmd_count;
    FlatProgramView program;
} ReplExecutionOptions;

void repl_execute_program(const ReplExecutionOptions *options);

#endif /* REPL_EXECUTOR_H */
