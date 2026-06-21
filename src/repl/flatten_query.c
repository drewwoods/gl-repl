/*
 * src/repl/flatten_query.c - Queries over the live flat command stream.
 */
#include "repl/flatten_query.h"

#include "repl/command.h"
#include "repl/state_owners.h"

#define FLAT_QUERY_FUNC_SCOPE_MASK_BITS 32
#define FLAT_QUERY_SCOPE_STACK_MAX 128

static int flatten_source_cmd_is_flat_omitted(CmdType type) {
    return repl_cmd_is_block_head(type) ||
           repl_cmd_is_block_end(type) ||
           type == CMD_CALL ||
           type == CMD_COMMENT ||
           type == CMD_EMPTY ||
           type == CMD_VAR_DECLARE;
}

/* Determine which flat-command range corresponds to the innermost
 * glBegin/glEnd block containing edit_line_idx. The result is stored
 * in the flat-program current-block fields and used by
 * repl_flat_cmd_matches_cursor() to highlight the active geometry
 * batch in the 3D view.
 *
 * edit_line_idx is supplied by the caller so this REPL pipeline helper does
 * not need to reach into editor state. */
void repl_flatten_refresh_current_block_highlight(int edit_line_idx) {
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();
    int flat_cmd_count = repl_state_flat_program_count();
    int current_block_begin = -1;
    int current_block_end = -1;

    /* Scan repl_state_document_cmds() alongside g_flat_cmds to find the
     * innermost BEGIN/END block (in flat-cmd indices) that contains
     * edit_line_idx in source-cmd space. Skips structural and
     * document-only rows that don't appear in the flat stream. */
    {
        int begin_src = -1, begin_flat = -1;
        int fcur = 0;
        const GLCmd *document_cmds = repl_state_document_cmds();
        for (int ci = 0; ci < repl_state_document_count() && fcur < flat_cmd_count; ci++) {
            if (!document_cmds[ci].valid) continue;
            CmdType ct = document_cmds[ci].type;
            if (flatten_source_cmd_is_flat_omitted(ct))
                continue;
            while (fcur < flat_cmd_count && !flat_cmds[fcur].valid) fcur++;
            if (fcur >= flat_cmd_count) break;
            if (document_cmds[ci].type == CMD_BEGIN) {
                if (ci <= edit_line_idx) { begin_src = ci; begin_flat = fcur; }
            } else if (document_cmds[ci].type == CMD_END) {
                if (begin_src >= 0 && ci > edit_line_idx) {
                    current_block_begin = begin_flat;
                    current_block_end = fcur;
                    break;
                } else if (begin_src >= 0 && ci <= edit_line_idx) {
                    begin_src = -1; begin_flat = -1;
                }
            }
            fcur++;
        }
    }

    repl_state_flat_program_set_current_block(current_block_begin,
                                              current_block_end,
                                              edit_line_idx);
}

/* ---- Flat-cost attribution (see flatten_query.h) -------------------
 *
 * All counters walk the live flat program once and count each flat
 * command at most once, so a sum over disjoint targets can never
 * exceed the flat total. */

static int flat_cost_count_func_mask(unsigned int bit) {
    const GLCmd *flat = repl_state_flat_program_cmds();
    int n = repl_state_flat_program_count();
    int count = 0;
    for (int i = 0; i < n; i++)
        if (flat[i].func_scope_mask & bit) count++;
    return count;
}

static int flat_cost_count_call_site(int line_idx) {
    const GLCmd *flat = repl_state_flat_program_cmds();
    int n = repl_state_flat_program_count();
    int count = 0;
    for (int i = 0; i < n; i++)
        if (flat[i].call_src_cmd_idx == line_idx ||
            flat[i].root_call_src_cmd_idx == line_idx) count++;
    return count;
}

static int flat_cost_count_range(int lo, int hi) {
    const GLCmd *flat = repl_state_flat_program_cmds();
    int n = repl_state_flat_program_count();
    int count = 0;
    for (int i = 0; i < n; i++) {
        const GLCmd *fc = &flat[i];
        if ((fc->src_cmd_idx >= lo && fc->src_cmd_idx <= hi) ||
            (fc->call_src_cmd_idx >= lo && fc->call_src_cmd_idx <= hi) ||
            (fc->root_call_src_cmd_idx >= lo && fc->root_call_src_cmd_idx <= hi))
            count++;
    }
    return count;
}

/* Innermost for/func/if block whose body contains line_idx; a block-end
 * line attributes to the block it closes. Returns the head's source
 * index, or -1 when line_idx sits at top level. */
