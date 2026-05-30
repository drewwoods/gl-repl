/*
 * src/repl/source_scope.c - Prefix-depth cache and source block queries.
 *
 * These helpers answer questions about the source command array before a
 * command at a given index is parsed, formatted, or structurally edited.
 */
#include "repl/source_scope.h"
#include "repl/state.h"

/* Lightweight prefix-depth caches for O(1) depth lookups at position `pos`. */
static int g_depth_cache_dirty = 1;
static int g_block_depth_prefix[MAX_COMMANDS + 1];
static int g_begin_depth_prefix[MAX_COMMANDS + 1];
static int g_tess_depth_prefix[MAX_COMMANDS + 1];
static int g_matrix_depth_prefix[MAX_COMMANDS + 1];

void repl_source_scope_depth_cache_invalidate(void) {
    g_depth_cache_dirty = 1;
}

/* Rebuild prefix-sum depth arrays so that depth_prefix[pos] gives the
 * nesting depth *before* command `pos`.  Each array tracks one kind of
 * scope opener/closer:
 *
 *   g_block_depth_prefix  - any block (for/func/if) nesting (used for indent)
 *   g_begin_depth_prefix  - glBegin/glEnd nesting
 *   g_tess_depth_prefix   - gluBegin/gluEnd nesting
 *   g_matrix_depth_prefix - glPushMatrix/glPopMatrix nesting (indents like glBegin)
 *
 * All queries call this first; repl_source_scope_depth_cache_invalidate() marks it dirty. */
static void depth_cache_rebuild(void) {
    if (!g_depth_cache_dirty) return;

    g_block_depth_prefix[0]  = 0;
    g_begin_depth_prefix[0]  = 0;
    g_tess_depth_prefix[0]   = 0;
    g_matrix_depth_prefix[0] = 0;

    for (int i = 0; i < repl_state_document_count(); i++) {
        int block_depth  = g_block_depth_prefix[i];
        int begin_depth  = g_begin_depth_prefix[i];
        int tess_depth   = g_tess_depth_prefix[i];
        int matrix_depth = g_matrix_depth_prefix[i];

        if (repl_state_document_cmds()[i].valid) {
            CmdType t = repl_state_document_cmds()[i].type;

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

        g_block_depth_prefix[i + 1]  = block_depth;
        g_begin_depth_prefix[i + 1]  = begin_depth;
        g_tess_depth_prefix[i + 1]   = tess_depth;
        g_matrix_depth_prefix[i + 1] = matrix_depth;
    }

    g_depth_cache_dirty = 0;
}

int repl_source_scope_in_begin_block_at(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > repl_state_document_count()) pos = repl_state_document_count();
    return g_begin_depth_prefix[pos] > 0;
}

int repl_source_scope_in_begin_block(void) {
    return repl_source_scope_in_begin_block_at(repl_state_document_count());
}

int repl_source_scope_block_depth_at(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > repl_state_document_count()) pos = repl_state_document_count();
    return g_block_depth_prefix[pos];
}

int repl_source_scope_tess_scope_depth_at(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > repl_state_document_count()) pos = repl_state_document_count();
    return g_tess_depth_prefix[pos];
}

int repl_source_scope_matrix_scope_depth_at(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > repl_state_document_count()) pos = repl_state_document_count();
    return g_matrix_depth_prefix[pos];
}

/* Collect the document indices of structurally unbalanced bracket
 * commands — glPushMatrix/glBegin openers with no matching close, and
 * orphan glPopMatrix/glEnd closers with no matching open. Matching is
 * linear (a stack per bracket type), the same relaxed model the
 * indentation depth uses, so the flagged lines agree with how the body
 * is indented. Fills `out_lines` (document order for orphans, then the
 * still-open openers) and returns the count, capped at `max`.
 *
 * LIMITATION — linear, block-unaware matching. The walk pairs brackets
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
 * Off-stack static scratch (MAX_COMMANDS deep) mirrors the depth-cache
 * arrays above and keeps this O(n) single-pass helper allocation-free. */
static int g_unbal_matrix_stack[MAX_COMMANDS];
static int g_unbal_begin_stack[MAX_COMMANDS];

int repl_source_scope_collect_unbalanced(int *out_lines, int max) {
    const GLCmd *cmds = repl_state_document_cmds();
    int count = repl_state_document_count();
    int msp = 0, bsp = 0, n = 0;

    if (!out_lines || max <= 0)
        return 0;

    for (int i = 0; i < count; i++) {
        if (!cmds[i].valid)
            continue;
        switch (cmds[i].type) {
        case CMD_PUSH_MATRIX:
            if (msp < MAX_COMMANDS) g_unbal_matrix_stack[msp++] = i;
            break;
        case CMD_POP_MATRIX:
            if (msp > 0)            msp--;                       /* matched */
            else if (n < max)       out_lines[n++] = i;          /* orphan pop */
            break;
        case CMD_BEGIN:
            if (bsp < MAX_COMMANDS) g_unbal_begin_stack[bsp++] = i;
            break;
        case CMD_END:
            if (bsp > 0)            bsp--;                       /* matched */
            else if (n < max)       out_lines[n++] = i;          /* orphan end */
            break;
        default:
            break;
        }
    }

    /* Whatever is left on each stack never closed. */
    for (int k = 0; k < msp && n < max; k++) out_lines[n++] = g_unbal_matrix_stack[k];
    for (int k = 0; k < bsp && n < max; k++) out_lines[n++] = g_unbal_begin_stack[k];
    return n;
}

