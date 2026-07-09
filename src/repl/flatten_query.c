/*
 * src/repl/flatten_query.c - Queries over the live flat command stream.
 */
#include "repl/flatten_query.h"

#include "repl/command.h"
#include "repl/state_owners.h"

#define FLAT_QUERY_FUNC_SCOPE_MASK_BITS 32
#define FLAT_QUERY_SCOPE_STACK_MAX 128

/* Find the source-level immediate-mode block that owns edit_line_idx.
 *
 * This deliberately walks the source document only. A previous version tried
 * to walk source and flat commands in lockstep, but a single source command can
 * expand to many flat commands once loops/functions are involved. Keeping this
 * as a source-only query gives us stable begin/end provenance to look up in the
 * flat stream afterward. */
static int flatten_current_begin_block_source_extent(int edit_line_idx,
                                                     int *out_begin,
                                                     int *out_end) {
    const GLCmd *document_cmds = repl_state_document_cmds();
    int document_count = repl_state_document_count();
    int open_begin = -1;

    if (out_begin) *out_begin = -1;
    if (out_end) *out_end = -1;
    if (edit_line_idx < 0 || edit_line_idx >= document_count)
        return 0;

    for (int i = 0; i < document_count; i++) {
        if (!document_cmds[i].valid)
            continue;

        if (document_cmds[i].type == CMD_BEGIN) {
            open_begin = i;
        } else if (document_cmds[i].type == CMD_END && open_begin >= 0) {
            if (open_begin <= edit_line_idx && edit_line_idx < i) {
                if (out_begin) *out_begin = open_begin;
                if (out_end) *out_end = i;
                return 1;
            }
            open_begin = -1;
        }
    }

    if (open_begin >= 0 && open_begin <= edit_line_idx) {
        if (out_begin) *out_begin = open_begin;
        if (out_end) *out_end = -1;
        return 1;
    }

    return 0;
}

/* Map a source glBegin/glEnd pair to the flat program by provenance.
 *
 * For loops there may be several flat begin/end pairs with the same source
 * indices; returning the first one preserves the long-standing "one instance"
 * cursor-block behavior. The important part is that we no longer infer the
 * flat index by counting source rows, which is what let earlier loop-expanded
 * geometry get selected for later source blocks. */