static int flat_cost_enclosing_block_head(const GLCmd *doc, int count,
                                          int line_idx) {
    int stack[FLAT_QUERY_SCOPE_STACK_MAX];
    int sp = 0;
    int limit = (line_idx < count &&
                 repl_cmd_is_block_end(doc[line_idx].type))
                ? line_idx + 1 : line_idx;
    for (int i = 0; i < limit; i++) {
        CmdType t = doc[i].type;
        if (repl_cmd_is_block_head(t)) {
            if (sp < (int)(sizeof(stack) / sizeof(stack[0])))
                stack[sp] = i;
            sp++;
        } else if (repl_cmd_is_block_end(t)) {
            /* The end line itself attributes to the block it closes:
             * with limit == line_idx + 1 this pop runs AT line_idx, so
             * report the head being popped instead of popping past it. */
            if (i == line_idx)
                break;
            if (sp > 0) sp--;
        }
    }
    if (sp <= 0) return -1;
    if (sp > (int)(sizeof(stack) / sizeof(stack[0])))
        sp = (int)(sizeof(stack) / sizeof(stack[0]));
    return stack[sp - 1];
}

ReplFlatCost repl_flatten_cost_at_line(int line_idx) {
    ReplFlatCost out = { REPL_FLAT_COST_NONE, 0 };
    const GLCmd *doc = repl_state_document_cmds();
    int doc_count = repl_state_document_count();
    if (line_idx < 0 || line_idx >= doc_count)
        return out;

    const GLCmd *cmd = &doc[line_idx];

    /* Cursor directly on a call site: that call's inclusive expansion
     * (wins over the enclosing scope so hovering a call answers "what
     * does THIS call cost"). */
    if (cmd->valid && cmd->type == CMD_CALL) {
        out.kind = REPL_FLAT_COST_CALL;
        out.count = flat_cost_count_call_site(line_idx);
        return out;
    }

    int head = (cmd->valid && repl_cmd_is_block_head(cmd->type))
               ? line_idx
               : flat_cost_enclosing_block_head(doc, doc_count, line_idx);

    if (head < 0) {
        /* Top level, no scope: the line's own emissions. */
        if (!cmd->valid)
            return out;
        out.count = 0;
        {
            const GLCmd *flat = repl_state_flat_program_cmds();
            int n = repl_state_flat_program_count();
            for (int i = 0; i < n; i++)
                if (flat[i].src_cmd_idx == line_idx) out.count++;
        }
        out.kind = out.count > 0 ? REPL_FLAT_COST_LINE : REPL_FLAT_COST_NONE;
        return out;
    }

    if (doc[head].type == CMD_FUNC_DEF) {
        int slot = (int)doc[head].args[0];
        if (slot >= 0 && slot < FLAT_QUERY_FUNC_SCOPE_MASK_BITS) {
            out.kind = REPL_FLAT_COST_FUNC;
            out.count = flat_cost_count_func_mask(1u << slot);
            return out;
        }
        /* Unrepresentable slot: fall through to the range count. */
    }

    {
        int end = line_idx;
        /* Scan for the matching end the same way flatten does
         * (unterminated blocks run to the document end). */
        int depth = 1;
        for (end = head + 1; end < doc_count; end++) {
            CmdType t = doc[end].type;
            if (repl_cmd_is_block_head(t)) depth++;
            else if (repl_cmd_is_block_end(t) && --depth == 0) break;
        }
        if (end >= doc_count) end = doc_count - 1;
        out.kind = (doc[head].type == CMD_FUNC_DEF)
                   ? REPL_FLAT_COST_FUNC : REPL_FLAT_COST_BLOCK;
        out.count = flat_cost_count_range(head, end);
    }
    return out;
}

static unsigned int line_func_scope_mask(int line) {
    unsigned int mask = 0;
    int stack[FLAT_QUERY_FUNC_SCOPE_MASK_BITS];
    int depth = 0;
    const GLCmd *document_cmds = repl_state_document_cmds();

    if (line < 0) return 0;
    if (line >= repl_state_document_count()) line = repl_state_document_count() - 1;

    for (int i = 0; i <= line && i < repl_state_document_count(); i++) {
        if (!document_cmds[i].valid) continue;

        if (document_cmds[i].type == CMD_FUNC_DEF) {
            int fn = (int)document_cmds[i].args[0];
            if (fn >= 0 && fn < FLAT_QUERY_FUNC_SCOPE_MASK_BITS &&
                depth < (int)(sizeof(stack) / sizeof(stack[0]))) {
                stack[depth++] = fn;
                mask |= (1u << fn);
            }
            continue;
        }

        if (document_cmds[i].type == CMD_FUNC_END) {
            if (i == line)
                return mask;
            if (depth > 0) {
                int fn = stack[--depth];
                mask &= ~(1u << fn);
            }
        }
    }

    return mask;
}

/* The cursor-highlight predicate: does flat command `flat_idx` belong
 * to the highlight set of the source line under the cursor? Match
 * rules, in precedence order:
 *   1. cursor on a funcN(...) call -> flat cmds whose immediate or
 *      root call site is that line
 *   2. cursor inside a funcN body -> flat cmds sharing that func scope
 *   3. cursor on a block head -> the cached current-block flat range
 *      (refreshed lazily when the cursor line changed)
 *   4. cursor on a top-level color/normal state line (immediate or
 *      tess flavor) -> the vertices it feeds, i.e. vertices whose most
 *      recent matching state cmd traces back to this source line.
 *      Immediate and tess vertices intentionally pair only with their
 *      own state-command families.
 * edit_line_idx is supplied by the caller so this REPL pipeline helper does
 * not need to reach into editor state. */
