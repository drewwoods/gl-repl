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

void repl_source_scope_depth_cache_invalidate(void) {
    g_depth_cache_dirty = 1;
}

/* Rebuild prefix-sum depth arrays so that depth_prefix[pos] gives the
 * nesting depth *before* command `pos`.  Each array tracks one kind of
 * scope opener/closer:
 *
 *   g_block_depth_prefix - any block (for/func/if) nesting (used for indent)
 *   g_begin_depth_prefix - glBegin/glEnd nesting
 *   g_tess_depth_prefix  - gluBegin/gluEnd nesting
 *
 * All queries call this first; repl_source_scope_depth_cache_invalidate() marks it dirty. */
static void depth_cache_rebuild(void) {
    if (!g_depth_cache_dirty) return;

    g_block_depth_prefix[0] = 0;
    g_begin_depth_prefix[0] = 0;
    g_tess_depth_prefix[0] = 0;

    for (int i = 0; i < repl_state_document_count(); i++) {
        int block_depth = g_block_depth_prefix[i];
        int begin_depth = g_begin_depth_prefix[i];
        int tess_depth  = g_tess_depth_prefix[i];

        if (repl_state_document_cmds_mut()[i].valid) {
            CmdType t = repl_state_document_cmds_mut()[i].type;

            if (repl_cmd_is_block_head(t)) block_depth++;
            else if (repl_cmd_is_block_end(t)) block_depth--;

            if (t == CMD_BEGIN) begin_depth++;
            else if (t == CMD_END) begin_depth--;

            if (t == CMD_TESS_BEGIN_POLYGON || t == CMD_TESS_BEGIN_CONTOUR) tess_depth++;
            else if (t == CMD_TESS_END) tess_depth--;
        }

        if (block_depth < 0) block_depth = 0;
        if (begin_depth < 0) begin_depth = 0;
        if (tess_depth < 0)  tess_depth = 0;

        g_block_depth_prefix[i + 1] = block_depth;
        g_begin_depth_prefix[i + 1] = begin_depth;
        g_tess_depth_prefix[i + 1]  = tess_depth;
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

/* Normal command indent: 2 + 2*tess + 2*begin + 2*block */
void repl_source_scope_cmd_indent(int pos, char *buf, int buf_sz) {
    if (buf_sz <= 0) return;
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > repl_state_document_count()) pos = repl_state_document_count();
    int td = g_tess_depth_prefix[pos];
    int bd = g_begin_depth_prefix[pos];
    int kd = g_block_depth_prefix[pos];
    int spaces = 2 + 2 * td + 2 * bd + 2 * kd;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

/* glBegin/glEnd-style indent: 2 + 2*tess + 2*block, with begin-depth
 * deliberately EXCLUDED (the glBegin/glEnd lines are not indented by
 * the block they open). Uses the same public depth queries the parser
 * previously open-coded at two sites, so it is byte-identical to the
 * code it replaces. */
void repl_source_scope_begin_indent(int pos, char *buf, int buf_sz) {
    int td = repl_source_scope_tess_scope_depth_at(pos);
    int kd = repl_source_scope_block_depth_at(pos);
    int spaces = 2 + 2 * td + 2 * kd;
    if (buf_sz <= 0) return;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

void repl_source_scope_tess_close_indent(int pos, char *buf, int buf_sz) {
    int td = repl_source_scope_tess_scope_depth_at(pos);
    int kd = repl_source_scope_block_depth_at(pos);
    int spaces;

    if (buf_sz <= 0) return;
    if (td > 0) td--;
    spaces = 2 + 2 * td + 2 * kd;
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
             + 2 * g_block_depth_prefix[pos];
}

/* Tessellator leaf command indent: 2 + 2*tess + 2*block  (begin depth ignored) */
void repl_source_scope_cmd_tess_indent(int pos, char *buf, int buf_sz) {
    if (buf_sz <= 0) return;
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > repl_state_document_count()) pos = repl_state_document_count();
    int td = g_tess_depth_prefix[pos];
    int kd = g_block_depth_prefix[pos];
    int spaces = 2 + 2 * td + 2 * kd;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

int repl_source_scope_find_block_end(int begin_idx) {
    int depth = 1;
    for (int j = begin_idx + 1; j < repl_state_document_count(); j++) {
        CmdType t = repl_state_document_cmds_mut()[j].type;
        if (repl_cmd_is_block_head(t)) depth++;
        else if (repl_cmd_is_block_end(t)) {
            depth--;
            if (depth == 0) return j;
        }
    }
    return repl_state_document_count();
}

CmdType repl_source_scope_nearest_open_block_at(int pos) {
    CmdType stack[64];
    int depth = 0;
    for (int i = 0; i < pos && i < repl_state_document_count(); i++) {
        CmdType t = repl_state_document_cmds_mut()[i].type;
        if (repl_cmd_is_block_head(t)) {
            if (depth < 64) stack[depth++] = t;
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
