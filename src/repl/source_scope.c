/*
 * src/repl/source_scope.c - Prefix-depth cache and source block queries.
 *
 * These helpers answer questions about the source command array before a
 * command at a given index is parsed, formatted, or structurally edited.
 */
#include <string.h>
#include "repl/source_scope.h"
#include "repl/state_views.h"

/* Live-document compatibility cache for O(1) depth lookups at position `pos`.
 * Explicit callers should bind their own ReplSourceScopeView instead. */
static int g_live_scope_dirty = 1;
static ReplSourceScopeView g_live_scope;

void repl_source_scope_depth_cache_invalidate(void) {
    g_live_scope_dirty = 1;
}

/* Rebuild prefix-sum depth arrays so that depth_prefix[pos] gives the
 * nesting depth *before* command `pos`.  Each array tracks one kind of
 * scope opener/closer:
 *
 *   block_depth_prefix  - any block (for/func/if) nesting (used for indent)
 *   begin_depth_prefix  - glBegin/glEnd nesting
 *   tess_depth_prefix   - gluBegin/gluEnd nesting
 *   matrix_depth_prefix - glPushMatrix/glPopMatrix nesting (indents like glBegin). */
static void source_scope_view_build(ReplSourceScopeView *view) {
    if (!view) return;

    view->block_depth_prefix[0]  = 0;
    view->begin_depth_prefix[0]  = 0;
    view->tess_depth_prefix[0]   = 0;
    view->matrix_depth_prefix[0] = 0;

    for (int i = 0; i < view->count; i++) {
        int block_depth  = view->block_depth_prefix[i];
        int begin_depth  = view->begin_depth_prefix[i];
        int tess_depth   = view->tess_depth_prefix[i];
        int matrix_depth = view->matrix_depth_prefix[i];

        if (view->cmds && view->cmds[i].valid) {
            CmdType t = view->cmds[i].type;

            if (repl_cmd_is_block_head(t)) block_depth++;
            else if (repl_cmd_is_block_end(t)) block_depth--;

            if (t == CMD_BEGIN) begin_depth++;
            else if (t == CMD_END) begin_depth--;

            if (t == CMD_TESS_BEGIN_POLYGON || t == CMD_TESS_BEGIN_CONTOUR) tess_depth++;
            else if (t == CMD_TESS_END) tess_depth--;

            if (t == CMD_PUSH_MATRIX) matrix_depth++;
            else if (t == CMD_POP_MATRIX) matrix_depth--;
        }

        if (block_depth < 0)  block_depth = 0;
        if (begin_depth < 0)  begin_depth = 0;
        if (tess_depth < 0)   tess_depth = 0;
        if (matrix_depth < 0) matrix_depth = 0;

        view->block_depth_prefix[i + 1]  = block_depth;
        view->begin_depth_prefix[i + 1]  = begin_depth;
        view->tess_depth_prefix[i + 1]   = tess_depth;
        view->matrix_depth_prefix[i + 1] = matrix_depth;
    }

    view->built = 1;
}

static int source_scope_clamp_pos(const ReplSourceScopeView *view, int pos) {
    int count = (view && view->count > 0) ? view->count : 0;
    if (pos < 0) pos = 0;
    if (pos > count) pos = count;
    return pos;
}

void repl_source_scope_view_bind(ReplSourceScopeView *view,
                                 const GLCmd *cmds, int count) {
    if (!view) return;
    if (count < 0) count = 0;
    if (count > MAX_EDITOR_COMMANDS) count = MAX_EDITOR_COMMANDS;
    view->cmds = cmds;
    view->count = count;
    view->built = 0;
    source_scope_view_build(view);
}

static const ReplSourceScopeView *live_source_scope_view(void) {
    if (g_live_scope_dirty) {
        repl_source_scope_view_bind(&g_live_scope,
                                    repl_state_document_cmds(),
                                    repl_state_document_count());
        g_live_scope_dirty = 0;
    }
    return &g_live_scope;
}

ReplSourceScopeLiveView repl_source_scope_live_view(void) {
    ReplSourceScopeLiveView view = { live_source_scope_view() };
    return view;
}