int repl_flat_cmd_matches_cursor(int flat_idx, int edit_line_idx) {
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();
    int flat_cmd_count = repl_state_flat_program_count();
    int current_block_begin = repl_state_flat_program_current_block_begin();
    int current_block_end = repl_state_flat_program_current_block_end();
    int current_block_line = repl_state_flat_program_current_block_source_line();

    if (flat_idx < 0 || flat_idx >= flat_cmd_count) return 0;
    if (edit_line_idx < 0 || edit_line_idx >= repl_state_document_count()) return 0;
    if (!flat_cmds[flat_idx].valid) return 0;
    if (current_block_line != edit_line_idx)
        repl_flatten_refresh_current_block_highlight(edit_line_idx);

    const GLCmd *cmd = &flat_cmds[flat_idx];
    const GLCmd *cursor_cmd = &repl_state_document_cmds()[edit_line_idx];

    if (cursor_cmd->valid && cursor_cmd->type == CMD_CALL) {
        return cmd->call_src_cmd_idx == edit_line_idx ||
               cmd->root_call_src_cmd_idx == edit_line_idx;
    }

    {
        unsigned int cursor_func_mask = line_func_scope_mask(edit_line_idx);
        if (cursor_func_mask != 0)
            return (cmd->func_scope_mask & cursor_func_mask) != 0;
    }

    if (current_block_begin >= 0 && current_block_end >= current_block_begin)
        return flat_idx >= current_block_begin && flat_idx <= current_block_end;

    /* Top-level color/normal commands outside glBegin/glEnd still affect later
     * vertices. Match those vertices to the most recent applicable state line
     * so block highlighting also works when the state is set before the block. */
    switch (cursor_cmd->type) {
    case CMD_COLOR3F:
    case CMD_COLOR4F: {
        /* glColor3f / glColor4f feed glVertex3f and glVertex2f. Tess
         * vertices have their own state line (CMD_TESS_COLOR) handled
         * by a sibling case below, so CMD_TESS_VERTEX is intentionally
         * excluded here. */
        if (cmd->type == CMD_VERTEX3F || cmd->type == CMD_VERTEX2F) {
            int last_color_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!flat_cmds[i].valid) continue;
                if (flat_cmds[i].type == CMD_COLOR3F ||
                    flat_cmds[i].type == CMD_COLOR4F)
                    last_color_src = flat_cmds[i].src_cmd_idx;
            }
            if (last_color_src == edit_line_idx)
                return 1;
        }
        break;
    }
    case CMD_NORMAL3F: {
        /* glNormal3f feeds glVertex3f and glVertex2f. Tess vertices
         * have their own state line (CMD_TESS_NORMAL) handled by a
         * sibling case below, so CMD_TESS_VERTEX is intentionally
         * excluded here. */
        if (cmd->type == CMD_VERTEX3F || cmd->type == CMD_VERTEX2F) {
            int last_normal_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!flat_cmds[i].valid) continue;
                if (flat_cmds[i].type == CMD_NORMAL3F)
                    last_normal_src = flat_cmds[i].src_cmd_idx;
            }
            if (last_normal_src == edit_line_idx)
                return 1;
        }
        break;
    }
    case CMD_TESS_COLOR: {
        /* CMD_TESS_COLOR feeds tess vertices only; immediate-mode
         * vertices (CMD_VERTEX3F / CMD_VERTEX2F) are fed by glColor3f
         * / glColor4f handled in the COLOR3F / COLOR4F cases above. */
        if (cmd->type == CMD_TESS_VERTEX) {
            int last_tess_color_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!flat_cmds[i].valid) continue;
                if (flat_cmds[i].type == CMD_TESS_COLOR)
                    last_tess_color_src = flat_cmds[i].src_cmd_idx;
            }
            if (last_tess_color_src == edit_line_idx)
                return 1;
        }
        break;
    }
    case CMD_TESS_NORMAL: {
        /* CMD_TESS_NORMAL feeds tess vertices only; immediate-mode
         * vertices are fed by glNormal3f handled in the NORMAL3F case
         * above. */
        if (cmd->type == CMD_TESS_VERTEX) {
            int last_tess_normal_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!flat_cmds[i].valid) continue;
                if (flat_cmds[i].type == CMD_TESS_NORMAL)
                    last_tess_normal_src = flat_cmds[i].src_cmd_idx;
            }
            if (last_tess_normal_src == edit_line_idx)
                return 1;
        }
        break;
    }
    default:
        break;
    }

    return cmd->src_cmd_idx == edit_line_idx;
}
