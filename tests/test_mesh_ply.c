/* test_mesh_ply.c - unit tests for the pure PLY writer (mesh_ply_write).
 *
 * Drives the parser/inverter/welder with synthetic GL_3D_COLOR feedback
 * buffers; no GL context needed. Output is written to a tmpfile and parsed
 * back to assert vertex/face counts, world-coord inversion, color mapping,
 * and normal direction/winding. */

#include "support/mesh_ply.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)        TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp)     TEST_ASSERT_INT(&g_harness, label, got, exp)
#define ASSERT_FLT(label, got, exp)     TEST_ASSERT_FLOAT(&g_harness, label, got, exp, 1e-2f)

/* A capture that maps the [-1000,1000]^3 world cube onto an 800x800 viewport
 * with depth range [0,1] — what glr_mesh_export installs. */
static const MeshPlyCapture CAP = {
    .ortho_r = 1000.0f,
    .vp_x = 0, .vp_y = 0, .vp_w = 800, .vp_h = 800,
    .depth_near = 0.0f, .depth_far = 1.0f,
};

static const MeshPlyOptions OPT_WELD = {
    .weld = 1, .weld_eps = 1e-3f, .smooth_normals = 1, .triangulate = 1,
};
static const MeshPlyOptions OPT_FLAT = {
    .weld = 0, .weld_eps = 1e-3f, .smooth_normals = 0, .triangulate = 1,
};

/* ---- synthetic feedback buffer builders ----------------------------------
 * Forward-project a world point to window coords — the exact inverse of the
 * writer's invert_vertex — so a round trip should reproduce the world coord. */
static void push_tok(float *buf, int *n, int tok) { buf[(*n)++] = (float)tok; }
static void push_val(float *buf, int *n, float v) { buf[(*n)++] = v; }

static void push_vert(float *buf, int *n, float wx, float wy, float wz,
                      float r, float g, float b, float a) {
    float ndc_x = wx / CAP.ortho_r;
    float ndc_y = wy / CAP.ortho_r;
    float ndc_z = -wz / CAP.ortho_r;
    buf[(*n)++] = CAP.vp_x + (ndc_x + 1.0f) * 0.5f * CAP.vp_w;
    buf[(*n)++] = CAP.vp_y + (ndc_y + 1.0f) * 0.5f * CAP.vp_h;
    buf[(*n)++] = CAP.depth_near + (ndc_z + 1.0f) * 0.5f *
                  (CAP.depth_far - CAP.depth_near);
    buf[(*n)++] = r; buf[(*n)++] = g; buf[(*n)++] = b; buf[(*n)++] = a;
}

/* ---- PLY readback --------------------------------------------------------- */
#define MAXV 64
typedef struct {
    int nverts, nfaces;
    float vx[MAXV], vy[MAXV], vz[MAXV], nx[MAXV], ny[MAXV], nz[MAXV];
    int cr[MAXV], cg[MAXV], cb[MAXV], ca[MAXV];
    int f[MAXV][3];
} Ply;

/* Run the writer to a tmpfile and slurp the text back into `text`. */
static int run_writer(const float *buf, int n, const MeshPlyOptions *opt,
                      char *text, size_t tcap) {
    FILE *fp = tmpfile();
    if (!fp) return -999;
    int rc = mesh_ply_write(fp, buf, n, &CAP, opt);
    fflush(fp);
    rewind(fp);
    size_t rd = fread(text, 1, tcap - 1, fp);
    text[rd] = '\0';
    fclose(fp);
    return rc;
}

