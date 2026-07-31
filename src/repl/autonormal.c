/*
 * src/repl/autonormal.c -- Auto-generated normal commands and feeding-state lookup.
 */
#include <stdio.h>
#include <string.h>
#include "repl/flatten.h"
#include "repl/pipeline.h"
#include "repl/command_store.h"
#include "repl/geometry_query.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#include "repl/transform_utils.h"  /* TransformScopeScan */
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

/* Unnormalized face normal: the edge cross product, whose magnitude is
 * twice the triangle's area. The smooth pass accumulates these raw so
 * larger faces weigh proportionally more at a shared vertex. */
static void face_normal_raw(const float *a, const float *b, const float *c,
                            float *n) {
    float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
    float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
    n[0] = e1[1]*e2[2] - e1[2]*e2[1];
    n[1] = e1[2]*e2[0] - e1[0]*e2[2];
    n[2] = e1[0]*e2[1] - e1[1]*e2[0];
}

static void normalize_or_zero(float *n) {
    float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    else { n[0] = 0; n[1] = 0; n[2] = 0; }
}

static void face_normal(const float *a, const float *b, const float *c,
                        float *n) {
    face_normal_raw(a, b, c, n);
    normalize_or_zero(n);
}

/* `type` is CMD_NORMAL3F for immediate-mode geometry and CMD_TESS_NORMAL
 * for a tessellator contour — the two families are otherwise identical
 * here, and the executor consumes each as the current normal for the
 * vertices that follow. */
static GLCmd make_auto_normal(CmdType type, float nx, float ny, float nz) {
    GLCmd c;
    memset(&c, 0, sizeof(c));
    c.type = type;
    c.args[0] = nx;
    c.args[1] = ny;
    c.args[2] = nz;
    c.num_args = 3;
    c.valid = 1;
    c.is_auto = 1;
    return c;
}

/* Canonical text for an auto-normal at insert_pos */
static void make_auto_normal_text(CmdType type, int insert_pos,
                                  float nx, float ny, float nz,
                                  char *text_out, int text_sz) {
    char ind[REPL_INDENT_TEXT_MAX];
    normal_indent(insert_pos, ind, sizeof(ind));
    snprintf(text_out, (size_t)text_sz,
             (type == CMD_TESS_NORMAL) ? "%sgluNormal(%g, %g, %g);"
                                       : "%sglNormal3f(%g, %g, %g);",
             ind, nx, ny, nz);
}