/* Normal command indent: 2 + 2*tess + 2*begin + 2*block + 2*matrix.
 * glPushMatrix/glPopMatrix nest, so their bodies indent one level per
 * open push (mirroring how glBegin opens a level via begin depth). */
void repl_source_scope_cmd_indent(int pos, char *buf, int buf_sz) {
    if (buf_sz <= 0) return;
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > repl_state_document_count()) pos = repl_state_document_count();
    int td = g_tess_depth_prefix[pos];
    int bd = g_begin_depth_prefix[pos];
    int kd = g_block_depth_prefix[pos];
    int md = g_matrix_depth_prefix[pos];
    int spaces = 2 + 2 * td + 2 * bd + 2 * kd + 2 * md;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

/* glBegin/glEnd-style indent: 2 + 2*tess + 2*block + 2*matrix, with
 * begin-depth deliberately EXCLUDED (the glBegin/glEnd lines are not
 * indented by the block they open). Matrix depth IS included so a
 * glBegin/glEnd nested inside a glPushMatrix block lands at the right
 * level. Uses the same public depth queries the parser previously
 * open-coded at two sites. */
void repl_source_scope_begin_indent(int pos, char *buf, int buf_sz) {
    int td = repl_source_scope_tess_scope_depth_at(pos);
    int kd = repl_source_scope_block_depth_at(pos);
    int md = repl_source_scope_matrix_scope_depth_at(pos);
    int spaces = 2 + 2 * td + 2 * kd + 2 * md;
    if (buf_sz <= 0) return;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

void repl_source_scope_tess_close_indent(int pos, char *buf, int buf_sz) {
    int td = repl_source_scope_tess_scope_depth_at(pos);
    int kd = repl_source_scope_block_depth_at(pos);
    int md = repl_source_scope_matrix_scope_depth_at(pos);
    int spaces;

    if (buf_sz <= 0) return;
    if (td > 0) td--;
    spaces = 2 + 2 * td + 2 * kd + 2 * md;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

/* glPopMatrix indent: aligns with its matching glPushMatrix by dropping
 * one matrix level (the push it closes is still counted in the prefix at
 * this position). Same shape as a normal command otherwise:
 * 2 + 2*tess + 2*begin + 2*block + 2*(matrix-1). */
void repl_source_scope_matrix_close_indent(int pos, char *buf, int buf_sz) {
    if (buf_sz <= 0) return;
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > repl_state_document_count()) pos = repl_state_document_count();
    int td = g_tess_depth_prefix[pos];
    int bd = g_begin_depth_prefix[pos];
    int kd = g_block_depth_prefix[pos];
    int md = g_matrix_depth_prefix[pos];
    if (md > 0) md--;
    int spaces = 2 + 2 * td + 2 * bd + 2 * kd + 2 * md;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

int repl_source_scope_cmd_indent_chars(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > repl_state_document_count()) pos = repl_state_document_count();
    return 2 + 2 * g_tess_depth_prefix[pos] + 2 * g_begin_depth_prefix[pos]
             + 2 * g_block_depth_prefix[pos] + 2 * g_matrix_depth_prefix[pos];
}

/* Tessellator leaf command indent: 2 + 2*tess + 2*block + 2*matrix
 * (begin depth ignored). */
void repl_source_scope_cmd_tess_indent(int pos, char *buf, int buf_sz) {
    if (buf_sz <= 0) return;
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > repl_state_document_count()) pos = repl_state_document_count();
    int td = g_tess_depth_prefix[pos];
    int kd = g_block_depth_prefix[pos];
    int md = g_matrix_depth_prefix[pos];
    int spaces = 2 + 2 * td + 2 * kd + 2 * md;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

int repl_source_scope_find_block_end(int begin_idx) {
    int depth = 1;
    for (int j = begin_idx + 1; j < repl_state_document_count(); j++) {
        CmdType t = repl_state_document_cmds()[j].type;
        if (repl_cmd_is_block_head(t)) depth++;
        else if (repl_cmd_is_block_end(t)) {
            depth--;
            if (depth == 0) return j;
        }
    }
    return repl_state_document_count();
}

CmdType repl_source_scope_nearest_open_block_at(int pos) {
    CmdType stack[REPL_MAX_BLOCK_NEST_DEPTH];
    int depth = 0;
    for (int i = 0; i < pos && i < repl_state_document_count(); i++) {
        CmdType t = repl_state_document_cmds()[i].type;
        if (repl_cmd_is_block_head(t)) {
            if (depth < REPL_MAX_BLOCK_NEST_DEPTH) stack[depth++] = t;
        } else if (repl_cmd_is_block_end(t)) {
            if (depth > 0) depth--;
        }
    }
    return depth > 0 ? stack[depth - 1] : CMD_TYPE_COUNT;
}

int repl_source_scope_block_extent(int line_idx,
                                   int *out_start, int *out_count) {
    int n = repl_state_document_count();
    if (line_idx < 0 || line_idx >= n) return 0;
    if (!repl_line_is_block_head(line_idx)) return 0;
    int end = repl_source_scope_find_block_end(line_idx);
    if (end >= n) end = n - 1;
    if (end < line_idx) return 0;
    if (out_start) *out_start = line_idx;
    if (out_count) *out_count = end - line_idx + 1;
    return 1;
}

int repl_line_is_block_head(int line_idx) {
    if (line_idx < 0 || line_idx >= repl_state_document_count()) return 0;
    return repl_cmd_is_block_head(repl_state_document_cmds()[line_idx].type);
}

int repl_line_is_label(int line_idx) {
    if (line_idx < 0 || line_idx >= repl_state_document_count()) return 0;
    return repl_state_document_cmds()[line_idx].type == CMD_GOTO_LABEL;
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
