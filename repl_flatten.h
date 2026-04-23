#ifndef REPL_FLATTEN_H
#define REPL_FLATTEN_H

#include "sample.h"

typedef struct {
    int     num_vars;
    ExprVar vars[MAX_EXPR_VARS];
} FlatCmdLocalVars;

/* View over an expanded command stream. The live view points at g_flat_cmds[]
 * and its per-command local-variable snapshots, but tests and future
 * replay/import paths can pass another flat stream without changing the
 * executor's control flow. */
typedef struct {
    const GLCmd      *cmds;
    FlatCmdLocalVars *local_vars;
    int               cmd_count;
} FlatProgramView;

typedef struct {
    const GLCmd      *source_cmds;
    int               source_cmd_count;
    GLCmd            *flat_cmds;
    FlatCmdLocalVars *flat_local_vars;
    int               flat_capacity;
    int               max_call_depth;
    int               visit_budget;
} ReplFlattenOptions;

typedef struct {
    int  ok;
    int  flat_cmd_count;
    int  user_lighting_enabled;
    char status[128];
} ReplFlattenResult;

FlatProgramView repl_flat_program_view_live(void);
int  repl_flatten_program(const ReplFlattenOptions *options,
                          ReplFlattenResult *result);

#endif /* REPL_FLATTEN_H */
