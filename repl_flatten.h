/*
 * repl_flatten.h - Expansion of source commands to flat (flattened) commands.
 *
 * Implements the second half of the two-level command model: source commands
 * (as edited by the user) are expanded into a flat program where:
 *
 *   - For-loops are unrolled (capped at 100k total command visits to prevent
 *     runaway expansion of deeply nested or high-iteration loops)
 *   - Function calls are inlined with actual arguments substituted for params
 *   - If-block conditions are evaluated; bodies included/skipped accordingly
 *   - Each flat command records its origin (source_cmd_idx) and active scopes
 *     (func_scope_mask, call_src_cmd_idx) for cursor highlighting & debugging
 *
 * Expansion is recursive: functions can call functions (depth limit 32),
 * loops can nest, if-blocks can contain anything. The flattening process is
 * lazy and triggered by mark_normals_dirty() after any source mutation.
 *
 * The live flat program (g_flat_cmds, g_flat_local_vars) is rebuilt once per
 * frame if the dirty flag is set. Tests and replay tools can flatten into
 * temporary buffers without mutating the live arrays (see FlatProgramView).
 *
 * Local variables (loop counters, function params) are snapshotted into
 * FlatCmdLocalVars for each flat command so that expressions with variable
 * references can be re-evaluated at execution time with the correct variable
 * values (important for time-dependent t, or slider-modified variables).
 */

#ifndef REPL_FLATTEN_H
#define REPL_FLATTEN_H

#include "sample.h"

/* Local variable snapshot for a single flat command. Captured when the
 * command is emitted (e.g., loop counter value, function parameter binding).
 * Used at execution time to re-evaluate expressions with the correct variable
 * scope (for time-dependent 't' or slider-modified variables). */
typedef struct {
    int     num_vars;
    ExprVar vars[MAX_EXPR_VARS];
} FlatCmdLocalVars;

/* Read-only view over an expanded command stream. The live view points at
 * g_flat_cmds[] and g_flat_local_vars[]; tests and replay tools can flatten
 * into temporary buffers and pass a view over them without changing the
 * executor's behavior. */
typedef struct {
    const GLCmd      *cmds;
    FlatCmdLocalVars *local_vars;
    int               cmd_count;
} FlatProgramView;

/* Input: source program to expand. Caller provides the source command array,
 * a target flat buffer (with capacity), and resource limits (call depth,
 * visit budget). Output written to flat_cmds[] and flat_local_vars[]. */
typedef struct {
    const GLCmd      *source_cmds;
    int               source_cmd_count;
    GLCmd            *flat_cmds;
    FlatCmdLocalVars *flat_local_vars;
    int               flat_capacity;
    int               max_call_depth;   /* recursion limit (default 32) */
    int               visit_budget;     /* total command visits allowed (default 100k) */
} ReplFlattenOptions;

/* Result: whether flattening succeeded, how many commands were generated,
 * which lighting mode won, and any error message. status[128] is always
 * null-terminated (may be empty if ok=1). */
typedef struct {
    int  ok;                             /* 1 if flattening succeeded */
    int  flat_cmd_count;
    int  user_lighting_enabled;
    char status[128];                    /* error or informational message */
} ReplFlattenResult;

/* Get a view over the live flat program (g_flat_cmds, g_flat_local_vars).
 * The pointers are valid until the next call to repl_flatten_program()
 * on the live buffers. */
FlatProgramView repl_flat_program_view_live(void);

/* Expand a source program into a flat command stream. Options specify the
 * source array, target buffers, and resource limits. Result contains the
 * command count and any error message. Returns 1 on success, 0 on error
 * (e.g., recursion depth exceeded, visit budget exhausted). */
int  repl_flatten_program(const ReplFlattenOptions *options,
                          ReplFlattenResult *result);

#endif /* REPL_FLATTEN_H */
