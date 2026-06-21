/*
 * src/repl/flatten_query.h - Queries over the live flat command stream.
 */
#ifndef REPL_FLATTEN_QUERY_H
#define REPL_FLATTEN_QUERY_H

/* Refresh the current-block highlight by scanning the flat program to find
 * the innermost BEGIN/END block containing the current edit line. Called
 * before building scene config to ensure cursor block bounds are current. */
void repl_flatten_refresh_current_block_highlight(int edit_line_idx);

int  repl_flat_cmd_matches_cursor(int flat_idx, int edit_line_idx);

/* ---- Flat-cost attribution (command-budget readout) ----
 *
 * How many commands of the flat (expanded) program are attributable to
 * the source line under the cursor — i.e. where the MAX_COMMANDS
 * flatten budget is being spent. Counts aggregate over the whole
 * frame: a line inside a loop counts once per iteration, a function
 * counts across every call site (via func_scope_mask, exact at any
 * nesting depth including recursion), a call site counts its inclusive
 * expansion (exact for top-level calls via root_call_src_cmd_idx), and
 * a for/if block counts its direct body plus expansions of calls made
 * inside it (exact to two levels of call nesting — deeper chains
 * attribute to the callee's own lines instead). Pure queries over the
 * live flat program; flatten first if the dirty flag is set. */
typedef enum {
    REPL_FLAT_COST_NONE = 0,  /* nothing attributable (comment/empty/decl) */
    REPL_FLAT_COST_LINE,      /* plain top-level line */
    REPL_FLAT_COST_CALL,      /* funcN()/alias call site */
    REPL_FLAT_COST_BLOCK,     /* for/if block (head, body, or end line) */
    REPL_FLAT_COST_FUNC,      /* funcN definition (head, body, or end line) */
} ReplFlatCostKind;

typedef struct {
    ReplFlatCostKind kind;
    int              count;
} ReplFlatCost;

ReplFlatCost repl_flatten_cost_at_line(int line_idx);

#endif /* REPL_FLATTEN_QUERY_H */
