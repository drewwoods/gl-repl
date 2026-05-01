/*
 * repl_autonormal.c -- Auto-generated normal commands and feeding-state lookup.
 */
#include "sample.h"
#include "repl_core.h"
#include "repl_command_store.h"
#include "repl_source_scope.h"
#include "repl_state.h"

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

static GLCmd make_auto_normal(float nx, float ny, float nz,
                              int insert_pos) {
    GLCmd c;
    memset(&c, 0, sizeof(c));
    c.type = CMD_NORMAL3F;
    c.args[0] = nx;
    c.args[1] = ny;
    c.args[2] = nz;
    c.num_args = 3;
    c.valid = 1;
    c.is_auto = 1;

    (void)insert_pos;  /* indent computed in make_auto_normal_text */
    return c;
}

/* Canonical text for an auto-normal at insert_pos */
static void make_auto_normal_text(int insert_pos, float nx, float ny, float nz,
                                  char *text_out, int text_sz) {
    char ind[32];
    normal_indent(insert_pos, ind, sizeof(ind));
    snprintf(text_out, (size_t)text_sz,
             "%sglNormal3f(%g, %g, %g);", ind, nx, ny, nz);
}

static void insert_cmd_at(int pos, const GLCmd *cmd,
                           float nx, float ny, float nz) {
    ReplCommandStore store = repl_command_store_live();
    char line[MAX_LINE_LEN];

    make_auto_normal_text(pos, nx, ny, nz, line, sizeof(line));
    repl_command_store_insert_one(&store, pos, cmd,
                                  REPL_COMMAND_STORE_ADJUST_EDIT_LINE,
                                  line);
}

static void apply_front_face_to_normal(GLenum front_face, float *n) {
    if (front_face == GL_CW) {
        n[0] = -n[0];
        n[1] = -n[1];
        n[2] = -n[2];
    }
}

static void compute_block_normals(GLenum mode, GLenum front_face,
                                  int *vi, int nv, float norms[][3]) {
    for (int idx = 0; idx < nv; idx++)
        norms[idx][0] = norms[idx][1] = norms[idx][2] = 0;

    float n[3];
    switch (mode) {
    case GL_TRIANGLES:
        for (int idx = 0; idx + 2 < nv; idx += 3) {
            face_normal(repl_state_document_cmds_mut()[vi[idx]].args, repl_state_document_cmds_mut()[vi[idx+1]].args,
                        repl_state_document_cmds_mut()[vi[idx+2]].args, n);
            apply_front_face_to_normal(front_face, n);
            for (int component_idx = 0; component_idx < 3; component_idx++)
                memcpy(norms[idx+component_idx], n, sizeof(n));
        }
        break;
    case GL_TRIANGLE_STRIP:
        for (int idx = 0; idx + 2 < nv; idx++) {
            if (idx % 2 == 0)
                face_normal(repl_state_document_cmds_mut()[vi[idx]].args, repl_state_document_cmds_mut()[vi[idx+1]].args,
                            repl_state_document_cmds_mut()[vi[idx+2]].args, n);
            else
                face_normal(repl_state_document_cmds_mut()[vi[idx]].args, repl_state_document_cmds_mut()[vi[idx+2]].args,
                            repl_state_document_cmds_mut()[vi[idx+1]].args, n);
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
            face_normal(repl_state_document_cmds_mut()[vi[0]].args, repl_state_document_cmds_mut()[vi[idx]].args,
                        repl_state_document_cmds_mut()[vi[idx+1]].args, n);
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
            face_normal(repl_state_document_cmds_mut()[vi[idx]].args, repl_state_document_cmds_mut()[vi[idx+1]].args,
                        repl_state_document_cmds_mut()[vi[idx+2]].args, n);
            apply_front_face_to_normal(front_face, n);
            for (int component_idx = 0; component_idx < 4; component_idx++)
                memcpy(norms[idx+component_idx], n, sizeof(n));
        }
        break;
    case GL_QUAD_STRIP:
        for (int idx = 0; idx + 3 < nv; idx += 2) {
            face_normal(repl_state_document_cmds_mut()[vi[idx]].args, repl_state_document_cmds_mut()[vi[idx+1]].args,
                        repl_state_document_cmds_mut()[vi[idx+2]].args, n);
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
            face_normal(repl_state_document_cmds_mut()[vi[0]].args, repl_state_document_cmds_mut()[vi[1]].args,
                        repl_state_document_cmds_mut()[vi[2]].args, n);
            apply_front_face_to_normal(front_face, n);
            for (int idx = 0; idx < nv; idx++)
                memcpy(norms[idx], n, sizeof(n));
        }
        break;
    default:
        break;
    }
}

