/*
 * src/repl/geometry_query.c - Source/flat geometry context queries.
 *
 * Cursor-context lookups over the source document and the flat program: which
 * normal/color command feeds the vertex under the cursor, which bracket
 * partner closes a push/begin, and which modelview transforms are in scope at
 * a color-consuming line. Consumed by the render3d transform guides, the edit
 * overlays, and the code panel - none of which is the autonormal pass these
 * queries used to share a file with.
 */
#include "repl/geometry_query.h"
#include "repl/state_owners.h"
#include "repl/transform_utils.h"  /* TransformScopeScan */

/* For the code panel's "feeding state" markers: given the cursor on a
 * vertex (or color-consuming glutSolid*) line, walk backward through
 * the source for the nearest normal (want_normal=1) or color
 * (want_normal=0) command of the matching family. Returns its source
 * index, or -1 when the cursor line isn't a consumer / nothing feeds
 * it. The family split is spelled out in the comment below. */
static int find_feeding_state_cmd(int line_idx, int want_normal) {
    if (line_idx < 0 || line_idx >= repl_state_document_count()) return -1;
    if (!repl_state_document_cmds()[line_idx].valid) return -1;

    /* Intentional split, not a uniform predicate: the three target
     * families consume different state-feeder commands.
     *   - gl vertices    look back for CMD_NORMAL3F + CMD_COLOR3F/4F
     *   - tess vertices  look back for CMD_TESS_NORMAL + CMD_TESS_COLOR
     *   - glutSolid*     look back for CMD_COLOR3F/4F only - they emit
     *                    their own normals but draw under the current
     *                    GL color via glColorMaterial / lighting. */
    CmdType target = repl_state_document_cmds()[line_idx].type;
    int is_gl_vtx = repl_cmd_emits_immediate_vertex(target);
    int is_tess_vtx = (target == CMD_TESS_VERTEX);
    int is_glut_solid = (!is_gl_vtx && !is_tess_vtx &&
                         repl_cmd_consumes_current_color(target));

    if (want_normal) {
        if (!is_gl_vtx && !is_tess_vtx) return -1;
    } else {
        if (!is_gl_vtx && !is_tess_vtx && !is_glut_solid) return -1;
    }

    for (int i = line_idx - 1; i >= 0; i--) {
        if (!repl_state_document_cmds()[i].valid) continue;
        CmdType t = repl_state_document_cmds()[i].type;
        if (want_normal) {
            if (is_gl_vtx && t == CMD_NORMAL3F) return i;
            if (is_tess_vtx && t == CMD_TESS_NORMAL) return i;
        } else {
            if ((is_gl_vtx || is_glut_solid) &&
                (t == CMD_COLOR3F || t == CMD_COLOR4F)) return i;
            if (is_tess_vtx && t == CMD_TESS_COLOR) return i;
        }
    }

    return -1;
}

int repl_find_feeding_normal_cmd(int line_idx) {
    return find_feeding_state_cmd(line_idx, 1);
}

int repl_find_feeding_color_cmd(int line_idx) {
    return find_feeding_state_cmd(line_idx, 0);
}

/* Source-order LIFO partner lookup shared by the matrix and attribute stack
 * bracket helpers. `open_type` pairs with `close_type`; direction -1 finds an
 * open partner for a close, and +1 finds a close partner for an open. Like the
 * callers it deliberately ignores block scope. */
static int repl_find_matching_bracket(int line_idx, CmdType open_type,
                                      CmdType close_type, int direction) {
    const GLCmd *cmds = repl_state_document_cmds();
    int n = repl_state_document_count();
    CmdType start_type = direction < 0 ? close_type : open_type;
    CmdType partner_type = direction < 0 ? open_type : close_type;
    int depth = 1;

    if (line_idx < 0 || line_idx >= n || direction == 0 ||
        !cmds[line_idx].valid || cmds[line_idx].type != start_type)
        return -1;
    for (int i = line_idx + direction; i >= 0 && i < n; i += direction) {
        CmdType type;
        if (!cmds[i].valid)
            continue;
        type = cmds[i].type;
        if (type == start_type) {
            depth++;
        } else if (type == partner_type) {
            depth--;
            if (depth == 0)
                return i;
        }
    }
    return -1;
}

