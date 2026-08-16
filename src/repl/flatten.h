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
 * time-dependent t and slider-modified variables - the executor consumes
 * baked args only); the stored snapshots serve consumers that reconstruct a
 * flat command's scope after the fact, e.g. replay's value-tracing
 * annotations.
 */

#ifndef REPL_FLATTEN_H
#define REPL_FLATTEN_H

#include "repl/command.h"
#include "repl/eval.h"
#include "repl/expr_program.h" /* ReplExprCache (optional compiled-expression cache) */
#include "source_document.h"  /* SourceTextView */
#include "config.h"           /* REPL_DIAG_TEXT_MAX */

#ifndef MAX_FLATTEN_CALL_DEPTH
#define MAX_FLATTEN_CALL_DEPTH 64
#endif

/* Soft intern caps. A program that stays inside the visit budget can still
 * exceed any fixed frame bound, so overflow latches and later commands
 * carry REPL_CALL_FRAME_NONE rather than failing the flatten. */
#ifndef MAX_CALL_FRAMES
#define MAX_CALL_FRAMES 16384
#endif
#ifndef MAX_CALL_FRAME_ARGS
#define MAX_CALL_FRAME_ARGS 65536
#endif

/* No interned frame: top-level command, or a call after the table latched. */
#define REPL_CALL_FRAME_NONE (-1)

/* Topology half of one dynamic invocation. Arguments live in the parallel
 * arena (arg_offset / arg_count). 8 ints, 32 B. */
typedef struct {
    int parent;             /* enclosing frame, REPL_CALL_FRAME_NONE at top */
    int call_src_cmd_idx;   /* the funcN(...) row that opened this frame */
    int func_slot;          /* 0..REPL_FUNC_SLOT_COUNT-1 */
    int depth;              /* == parent->depth + 1 */
    int flat_begin, flat_end;   /* the frame's contiguous subtree range */
    int arg_offset, arg_count;  /* window into FlatProgramView.call_frame_args */
} ReplCallFrame;

/* Local variable snapshot for a single flat command. Captured when the
 * command is emitted (e.g., loop counter value, function parameter binding).
 * Not read by the executor (which consumes baked args); consumers that need
 * a flat command's scope after the fact - replay's value-tracing
 * annotations - read it to reconstruct per-instance bindings. */
typedef struct {
    int   source_cmd_idx;
    float iter_value;
    float end_value;
} FlatCmdActiveLoop;

typedef struct {
    int     num_vars;
    ExprVar vars[MAX_EXPR_VARS];
    /* Dynamic loop ancestry is separate from lexical vars: a callee must not
     * be able to evaluate expressions against its caller's iterators, but
     * replay still needs those values to annotate loop headers while showing
     * commands several calls deeper. Innermost entries are stored last. */
    int               num_active_loops;
    FlatCmdActiveLoop active_loops[MAX_EXPR_VARS];
} FlatCmdLocalVars;

/* Read-only view over an expanded command stream. The live view points at
 * the arrays in ReplFlatProgramState; tests and replay tools can flatten
 * into temporary buffers and pass a view over them without changing the
 * executor's behavior. Frame-table pointers may be NULL when the caller
 * flattened without an intern buffer; then every command is unindexed. */
typedef struct {
    const GLCmd      *cmds;
    const FlatCmdLocalVars *local_vars;
    int               cmd_count;
    int               overflow_cmd_count; /* exact required count after overflow */
    const int            *call_frame_idx;     /* parallel to cmds, or NULL */
    const ReplCallFrame  *call_frames;
    int                   call_frame_count;
    const float          *call_frame_args;
    int                   call_frame_arg_count;
    int                   call_frame_overflow; /* latch: later calls unindexed */
} FlatProgramView;

static inline int repl_call_frame_ok(const FlatProgramView *view, int frame) {
    return view && view->call_frames &&
           frame >= 0 && frame < view->call_frame_count;
}

static inline int repl_flat_cmd_call_frame(const FlatProgramView *view,
                                           int flat_idx) {
    if (!view || !view->call_frame_idx ||
        flat_idx < 0 || flat_idx >= view->cmd_count)
        return REPL_CALL_FRAME_NONE;
    return view->call_frame_idx[flat_idx];
}

/* Walk parent links outermost-first. Returns the number of frames written
 * (capped at max_out). 0 if `frame` is unindexed. */
int repl_call_frame_walk_chain(FlatProgramView view, int frame,
                               int *out_frames, int max_out);

/* Re-derive the four legacy provenance fields from an interned chain.
 * Returns 1 on success. Indexed frames only - do not call on
 * REPL_CALL_FRAME_NONE (the overflow fallback is the stored fields). */
typedef struct {
    int          call_src_cmd_idx;
    int          root_call_src_cmd_idx;
    int          call_depth;
    unsigned int func_scope_mask;
} ReplCallFrameDerivedProv;

int repl_call_frame_derive_prov(FlatProgramView view, int frame,
                                ReplCallFrameDerivedProv *out);

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
    /* Optional call-frame intern. NULL / zero capacity skips interning
     * (every written call_frame_idx slot is REPL_CALL_FRAME_NONE). The
     * live wrapper always supplies the ReplFlatProgramState arrays. A
     * short table or arena latches overflow; flatten itself still
     * succeeds. */
    int              *flat_call_frame_idx;   /* parallel to flat_cmds */
    ReplCallFrame    *call_frames;
    int               call_frame_capacity;
    float            *call_frame_args;
    int               call_frame_arg_capacity;
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
     * structural roots can change flat-stream
     * topology or frozen local snapshots (loop bounds, if/else-if
     * conditions, call args - plus everything conservatively widened to
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
    int  call_frame_count;
    int  call_frame_arg_count;
    int  call_frame_overflow;
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

/* ---- In-place rebake ----------------------------------------------------
 *
 * Re-evaluate the VALUES of an existing flat command stream without
 * re-expanding it: baked argument slots and assignment results are
 * recomputed from each command's compiled programs under its frozen local
 * snapshot, and assignments (scalar + scratch) are re-applied in stream
 * order so later reads see them - the same threading a full flatten
 * performs. Topology, provenance, flags, local snapshots, call-frame
 * intern, and lighting classification are never touched; that is what
 * makes it valid only for
 * value-routed changes (args_dirty_mask): any structural root change must
 * take repl_flatten_program instead. Compiled-only by design - a needed
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