int repl_source_scope_view_in_begin_block_at(const ReplSourceScopeView *view,
                                             int line_idx) {
    if (!view || !view->built) return 0;
    line_idx = source_scope_clamp_pos(view, line_idx);
    return view->begin_depth_prefix[line_idx] > 0;
}

int repl_source_scope_view_block_depth_at(const ReplSourceScopeView *view,
                                          int pos) {
    if (!view || !view->built) return 0;
    pos = source_scope_clamp_pos(view, pos);
    return view->block_depth_prefix[pos];
}

int repl_source_scope_view_tess_scope_depth_at(const ReplSourceScopeView *view,
                                               int pos) {
    if (!view || !view->built) return 0;
    pos = source_scope_clamp_pos(view, pos);
    return view->tess_depth_prefix[pos];
}

int repl_source_scope_view_matrix_scope_depth_at(const ReplSourceScopeView *view,
                                                 int pos) {
    if (!view || !view->built) return 0;
    pos = source_scope_clamp_pos(view, pos);
    return view->matrix_depth_prefix[pos];
}

int repl_source_scope_in_begin_block_at(int line_idx) {
    return repl_source_scope_view_in_begin_block_at(live_source_scope_view(),
                                                    line_idx);
}

int repl_source_scope_in_begin_block(void) {
    return repl_source_scope_in_begin_block_at(repl_state_document_count());
}

int repl_source_scope_block_depth_at(int pos) {
    return repl_source_scope_view_block_depth_at(live_source_scope_view(), pos);
}

int repl_source_scope_tess_scope_depth_at(int pos) {
    return repl_source_scope_view_tess_scope_depth_at(live_source_scope_view(), pos);
}

int repl_source_scope_matrix_scope_depth_at(int pos) {
    return repl_source_scope_view_matrix_scope_depth_at(live_source_scope_view(), pos);
}

/* Collect the document indices of structurally unbalanced bracket
 * commands - glPushMatrix/glBegin openers with no matching close, and
 * orphan glPopMatrix/glEnd closers with no matching open. Matching is
 * linear (a stack per bracket type), the same relaxed model the
 * indentation depth uses, so the flagged lines agree with how the body
 * is indented. Fills `out_lines` (document order for orphans, then the
 * still-open openers) and returns the count, capped at `max`.
 *
 * LIMITATION - linear, block-unaware matching. The walk pairs brackets
 * purely in source order and does NOT respect for/funcN/if block
 * boundaries. So a glPushMatrix inside a loop/function body pairs with
 * the next glPopMatrix in source order even if that pop sits outside the
 * block (which, once the loop unrolls or the function is inlined, would
 * actually be unbalanced per iteration/call). This matches the rest of
 * the editor's source-level scope model (indentation, the push/pop
 * cursor-link); it is a heuristic for "obviously unbalanced", not a
 * per-iteration GL-correctness check. Block-scoped matching would be a
 * follow-up.
 *
 * Off-stack static scratch (MAX_EDITOR_COMMANDS deep) mirrors the depth-cache
 * arrays above and keeps this O(n) single-pass helper allocation-free. */
static int g_unbal_matrix_stack[MAX_EDITOR_COMMANDS];
static int g_unbal_begin_stack[MAX_EDITOR_COMMANDS];
static int g_unbal_attrib_stack[MAX_EDITOR_COMMANDS];