/* Parse the subset of PLY we emit. Returns 1 on success. */
static int parse_ply(const char *src, Ply *p) {
    char buf[8192];
    strncpy(buf, src, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    memset(p, 0, sizeof(*p));

    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    int in_body = 0, vi = 0, fi = 0;
    while (line) {
        if (!in_body) {
            if (strncmp(line, "element vertex ", 15) == 0)
                p->nverts = atoi(line + 15);
            else if (strncmp(line, "element face ", 13) == 0)
                p->nfaces = atoi(line + 13);
            else if (strcmp(line, "end_header") == 0)
                in_body = 1;
        } else if (vi < p->nverts) {
            if (vi >= MAXV) return 0;
            int got = sscanf(line, "%f %f %f %f %f %f %d %d %d %d",
                             &p->vx[vi], &p->vy[vi], &p->vz[vi],
                             &p->nx[vi], &p->ny[vi], &p->nz[vi],
                             &p->cr[vi], &p->cg[vi], &p->cb[vi], &p->ca[vi]);
            if (got != 10) return 0;
            vi++;
        } else if (fi < p->nfaces) {
            if (fi >= MAXV) return 0;
            int cnt = 0;
            int got = sscanf(line, "%d %d %d %d", &cnt,
                             &p->f[fi][0], &p->f[fi][1], &p->f[fi][2]);
            if (got != 4 || cnt != 3) return 0;
            fi++;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return vi == p->nverts && fi == p->nfaces;
}

/* ---- tests ---------------------------------------------------------------- */

static void test_single_triangle(void) {
    printf("test_single_triangle\n");
    float buf[64]; int n = 0;
    push_tok(buf, &n, MESH_PLY_TOK_POLYGON);
    push_val(buf, &n, 3);
    push_vert(buf, &n, 0, 0, 0, 1, 0, 0, 1);   /* A red   */
    push_vert(buf, &n, 1, 0, 0, 0, 1, 0, 1);   /* B green */
    push_vert(buf, &n, 0, 1, 0, 0, 0, 1, 1);   /* C blue  */

    char text[8192];
    int rc = run_writer(buf, n, &OPT_WELD, text, sizeof(text));
    ASSERT_INT("single tri returns 1 triangle", rc, 1);

    Ply p;
    ASSERT_TRUE("single tri parses", parse_ply(text, &p));
    ASSERT_INT("single tri nverts", p.nverts, 3);
    ASSERT_INT("single tri nfaces", p.nfaces, 1);

    /* World-coord inversion round-trips. */
    ASSERT_FLT("A.x", p.vx[0], 0.0f);
    ASSERT_FLT("A.y", p.vy[0], 0.0f);
    ASSERT_FLT("A.z", p.vz[0], 0.0f);
    ASSERT_FLT("B.x", p.vx[1], 1.0f);
    ASSERT_FLT("C.y", p.vy[2], 1.0f);

    /* Colors 0..1 -> 0..255. */
    ASSERT_INT("A red=255", p.cr[0], 255);
    ASSERT_INT("A green=0", p.cg[0], 0);
    ASSERT_INT("A alpha=255", p.ca[0], 255);
    ASSERT_INT("B green=255", p.cg[1], 255);
    ASSERT_INT("C blue=255", p.cb[2], 255);

    /* Winding A->B->C (CCW in XY) yields +Z normal at every vertex. */
    ASSERT_FLT("normal x", p.nx[0], 0.0f);
    ASSERT_FLT("normal y", p.ny[0], 0.0f);
    ASSERT_FLT("normal z", p.nz[0], 1.0f);
}

static void test_quad_two_faces(void) {
    printf("test_quad_two_faces\n");
    float buf[64]; int n = 0;
    push_tok(buf, &n, MESH_PLY_TOK_POLYGON);
    push_val(buf, &n, 4);
    push_vert(buf, &n, 0, 0, 0, 1, 1, 1, 1);
    push_vert(buf, &n, 1, 0, 0, 1, 1, 1, 1);
    push_vert(buf, &n, 1, 1, 0, 1, 1, 1, 1);
    push_vert(buf, &n, 0, 1, 0, 1, 1, 1, 1);

    char text[8192];
    int rc = run_writer(buf, n, &OPT_WELD, text, sizeof(text));
    ASSERT_INT("quad returns 2 triangles", rc, 2);

    Ply p;
    ASSERT_TRUE("quad parses", parse_ply(text, &p));
    ASSERT_INT("quad welds to 4 verts", p.nverts, 4);
    ASSERT_INT("quad nfaces", p.nfaces, 2);
}

static void test_shared_edge_weld(void) {
    printf("test_shared_edge_weld\n");
    /* Two separate POLYGON tokens forming a quad: (A,B,C) and (A,C,D),
     * coplanar in z=0. Welding should merge to 4 vertices; the shared
     * vertices A and C keep the +Z normal. */
    float buf[64]; int n = 0;
    push_tok(buf, &n, MESH_PLY_TOK_POLYGON);
    push_val(buf, &n, 3);
    push_vert(buf, &n, 0, 0, 0, 1, 1, 1, 1);   /* A */
    push_vert(buf, &n, 1, 0, 0, 1, 1, 1, 1);   /* B */
    push_vert(buf, &n, 1, 1, 0, 1, 1, 1, 1);   /* C */
    push_tok(buf, &n, MESH_PLY_TOK_POLYGON);
    push_val(buf, &n, 3);
    push_vert(buf, &n, 0, 0, 0, 1, 1, 1, 1);   /* A (shared) */
    push_vert(buf, &n, 1, 1, 0, 1, 1, 1, 1);   /* C (shared) */
    push_vert(buf, &n, 0, 1, 0, 1, 1, 1, 1);   /* D */

    char text[8192];
    int rc = run_writer(buf, n, &OPT_WELD, text, sizeof(text));
    ASSERT_INT("shared-edge returns 2 triangles", rc, 2);

    Ply p;
    ASSERT_TRUE("shared-edge parses", parse_ply(text, &p));
    ASSERT_INT("shared-edge welds to 4 verts", p.nverts, 4);
    ASSERT_INT("shared-edge nfaces", p.nfaces, 2);
    /* Every welded vertex normal is +Z (all faces coplanar). */
    for (int v = 0; v < p.nverts; v++)
        ASSERT_FLT("shared-edge vert normal z", p.nz[v], 1.0f);

    /* Same geometry without welding stays 6 verts (flat, 2 tris x 3). */
    rc = run_writer(buf, n, &OPT_FLAT, text, sizeof(text));
    ASSERT_INT("flat returns 2 triangles", rc, 2);
    ASSERT_TRUE("flat parses", parse_ply(text, &p));
    ASSERT_INT("flat keeps 6 verts", p.nverts, 6);
}

static void test_skip_alignment(void) {
    printf("test_skip_alignment\n");
    /* A polygon preceded by every skipped token kind. If any skip length is
     * wrong, the trailing triangle misaligns and the counts/coords break. */
    float buf[128]; int n = 0;
    push_tok(buf, &n, MESH_PLY_TOK_POINT);
    push_vert(buf, &n, 5, 5, 5, 1, 1, 1, 1);             /* 1 vertex */
    push_tok(buf, &n, MESH_PLY_TOK_LINE);
    push_vert(buf, &n, 1, 1, 1, 1, 1, 1, 1);             /* 2 vertices */
    push_vert(buf, &n, 2, 2, 2, 1, 1, 1, 1);
    push_tok(buf, &n, MESH_PLY_TOK_LINE_RESET);
    push_vert(buf, &n, 3, 3, 3, 1, 1, 1, 1);             /* 2 vertices */
    push_vert(buf, &n, 4, 4, 4, 1, 1, 1, 1);
    push_tok(buf, &n, MESH_PLY_TOK_PASS_THROUGH);
    push_val(buf, &n, 42.0f);                            /* 1 value */
    push_tok(buf, &n, MESH_PLY_TOK_POLYGON);
    push_val(buf, &n, 3);
    push_vert(buf, &n, 0, 0, 0, 1, 0, 0, 1);
    push_vert(buf, &n, 2, 0, 0, 0, 1, 0, 1);
    push_vert(buf, &n, 0, 2, 0, 0, 0, 1, 1);

    char text[8192];
    int rc = run_writer(buf, n, &OPT_WELD, text, sizeof(text));
    ASSERT_INT("alignment: only the polygon survives", rc, 1);

    Ply p;
    ASSERT_TRUE("alignment parses", parse_ply(text, &p));
    ASSERT_INT("alignment nverts", p.nverts, 3);
    ASSERT_INT("alignment nfaces", p.nfaces, 1);
    /* The trailing triangle's coords are intact (skips consumed exactly). */
    ASSERT_FLT("alignment B.x", p.vx[1], 2.0f);
    ASSERT_FLT("alignment C.y", p.vy[2], 2.0f);
}

static void test_error_cases(void) {
    printf("test_error_cases\n");
    char text[8192];

    /* Unknown token marker -> hard error. */
    float bad[4]; int n = 0;
    push_val(bad, &n, 9999.0f);
    ASSERT_TRUE("unknown token errors", run_writer(bad, n, &OPT_WELD, text,
                                                   sizeof(text)) < 0);

    /* Truncated polygon (claims 3 verts, only 2 floats follow) -> error. */
    float trunc[8]; n = 0;
    push_tok(trunc, &n, MESH_PLY_TOK_POLYGON);
    push_val(trunc, &n, 3);
    push_val(trunc, &n, 1.0f);
    push_val(trunc, &n, 2.0f);
    ASSERT_TRUE("truncated polygon errors", run_writer(trunc, n, &OPT_WELD, text,
                                                       sizeof(text)) < 0);
}

static void test_empty_buffer(void) {
    printf("test_empty_buffer\n");
    char text[8192];
    int rc = run_writer(NULL, 0, &OPT_WELD, text, sizeof(text));
    ASSERT_INT("empty buffer returns 0 triangles", rc, 0);

    Ply p;
    ASSERT_TRUE("empty buffer parses", parse_ply(text, &p));
    ASSERT_INT("empty buffer nverts", p.nverts, 0);
    ASSERT_INT("empty buffer nfaces", p.nfaces, 0);
    ASSERT_TRUE("empty buffer has header", strstr(text, "format ascii 1.0") != NULL);
}

static void test_color_clamp(void) {
    printf("test_color_clamp\n");
    float buf[64]; int n = 0;
    push_tok(buf, &n, MESH_PLY_TOK_POLYGON);
    push_val(buf, &n, 3);
    push_vert(buf, &n, 0, 0, 0, -0.5f, 0.5f, 2.0f, 1);  /* out-of-range RGB */
    push_vert(buf, &n, 1, 0, 0, -0.5f, 0.5f, 2.0f, 1);
    push_vert(buf, &n, 0, 1, 0, -0.5f, 0.5f, 2.0f, 1);

    char text[8192];
    int rc = run_writer(buf, n, &OPT_WELD, text, sizeof(text));
    ASSERT_INT("color-clamp returns 1 triangle", rc, 1);

    Ply p;
    ASSERT_TRUE("color-clamp parses", parse_ply(text, &p));
    ASSERT_INT("clamp low -> 0", p.cr[0], 0);
    ASSERT_INT("mid 0.5 -> ~128", p.cg[0], 128);
    ASSERT_INT("clamp high -> 255", p.cb[0], 255);
}

int main(void) {
    printf("=== mesh_ply tests ===\n\n");
    test_single_triangle();
    test_quad_two_faces();
    test_shared_edge_weld();
    test_skip_alignment();
    test_error_cases();
    test_empty_buffer();
    test_color_clamp();

    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "mesh_ply");
}
