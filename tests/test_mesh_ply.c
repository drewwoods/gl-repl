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

/* Same capture but the GL_3D_COLOR_TEXTURE 11-float stride: each vertex also
 * carries an encoded world-space normal in the texcoord channel. */
static const MeshPlyCapture CAP_TEX = {
    .ortho_r = 1000.0f,
    .vp_x = 0, .vp_y = 0, .vp_w = 800, .vp_h = 800,
    .depth_near = 0.0f, .depth_far = 1.0f,
    .floats_per_vertex = MESH_PLY_FLOATS_PER_VERTEX_TEX,
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

/* GL_3D_COLOR_TEXTURE vertex (11 floats): the 7-float vertex above plus the
 * texcoord channel (s,t,r,q). The encoded world normal goes in (s,t,r) raw —
 * the executor emits it pre-transformed, texture matrix identity, so it is
 * NOT projected. */
static void push_vert_tex(float *buf, int *n, float wx, float wy, float wz,
                          float r, float g, float b, float a,
                          float nx, float ny, float nz, float q) {
    push_vert(buf, n, wx, wy, wz, r, g, b, a);
    buf[(*n)++] = nx; buf[(*n)++] = ny; buf[(*n)++] = nz; buf[(*n)++] = q;
}

/* ---- PLY readback --------------------------------------------------------- */
#define MAXV 64
typedef struct {
    int nverts, nfaces, nedges;
    float vx[MAXV], vy[MAXV], vz[MAXV], nx[MAXV], ny[MAXV], nz[MAXV];
    int cr[MAXV], cg[MAXV], cb[MAXV], ca[MAXV];
    int f[MAXV][3];
    int e[MAXV][2];
} Ply;

/* Run the writer to a tmpfile (explicit capture) and slurp the text back. */
static int run_writer_cap(const float *buf, int n, const MeshPlyCapture *cap,
                          const MeshPlyOptions *opt, char *text, size_t tcap) {
    FILE *fp = tmpfile();
    if (!fp) return -999;
    int rc = mesh_ply_write(fp, buf, n, cap, opt, NULL);
    fflush(fp);
    rewind(fp);
    size_t rd = fread(text, 1, tcap - 1, fp);
    text[rd] = '\0';
    fclose(fp);
    return rc;
}

static int run_writer(const float *buf, int n, const MeshPlyOptions *opt,
                      char *text, size_t tcap) {
    return run_writer_cap(buf, n, &CAP, opt, text, tcap);
}

/* Parse the subset of PLY we emit. Returns 1 on success. */
static int parse_ply(const char *src, Ply *p) {
    char buf[8192];
    strncpy(buf, src, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    memset(p, 0, sizeof(*p));

    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    int in_body = 0, vi = 0, fi = 0, ei = 0;
    while (line) {
        if (!in_body) {
            if (strncmp(line, "element vertex ", 15) == 0)
                p->nverts = atoi(line + 15);
            else if (strncmp(line, "element face ", 13) == 0)
                p->nfaces = atoi(line + 13);
            else if (strncmp(line, "element edge ", 13) == 0)
                p->nedges = atoi(line + 13);
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
        } else if (ei < p->nedges) {
            if (ei >= MAXV) return 0;
            int got = sscanf(line, "%d %d", &p->e[ei][0], &p->e[ei][1]);
            if (got != 2) return 0;
            ei++;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return vi == p->nverts && fi == p->nfaces && ei == p->nedges;
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

    /* No lines captured -> no edge element declared. */
    ASSERT_TRUE("no edge element without lines",
                strstr(text, "element edge") == NULL);
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
    /* A polygon preceded by every other token kind. If any record length is
     * wrong, the trailing triangle misaligns and the counts/coords break.
     * Points and lines are captured (loose vertex / edge); bitmap stays a
     * pure skip. */
    float buf[128]; int n = 0;
    push_tok(buf, &n, MESH_PLY_TOK_POINT);
    push_vert(buf, &n, 5, 5, 5, 1, 1, 1, 1);             /* 1 vertex */
    push_tok(buf, &n, MESH_PLY_TOK_LINE);
    push_vert(buf, &n, 1, 1, 1, 1, 1, 1, 1);             /* 2 vertices */
    push_vert(buf, &n, 2, 2, 2, 1, 1, 1, 1);
    push_tok(buf, &n, MESH_PLY_TOK_LINE_RESET);
    push_vert(buf, &n, 3, 3, 3, 1, 1, 1, 1);             /* 2 vertices */
    push_vert(buf, &n, 4, 4, 4, 1, 1, 1, 1);
    push_tok(buf, &n, MESH_PLY_TOK_BITMAP);
    push_vert(buf, &n, 6, 6, 6, 1, 1, 1, 1);             /* 1 vertex, skipped */
    push_tok(buf, &n, MESH_PLY_TOK_PASS_THROUGH);
    push_val(buf, &n, 42.0f);                            /* 1 value */
    push_tok(buf, &n, MESH_PLY_TOK_POLYGON);
    push_val(buf, &n, 3);
    push_vert(buf, &n, 0, 0, 0, 1, 0, 0, 1);
    push_vert(buf, &n, 2, 0, 0, 0, 1, 0, 1);
    push_vert(buf, &n, 0, 2, 0, 0, 0, 1, 1);

    char text[8192];
    int rc = run_writer(buf, n, &OPT_WELD, text, sizeof(text));
    ASSERT_INT("alignment: one triangle", rc, 1);

    Ply p;
    ASSERT_TRUE("alignment parses", parse_ply(text, &p));
    /* 3 tri corners + 4 line endpoints + 1 point; bitmap vertex skipped. */
    ASSERT_INT("alignment nverts", p.nverts, 8);
    ASSERT_INT("alignment nfaces", p.nfaces, 1);
    ASSERT_INT("alignment nedges", p.nedges, 2);
    /* The trailing triangle's coords are intact (records consumed exactly);
     * triangle corners always precede line/point vertices in the output. */
    ASSERT_FLT("alignment B.x", p.vx[1], 2.0f);
    ASSERT_FLT("alignment C.y", p.vy[2], 2.0f);
    /* Edges reference the line-endpoint vertices behind the tri corners. */
    ASSERT_INT("alignment edge0 v1", p.e[0][0], 3);
    ASSERT_INT("alignment edge0 v2", p.e[0][1], 4);
    ASSERT_INT("alignment edge1 v1", p.e[1][0], 5);
    ASSERT_INT("alignment edge1 v2", p.e[1][1], 6);
    /* The loose point vertex lands last. */
    ASSERT_FLT("alignment point x", p.vx[7], 5.0f);
}

static void test_points_and_lines(void) {
    printf("test_points_and_lines\n");
    /* Two colored points + a two-segment line strip (feedback emits the strip
     * pairwise: LINE_RESET then LINE, repeating the shared middle vertex).
     * Points become loose vertices; segments become edge records; welding
     * merges the strip's shared endpoint so the edges chain. */
    float buf[128]; int n = 0;
    push_tok(buf, &n, MESH_PLY_TOK_POINT);
    push_vert(buf, &n, 10, 0, 0, 1, 0, 0, 1);            /* red point */
    push_tok(buf, &n, MESH_PLY_TOK_POINT);
    push_vert(buf, &n, 0, 10, 0, 0, 1, 0, 1);            /* green point */
    push_tok(buf, &n, MESH_PLY_TOK_LINE_RESET);
    push_vert(buf, &n, 0, 0, 0, 1, 1, 1, 1);
    push_vert(buf, &n, 1, 0, 0, 1, 1, 1, 1);
    push_tok(buf, &n, MESH_PLY_TOK_LINE);
    push_vert(buf, &n, 1, 0, 0, 1, 1, 1, 1);             /* shared endpoint */
    push_vert(buf, &n, 2, 0, 0, 1, 1, 1, 1);

    char text[8192];
    int rc = run_writer(buf, n, &OPT_WELD, text, sizeof(text));
    ASSERT_INT("points+lines: 0 triangles", rc, 0);
    ASSERT_TRUE("points+lines declares edge element",
                strstr(text, "element edge 2") != NULL);

    Ply p;
    ASSERT_TRUE("points+lines parses", parse_ply(text, &p));
    /* 3 welded strip vertices + 2 points. */
    ASSERT_INT("points+lines nverts", p.nverts, 5);
    ASSERT_INT("points+lines nfaces", p.nfaces, 0);
    ASSERT_INT("points+lines nedges", p.nedges, 2);
    /* The strip chains through the welded shared vertex. */
    ASSERT_INT("strip edge0 v2 == edge1 v1", p.e[0][1], p.e[1][0]);
    ASSERT_FLT("strip start x", p.vx[p.e[0][0]], 0.0f);
    ASSERT_FLT("strip mid x", p.vx[p.e[0][1]], 1.0f);
    ASSERT_FLT("strip end x", p.vx[p.e[1][1]], 2.0f);
    /* Point coords and colors round-trip (points land after line verts). */
    ASSERT_FLT("point0 x", p.vx[3], 10.0f);
    ASSERT_INT("point0 red", p.cr[3], 255);
    ASSERT_FLT("point1 y", p.vy[4], 10.0f);
    ASSERT_INT("point1 green", p.cg[4], 255);

    /* Flat mode keeps every endpoint distinct: 4 line verts + 2 points. */
    rc = run_writer(buf, n, &OPT_FLAT, text, sizeof(text));
    ASSERT_INT("flat points+lines: 0 triangles", rc, 0);
    ASSERT_TRUE("flat points+lines parses", parse_ply(text, &p));
    ASSERT_INT("flat points+lines nverts", p.nverts, 6);
    ASSERT_INT("flat points+lines nedges", p.nedges, 2);
    ASSERT_INT("flat edge1 v1", p.e[1][0], 2);
    ASSERT_INT("flat edge1 v2", p.e[1][1], 3);
}

static void test_point_line_authored_normals(void) {
    printf("test_point_line_authored_normals\n");
    /* Under a NORMALS passthrough marker (11-float stride), point and line
     * vertices carry authored world-space normals in the texcoord channel,
     * exactly like polygon corners. */
    float buf[128]; int n = 0;
    push_tok(buf, &n, MESH_PLY_TOK_PASS_THROUGH);
    push_val(buf, &n, MESH_PLY_PASS_NORMALS);
    push_tok(buf, &n, MESH_PLY_TOK_POINT);
    push_vert_tex(buf, &n, 5, 0, 0, 1, 1, 1, 1,   0, 2, 0, 0);   /* +Y */
    push_tok(buf, &n, MESH_PLY_TOK_LINE);
    push_vert_tex(buf, &n, 0, 0, 0, 1, 1, 1, 1,   3, 0, 0, 0);   /* +X */
    push_vert_tex(buf, &n, 1, 0, 0, 1, 1, 1, 1,   3, 0, 0, 0);

    char text[8192];
    int rc = run_writer_cap(buf, n, &CAP_TEX, &OPT_WELD, text, sizeof text);
    ASSERT_INT("authored pt/line: 0 triangles", rc, 0);

    Ply p;
    ASSERT_TRUE("authored pt/line parses", parse_ply(text, &p));
    ASSERT_INT("authored pt/line nverts", p.nverts, 3);
    ASSERT_INT("authored pt/line nedges", p.nedges, 1);
    /* Line endpoints (first two verts) -> normalized +X. */
    ASSERT_FLT("line normal nx=1", p.nx[0], 1.0f);
    ASSERT_FLT("line normal ny=0", p.ny[0], 0.0f);
    /* Point (last vert) -> normalized +Y. */
    ASSERT_FLT("point normal ny=1", p.ny[2], 1.0f);
    ASSERT_FLT("point normal nx=0", p.nx[2], 0.0f);
}

static void test_solidified_points_and_lines(void) {
    printf("test_solidified_points_and_lines\n");
    /* Mesh-only viewers ignore loose vertices and edge elements. With the
     * compatibility scale enabled, one point becomes an 8-triangle octahedron
     * and one line becomes a 24-triangle capped six-sided tube. The semantic
     * point vertex and edge record remain present too. */
    float buf[64]; int n = 0;
    push_tok(buf, &n, MESH_PLY_TOK_POINT);
    push_vert(buf, &n, 0, 2, 0, 1, 0.5f, 0, 1);
    push_tok(buf, &n, MESH_PLY_TOK_LINE);
    push_vert(buf, &n, 0, 0, 0, 0, 1, 0, 1);
    push_vert(buf, &n, 2, 0, 0, 0, 0, 1, 1);

    MeshPlyOptions opt = OPT_WELD;
    opt.primitive_radius_scale = 0.1f;
    char text[8192];
    int rc = run_writer(buf, n, &opt, text, sizeof text);
    ASSERT_INT("solidified point+line triangle count", rc, 32);

    Ply p;
    ASSERT_TRUE("solidified point+line parses", parse_ply(text, &p));
    ASSERT_INT("solidified point+line faces", p.nfaces, 32);
    ASSERT_INT("solidified point+line keeps edge", p.nedges, 1);
    ASSERT_TRUE("solidified point expands scene bounds", p.nverts > 3);
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

static void test_srgb_decode(void) {
    printf("test_srgb_decode\n");
    /* Same geometry, two passes: default (raw) vs srgb_decode. RGB = (0, .5, 1),
     * alpha = .5. Decoding the sRGB EOTF maps mid 0.5 -> ~0.214 -> byte 55 on
     * R/G/B; the 0/1 endpoints are fixed points; alpha stays linear (128). */
    float buf[64]; int n = 0;
    push_tok(buf, &n, MESH_PLY_TOK_POLYGON);
    push_val(buf, &n, 3);
    push_vert(buf, &n, 0, 0, 0, 0.0f, 0.5f, 1.0f, 0.5f);
    push_vert(buf, &n, 1, 0, 0, 0.0f, 0.5f, 1.0f, 0.5f);
    push_vert(buf, &n, 0, 1, 0, 0.0f, 0.5f, 1.0f, 0.5f);

    char text[8192];
    Ply p;

    /* Default: linear pass-through. Mid 0.5 -> ~128. */
    int rc = run_writer(buf, n, &OPT_WELD, text, sizeof(text));
    ASSERT_INT("srgb-off returns 1 triangle", rc, 1);
    ASSERT_TRUE("srgb-off parses", parse_ply(text, &p));
    ASSERT_INT("srgb-off mid 0.5 -> ~128", p.cg[0], 128);

    /* srgb_decode on: R 0 -> 0, G 0.5 -> ~55, B 1 -> 255, A 0.5 stays 128. */
    MeshPlyOptions opt_srgb = OPT_WELD;
    opt_srgb.srgb_decode = 1;
    rc = run_writer(buf, n, &opt_srgb, text, sizeof(text));
    ASSERT_INT("srgb-on returns 1 triangle", rc, 1);
    ASSERT_TRUE("srgb-on parses", parse_ply(text, &p));
    ASSERT_INT("srgb-on red 0 stays 0", p.cr[0], 0);
    ASSERT_INT("srgb-on green 0.5 -> ~55", p.cg[0], 55);
    ASSERT_INT("srgb-on blue 1 stays 255", p.cb[0], 255);
    ASSERT_INT("srgb-on alpha stays linear 128", p.ca[0], 128);
}

static void test_texcoord_normals(void) {
    printf("test_texcoord_normals\n");
    float buf[128]; int n = 0;

    /* NORMALS-mode triangle: geometric normal would be +Z (z=0 plane, CCW),
     * but the texcoord encodes a non-unit +X normal (2,0,0). Authored must
     * win and be normalized to (1,0,0). */
    push_tok(buf, &n, MESH_PLY_TOK_PASS_THROUGH);
    push_val(buf, &n, MESH_PLY_PASS_NORMALS);
    push_tok(buf, &n, MESH_PLY_TOK_POLYGON);
    push_val(buf, &n, 3);
    push_vert_tex(buf, &n, 0, 0, 0, 1, 0, 0, 1,   2, 0, 0, 0);
    push_vert_tex(buf, &n, 1, 0, 0, 0, 1, 0, 1,   2, 0, 0, 0);
    push_vert_tex(buf, &n, 0, 1, 0, 0, 0, 1, 1,   2, 0, 0, 0);

    /* NO_NORMALS-mode triangle (z=0 plane, geometric +Z): texcoord is garbage
     * and must be ignored -> synthesized +Z. */
    push_tok(buf, &n, MESH_PLY_TOK_PASS_THROUGH);
    push_val(buf, &n, MESH_PLY_PASS_NO_NORMALS);
    push_tok(buf, &n, MESH_PLY_TOK_POLYGON);
    push_val(buf, &n, 3);
    push_vert_tex(buf, &n, 5, 0, 0, 1, 1, 1, 1,   9, 9, 9, 0);
    push_vert_tex(buf, &n, 6, 0, 0, 1, 1, 1, 1,   9, 9, 9, 0);
    push_vert_tex(buf, &n, 5, 1, 0, 1, 1, 1, 1,   9, 9, 9, 0);

    char text[8192];
    int rc = run_writer_cap(buf, n, &CAP_TEX, &OPT_WELD, text, sizeof text);
    ASSERT_INT("texcoord-normals returns 2 triangles", rc, 2);

    Ply p;
    ASSERT_TRUE("texcoord-normals parses", parse_ply(text, &p));
    ASSERT_INT("texcoord-normals nverts", p.nverts, 6);
    ASSERT_INT("texcoord-normals nfaces", p.nfaces, 2);
    for (int v = 0; v < p.nverts; v++) {
        if (p.vx[v] < 2.0f) {          /* authored triangle -> +X */
            ASSERT_FLT("authored normal nx=1", p.nx[v], 1.0f);
            ASSERT_FLT("authored normal nz=0", p.nz[v], 0.0f);
        } else {                       /* NO_NORMALS triangle -> synthesized +Z */
            ASSERT_FLT("synthesized normal nz=1", p.nz[v], 1.0f);
            ASSERT_FLT("synthesized normal nx=0", p.nx[v], 0.0f);
        }
    }
}

int main(void) {
    printf("=== mesh_ply tests ===\n\n");
    test_single_triangle();
    test_quad_two_faces();
    test_shared_edge_weld();
    test_skip_alignment();
    test_points_and_lines();
    test_point_line_authored_normals();
    test_solidified_points_and_lines();
    test_error_cases();
    test_empty_buffer();
    test_color_clamp();
    test_srgb_decode();
    test_texcoord_normals();

    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "mesh_ply");
}