int repl_source_scope_view_collect_unbalanced(const ReplSourceScopeView *view,
                                              int *out_lines, int max) {
    const GLCmd *cmds = view ? view->cmds : NULL;
    int count = view ? view->count : 0;
    int msp = 0, bsp = 0, asp = 0, n = 0;

    if (!cmds || !out_lines || max <= 0)
        return 0;

    for (int i = 0; i < count; i++) {
        if (!cmds[i].valid)
            continue;
        switch (cmds[i].type) {
        case CMD_PUSH_MATRIX:
            if (msp < MAX_EDITOR_COMMANDS) g_unbal_matrix_stack[msp++] = i;
            break;
        case CMD_POP_MATRIX:
            if (msp > 0)            msp--;                       /* matched */
            else if (n < max)       out_lines[n++] = i;          /* orphan pop */
            break;
        case CMD_BEGIN:
            if (bsp < MAX_EDITOR_COMMANDS) g_unbal_begin_stack[bsp++] = i;
            break;
        case CMD_END:
            if (bsp > 0)            bsp--;                       /* matched */
            else if (n < max)       out_lines[n++] = i;          /* orphan end */
            break;
        case CMD_PUSH_ATTRIB:
            if (asp < MAX_EDITOR_COMMANDS) g_unbal_attrib_stack[asp++] = i;
            break;
        case CMD_POP_ATTRIB:
            if (asp > 0)            asp--;                       /* matched */
            else if (n < max)       out_lines[n++] = i;          /* orphan pop */
            break;
        default:
            break;
        }
    }

    /* Whatever is left on each stack never closed. */
    for (int k = 0; k < msp && n < max; k++) out_lines[n++] = g_unbal_matrix_stack[k];
    for (int k = 0; k < bsp && n < max; k++) out_lines[n++] = g_unbal_begin_stack[k];
    for (int k = 0; k < asp && n < max; k++) out_lines[n++] = g_unbal_attrib_stack[k];
    return n;
}

int repl_source_scope_collect_unbalanced(int *out_lines, int max) {
    return repl_source_scope_view_collect_unbalanced(live_source_scope_view(),
                                                    out_lines, max);
}

static void source_scope_write_indent(int spaces, char *buf, int buf_sz) {
    if (buf_sz <= 0) return;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

static int source_scope_view_line_is_if_branch_separator(
        const ReplSourceScopeView *view, int pos) {
    return (view && view->cmds && pos >= 0 && pos < view->count &&
            view->cmds[pos].valid &&
            repl_cmd_is_if_branch_separator(view->cmds[pos].type));
}

/* Normal command indent: 2 + 2*tess + 2*begin + 2*block + 2*matrix.
 * glPushMatrix/glPopMatrix nest, so their bodies indent one level per
 * open push (mirroring how glBegin opens a level via begin depth). */
void repl_source_scope_view_cmd_indent(const ReplSourceScopeView *view,
                                       int pos, char *buf, int buf_sz) {
    pos = source_scope_clamp_pos(view, pos);
    int td = repl_source_scope_view_tess_scope_depth_at(view, pos);
    int bd = view && view->built ? view->begin_depth_prefix[pos] : 0;
    int kd = repl_source_scope_view_block_depth_at(view, pos);
    int md = repl_source_scope_view_matrix_scope_depth_at(view, pos);
    if (source_scope_view_line_is_if_branch_separator(view, pos) && kd > 0)
        kd--;
    int spaces = 2 + 2 * td + 2 * bd + 2 * kd + 2 * md;
    source_scope_write_indent(spaces, buf, buf_sz);
}

void repl_source_scope_cmd_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_view_cmd_indent(live_source_scope_view(), pos, buf, buf_sz);
}

/* glBegin/glEnd-style indent: 2 + 2*tess + 2*block + 2*matrix, with
 * begin-depth deliberately EXCLUDED (the glBegin/glEnd lines are not
 * indented by the block they open). Matrix depth IS included so a
 * glBegin/glEnd nested inside a glPushMatrix block lands at the right
 * level. Uses the same public depth queries the parser previously
 * open-coded at two sites. */
void repl_source_scope_view_begin_indent(const ReplSourceScopeView *view,
                                         int pos, char *buf, int buf_sz) {
    int td = repl_source_scope_view_tess_scope_depth_at(view, pos);
    int kd = repl_source_scope_view_block_depth_at(view, pos);
    int md = repl_source_scope_view_matrix_scope_depth_at(view, pos);
    int spaces = 2 + 2 * td + 2 * kd + 2 * md;
    source_scope_write_indent(spaces, buf, buf_sz);
}

void repl_source_scope_begin_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_view_begin_indent(live_source_scope_view(), pos, buf, buf_sz);
}

void repl_source_scope_view_tess_close_indent(const ReplSourceScopeView *view,
                                              int pos, char *buf, int buf_sz) {
    int td = repl_source_scope_view_tess_scope_depth_at(view, pos);
    int kd = repl_source_scope_view_block_depth_at(view, pos);
    int md = repl_source_scope_view_matrix_scope_depth_at(view, pos);
    int spaces;

    if (td > 0) td--;
    spaces = 2 + 2 * td + 2 * kd + 2 * md;
    source_scope_write_indent(spaces, buf, buf_sz);
}