static void insert_cmd_at(int pos, const GLCmd *cmd,
                           float nx, float ny, float nz,
                           int *edit_line_inout) {
    ReplCommandStore store = repl_command_store_live();
    char line[MAX_LINE_LEN];

    make_auto_normal_text(cmd->type, pos, nx, ny, nz, line, sizeof(line));
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

/* Drop a generated row the dedup pass has made redundant. The mirror of
 * insert_cmd_at, with the order reversed: the store delete is attempted
 * first because it is the only half that can refuse (a bad range leaves
 * the store untouched), so the text delete only runs once the row is
 * known to be gone. Capacity cannot fail a delete, so there is no
 * rollback counterpart. Returns 1 when the row was removed. */
static int delete_cmd_at(int pos, int *edit_line_inout) {
    ReplCommandStore store = repl_command_store_live();
    ReplStoreMutOpts opts = {
        .flags        = 0,   /* delete gates on cursor_inout, not flags */
        .cursor_inout = edit_line_inout,
    };
    SourceTextChange change = {
        .kind         = SOURCE_TEXT_DELETE_RANGE,
        .pos          = pos,
        .count        = 1,
        .delete_pos   = -1,
        .delete_count = 0,
    };

    if (!repl_command_store_delete_range(&store, pos, 1, &opts))
        return 0;
    source_document_apply_change(&change);
    return 1;
}

/* Two generated normals are interchangeable when they agree to within a
 * hair. These are computed cross products rather than source literals —
 * the exact-match rule the smooth pass uses for *positions* would leave
 * coplanar faces looking distinct over rounding — so the comparison is
 * epsilon-based. */
#define AUTONORMAL_SAME_EPS 1e-6f

static int normals_match(const float *a, const float *b) {
    return fabsf(a[0] - b[0]) <= AUTONORMAL_SAME_EPS &&
           fabsf(a[1] - b[1]) <= AUTONORMAL_SAME_EPS &&
           fabsf(a[2] - b[2]) <= AUTONORMAL_SAME_EPS;
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

/* Accumulate the face wound (vi[a], vi[b], vi[c]) onto every local vertex
 * listed in `verts`. The winding triple and the receiving set are separate
 * arguments because a strip/fan face is wound from three specific corners
 * but deposits on all of its vertices (four, for the quad primitives). */
static void accum_face(GLenum front_face, const int *vi, float norms[][3],
                       int a, int b, int c, const int *verts, int nverts) {
    const GLCmd *cmds = repl_state_document_cmds();
    float n[3];

    face_normal_raw(cmds[vi[a]].args, cmds[vi[b]].args, cmds[vi[c]].args, n);
    apply_front_face_to_normal(front_face, n);
    for (int k = 0; k < nverts; k++) {
        norms[verts[k]][0] += n[0];
        norms[verts[k]][1] += n[1];
        norms[verts[k]][2] += n[2];
    }
}

/* Positions weld on an exact match, never a tolerance. Both sides come
 * from parsed source literals, so the same corner written the same way
 * parses to identical bits, and matching on bits keeps the weld a single
 * hash pass — a tolerance would force an O(nv^2) sweep, which does not
 * survive a large unrolled mesh. A vertex that matches nothing simply
 * keeps its own face normal, i.e. a break in the geometry falls back to
 * flat shading exactly where the break is. */
static unsigned pos_key_bits(float v) {
    unsigned bits;
    memcpy(&bits, &v, sizeof(bits));
    /* -0.0f and 0.0f compare equal but encode differently: one key. */
    if ((bits & 0x7fffffffu) == 0) bits = 0;
    return bits;
}

static unsigned pos_hash(const float *p) {
    unsigned h = 2166136261u;   /* FNV-1a over the three canonical keys */
    for (int k = 0; k < 3; k++) {
        h ^= pos_key_bits(p[k]);
        h *= 16777619u;
    }
    return h;
}

static int pos_equal_exact(const float *a, const float *b) {
    return pos_key_bits(a[0]) == pos_key_bits(b[0]) &&
           pos_key_bits(a[1]) == pos_key_bits(b[1]) &&
           pos_key_bits(a[2]) == pos_key_bits(b[2]);
}

/* The two arrays behind the weld. They do different jobs and are indexed
 * differently — the distinction is the whole trick, so spelling it out:
 *
 *   g_weld_slots[]  is the hash table. Indexed by pos_hash() % cap with
 *                   linear probing, each slot holds a *vertex index*
 *                   meaning "the first vertex seen at this position". No
 *                   key is ever copied into it: on a probe collision
 *                   pos_equal_exact() re-reads the stored vertex's args
 *                   straight out of the document, so a hash collision
 *                   costs an extra probe and can never mis-weld. Sized
 *                   2 * MAX_EDITOR_COMMANDS so the load factor stays at
 *                   or below 0.5 even for a block that uses every command
 *                   slot in the document.
 *
 *   g_weld_rep[]    is the result, indexed by vertex: g_weld_rep[i] is the
 *                   vertex that i welds onto (itself when i is the first
 *                   at its position). The table above is scratch that
 *                   exists only to fill this in; the accumulate and
 *                   write-back passes read nothing else.
 *
 * Both are indexed by *local* vertex index (0..nv-1 within the block), not
 * by document command index — vi[] is the indirection to the document row.
 * That is also why MAX_EDITOR_COMMANDS bounds g_weld_rep: a block cannot
 * hold more vertices than the document holds commands.
 *
 * The representative chain is one level deep by construction. A vertex
 * either claims an empty slot (and becomes its own rep) or finds an
 * occupied one whose stored index is already a rep, so there is no
 * union-find path to compress.
 *
 * File-static rather than automatic: both are sized off
 * MAX_EDITOR_COMMANDS, which a custom build can raise far enough to blow
 * the stack. Only the leading 2*nv slots are used and cleared per block,
 * so a document of many small blocks pays for its blocks, not for the
 * command cap. */
static int g_weld_slots[2 * MAX_EDITOR_COMMANDS];
static int g_weld_rep[MAX_EDITOR_COMMANDS];

/* Join vertices that occupy the same position so a corner spelled as
 * several separate glVertex3f lines (GL_TRIANGLES, GL_QUADS) shades as one
 * — strips and fans already share by index, and accumulate for free.
 * Each vertex accumulates into the first vertex at its position, then
 * reads the total back. */
static void weld_coincident(const int *vi, int nv, float norms[][3]) {
    const GLCmd *cmds = repl_state_document_cmds();
    unsigned cap = (unsigned)nv * 2u;   /* load factor 0.5 by construction */

    if (nv <= 1) return;
    for (unsigned s = 0; s < cap; s++)
        g_weld_slots[s] = -1;

    for (int i = 0; i < nv; i++) {
        unsigned slot = pos_hash(cmds[vi[i]].args) % cap;
        for (;;) {
            int held = g_weld_slots[slot];
            if (held < 0) {
                g_weld_slots[slot] = i;
                g_weld_rep[i] = i;
                break;
            }
            if (pos_equal_exact(cmds[vi[held]].args, cmds[vi[i]].args)) {
                g_weld_rep[i] = held;
                break;
            }
            if (++slot == cap) slot = 0;
        }
    }

    for (int i = 0; i < nv; i++) {
        if (g_weld_rep[i] == i) continue;
        for (int k = 0; k < 3; k++) {
            norms[g_weld_rep[i]][k] += norms[i][k];
            norms[i][k] = 0;
        }
    }
    for (int i = 0; i < nv; i++)
        if (g_weld_rep[i] != i)
            memcpy(norms[i], norms[g_weld_rep[i]], sizeof(norms[i]));
}

/* Smooth counterpart of compute_block_normals: every face deposits its
 * area-weighted normal on each of its own vertices, coincident positions
 * are welded, and the sums are normalized. Kept separate from the face
 * walk rather than folded into it — the face pass writes one normal per
 * face and leans on primitive-specific "which vertex owns this face"
 * rules that have no meaning once a vertex can hold several faces. */
static void compute_block_normals_smooth(GLenum mode, GLenum front_face,
                                         int *vi, int nv, float norms[][3]) {
    for (int idx = 0; idx < nv; idx++)
        norms[idx][0] = norms[idx][1] = norms[idx][2] = 0;

    switch (mode) {
    case GL_TRIANGLES:
        for (int idx = 0; idx + 2 < nv; idx += 3) {
            int f[3] = { idx, idx+1, idx+2 };
            accum_face(front_face, vi, norms, idx, idx+1, idx+2, f, 3);
        }
        break;
    case GL_TRIANGLE_STRIP:
        for (int idx = 0; idx + 2 < nv; idx++) {
            int f[3] = { idx, idx+1, idx+2 };
            /* Same alternating winding correction as the face pass. */
            if (idx % 2 == 0)
                accum_face(front_face, vi, norms, idx, idx+1, idx+2, f, 3);
            else
                accum_face(front_face, vi, norms, idx, idx+2, idx+1, f, 3);
        }
        break;
    case GL_TRIANGLE_FAN:
        for (int idx = 1; idx + 1 < nv; idx++) {
            int f[3] = { 0, idx, idx+1 };
            accum_face(front_face, vi, norms, 0, idx, idx+1, f, 3);
        }
        break;
    case GL_QUADS:
        for (int idx = 0; idx + 3 < nv; idx += 4) {
            int f[4] = { idx, idx+1, idx+2, idx+3 };
            accum_face(front_face, vi, norms, idx, idx+1, idx+2, f, 4);
        }
        break;
    case GL_QUAD_STRIP:
        for (int idx = 0; idx + 3 < nv; idx += 2) {
            int f[4] = { idx, idx+1, idx+2, idx+3 };
            accum_face(front_face, vi, norms, idx, idx+1, idx+2, f, 4);
        }
        break;
    case GL_POLYGON:
        if (nv >= 3) {
            /* One planar face: wind it from the first three vertices and
             * give every vertex the same normal, exactly as the face pass
             * does. Nothing to average. */
            for (int idx = 0; idx < nv; idx++) {
                int one = idx;
                accum_face(front_face, vi, norms, 0, 1, 2, &one, 1);
            }
        }
        break;
    default:
        break;
    }

    weld_coincident(vi, nv, norms);
    for (int idx = 0; idx < nv; idx++)
        normalize_or_zero(norms[idx]);
}

/* ------------------------------------------------------------------ */
/* Tessellator contours (gluBegin(GLU_CONTOUR) / gluVertex / gluEnd)    */
/* ------------------------------------------------------------------ */

/* Newell's method over the whole contour rather than a cross product of
 * the first three vertices: a GLU contour is an arbitrary polygon, so its
 * leading vertices may be collinear (which a cross product answers with
 * (0,0,0)) or locally concave (which answers with a flipped normal).
 * Summing every edge's contribution is immune to both and costs one pass.
 * The immediate-mode GL_POLYGON case deliberately keeps its first-three
 * cross product — changing it would move existing scenes' normals. */
static void contour_normal_newell(const int *vi, int nv, float *n) {
    const GLCmd *cmds = repl_state_document_cmds();

    n[0] = n[1] = n[2] = 0.0f;
    for (int i = 0; i < nv; i++) {
        const float *a = cmds[vi[i]].args;
        const float *b = cmds[vi[(i + 1) % nv]].args;
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    normalize_or_zero(n);
}

/* One auto `gluNormal` per contour, never one per vertex. The GLU
 * tessellator re-triangulates the contour into faces that have no 1:1
 * correspondence with the gluVertex rows you wrote, so a per-vertex normal
 * would be a claim the geometry cannot honor; the contour is the unit that
 * means something. That is also why Smooth mode routes here unchanged —
 * a contour is planar by construction, so averaging within it just returns
 * the contour normal. (Averaging *across* contours that share exact
 * positions would be a real feature; it is deliberately not this one.)
 *
 * Returns the index one past the contour, for the caller's walk.
 */
static int autonormal_tess_contour(int begin_idx, GLenum front_face,
                                   int *edit_line_inout) {
    const GLCmd *cmds = repl_state_document_cmds();
    int vi[MAX_EDITOR_COMMANDS];
    int nv = 0;
    int any_vertex_has_vars = 0;
    int existing_normal = -1;
    int contour_end = repl_state_document_count();
    float n[3];

    for (int j = begin_idx + 1; j < repl_state_document_count(); j++) {
        if (!cmds[j].valid) continue;
        if (cmds[j].type == CMD_TESS_END ||
            cmds[j].type == CMD_TESS_BEGIN_CONTOUR ||
            cmds[j].type == CMD_TESS_BEGIN_POLYGON) {
            contour_end = j;
            break;
        }
        if (cmds[j].type == CMD_TESS_NORMAL) {
            if (existing_normal < 0) existing_normal = j;
            continue;
        }
        if (cmds[j].type == CMD_TESS_VERTEX) {
            vi[nv++] = j;
            if (cmds[j].has_vars) any_vertex_has_vars = 1;
        }
    }

    /* Same bail-outs as the immediate-mode walk: vars-bearing coordinates
     * carry parse-time values, not evaluated ones, and a hand-written
     * gluNormal owns its contour outright — anywhere in the contour, not
     * just at the top, because the contour is the unit here. */
    if (nv < 3 || any_vertex_has_vars)
        return contour_end + 1;
    if (existing_normal >= 0 && !cmds[existing_normal].is_auto)
        return contour_end + 1;

    contour_normal_newell(vi, nv, n);
    apply_front_face_to_normal(front_face, n);

    if (existing_normal >= 0) {
        ReplCommandStore store = repl_command_store_live();
        GLCmd auto_normal = make_auto_normal(CMD_TESS_NORMAL, n[0], n[1], n[2]);
        char line[MAX_LINE_LEN];

        make_auto_normal_text(CMD_TESS_NORMAL, existing_normal,
                              n[0], n[1], n[2], line, sizeof(line));
        if (source_document_replace_line(existing_normal, line))
            repl_command_store_replace_one(&store, existing_normal,
                                           &auto_normal);
        return contour_end + 1;
    }

    {
        /* Top of the contour, so every gluVertex in it is covered. */
        GLCmd nc = make_auto_normal(CMD_TESS_NORMAL, n[0], n[1], n[2]);
        insert_cmd_at(begin_idx + 1, &nc, n[0], n[1], n[2], edit_line_inout);
        return contour_end + 2;   /* +1 for the row just inserted */
    }
}

/* The `autonormal` toggle moved out of REPL state onto `glr_state`. The
 * autonormal pass is a REPL pipeline TU and cannot include
 * `glr_state.h`, so the caller (controller / tests) gates the call
 * by passing the toggle explicitly (implemented as step 7a of
 * feature/decouple-repl-from-gl-repl-alt.md). */
void repl_recompute_autonormals(int autonormal_mode,
                                int *edit_line_inout) {
    if (autonormal_mode == REPL_AUTONORMAL_OFF) return;

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
        if (repl_state_document_cmds()[i].valid &&
            repl_state_document_cmds()[i].type == CMD_TESS_BEGIN_CONTOUR) {
            i = autonormal_tess_contour(i, front_face, edit_line_inout);
            continue;
        }
        if (!repl_state_document_cmds()[i].valid || repl_state_document_cmds()[i].type != CMD_BEGIN) { i++; continue; }

        GLenum mode = (GLenum)repl_state_document_cmds()[i].args[0];
        i++;

        int vi[MAX_EDITOR_COMMANDS];
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

        float norms[MAX_EDITOR_COMMANDS][3];
        if (autonormal_mode == REPL_AUTONORMAL_SMOOTH)
            compute_block_normals_smooth(mode, front_face, vi, nv, norms);
        else
            compute_block_normals(mode, front_face, vi, nv, norms);

        int offset = 0;
        /* The normal GL is left holding as the walk reaches each vertex.
         * A vertex whose face normal already matches it needs no row of
         * its own — the normal is current state, not a per-vertex
         * attribute, so a flat face spelled as four glVertex3f lines
         * needs one glNormal3f, not four identical ones.
         *
         * Deliberately reset per glBegin block rather than carried
         * across blocks: whatever normal was in effect beforehand would
         * have to be tracked through arbitrary intervening commands to
         * be trusted, so the first vertex of every block always gets its
         * own row. One redundant row per block buys not having to model
         * GL state outside it. */
        int have_current = 0;
        float current[3] = { 0, 0, 0 };

        for (int v = 0; v < nv; v++) {
            int vidx = vi[v] + offset;
            float nx = norms[v][0], ny = norms[v][1], nz = norms[v][2];
            float n[3];
            const GLCmd *prev = (vidx > 0) ? &repl_state_document_cmds()[vidx - 1]
                                           : NULL;
            int prev_is_normal = prev && prev->valid &&
                                 prev->type == CMD_NORMAL3F;

            n[0] = nx; n[1] = ny; n[2] = nz;

            /* A hand-written normal owns its row *and* the state that
             * follows it: it becomes what GL is holding, so a later
             * vertex matching it is redundant for the same reason. */
            if (prev_is_normal && !prev->is_auto) {
                current[0] = prev->args[0];
                current[1] = prev->args[1];
                current[2] = prev->args[2];
                have_current = 1;
                continue;
            }

            if (have_current && normals_match(current, n)) {
                /* Redundant. Drop the generated row if a previous pass
                 * left one here; the values it carried are exactly what
                 * is already in effect, so nothing renders differently. */
                if (prev_is_normal && prev->is_auto &&
                    delete_cmd_at(vidx - 1, edit_line_inout)) {
                    offset--;
                    block_end--;
                }
                continue;
            }

            if (prev_is_normal) {
                ReplCommandStore store = repl_command_store_live();
                GLCmd auto_normal = make_auto_normal(CMD_NORMAL3F, nx, ny, nz);
                char line[MAX_LINE_LEN];

                make_auto_normal_text(CMD_NORMAL3F, vidx - 1, nx, ny, nz,
                                      line, sizeof(line));
                /* In-place replacement: both ops touch the same
                 * row, no order-dependent rollback to worry about.
                 * Both writes are required for consistency, so
                 * gate the cmd-store on a successful text write. */
                if (source_document_replace_line(vidx - 1, line))
                    repl_command_store_replace_one(&store, vidx - 1,
                                                   &auto_normal);
            } else {
                GLCmd nc = make_auto_normal(CMD_NORMAL3F, nx, ny, nz);
                insert_cmd_at(vidx, &nc, nx, ny, nz, edit_line_inout);
                offset++;
                block_end++;
            }

            current[0] = nx;
            current[1] = ny;
            current[2] = nz;
            have_current = 1;
        }

        i = block_end + 1;
    }
}

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
 * matrix pair above (same block-unaware heuristic — see the collect_unbalanced
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
