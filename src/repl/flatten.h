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
#include "repl/expr_program.h" /* ReplExprCache (optional compiled-expression cache) */
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
    int               overflow_cmd_count; /* exact required count after overflow */
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
    /* Differential seam: force every command line back through the text
     * parser, disabling BOTH the literal-command fast path (which appends a
     * `has_vars == 0` source command verbatim) and the compiled-expression
     * cache below. Zero (the live default) takes the fast paths; the
     * flatten differential test flattens each corpus scene both ways and
     * compares. Not a user-facing knob. */
    int               force_reparse;
    /* Optional compiled-expression cache. NULL (the zero-initialized
     * default for tests/tools and temporary-buffer callers) keeps the pure
     * text paths. The live repl_flatten_commands wrapper passes
     * repl_expr_cache_live(): lines whose expressions compiled on an
     * earlier flatten evaluate their programs instead of re-parsing text;
     * EMPTY lines are built (compiled during the text parse via the capture
     * sink) as they are first visited; FAILED lines stay on the text path
     * until the next source-dirty invalidation. */
    ReplExprCache    *expr_cache;
} ReplFlattenOptions;

/* Result: whether flattening succeeded, how many commands were generated,
 * which lighting mode won, and any error message. status is always
 * null-terminated (may be empty if ok=1); its width is REPL_DIAG_TEXT_MAX
 * (shared with the rest of the REPL pipeline's intermediate diagnostic
 * buffers; see config.h). */
typedef struct {
    int  ok;                             /* 1 if flattening succeeded */
    int  flat_cmd_count;
    /* Exact destination size needed when flat_capacity was too small.
     * The executable flat_cmd_count remains zero on that failure. Zero for
     * other failures; equal to flat_cmd_count on success. */
    int  required_flat_capacity;
    int  user_lighting_enabled;
    /* Per-predef-root dependency masks for the produced flat program
     * (flatten plan phase 3): structural roots can change flat-stream
     * topology or frozen local snapshots (loop bounds, if/else-if
     * conditions, call args — plus everything conservatively widened to
     * all-bits: scratch reads in structural positions, uncached/failed
     * expressions); value roots feed baked args or assignments. Runs
     * without an expression cache report all-bits in both (fully
     * conservative). rebake_ok is 1 when every has_vars flat command has
     * compiled programs, so an in-place rebake could re-evaluate the
     * whole stream. */
    ReplExprDepMask structural_dep_mask;
    ReplExprDepMask value_dep_mask;
    int  rebake_ok;
    char status[REPL_DIAG_TEXT_MAX];     /* error or informational message */
} ReplFlattenResult;


/* Expand a source program into a flat command stream. Options specify the
 * source array, target buffers, and resource limits. Result contains the
 * command count and any error message. Returns 1 on success, 0 on error
 * (e.g., recursion depth exceeded, visit budget exhausted). */
int  repl_flatten_program(const ReplFlattenOptions *options,
                          ReplFlattenResult *result);

/* True when the executable (post-expansion) stream clears the stencil buffer.
 * Kept as a flat-program query so dead branches and uncalled functions do not
 * suppress the stencil-view clear warning. */
int repl_flat_clears_stencil(const GLCmd *flat_cmds, int flat_count);

/* ---- In-place rebake (flatten plan phase 3b) -----------------------------
 *
 * Re-evaluate the VALUES of an existing flat command stream without
 * re-expanding it: baked argument slots and assignment results are
 * recomputed from each command's compiled programs under its frozen local
 * snapshot, and assignments (scalar + scratch) are re-applied in stream
 * order so later reads see them — the same threading a full flatten
 * performs. Topology, provenance, flags, local snapshots, and lighting
 * classification are never touched; that is what makes it valid only for
 * value-routed changes (args_dirty_mask): any structural root change must
 * take repl_flatten_program instead. Compiled-only by design — a needed
 * program that is missing (line not READY) fails the walk, it never falls
 * back to text parsing. */
typedef struct {
    GLCmd                  *flat_cmds;        /* args re-baked in place */
    const FlatCmdLocalVars *flat_local_vars;  /* frozen per-cmd local snapshots
                                               * (NULL: no command has locals) */
    int                     flat_count;
    ReplExprCache          *expr_cache;       /* required */
} ReplRebakeOptions;

typedef struct {
    int  ok;
    char status[REPL_DIAG_TEXT_MAX];
} ReplRebakeResult;

/* Returns 1 on success. On failure (invalid options, a required program
 * missing, scratch index out of range) returns 0 with result->status set;
 * the stream may be partially re-baked and the caller owns recovery (the
 * live wrapper below restores state and callers escalate to a full
 * flatten, which overwrites the stream wholesale). Like
 * repl_flatten_program, the walk applies assignments to the live
 * predef/scratch tables as it goes. */
int  repl_flatten_rebake_program(const ReplRebakeOptions *options,
                                 ReplRebakeResult *result);

#endif /* REPL_FLATTEN_H */