void recompute_autonormals(void) {
    if (!repl_state_presentation().autonormal) return;

    int i = 0;
    GLenum front_face = GL_CCW;
    while (i < repl_state_document_count()) {
        if (repl_state_document_cmds_mut()[i].valid && repl_state_document_cmds_mut()[i].type == CMD_FRONT_FACE) {
            front_face = repl_state_document_cmds_mut()[i].mode;
            i++;
            continue;
        }
        if (repl_state_document_cmds_mut()[i].type == CMD_FOR_BEGIN ||
            repl_state_document_cmds_mut()[i].type == CMD_FUNC_DEF ||
            repl_state_document_cmds_mut()[i].type == CMD_IF_BEGIN) {
            i = repl_source_scope_find_block_end(i);
            if (i < repl_state_document_count()) i++;
            continue;
        }
        if (!repl_state_document_cmds_mut()[i].valid || repl_state_document_cmds_mut()[i].type != CMD_BEGIN) { i++; continue; }

        GLenum mode = repl_state_document_cmds_mut()[i].mode;
        i++;

        int vi[MAX_COMMANDS];
        int nv = 0;
        int block_end = repl_state_document_count();
        for (int j = i; j < repl_state_document_count(); j++) {
            if (!repl_state_document_cmds_mut()[j].valid) continue;
            if (repl_state_document_cmds_mut()[j].type == CMD_END) { block_end = j; break; }
            if (repl_state_document_cmds_mut()[j].type == CMD_BEGIN) { block_end = j; break; }
            if (repl_state_document_cmds_mut()[j].type == CMD_VERTEX3F)
                vi[nv++] = j;
        }

        float norms[MAX_COMMANDS][3];
        compute_block_normals(mode, front_face, vi, nv, norms);

        int offset = 0;
        for (int v = 0; v < nv; v++) {
            int vidx = vi[v] + offset;
            float nx = norms[v][0], ny = norms[v][1], nz = norms[v][2];

            if (vidx > 0 && repl_state_document_cmds_mut()[vidx - 1].valid &&
                repl_state_document_cmds_mut()[vidx - 1].type == CMD_NORMAL3F) {
                if (repl_state_document_cmds_mut()[vidx - 1].is_auto) {
                    ReplCommandStore store = repl_command_store_live();
                    GLCmd auto_normal = make_auto_normal(nx, ny, nz, vidx - 1);
                    char line[MAX_LINE_LEN];

                    make_auto_normal_text(vidx - 1, nx, ny, nz, line, sizeof(line));
                    repl_command_store_replace_one(&store, vidx - 1,
                                                   &auto_normal, line);
                }
                continue;
            }

            GLCmd nc = make_auto_normal(nx, ny, nz, vidx);
            insert_cmd_at(vidx, &nc, nx, ny, nz);
            offset++;
            block_end++;
        }

        i = block_end + 1;
    }
}

static int find_feeding_state_cmd(int line_idx, int want_normal) {
    if (line_idx < 0 || line_idx >= repl_state_document_count()) return -1;
    if (!repl_state_document_cmds_mut()[line_idx].valid) return -1;

    CmdType target = repl_state_document_cmds_mut()[line_idx].type;
    int is_gl_vtx = (target == CMD_VERTEX3F || target == CMD_VERTEX2F);
    int is_tess_vtx = (target == CMD_TESS_VERTEX);
    if (!is_gl_vtx && !is_tess_vtx) return -1;

    for (int i = line_idx - 1; i >= 0; i--) {
        if (!repl_state_document_cmds_mut()[i].valid) continue;
        CmdType t = repl_state_document_cmds_mut()[i].type;
        if (want_normal) {
            if (is_gl_vtx && t == CMD_NORMAL3F) return i;
            if (is_tess_vtx && t == CMD_TESS_NORMAL) return i;
        } else {
            if (is_gl_vtx && (t == CMD_COLOR3F || t == CMD_COLOR4F)) return i;
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

void repl_recompute_autonormals(void) {
    recompute_autonormals();
}
