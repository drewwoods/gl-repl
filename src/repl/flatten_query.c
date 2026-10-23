/*
 * src/repl/flatten_query.c - Queries over the live flat command stream.
 */
#include "repl/flatten_query.h"

#include "repl/command.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"

#define FLAT_QUERY_FUNC_SCOPE_MASK_BITS 32
#define FLAT_QUERY_SCOPE_STACK_MAX 128

int repl_cursor_flat_cmd_matches(FlatProgramView program,
                                 const CursorBlockState *cursor, int flat_idx) {
    if (!cursor || cursor->edit_line_idx < 0 || !program.cmds ||
        flat_idx < 0 || flat_idx >= program.cmd_count) return 0;
    const GLCmd *cmd = &program.cmds[flat_idx];
    if (!cmd->valid) return 0;
    if (cmd->call_src_cmd_idx == cursor->edit_line_idx ||
        cmd->root_call_src_cmd_idx == cursor->edit_line_idx) return 1;
    if (cursor->cursor_func_scope_mask != 0 &&
        (cmd->func_scope_mask & cursor->cursor_func_scope_mask) != 0) return 1;
    if (cursor->cursor_block_begin >= 0 &&
        flat_idx >= cursor->cursor_block_begin &&
        flat_idx <= cursor->cursor_block_end) return 1;
    return cmd->src_cmd_idx == cursor->edit_line_idx;
}

int repl_cursor_source_begin_matches(const CursorBlockState *cursor,
                                     const GLCmd *head, const GLCmd *end) {
    if (!cursor || !cursor->cursor_source_block_valid || !head ||
        !head->valid || head->type != CMD_BEGIN ||
        head->src_cmd_idx != cursor->cursor_source_block_begin) return 0;
    if (end)
        return end->valid && end->type == CMD_END &&
               end->src_cmd_idx == cursor->cursor_source_block_end;
    return cursor->cursor_source_block_end >= cursor->cursor_source_block_begin;
}

int repl_cursor_source_begin_head_matches(FlatProgramView program,
                                          const CursorBlockState *cursor,
                                          int begin_idx) {
    if (!program.cmds || begin_idx < 0 || begin_idx >= program.cmd_count) return 0;
    const GLCmd *head = &program.cmds[begin_idx];
    if (!repl_cursor_source_begin_matches(cursor, head, NULL)) return 0;
    for (int i = begin_idx + 1; i < program.cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        if (!cmd->valid) continue;
        if (cmd->type == CMD_END)
            return repl_cursor_source_begin_matches(cursor, head, cmd);
        if (cmd->type == CMD_BEGIN) break;
    }
    return 1;
}

int repl_cursor_flat_block_matches(FlatProgramView program,
                                   const CursorBlockState *cursor,
                                   int begin_idx, int is_tess) {
    if (!cursor || !program.cmds || begin_idx < 0 || begin_idx >= program.cmd_count)
        return 0;
    const GLCmd *head = &program.cmds[begin_idx];
    if (!head->valid || head->type != (is_tess ? CMD_TESS_BEGIN_POLYGON : CMD_BEGIN))
        return 0;
    if (!is_tess && cursor->cursor_source_block_valid)
        return repl_cursor_source_begin_head_matches(program, cursor, begin_idx);
    if (is_tess && repl_cursor_source_tess_head_matches(cursor, head)) return 1;
    int depth = 1;
    for (int i = begin_idx; i < program.cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        if (!cmd->valid) continue;
        if (repl_cursor_flat_cmd_matches(program, cursor, i)) return 1;
        if (!is_tess && i > begin_idx && cmd->type == CMD_END) break;
        if (is_tess && i > begin_idx) {
            depth = repl_tess_advance_depth(cmd->type, depth);
            if (depth == 0) break;
        }
    }
    return 0;
}