void repl_source_scope_tess_close_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_view_tess_close_indent(live_source_scope_view(), pos,
                                             buf, buf_sz);
}

/* glPopMatrix indent: aligns with its matching glPushMatrix by dropping
 * one matrix level (the push it closes is still counted in the prefix at
 * this position). Same shape as a normal command otherwise:
 * 2 + 2*tess + 2*begin + 2*block + 2*(matrix-1). */
void repl_source_scope_view_matrix_close_indent(const ReplSourceScopeView *view,
                                                int pos, char *buf, int buf_sz) {
    pos = source_scope_clamp_pos(view, pos);
    int td = repl_source_scope_view_tess_scope_depth_at(view, pos);
    int bd = view && view->built ? view->begin_depth_prefix[pos] : 0;
    int kd = repl_source_scope_view_block_depth_at(view, pos);
    int md = repl_source_scope_view_matrix_scope_depth_at(view, pos);
    if (md > 0) md--;
    int spaces = 2 + 2 * td + 2 * bd + 2 * kd + 2 * md;
    source_scope_write_indent(spaces, buf, buf_sz);
}

void repl_source_scope_matrix_close_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_view_matrix_close_indent(live_source_scope_view(), pos,
                                               buf, buf_sz);
}

int repl_source_scope_view_cmd_indent_chars(const ReplSourceScopeView *view,
                                            int pos) {
    pos = source_scope_clamp_pos(view, pos);
    if (!view || !view->built) return 2;
    int kd = view->block_depth_prefix[pos];
    if (source_scope_view_line_is_if_branch_separator(view, pos) && kd > 0)
        kd--;
    return 2 + 2 * view->tess_depth_prefix[pos] + 2 * view->begin_depth_prefix[pos]
             + 2 * kd + 2 * view->matrix_depth_prefix[pos];
}

int repl_source_scope_cmd_indent_chars(int pos) {
    return repl_source_scope_view_cmd_indent_chars(live_source_scope_view(), pos);
}

/* Tessellator leaf command indent: 2 + 2*tess + 2*block + 2*matrix
 * (begin depth ignored). */
void repl_source_scope_view_cmd_tess_indent(const ReplSourceScopeView *view,
                                            int pos, char *buf, int buf_sz) {
    int td = repl_source_scope_view_tess_scope_depth_at(view, pos);
    int kd = repl_source_scope_view_block_depth_at(view, pos);
    int md = repl_source_scope_view_matrix_scope_depth_at(view, pos);
    int spaces = 2 + 2 * td + 2 * kd + 2 * md;
    source_scope_write_indent(spaces, buf, buf_sz);
}

void repl_source_scope_cmd_tess_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_view_cmd_tess_indent(live_source_scope_view(), pos,
                                           buf, buf_sz);
}

int repl_source_scope_view_find_block_end(const ReplSourceScopeView *view,
                                          int begin_idx) {
    if (!view || !view->cmds) return 0;
    int depth = 1;
    for (int j = begin_idx + 1; j < view->count; j++) {
        CmdType t = view->cmds[j].type;
        if (repl_cmd_is_block_head(t)) depth++;
        else if (repl_cmd_is_block_end(t)) {
            depth--;
            if (depth == 0) return j;
        }
    }
    return view->count;
}

int repl_source_scope_find_block_end(int begin_idx) {
    return repl_source_scope_view_find_block_end(live_source_scope_view(),
                                                begin_idx);
}

CmdType repl_source_scope_view_nearest_open_block_at(const ReplSourceScopeView *view,
                                                     int pos) {
    if (!view || !view->cmds) return CMD_TYPE_COUNT;
    CmdType stack[REPL_MAX_BLOCK_NEST_DEPTH];
    int depth = 0;
    for (int i = 0; i < pos && i < view->count; i++) {
        CmdType t = view->cmds[i].type;
        if (repl_cmd_is_block_head(t)) {
            if (depth < REPL_MAX_BLOCK_NEST_DEPTH) stack[depth++] = t;
        } else if (repl_cmd_is_block_end(t)) {
            if (depth > 0) depth--;
        }
    }
    return depth > 0 ? stack[depth - 1] : CMD_TYPE_COUNT;
}