int repl_find_matching_push_matrix(int line_idx) {
    return repl_find_matching_bracket(line_idx, CMD_PUSH_MATRIX, CMD_POP_MATRIX,
                                      -1);
}

int repl_find_matching_pop_matrix(int line_idx) {
    return repl_find_matching_bracket(line_idx, CMD_PUSH_MATRIX, CMD_POP_MATRIX,
                                      1);
}

/* Attribute-stack bracket matching, the exact source-order LIFO mirror of the
 * matrix pair above (same block-unaware heuristic - see the collect_unbalanced
 * LIMITATION note). Cursor on a CMD_POP_ATTRIB walks back to the nearest
 * earlier CMD_PUSH_ATTRIB at the same nesting level; cursor on a
 * CMD_PUSH_ATTRIB walks forward to the matching CMD_POP_ATTRIB. Returns -1 when
 * the cursor is not on the right bracket or no partner exists. */
int repl_find_matching_push_attrib(int line_idx) {
    return repl_find_matching_bracket(line_idx, CMD_PUSH_ATTRIB, CMD_POP_ATTRIB,
                                      -1);
}

int repl_find_matching_pop_attrib(int line_idx) {
    return repl_find_matching_bracket(line_idx, CMD_PUSH_ATTRIB, CMD_POP_ATTRIB,
                                      1);
}

/* Primitive-block (glBegin/glEnd) bracket matching, same source-order LIFO
 * heuristic again. Real GL forbids nesting, but the REPL tolerates an
 * unbalanced document, so the shared depth walk is what makes an orphan
 * glEnd pair with nothing instead of stealing an unrelated glBegin. */
int repl_find_matching_begin(int line_idx) {
    return repl_find_matching_bracket(line_idx, CMD_BEGIN, CMD_END, -1);
}

int repl_find_matching_end(int line_idx) {
    return repl_find_matching_bracket(line_idx, CMD_BEGIN, CMD_END, 1);
}

/* Walk backwards from line_idx past a CMD_FUNC_END to its matching
 * CMD_FUNC_DEF. Returns the source index of the matching CMD_FUNC_DEF
 * (or -1 if unbalanced). The caller passes the index of the FUNC_END
 * itself; the returned index points at the FUNC_DEF so the outer
 * loop's i-- moves past it. */
