/* Pure PLY writer for GL_FEEDBACK (GL_3D_COLOR) buffers. See mesh_ply.h. */

#include "support/mesh_ply.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A triangle corner: world-space position + [0,1] RGBA color, plus the
 * optional authored WORLD-space normal recovered from the texcoord channel
 * (has_authored == 0 means "synthesize the normal for this corner"). */
typedef struct {
    float x, y, z, r, g, b, a;
    float anx, any, anz;
    int   has_authored;
} RawVert;
typedef struct { float x, y, z; } Vec3;

/* Weld key: position quantized to an integer grid + 8-bit color. Compared
 * field-by-field (never memcmp, which would read struct padding). */
typedef struct { int32_t qx, qy, qz; uint8_t r, g, b, a; } WeldKey;

static int token_at(const float *fb, int i) {
    return (int)lroundf(fb[i]);
}

static unsigned char to_u8(float c) {
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255;
    return (unsigned char)lroundf(c * 255.0f);
}

/* sRGB EOTF: decode a display-referred (sRGB-encoded) channel to linear light.
 * Used (optionally) before quantizing so color-managed viewers that read PLY
 * vertex colors as linear don't render them washed out. */
static float srgb_to_linear(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return (c <= 0.04045f) ? c / 12.92f
                           : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* Invert one feedback vertex (window coords + color) to world space. When
 * has_tex, also reads the encoded world-space normal from the texcoord
 * (s, t, r) at v[7..9]; q (v[10]) is unused. Whether that normal is *used*
 * (has_authored) is decided by the caller from the passthrough mode. */
static RawVert invert_vertex(const MeshPlyCapture *cap, const float *v, int has_tex) {
    RawVert rv;
    float dz = cap->depth_far - cap->depth_near;
    float ndc_x = 2.0f * (v[0] - (float)cap->vp_x) / (float)cap->vp_w - 1.0f;
    float ndc_y = 2.0f * (v[1] - (float)cap->vp_y) / (float)cap->vp_h - 1.0f;
    float ndc_z = (dz != 0.0f) ? (2.0f * (v[2] - cap->depth_near) / dz - 1.0f)
                               : (2.0f * v[2] - 1.0f);
    rv.x = ndc_x * cap->ortho_r;
    rv.y = ndc_y * cap->ortho_r;
    rv.z = -ndc_z * cap->ortho_r;  /* glOrtho maps world z -> -z/R */
    rv.r = v[3]; rv.g = v[4]; rv.b = v[5]; rv.a = v[6];
    rv.anx = has_tex ? v[7] : 0.0f;
    rv.any = has_tex ? v[8] : 0.0f;
    rv.anz = has_tex ? v[9] : 0.0f;
    rv.has_authored = 0;
    return rv;
}

static Vec3 face_normal(const RawVert *a, const RawVert *b, const RawVert *c) {
    float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
    float vx = c->x - a->x, vy = c->y - a->y, vz = c->z - a->z;
    Vec3 n;
    n.x = uy * vz - uz * vy;
    n.y = uz * vx - ux * vz;
    n.z = ux * vy - uy * vx;
    return n;
}

static WeldKey weld_key(const RawVert *v, float eps) {
    WeldKey k;
    k.qx = (int32_t)lroundf(v->x / eps);
    k.qy = (int32_t)lroundf(v->y / eps);
    k.qz = (int32_t)lroundf(v->z / eps);
    k.r = to_u8(v->r); k.g = to_u8(v->g); k.b = to_u8(v->b); k.a = to_u8(v->a);
    return k;
}

static int key_eq(const WeldKey *a, const WeldKey *b) {
    return a->qx == b->qx && a->qy == b->qy && a->qz == b->qz &&
           a->r == b->r && a->g == b->g && a->b == b->b && a->a == b->a;
}

static uint32_t key_hash(const WeldKey *k) {
    uint32_t h = 2166136261u;  /* FNV-1a over the key fields */
    const uint32_t parts[4] = {
        (uint32_t)k->qx, (uint32_t)k->qy, (uint32_t)k->qz,
        ((uint32_t)k->r) | ((uint32_t)k->g << 8) |
        ((uint32_t)k->b << 16) | ((uint32_t)k->a << 24)
    };
    for (int i = 0; i < 4; i++) {
        h = (h ^ parts[i]) * 16777619u;
    }
    return h;
}

static uint32_t next_pow2(uint32_t n) {
    uint32_t p = 16;
    while (p < n) p <<= 1;
    return p;
}

/* Append `n` bytes-worth growth helper: grow *arr (element size esz) to hold
 * at least `need` elements, doubling `*cap`. Returns 1 on success, 0 on OOM. */
static int ensure_cap(void **arr, int *cap, int need, size_t esz) {
    if (need <= *cap) return 1;
    int nc = (*cap > 0) ? *cap : 256;
    while (nc < need) nc <<= 1;
    void *p = realloc(*arr, (size_t)nc * esz);
    if (!p) return 0;
    *arr = p;
    *cap = nc;
    return 1;
}

int mesh_ply_bounds(const float *feedback, int float_count,
                    const MeshPlyCapture *cap,
                    float out_min[3], float out_max[3]) {
    if (float_count < 0 || (float_count > 0 && !feedback) || !cap ||
        !out_min || !out_max)
        return -1;

    int stride  = (cap->floats_per_vertex >= MESH_PLY_FLOATS_PER_VERTEX_TEX)
                      ? MESH_PLY_FLOATS_PER_VERTEX_TEX : MESH_PLY_FLOATS_PER_VERTEX;
    int has_tex = (stride >= MESH_PLY_FLOATS_PER_VERTEX_TEX);

    float mn[3] = {0}, mx[3] = {0};
    int nverts = 0;

    /* accumulate one vertex's inverted world position into mn/mx */
    #define MESH_PLY_BOUNDS_ACC(base) do {                                   \
        RawVert rv__ = invert_vertex(cap, (base), has_tex);                  \
        float p__[3] = { rv__.x, rv__.y, rv__.z };                          \
        for (int a__ = 0; a__ < 3; a__++) {                                  \
            if (nverts == 0 || p__[a__] < mn[a__]) mn[a__] = p__[a__];       \
            if (nverts == 0 || p__[a__] > mx[a__]) mx[a__] = p__[a__];       \
        }                                                                    \
        nverts++;                                                            \
    } while (0)

    /* Mirror mesh_ply_write's token walk, but only read vertex positions. */
    int i = 0;
    while (i < float_count) {
        int tok = token_at(feedback, i);
        i++;
        if (tok == MESH_PLY_TOK_POLYGON) {
            if (i >= float_count) return -1;            /* missing vertex count */
            int n = token_at(feedback, i);
            i++;
            if (n < 0 || (long)n * stride > (long)(float_count - i))
                return -1;                              /* truncated / misaligned */
            for (int k = 0; k < n; k++)
                MESH_PLY_BOUNDS_ACC(&feedback[i + k * stride]);
            i += n * stride;
        } else if (tok == MESH_PLY_TOK_POINT || tok == MESH_PLY_TOK_BITMAP ||
                   tok == MESH_PLY_TOK_DRAW_PIXEL || tok == MESH_PLY_TOK_COPY_PIXEL) {
            if (stride > float_count - i) return -1;
            MESH_PLY_BOUNDS_ACC(&feedback[i]);
            i += stride;                                /* 1 vertex */
        } else if (tok == MESH_PLY_TOK_LINE || tok == MESH_PLY_TOK_LINE_RESET) {
            if (2 * stride > float_count - i) return -1;
            MESH_PLY_BOUNDS_ACC(&feedback[i]);
            MESH_PLY_BOUNDS_ACC(&feedback[i + stride]);
            i += 2 * stride;                            /* 2 vertices */
        } else if (tok == MESH_PLY_TOK_PASS_THROUGH) {
            if (1 > float_count - i) return -1;
            i += 1;                                     /* the pass-through value */
        } else {
            return -1;                                  /* unknown token */
        }
    }

    #undef MESH_PLY_BOUNDS_ACC

    if (nverts > 0) {
        for (int a = 0; a < 3; a++) { out_min[a] = mn[a]; out_max[a] = mx[a]; }
    }
    return nverts;
}

