/*
 * src/repl/autonormal.c -- Auto-generated normal commands and feeding-state lookup.
 */
#include "repl/core.h"
#include "repl/command_store.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#include "source_document.h"   /* source_document_insert_line / _replace_line */
#include "config.h"            /* REPL_INDENT_TEXT_MAX */

static void normal_indent(int pos, char *buf, int buf_sz) {
    int spaces;

    if (!buf || buf_sz <= 0)
        return;

    spaces = repl_source_scope_cmd_indent_chars(pos);
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

static void face_normal(const float *a, const float *b, const float *c,
                        float *n) {
    float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
    float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
    n[0] = e1[1]*e2[2] - e1[2]*e2[1];
    n[1] = e1[2]*e2[0] - e1[0]*e2[2];
    n[2] = e1[0]*e2[1] - e1[1]*e2[0];
    float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    else { n[0] = 0; n[1] = 0; n[2] = 0; }
}

static GLCmd make_auto_normal(float nx, float ny, float nz) {
    GLCmd c;
    memset(&c, 0, sizeof(c));
    c.type = CMD_NORMAL3F;
    c.args[0] = nx;
    c.args[1] = ny;
    c.args[2] = nz;
    c.num_args = 3;
    c.valid = 1;
    c.is_auto = 1;
    return c;
}

/* Canonical text for an auto-normal at insert_pos */
static void make_auto_normal_text(int insert_pos, float nx, float ny, float nz,
                                  char *text_out, int text_sz) {
    char ind[REPL_INDENT_TEXT_MAX];
    normal_indent(insert_pos, ind, sizeof(ind));
    snprintf(text_out, (size_t)text_sz,
             "%sglNormal3f(%g, %g, %g);", ind, nx, ny, nz);
}

static void insert_cmd_at(int pos, const GLCmd *cmd,
                           float nx, float ny, float nz,
                           int *edit_line_inout) {
    ReplCommandStore store = repl_command_store_live();
    char line[MAX_LINE_LEN];

    make_auto_normal_text(pos, nx, ny, nz, line, sizeof(line));
    /* Text first; if the cmd-store insert fails (capacity), roll the
     * text back so the auto-normal line doesn't linger without its
     * matching GLCmd. */
    if (!source_document_insert_line(pos, line))
        return;
    /* Caller-owned cursor: threaded down from
     * repl_recompute_autonormals's edit_line_inout parameter so the
     * read/write lives at the controller (β: REPL files do not call
     * editor_state_*). */
    ReplStoreMutOpts opts = {
        .flags        = REPL_COMMAND_STORE_ADJUST_EDIT_LINE,
        .cursor_inout = edit_line_inout,
    };
    if (!repl_command_store_insert_one(&store, pos, cmd, &opts)) {
        SourceTextChange rollback = {
            .kind         = SOURCE_TEXT_DELETE_RANGE,
            .pos          = pos,
            .count        = 1,
            .delete_pos   = -1,
            .delete_count = 0,
        };
        source_document_apply_change(&rollback);
    }
}

static void apply_front_face_to_normal(GLenum front_face, float *n) {
    if (front_face == GL_CW) {
        n[0] = -n[0];
        n[1] = -n[1];
        n[2] = -n[2];
    }
}

/* For each primitive in the glBegin block, compute one flat face normal
 * and copy it to every vertex of that primitive (so the whole face shades
 * flat). `vi` maps local vertex index -> source-cmd index; `norms` is the
 * parallel per-vertex output. */
static void compute_block_normals(GLenum mode, GLenum front_face,
                                  int *vi, int nv, float norms[][3]) {
    for (int idx = 0; idx < nv; idx++)
        norms[idx][0] = norms[idx][1] = norms[idx][2] = 0;

    float n[3];
    switch (mode) {
    case GL_TRIANGLES:
        for (int idx = 0; idx + 2 < nv; idx += 3) {
            face_normal(repl_state_document_cmds()[vi[idx]].args, repl_state_document_cmds()[vi[idx+1]].args,
                        repl_state_document_cmds()[vi[idx+2]].args, n);
            apply_front_face_to_normal(front_face, n);
            for (int face_vert = 0; face_vert < 3; face_vert++)
                memcpy(norms[idx+face_vert], n, sizeof(n));
        }
        break;
    case GL_TRIANGLE_STRIP:
        for (int idx = 0; idx + 2 < nv; idx++) {
            /* Strips alternate winding: every other triangle swaps its
             * 2nd/3rd vertices so all faces end up wound consistently. */
            if (idx % 2 == 0)
                face_normal(repl_state_document_cmds()[vi[idx]].args, repl_state_document_cmds()[vi[idx+1]].args,
                            repl_state_document_cmds()[vi[idx+2]].args, n);
            else
                face_normal(repl_state_document_cmds()[vi[idx]].args, repl_state_document_cmds()[vi[idx+2]].args,
                            repl_state_document_cmds()[vi[idx+1]].args, n);
            apply_front_face_to_normal(front_face, n);
            memcpy(norms[idx+2], n, sizeof(n));
            if (idx == 0) {
                memcpy(norms[0], n, sizeof(n));
                memcpy(norms[1], n, sizeof(n));
            }
        }
        break;
    case GL_TRIANGLE_FAN:
        for (int idx = 1; idx + 1 < nv; idx++) {
            face_normal(repl_state_document_cmds()[vi[0]].args, repl_state_document_cmds()[vi[idx]].args,
                        repl_state_document_cmds()[vi[idx+1]].args, n);
            apply_front_face_to_normal(front_face, n);
            memcpy(norms[idx+1], n, sizeof(n));
            if (idx == 1) {
                memcpy(norms[0], n, sizeof(n));
                memcpy(norms[1], n, sizeof(n));
            }
        }
        break;
    case GL_QUADS:
        for (int idx = 0; idx + 3 < nv; idx += 4) {
            face_normal(repl_state_document_cmds()[vi[idx]].args, repl_state_document_cmds()[vi[idx+1]].args,
                        repl_state_document_cmds()[vi[idx+2]].args, n);
            apply_front_face_to_normal(front_face, n);
            for (int face_vert = 0; face_vert < 4; face_vert++)
                memcpy(norms[idx+face_vert], n, sizeof(n));
        }
        break;
    case GL_QUAD_STRIP:
        for (int idx = 0; idx + 3 < nv; idx += 2) {
            face_normal(repl_state_document_cmds()[vi[idx]].args, repl_state_document_cmds()[vi[idx+1]].args,
                        repl_state_document_cmds()[vi[idx+2]].args, n);
            apply_front_face_to_normal(front_face, n);
            memcpy(norms[idx+2], n, sizeof(n));
            memcpy(norms[idx+3], n, sizeof(n));
            if (idx == 0) {
                memcpy(norms[0], n, sizeof(n));
                memcpy(norms[1], n, sizeof(n));
            }
        }
        break;
    case GL_POLYGON:
        if (nv >= 3) {
            face_normal(repl_state_document_cmds()[vi[0]].args, repl_state_document_cmds()[vi[1]].args,
                        repl_state_document_cmds()[vi[2]].args, n);
            apply_front_face_to_normal(front_face, n);
            for (int idx = 0; idx < nv; idx++)
                memcpy(norms[idx], n, sizeof(n));
        }
        break;
    default:
        break;
    }
}

/* The `autonormal` toggle moved out of REPL state onto `glr_state`. The
 * autonormal pass is a REPL pipeline TU and cannot include
 * `glr_state.h`, so the caller (controller / tests) gates the call
 * by passing the toggle explicitly (implemented as step 7a of
 * feature/decouple-repl-from-gl-repl-alt.md). */
void repl_recompute_autonormals(int autonormal_enabled,
                                int *edit_line_inout) {
    if (!autonormal_enabled) return;

    int i = 0;
    GLenum front_face = GL_CCW;
    while (i < repl_state_document_count()) {
        if (repl_state_document_cmds()[i].valid && repl_state_document_cmds()[i].type == CMD_FRONT_FACE) {
            front_face = (GLenum)repl_state_document_cmds()[i].args[0];
            i++;
            continue;
        }
        /* CMD_FUNC_DEF / CMD_IF_BEGIN / CMD_FOR_BEGIN markers are
         * structural — we don't auto-normal them, but we *do* enter
         * the body. A glBegin inside a funcN body should still pick
         * up an auto-normal as long as its vertices have literal
         * coords (see the has_vars guard below). The matching END
         * markers fall through to the `i++` at the bottom. */
        if (!repl_state_document_cmds()[i].valid || repl_state_document_cmds()[i].type != CMD_BEGIN) { i++; continue; }

        GLenum mode = (GLenum)repl_state_document_cmds()[i].args[0];
        i++;

        int vi[MAX_COMMANDS];
        int nv = 0;
        int any_vertex_has_vars = 0;
        int block_end = repl_state_document_count();
        for (int j = i; j < repl_state_document_count(); j++) {
            if (!repl_state_document_cmds()[j].valid) continue;
            if (repl_state_document_cmds()[j].type == CMD_END) { block_end = j; break; }
            if (repl_state_document_cmds()[j].type == CMD_BEGIN) { block_end = j; break; }
            /* CMD_TESS_VERTEX is intentionally absent: it never appears
             * inside a CMD_BEGIN block — tess vertices live between
             * CMD_TESS_BEGIN_CONTOUR / CMD_TESS_END and use their own
             * normal feeder (CMD_TESS_NORMAL). glVertex2f sets args[2]
             * to 0 (parser default), so its cross product folds into
             * the same (x, y, 0) plane as a 3D vertex with z=0. */
            if (repl_state_document_cmds()[j].type == CMD_VERTEX3F ||
                repl_state_document_cmds()[j].type == CMD_VERTEX2F) {
                vi[nv++] = j;
                if (repl_state_document_cmds()[j].has_vars)
                    any_vertex_has_vars = 1;
            }
        }

        /* Vertices with `has_vars` carry parse-time args (often the
         * predef default, 0). A cross product on those would emit a
         * degenerate (0, 0, 0) normal — strictly worse than leaving
         * the block alone and letting GL fall back to the default
         * normal or the user's manual glNormal3f. Skip the block as
         * a whole rather than mixing literal-coord and vars-bearing
         * normals from the same primitive. */
        if (any_vertex_has_vars) {
            i = block_end + 1;
            continue;
        }

        float norms[MAX_COMMANDS][3];
        compute_block_normals(mode, front_face, vi, nv, norms);

        int offset = 0;
        for (int v = 0; v < nv; v++) {
            int vidx = vi[v] + offset;
            float nx = norms[v][0], ny = norms[v][1], nz = norms[v][2];

            if (vidx > 0 && repl_state_document_cmds()[vidx - 1].valid &&
                repl_state_document_cmds()[vidx - 1].type == CMD_NORMAL3F) {
                if (repl_state_document_cmds()[vidx - 1].is_auto) {
                    ReplCommandStore store = repl_command_store_live();
                    GLCmd auto_normal = make_auto_normal(nx, ny, nz);
                    char line[MAX_LINE_LEN];

                    make_auto_normal_text(vidx - 1, nx, ny, nz, line, sizeof(line));
                    /* In-place replacement: both ops touch the same
                     * row, no order-dependent rollback to worry about.
                     * Both writes are required for consistency, so
                     * gate the cmd-store on a successful text write. */
                    if (source_document_replace_line(vidx - 1, line))
                        repl_command_store_replace_one(&store, vidx - 1,
                                                       &auto_normal);
                }
                continue;
            }

            GLCmd nc = make_auto_normal(nx, ny, nz);
            insert_cmd_at(vidx, &nc, nx, ny, nz, edit_line_inout);
            offset++;
            block_end++;
        }

        i = block_end + 1;
    }
}

static int find_feeding_state_cmd(int line_idx, int want_normal) {
    if (line_idx < 0 || line_idx >= repl_state_document_count()) return -1;
    if (!repl_state_document_cmds()[line_idx].valid) return -1;

    /* Intentional split, not a uniform predicate: the three target
     * families consume different state-feeder commands.
     *   - gl vertices    look back for CMD_NORMAL3F + CMD_COLOR3F/4F
     *   - tess vertices  look back for CMD_TESS_NORMAL + CMD_TESS_COLOR
     *   - glutSolid*     look back for CMD_COLOR3F/4F only — they emit
     *                    their own normals but draw under the current
     *                    GL color via glColorMaterial / lighting. */
    CmdType target = repl_state_document_cmds()[line_idx].type;
    int is_gl_vtx = (target == CMD_VERTEX3F || target == CMD_VERTEX2F);
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

int repl_find_matching_push_matrix(int line_idx) {
    if (line_idx < 0 || line_idx >= repl_state_document_count()) return -1;
    const GLCmd *cmds = repl_state_document_cmds();
    if (!cmds[line_idx].valid || cmds[line_idx].type != CMD_POP_MATRIX) return -1;

    int depth = 1;
    for (int i = line_idx - 1; i >= 0; i--) {
        if (!cmds[i].valid) continue;
        CmdType t = cmds[i].type;
        if (t == CMD_POP_MATRIX) {
            depth++;
        } else if (t == CMD_PUSH_MATRIX) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
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
             * cursor's own scope boundary — keep walking into the
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