CmdType repl_source_scope_nearest_open_block_at(int pos) {
    return repl_source_scope_view_nearest_open_block_at(live_source_scope_view(), pos);
}

int repl_source_scope_view_enclosing_func_at(const ReplSourceScopeView *view,
                                            int pos,
                                            int *out_def, int *out_end) {
    int stack[REPL_MAX_BLOCK_NEST_DEPTH];
    int depth = 0;
    int def = -1;

    if (!view || !view->cmds)
        return 0;
    if (pos < 0)
        pos = 0;
    if (pos > view->count)
        pos = view->count;

    for (int i = 0; i < pos && i < view->count; i++) {
        if (!view->cmds[i].valid)
            continue;
        CmdType t = view->cmds[i].type;
        if (repl_cmd_is_block_head(t)) {
            if (depth < REPL_MAX_BLOCK_NEST_DEPTH)
                stack[depth++] = i;
        } else if (repl_cmd_is_block_end(t)) {
            if (depth > 0)
                depth--;
        }
    }
    /* Sitting on a FUNC_DEF row belongs to that function even if an
     * unmatched outer function is still on the stack. The walk is i < pos,
     * so a header at `pos` has not been pushed. */
    if (pos >= 0 && pos < view->count &&
        view->cmds[pos].valid && view->cmds[pos].type == CMD_FUNC_DEF) {
        def = pos;
    } else {
        for (int d = depth - 1; d >= 0; d--) {
            if (view->cmds[stack[d]].type == CMD_FUNC_DEF) {
                def = stack[d];
                break;
            }
        }
    }
    if (def < 0)
        return 0;
    int end = repl_source_scope_view_find_block_end(view, def);
    if (end >= view->count || !view->cmds[end].valid || view->cmds[end].type != CMD_FUNC_END)
        end = -1;
    if (out_def)
        *out_def = def;
    if (out_end)
        *out_end = end;
    return 1;
}

int repl_source_scope_enclosing_func_at(int pos, int *out_def, int *out_end) {
    return repl_source_scope_view_enclosing_func_at(live_source_scope_view(),
                                                   pos, out_def, out_end);
}

int repl_source_scope_view_next_func_def(const ReplSourceScopeView *view,
                                         int pos) {
    if (!view || !view->cmds)
        return -1;
    if (pos < 0)
        pos = -1;
    for (int i = pos + 1; i < view->count; i++) {
        if (view->cmds[i].valid && view->cmds[i].type == CMD_FUNC_DEF)
            return i;
    }
    return -1;
}

int repl_source_scope_next_func_def(int pos) {
    return repl_source_scope_view_next_func_def(live_source_scope_view(), pos);
}

int repl_source_scope_view_prev_func_def(const ReplSourceScopeView *view,
                                         int pos) {
    if (!view || !view->cmds)
        return -1;
    if (pos > view->count)
        pos = view->count;
    for (int i = pos - 1; i >= 0; i--) {
        if (view->cmds[i].valid && view->cmds[i].type == CMD_FUNC_DEF)
            return i;
    }
    return -1;
}

int repl_source_scope_prev_func_def(int pos) {
    return repl_source_scope_view_prev_func_def(live_source_scope_view(), pos);
}

int repl_source_scope_view_find_func_def_line(const ReplSourceScopeView *view,
                                             int fn) {
    if (!view || !view->cmds || fn < 0 || fn >= REPL_FUNC_SLOT_COUNT)
        return -1;
    for (int i = 0; i < view->count; i++) {
        if (!view->cmds[i].valid || view->cmds[i].type != CMD_FUNC_DEF)
            continue;
        /* `fn` is range-checked above, so matching it is the whole test -
         * a malformed def carrying an out-of-range args[0] can never
         * equal it. */
        if ((int)view->cmds[i].args[0] == fn)
            return i;
    }
    return -1;
}