void repl_flatten_vertex_runs_by_row(int *out, int row_count, int lo, int hi) {
    FlatProgramView program = repl_state_flat_program_view();

    if (!out || row_count <= 0)
        return;
    for (int r = 0; r < row_count; r++)
        out[r] = REPL_VERTEX_ORDINAL_NONE;
    if (lo > hi || !program.vertex_ord)
        return;
    for (int i = 0; i < program.cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        int row = cmd->src_cmd_idx;

        if (!cmd->valid || !repl_cmd_emits_vertex(cmd->type) ||
            row < lo || row > hi || row >= row_count)
            continue;
        if (program.vertex_ord[i].run >= 0)
            out[row] = program.vertex_ord[i].run;   /* a later copy wins */
    }
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

        int j;
        for (j = i + 1; j < flat_cmd_count; j++) {
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

        if (j == flat_cmd_count) {
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
    ReplCursorScope scope;

    /* Two-step lookup:
     *   1. identify the source glBegin/glEnd block under the cursor;
     *   2. find the corresponding flat begin/end pair by src_cmd_idx.
     *
     * This avoids assuming flat/source alignment across unrolled loops and
     * inlined function bodies. */
    repl_source_scope_cursor_scope(edit_line_idx, &scope);
    if (scope.begin_block.valid)
        flatten_find_flat_begin_block_extent(scope.begin_block.start,
                                             scope.begin_block.end,
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

/* Does flat command `fc` trace back to a source line in [lo, hi] -
 * directly, or via the call site / root call site of an inlined body? */
static int flat_scope_member(const GLCmd *fc, int lo, int hi) {
    return (fc->src_cmd_idx >= lo && fc->src_cmd_idx <= hi) ||
           (fc->call_src_cmd_idx >= lo && fc->call_src_cmd_idx <= hi) ||
           (fc->root_call_src_cmd_idx >= lo && fc->root_call_src_cmd_idx <= hi);
}

/* Flat commands attributable to a source range, counted for a SINGLE
 * invocation of that range rather than aggregated over the whole frame.
 *
 * A block's flat expansion is contiguous per invocation: within one pass
 * through a for/if scope every command it emits (its own iterations,
 * inlined calls, and all) stays in-scope, and the run breaks only when
 * control leaves the scope. So the FIRST maximal in-scope run is exactly
 * one invocation's cost. This is what makes nested scopes distinct: an
 * inner loop reports one enclosing-iteration's worth (e.g. sea()'s inner
 * strip loop), while the outer loop - invoked once - still reports its
 * whole run. A top-level or singly-invoked block's first run equals its
 * frame total, so those readouts are unchanged. */
static int flat_cost_count_first_run(int lo, int hi) {
    const GLCmd *flat = repl_state_flat_program_cmds();
    int n = repl_state_flat_program_count();
    int i = 0;
    int count = 0;
    while (i < n && !flat_scope_member(&flat[i], lo, hi))
        i++;
    for (; i < n && flat_scope_member(&flat[i], lo, hi); i++)
        count++;
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
        out.count = flat_cost_count_first_run(head, end);
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

/* The contour holding edit_line_idx (begin and gluEnd() lines included), as
 * the polygon-local tess ordinals of its gluVertex rows - read from the
 * program's numbering, a nested strip's rows included. */
ReplCursorPolygon repl_flatten_cursor_tess_contour(int edit_line_idx) {
    ReplCursorPolygon out = { 0, 0, 0, 0 };
    FlatProgramView program = repl_state_flat_program_view();
    ReplCursorScope scope;
    int depth = 0;
    int in_contour = 0;
    int first = -1;
    int last = -1;

    if (edit_line_idx < 0 || edit_line_idx >= repl_state_document_count())
        return out;
    repl_source_scope_cursor_scope(edit_line_idx, &scope);
    if (!scope.tess_contour.valid)
        return out;

    for (int i = 0; i < program.cmd_count; i++) {
        CmdType t;
        int prev;

        if (!program.cmds[i].valid)
            continue;
        t = program.cmds[i].type;
        if (t == CMD_TESS_BEGIN_POLYGON && in_contour)
            break;  /* unterminated contour: its polygon ended here */
        prev = depth;
        depth = repl_tess_advance_depth(t, depth);
        if (prev == 1 && depth == 2 &&
            program.cmds[i].src_cmd_idx == scope.tess_contour.start) {
            in_contour = 1;
        } else if (prev == 2 && depth < 2 && in_contour) {
            break;
        }
        if (in_contour && t == CMD_TESS_VERTEX) {
            int ord = repl_vertex_ordinal_at(program.vertex_ord,
                                             program.cmd_count, i).tess;
            if (ord >= 0) {
                if (first < 0)
                    first = ord;
                last = ord;
            }
        }
    }

    if (first < 0)
        return out;  /* contour not in the flat program, or vertex-less */
    out.valid = 1;
    out.first = first;
    out.last = last;
    return out;
}

ReplCursorPolygon repl_flatten_cursor_polygon(int edit_line_idx) {
    ReplCursorPolygon out = { 0, 0, 0, 0 };
    FlatProgramView program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = program.cmds;
    int flat_cmd_count = program.cmd_count;
    int begin, end;
    int block_vertices = 0;
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
     * resolves a cursor inside an inner loop body against the cached
     * (first) unrolled iteration; the ordinals are the program's glVertex-only
     * block-local numbering, so they address the same primitive in every
     * copy. */
    for (int i = begin + 1; i < end; i++) {
        int ord;

        if (!flat_cmds[i].valid) continue;
        if (!repl_cmd_emits_immediate_vertex(flat_cmds[i].type))
            continue;
        ord = repl_vertex_ordinal_at(program.vertex_ord, flat_cmd_count, i).imm;
        if (ord < 0)
            continue;
        if (cursor_ord < 0 && flat_cmds[i].src_cmd_idx >= edit_line_idx)
            cursor_ord = ord;
        block_vertices = ord + 1;
    }
    if (block_vertices <= 0)
        return out;
    if (cursor_ord < 0)
        cursor_ord = block_vertices - 1;

    out.valid = cursor_polygon_group_range((GLenum)flat_cmds[begin].args[0],
                                           cursor_ord, block_vertices, &out);
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
        if (repl_cmd_emits_immediate_vertex(cmd->type)) {
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
        if (repl_cmd_emits_immediate_vertex(cmd->type)) {
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