int mesh_ply_write(FILE *out, const float *feedback, int float_count,
                   const MeshPlyCapture *cap, const MeshPlyOptions *opts) {
    int rc = -1;

    /* Parsed triangle corners (3 per triangle) + per-triangle face normal. */
    RawVert *corners = NULL; int corners_cap = 0, ncorners = 0;
    Vec3 *fnorm = NULL;      int fnorm_cap = 0, ntris = 0;
    RawVert *poly = NULL;    int poly_cap = 0;   /* reused per polygon */

    /* Output (welded or 1:1) vertices. */
    Vec3 *opos = NULL; unsigned char (*ocol)[4] = NULL; Vec3 *onrm = NULL;
    Vec3 *oauth = NULL; int *oauth_n = NULL;  /* authored-normal sum + count per vert */
    WeldKey *okey = NULL;
    int *table = NULL;       /* open-addressing index map (vidx + 1; 0 = empty) */
    int *corner_vidx = NULL; /* corner -> output vertex index */

    if (float_count < 0 || (float_count > 0 && !feedback) || !cap || !opts)
        return -1;

    float eps = (opts->weld_eps > 0.0f) ? opts->weld_eps : 1e-4f;

    /* Feedback record stride: 7 (GL_3D_COLOR) or 11 (GL_3D_COLOR_TEXTURE, with
     * the world-space normal encoded in the texcoord channel). */
    int stride  = (cap->floats_per_vertex >= MESH_PLY_FLOATS_PER_VERTEX_TEX)
                      ? MESH_PLY_FLOATS_PER_VERTEX_TEX : MESH_PLY_FLOATS_PER_VERTEX;
    int has_tex = (stride >= MESH_PLY_FLOATS_PER_VERTEX_TEX);

    /* --- Pass 1: parse the token stream, fan-triangulate polygons. -------
     * normals_mode follows the out-of-band glPassThrough markers: when a
     * NORMALS marker is in effect, the following polygons' texcoords are
     * authored world-space normals; otherwise the normal is synthesized
     * (solids, GLU tess, gaps). Defaults to synthesize until a marker says so. */
    int normals_mode = 0;
    int i = 0;
    while (i < float_count) {
        int tok = token_at(feedback, i);
        i++;
        if (tok == MESH_PLY_TOK_POLYGON) {
            if (i >= float_count) goto done;            /* missing vertex count */
            int n = token_at(feedback, i);
            i++;
            if (n < 0 || (long)n * stride > (long)(float_count - i))
                goto done;                              /* truncated / misaligned */
            if (!ensure_cap((void **)&poly, &poly_cap, n, sizeof *poly))
                goto done;
            for (int k = 0; k < n; k++) {
                poly[k] = invert_vertex(cap, &feedback[i + k * stride], has_tex);
                if (opts->srgb_decode) {
                    poly[k].r = srgb_to_linear(poly[k].r);
                    poly[k].g = srgb_to_linear(poly[k].g);
                    poly[k].b = srgb_to_linear(poly[k].b);  /* alpha stays linear */
                }
                if (has_tex && normals_mode) {
                    float l2 = poly[k].anx * poly[k].anx +
                               poly[k].any * poly[k].any +
                               poly[k].anz * poly[k].anz;
                    poly[k].has_authored = (l2 > 1e-12f);  /* nonzero = real normal */
                }
            }
            i += n * stride;
            /* Fan-triangulate: (0, k, k+1) for k in 1..n-2. */
            for (int k = 1; k + 1 < n; k++) {
                if (!ensure_cap((void **)&corners, &corners_cap, ncorners + 3,
                                sizeof *corners) ||
                    !ensure_cap((void **)&fnorm, &fnorm_cap, ntris + 1,
                                sizeof *fnorm))
                    goto done;
                corners[ncorners + 0] = poly[0];
                corners[ncorners + 1] = poly[k];
                corners[ncorners + 2] = poly[k + 1];
                fnorm[ntris] = face_normal(&poly[0], &poly[k], &poly[k + 1]);
                ncorners += 3;
                ntris++;
            }
        } else if (tok == MESH_PLY_TOK_POINT || tok == MESH_PLY_TOK_BITMAP ||
                   tok == MESH_PLY_TOK_DRAW_PIXEL || tok == MESH_PLY_TOK_COPY_PIXEL) {
            if (stride > float_count - i) goto done;
            i += stride;                                /* skip 1 vertex */
        } else if (tok == MESH_PLY_TOK_LINE || tok == MESH_PLY_TOK_LINE_RESET) {
            if (2 * stride > float_count - i) goto done;
            i += 2 * stride;                            /* skip 2 vertices */
        } else if (tok == MESH_PLY_TOK_PASS_THROUGH) {
            if (1 > float_count - i) goto done;
            normals_mode = (fabsf(feedback[i] - MESH_PLY_PASS_NORMALS) < 0.5f);
            i += 1;                                     /* the pass-through value */
        } else {
            goto done;                                  /* unknown token -> error */
        }
    }

    /* --- Build the output vertex set. ------------------------------------ */
    /* Welding requires shared vertices, which only makes sense for smooth
     * shading; flat shading keeps each face's corners distinct. */
    int do_weld = opts->weld && opts->smooth_normals;
    int nverts = 0;

    /* Upper bound on output vertices is one per corner (no welding). */
    int vcap = ncorners > 0 ? ncorners : 1;
    corner_vidx = malloc((size_t)vcap * sizeof *corner_vidx);
    opos = malloc((size_t)vcap * sizeof *opos);
    ocol = malloc((size_t)vcap * sizeof *ocol);
    if (!corner_vidx || !opos || !ocol) goto done;

    if (do_weld && ncorners > 0) {
        uint32_t tsize = next_pow2((uint32_t)ncorners * 2u);
        uint32_t mask = tsize - 1;
        table = calloc(tsize, sizeof *table);
        okey = malloc((size_t)ncorners * sizeof *okey);
        if (!table || !okey) goto done;
        for (int c = 0; c < ncorners; c++) {
            WeldKey k = weld_key(&corners[c], eps);
            uint32_t h = key_hash(&k) & mask;
            for (;;) {
                if (table[h] == 0) {                    /* empty slot -> new vert */
                    table[h] = nverts + 1;
                    okey[nverts] = k;
                    opos[nverts].x = corners[c].x;
                    opos[nverts].y = corners[c].y;
                    opos[nverts].z = corners[c].z;
                    ocol[nverts][0] = to_u8(corners[c].r);
                    ocol[nverts][1] = to_u8(corners[c].g);
                    ocol[nverts][2] = to_u8(corners[c].b);
                    ocol[nverts][3] = to_u8(corners[c].a);
                    corner_vidx[c] = nverts;
                    nverts++;
                    break;
                }
                int vi = table[h] - 1;
                if (key_eq(&okey[vi], &k)) { corner_vidx[c] = vi; break; }
                h = (h + 1) & mask;
            }
        }
    } else {
        nverts = ncorners;                              /* 1:1, flat */
        for (int c = 0; c < ncorners; c++) {
            opos[c].x = corners[c].x;
            opos[c].y = corners[c].y;
            opos[c].z = corners[c].z;
            ocol[c][0] = to_u8(corners[c].r);
            ocol[c][1] = to_u8(corners[c].g);
            ocol[c][2] = to_u8(corners[c].b);
            ocol[c][3] = to_u8(corners[c].a);
            corner_vidx[c] = c;
        }
    }

    /* --- Resolve per-vertex normals. ------------------------------------- *
     * Authored normals (from the texcoord, where present) win; otherwise
     * synthesize from face normals. Both are accumulated across welded
     * vertices and normalized at the end. */
    int nslots = nverts > 0 ? nverts : 1;
    onrm    = calloc((size_t)nslots, sizeof *onrm);
    oauth   = calloc((size_t)nslots, sizeof *oauth);
    oauth_n = calloc((size_t)nslots, sizeof *oauth_n);
    if (!onrm || !oauth || !oauth_n) goto done;
    for (int t = 0; t < ntris; t++) {
        Vec3 fn = fnorm[t];
        for (int j = 0; j < 3; j++) {
            int vi = corner_vidx[3 * t + j];
            onrm[vi].x += fn.x; onrm[vi].y += fn.y; onrm[vi].z += fn.z;
        }
    }
    for (int c = 0; c < ncorners; c++) {
        if (corners[c].has_authored) {
            int vi = corner_vidx[c];
            oauth[vi].x += corners[c].anx;
            oauth[vi].y += corners[c].any;
            oauth[vi].z += corners[c].anz;
            oauth_n[vi]++;
        }
    }
    for (int v = 0; v < nverts; v++) {
        Vec3 nrm = (oauth_n[v] > 0) ? oauth[v] : onrm[v];   /* authored wins */
        float len = sqrtf(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
        if (len > 0.0f) { nrm.x /= len; nrm.y /= len; nrm.z /= len; }
        onrm[v] = nrm;
    }

    /* --- Write the PLY document. ----------------------------------------- */
    if (fprintf(out,
                "ply\n"
                "format ascii 1.0\n"
                "comment generated by gl-repl\n"
                "element vertex %d\n"
                "property float x\n"
                "property float y\n"
                "property float z\n"
                "property float nx\n"
                "property float ny\n"
                "property float nz\n"
                "property uchar red\n"
                "property uchar green\n"
                "property uchar blue\n"
                "property uchar alpha\n"
                "element face %d\n"
                "property list uchar int vertex_indices\n"
                "end_header\n",
                nverts, ntris) < 0)
        goto done;

    for (int v = 0; v < nverts; v++) {
        if (fprintf(out, "%.6f %.6f %.6f %.6f %.6f %.6f %u %u %u %u\n",
                    opos[v].x, opos[v].y, opos[v].z,
                    onrm[v].x, onrm[v].y, onrm[v].z,
                    ocol[v][0], ocol[v][1], ocol[v][2], ocol[v][3]) < 0)
            goto done;
    }
    for (int t = 0; t < ntris; t++) {
        if (fprintf(out, "3 %d %d %d\n", corner_vidx[3 * t + 0],
                    corner_vidx[3 * t + 1], corner_vidx[3 * t + 2]) < 0)
            goto done;
    }
    if (ferror(out)) goto done;

    rc = ntris;

done:
    free(corners);
    free(fnorm);
    free(poly);
    free(opos);
    free(ocol);
    free(onrm);
    free(oauth);
    free(oauth_n);
    free(okey);
    free(table);
    free(corner_vidx);
    return rc;
}
