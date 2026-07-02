/*
 * src/repl/flatten.h - Expansion of source commands to flat (flattened) commands.
 *
 * Implements the second half of the two-level command model: source commands
 * are expanded into a flat program where for-loops are unrolled, function
 * calls are inlined, and if-block conditions are evaluated. Expansion is
 * recursive and bounded by MAX_FLATTEN_CALL_DEPTH and MAX_FLATTEN_VISIT_BUDGET.
 *
 * The live flat program is rebuilt once per frame if the dirty flag is set.
 * Tests and replay tools can flatten into temporary buffers without mutating
 * the live arrays (see FlatProgramView). Query helpers over the live flat
 * result live in flatten_query.h.
 *
 * Local variables (loop counters, function params) are snapshotted into
 * FlatCmdLocalVars for each flat command. Argument re-evaluation itself
 * happens at flatten time (the per-frame re-flatten is what animates
 * time-dependent t and slider-modified variables — the executor consumes
 * baked args only); the stored snapshots serve consumers that reconstruct a
 * flat command's scope after the fact, e.g. replay's value-tracing
 * annotations.
 */

#ifndef REPL_FLATTEN_H
#define REPL_FLATTEN_H

#include "repl/command.h"
#include "repl/eval.h"
#include "source_document.h"  /* SourceTextView (Phase 1 of feature/source-document-port.md) */
#include "config.h"           /* REPL_DIAG_TEXT_MAX */

/* Local variable snapshot for a single flat command. Captured when the
 * command is emitted (e.g., loop counter value, function parameter binding).
 * Not read by the executor (which consumes baked args); consumers that need
 * a flat command's scope after the fact — replay's value-tracing
 * annotations — read it to reconstruct per-instance bindings. */
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
    const FlatCmdLocalVars *local_vars;
    int               cmd_count;
} FlatProgramView;

typedef struct {
    int          edit_line_idx;
    int          cursor_block_begin;
    int          cursor_block_end;
    int          cursor_source_block_valid;
    int          cursor_source_block_begin;
    int          cursor_source_block_end;
    unsigned int cursor_func_scope_mask;
} CursorBlockState;

/* Input: source program to expand. Caller provides the source command array,
 * a target flat buffer (with capacity), an editor-text view for the
 * expansion's text reads, and resource limits (call depth, visit
 * budget). Output written to flat_cmds[] and flat_local_vars[]. */
typedef struct {
    const GLCmd      *source_cmds;
    int               source_cmd_count;
    GLCmd            *flat_cmds;
    FlatCmdLocalVars *flat_local_vars;
    int               flat_capacity;
    SourceTextView    text;             /* source-text view used for inline expansion */
    ReplFuncAliasView func_aliases;     /* aliases visible while reparsing source text */
    int               max_call_depth;   /* recursion limit (default MAX_FLATTEN_CALL_DEPTH) */
    int               visit_budget;     /* total command visits allowed (default MAX_FLATTEN_VISIT_BUDGET) */
} ReplFlattenOptions;

/* Result: whether flattening succeeded, how many commands were generated,
 * which lighting mode won, and any error message. status is always
 * null-terminated (may be empty if ok=1); its width is REPL_DIAG_TEXT_MAX
 * (shared with the rest of the REPL pipeline's intermediate diagnostic
 * buffers; see config.h). */
typedef struct {
    int  ok;                             /* 1 if flattening succeeded */
    int  flat_cmd_count;
    int  user_lighting_enabled;
    char status[REPL_DIAG_TEXT_MAX];     /* error or informational message */
} ReplFlattenResult;


/* Expand a source program into a flat command stream. Options specify the
 * source array, target buffers, and resource limits. Result contains the
 * command count and any error message. Returns 1 on success, 0 on error
 * (e.g., recursion depth exceeded, visit budget exhausted). */
int  repl_flatten_program(const ReplFlattenOptions *options,
                          ReplFlattenResult *result);

#endif /* REPL_FLATTEN_H */