int repl_source_scope_find_func_def_line(int fn) {
    return repl_source_scope_view_find_func_def_line(live_source_scope_view(), fn);
}

/* Is `pos` inside a for-loop body that `break` / `continue` may target?
 *
 * Same open-block stack as nearest_open_block_at, but read from the
 * innermost frame outward rather than just at the top: an if-block between
 * the statement and its loop is transparent (C's rule), so
 * `for { if { break; } }` is legal. A CMD_FUNC_DEF frame ends the search -
 * a callee's break can no more escape into its caller's loop here than it
 * can in C, and flatten enforces the same boundary at the call frame. */
int repl_source_scope_view_in_loop_at(const ReplSourceScopeView *view,
                                      int pos) {
    CmdType stack[REPL_MAX_BLOCK_NEST_DEPTH];
    int depth = 0;

    if (!view || !view->cmds) return 0;
    for (int i = 0; i < pos && i < view->count; i++) {
        CmdType t = view->cmds[i].type;
        if (repl_cmd_is_block_head(t)) {
            if (depth < REPL_MAX_BLOCK_NEST_DEPTH) stack[depth++] = t;
        } else if (repl_cmd_is_block_end(t)) {
            if (depth > 0) depth--;
        }
    }
    while (depth-- > 0) {
        if (stack[depth] == CMD_FOR_BEGIN) return 1;
        if (stack[depth] == CMD_FUNC_DEF) return 0;
    }
    return 0;
}

int repl_source_scope_in_loop_at(int pos) {
    return repl_source_scope_view_in_loop_at(live_source_scope_view(), pos);
}

static int source_scope_view_find_if_chain_head(const ReplSourceScopeView *view,
                                                int line_idx) {
    int depth = 0;

    if (!view || !view->cmds)
        return -1;
    for (int j = line_idx - 1; j >= 0; j--) {
        CmdType t = view->cmds[j].type;
        if (repl_cmd_is_block_end(t)) {
            depth++;
        } else if (repl_cmd_is_block_head(t)) {
            if (depth == 0)
                return t == CMD_IF_BEGIN ? j : -1;
            depth--;
        }
    }
    return -1;
}

int repl_source_scope_view_block_extent(const ReplSourceScopeView *view,
                                        int line_idx,
                                        int *out_start, int *out_count) {
    int n = view ? view->count : 0;
    int start = line_idx;
    if (line_idx < 0 || line_idx >= n) return 0;
    if (source_scope_view_line_is_if_branch_separator(view, line_idx)) {
        start = source_scope_view_find_if_chain_head(view, line_idx);
        if (start < 0)
            return 0;
    } else if (!repl_source_scope_view_line_is_block_head(view, line_idx)) {
        return 0;
    }
    int end = repl_source_scope_view_find_block_end(view, start);
    if (end >= n) end = n - 1;
    if (end < start) return 0;
    if (out_start) *out_start = start;
    if (out_count) *out_count = end - start + 1;
    return 1;
}

int repl_source_scope_block_extent(int line_idx,
                                   int *out_start, int *out_count) {
    return repl_source_scope_view_block_extent(live_source_scope_view(), line_idx,
                                               out_start, out_count);
}

int repl_source_scope_view_line_is_block_head(const ReplSourceScopeView *view,
                                              int line_idx) {
    if (!view || !view->cmds || line_idx < 0 || line_idx >= view->count) return 0;
    return repl_cmd_is_block_head(view->cmds[line_idx].type);
}

int repl_line_is_block_head(int line_idx) {
    return repl_source_scope_view_line_is_block_head(live_source_scope_view(),
                                                    line_idx);
}

int repl_array_contains_var_decl(const GLCmd *cmds, int count) {
    if (!cmds || count <= 0) return 0;
    for (int i = 0; i < count; i++) {
        if (cmds[i].type == CMD_VAR_DECLARE) return 1;
    }
    return 0;
}

int repl_range_contains_var_decl(int start, int count) {
    int n = repl_state_document_count();
    if (start < 0 || count <= 0 || start >= n) return 0;
    if (start + count > n) count = n - start;
    return repl_array_contains_var_decl(
        repl_state_document_cmds() + start, count);
}