static int flatten_find_flat_begin_block_extent(int source_begin,
                                                int source_end,
                                                int *out_begin,
                                                int *out_end) {
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();
    int flat_cmd_count = repl_state_flat_program_count();

    if (out_begin) *out_begin = -1;
    if (out_end) *out_end = -1;
    if (source_begin < 0)
        return 0;

    for (int i = 0; i < flat_cmd_count; i++) {
        if (!flat_cmds[i].valid)
            continue;
        if (flat_cmds[i].type != CMD_BEGIN ||
            flat_cmds[i].src_cmd_idx != source_begin)
            continue;

        for (int j = i + 1; j < flat_cmd_count; j++) {
            if (!flat_cmds[j].valid)
                continue;
            if (flat_cmds[j].type != CMD_END)
                continue;
            if (source_end < 0 || flat_cmds[j].src_cmd_idx == source_end) {
                if (out_begin) *out_begin = i;
                if (out_end) *out_end = j;
                return 1;
            }
            break;
        }

        if (source_end < 0) {
            if (out_begin) *out_begin = i;
            if (out_end) *out_end = flat_cmd_count - 1;
            return 1;
        }
    }

    return 0;
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
    int current_block_begin = -1;
    int current_block_end = -1;
    int source_begin = -1;
    int source_end = -1;

    /* Two-step lookup:
     *   1. identify the source glBegin/glEnd block under the cursor;
     *   2. find the corresponding flat begin/end pair by src_cmd_idx.
     *
     * This avoids assuming flat/source alignment across unrolled loops and
     * inlined function bodies. */
    if (flatten_current_begin_block_source_extent(edit_line_idx,
                                                  &source_begin,
                                                  &source_end))
        flatten_find_flat_begin_block_extent(source_begin, source_end,
                                             &current_block_begin,
                                             &current_block_end);

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

/* ---- Single-polygon cursor resolution (see flatten_query.h) ---------- */

/* Vertex-ordinal range of the primitive containing (or being built at)
 * cursor ordinal `ord` under glBegin mode `mode`, over a block of
 * `count` vertices. Fixed-size modes group independently; strips and
 * fans resolve to the single segment/triangle/quad the vertex
 * completes (the first full primitive when ord sits below one window).
 * Returns 0 when the whole block is one primitive (GL_POLYGON,
 * GL_LINE_LOOP) or the mode is unknown. */
static int cursor_polygon_group_range(GLenum mode, int ord, int count,
                                      ReplCursorPolygon *out) {
    int first, last;

    switch (mode) {
    case GL_POINTS:
        first = ord; last = ord;
        break;
    case GL_LINES:
        first = (ord / 2) * 2; last = first + 1;
        break;
    case GL_TRIANGLES:
        first = (ord / 3) * 3; last = first + 2;
        break;
    case GL_QUADS:
        first = (ord / 4) * 4; last = first + 3;
        break;
    case GL_LINE_STRIP:
        /* Segment completed by the vertex. */
        first = (ord < 1) ? 0 : ord - 1; last = first + 1;
        break;
    case GL_TRIANGLE_STRIP:
        /* Triangle completed by the vertex. */
        first = (ord < 2) ? 0 : ord - 2; last = first + 2;
        break;
    case GL_TRIANGLE_FAN:
        /* Triangle completed by the vertex: {0, ord-1, ord}. */
        first = (ord < 2) ? 1 : ord - 1; last = first + 1;
        out->fan_anchor = 1;
        break;
    case GL_QUAD_STRIP:
        /* Quad k spans ordinals 2k..2k+3; pick the quad the vertex
         * completes, else the earliest quad containing it. */
        first = (ord < 2) ? 0 : ((ord - 2) / 2) * 2;
        last = first + 3;
        break;
    default:
        return 0;  /* GL_POLYGON / GL_LINE_LOOP: the block IS the primitive */
    }

    if (last > count - 1) last = count - 1;
    if (first > last) first = last;
    if (first < 0) first = 0;
    out->first = first;
    out->last = last;
    return 1;
}

ReplCursorPolygon repl_flatten_cursor_polygon(int edit_line_idx) {
    ReplCursorPolygon out = { 0, 0, 0, 0 };
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();
    int flat_cmd_count = repl_state_flat_program_count();
    int begin, end;
    int vtx_count = 0;
    int cursor_ord = -1;

    if (edit_line_idx < 0 || edit_line_idx >= repl_state_document_count())
        return out;

    /* Reuse the cached cursor-block resolution (lazy refresh, same as
     * repl_flat_cmd_matches_cursor). The cache only tracks CMD_BEGIN
     * blocks, so tess (GLU) polygons never resolve here by design. */
    if (repl_state_flat_program_current_block_source_line() != edit_line_idx)
        repl_flatten_refresh_current_block_highlight(edit_line_idx);
    begin = repl_state_flat_program_current_block_begin();
    end = repl_state_flat_program_current_block_end();

    if (begin < 0 || end <= begin || end >= flat_cmd_count)
        return out;
    if (!flat_cmds[begin].valid || flat_cmds[begin].type != CMD_BEGIN)
        return out;
    /* Cursor on the glBegin line itself: the whole block is current. */
    if (flat_cmds[begin].src_cmd_idx == edit_line_idx)
        return out;

    /* Cursor ordinal = the first vertex (in emission order) whose source
     * line is at or after the cursor; a cursor past the last vertex line
     * builds the block's final primitive. Matching in emission order
     * resolves a cursor inside an inner loop body to the first unrolled
     * iteration (first-instance semantics). */
    for (int i = begin + 1; i < end; i++) {
        if (!flat_cmds[i].valid) continue;
        if (flat_cmds[i].type != CMD_VERTEX3F &&
            flat_cmds[i].type != CMD_VERTEX2F)
            continue;
        if (cursor_ord < 0 && flat_cmds[i].src_cmd_idx >= edit_line_idx)
            cursor_ord = vtx_count;
        vtx_count++;
    }
    if (vtx_count <= 0)
        return out;
    if (cursor_ord < 0)
        cursor_ord = vtx_count - 1;

    out.valid = cursor_polygon_group_range((GLenum)flat_cmds[begin].args[0],
                                           cursor_ord, vtx_count, &out);
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
    if (current_block_line != edit_line_idx) {
        repl_flatten_refresh_current_block_highlight(edit_line_idx);
        current_block_begin = repl_state_flat_program_current_block_begin();
        current_block_end = repl_state_flat_program_current_block_end();
    }

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