static int skip_function_body_backward(const GLCmd *cmds, int func_end_idx) {
    int depth = 1;
    for (int i = func_end_idx - 1; i >= 0; i--) {
        if (!cmds[i].valid) continue;
        if (cmds[i].type == CMD_FUNC_END) {
            depth++;
        } else if (cmds[i].type == CMD_FUNC_DEF) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

int repl_find_affecting_transforms(int line_idx, int *out, int out_cap) {
    if (!out || out_cap <= 0) return 0;
    int n = repl_state_document_count();
    if (line_idx < 0 || line_idx >= n) return 0;
    const GLCmd *cmds = repl_state_document_cmds();
    if (!cmds[line_idx].valid) return 0;
    if (!repl_cmd_consumes_current_color(cmds[line_idx].type)) return 0;

    int count = 0;
    int popped_depth = 0;
    int i = line_idx - 1;
    while (i >= 0 && count < out_cap) {
        if (!cmds[i].valid) { i--; continue; }
        CmdType t = cmds[i].type;

        if (t == CMD_FUNC_END) {
            /* Function bodies are opaque: their transforms only run
             * when the func is called, not at this source position.
             * Skip from FUNC_END back to the matching FUNC_DEF, then
             * continue walking the surrounding scope. */
            int def_idx = skip_function_body_backward(cmds, i);
            i = (def_idx >= 0) ? def_idx - 1 : -1;
            continue;
        }
        if (t == CMD_FUNC_DEF) {
            /* The cursor lives inside this function body; the caller's
             * matrix state is unknown to the static walk. Stop here. */
            break;
        }
        if (t == CMD_POP_MATRIX) {
            popped_depth++;
        } else if (t == CMD_PUSH_MATRIX) {
            /* A push at popped_depth>0 closes one of the popped scopes
             * we crossed walking back. A push at popped_depth==0 is the
             * cursor's own scope boundary - keep walking into the
             * parent, whose transforms still apply. */
            if (popped_depth > 0) popped_depth--;
        } else if (t == CMD_LOAD_IDENTITY) {
            if (popped_depth == 0) break;
        } else if (t == CMD_TRANSLATE3F || t == CMD_SCALEF || t == CMD_ROTATEF) {
            if (popped_depth == 0) out[count++] = i;
        }
        i--;
    }
    return count;
}

/* Append src_line to out[] if not already present and within capacity.
 * Returns the (possibly unchanged) count. Shared by the two flat-program
 * affecting-transform resolvers so a transform that appears in multiple
 * function expansions only lights its source line once. */
static int append_unique_src_line(int *out, int count, int out_cap, int src_line) {
    if (src_line < 0 || count >= out_cap) return count;
    for (int k = 0; k < count; k++)
        if (out[k] == src_line) return count;
    out[count++] = src_line;
    return count;
}

int repl_find_affecting_transforms_for_flat_vertex(int flat_idx,
                                                   int *out, int out_cap) {
    if (!out || out_cap <= 0) return 0;
    FlatProgramView flat = repl_state_flat_program_view();
    if (flat_idx < 0 || flat_idx >= flat.cmd_count) return 0;
    const GLCmd *cmds = flat.cmds;
    if (!cmds[flat_idx].valid) return 0;
    if (!repl_cmd_consumes_current_color(cmds[flat_idx].type)) return 0;

    /* The flat program has already inlined every funcN body and unrolled
     * every loop, so a straight backward walk crosses function boundaries
     * for free: call-site-scope transforms and in-body transforms both sit
     * inline ahead of the vertex. No CMD_FUNC_DEF/_END markers survive
     * flattening (flatten_range skips them), so unlike the source walk we
     * never have to stop at a function boundary or skip an opaque body.
     * The push/pop/load-identity scope accounting is the shared
     * TransformScopeScan from transform_utils.h. */
    TransformScopeScan scan;
    int count = 0;
    int idx;

    transform_scope_scan_init(&scan, cmds, flat_idx);
    while (count < out_cap && (idx = transform_scope_scan_next(&scan)) >= 0)
        count = append_unique_src_line(out, count, out_cap,
                                       cmds[idx].src_cmd_idx);
    return count;
}

int repl_find_affecting_transforms_flat(int line_idx, int *out, int out_cap) {
    if (!out || out_cap <= 0) return 0;
    int n = repl_state_document_count();
    if (line_idx < 0 || line_idx >= n) return 0;
    const GLCmd *src = repl_state_document_cmds();
    if (!src[line_idx].valid) return 0;
    if (!repl_cmd_consumes_current_color(src[line_idx].type)) return 0;

    /* Without a selected invocation the live cursor view is the union of
     * affecting transforms across every flat expansion of this source
     * vertex line. We deliberately key off src_cmd_idx == line_idx (the
     * exact owning vertex/glut-solid), NOT repl_flat_cmd_matches_cursor(),
     * which matches whole function scopes and call-site blocks and would be
     * far too broad for "this one vertex line." */
    FlatProgramView flat = repl_state_flat_program_view();
    int count = 0;
    for (int fi = 0; fi < flat.cmd_count && count < out_cap; fi++) {
        if (!flat.cmds[fi].valid) continue;
        if (flat.cmds[fi].src_cmd_idx != line_idx) continue;
        if (!repl_cmd_consumes_current_color(flat.cmds[fi].type)) continue;

        int tmp[MAX_AFFECTING_TRANSFORMS];
        int tn = repl_find_affecting_transforms_for_flat_vertex(
            fi, tmp, MAX_AFFECTING_TRANSFORMS);
        for (int k = 0; k < tn; k++)
            count = append_unique_src_line(out, count, out_cap, tmp[k]);
    }
    return count;
}
