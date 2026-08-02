#ifndef MESH_PLY_H
#define MESH_PLY_H

#include <stdio.h>  /* FILE */

/* Pure PLY mesh writer for OpenGL GL_FEEDBACK (GL_3D_COLOR) buffers.
 *
 * Parses a feedback float stream (per vertex: window-coord x y z + RGBA),
 * inverts the known ortho + viewport + depth-range transform back to world
 * coordinates, fan-triangulates polygons, optionally welds shared vertices
 * and smooths normals, then writes an ASCII PLY document. Point and line
 * primitives are exported too: each point becomes a loose vertex (the PLY
 * point-cloud convention - a vertex referenced by no face), and each line
 * segment becomes an `element edge` record (`property int vertex1/vertex2`,
 * the CloudCompare/Blender convention; the edge element is only emitted when
 * the capture contains lines, so triangle-only output is unchanged). Callers
 * may also request triangle proxies for points and lines because mesh-only
 * viewers such as Xcode ignore loose vertices and edge elements.
 *
 * This module calls NO GL functions and includes NO GL header - it only
 * reads a plain float buffer, so it is fully unit-testable with synthetic
 * buffers and no GL context. The GL-coupled capture that fills the buffer
 * lives in src/app/glr_mesh_export.c. See
 * docs/plans/.../ply-feedback-export.md. */

/* Floats per feedback vertex. GL_3D_COLOR mode = 7 (x y z r g b a). When the
 * capture also records a per-vertex normal in the texture channel
 * (GL_3D_COLOR_TEXTURE), each vertex is 11 floats: x y z, r g b a, then the
 * texcoord (s t r q) with the encoded WORLD-space normal in (s, t, r). */
#define MESH_PLY_FLOATS_PER_VERTEX     7
#define MESH_PLY_FLOATS_PER_VERTEX_TEX 11

/* Out-of-band glPassThrough sentinels the executor emits to mark whether the
 * texcoords of the following polygons carry an encoded normal. The executor is
 * the only passthrough emitter, so these can't collide with real data (the
 * whole reason for using an out-of-band marker rather than an in-band tag).
 * The parser defaults to "synthesize" until it sees a NORMALS marker. */
#define MESH_PLY_PASS_NORMALS     1.0f   /* following texcoords are normals    */
#define MESH_PLY_PASS_NO_NORMALS  0.0f   /* synthesize (solids, tess, gaps)    */

/* OpenGL feedback token markers. These are frozen OpenGL spec values
 * (unchanged since GL 1.0), redefined here so the pure writer needs no GL
 * header. glr_mesh_export.c STATIC_ASSERTs them against the real
 * GL_*_TOKEN macros so any drift fails at compile time. */
enum {
    MESH_PLY_TOK_PASS_THROUGH = 0x0700,
    MESH_PLY_TOK_POINT        = 0x0701,
    MESH_PLY_TOK_LINE         = 0x0702,
    MESH_PLY_TOK_POLYGON      = 0x0703,
    MESH_PLY_TOK_BITMAP       = 0x0704,
    MESH_PLY_TOK_DRAW_PIXEL   = 0x0705,
    MESH_PLY_TOK_COPY_PIXEL   = 0x0706,
    MESH_PLY_TOK_LINE_RESET   = 0x0707
};

/* The fixed transform glr_mesh_export installed for the feedback pass, used
 * to invert window coordinates back to world space. With modelview = identity
 * and projection = glOrtho(-R, R, -R, R, -R, R), the inverse per axis is:
 *   ndc = 2*(win - vp_origin)/vp_size - 1   (x, y)
 *   ndc_z = 2*(win_z - depth_near)/(depth_far - depth_near) - 1
 *   world_{x,y} =  ndc_{x,y} * R
 *   world_z     = -ndc_z     * R            (glOrtho negates z) */
typedef struct {
    float ortho_r;                 /* glOrtho half-extent R (cube is [-R, R]^3) */
    int   vp_x, vp_y, vp_w, vp_h;  /* glViewport origin + size */
    float depth_near, depth_far;   /* glDepthRange (capture uses 0, 1) */
    int   floats_per_vertex;       /* feedback record stride: 7 (GL_3D_COLOR) or
                                      11 (GL_3D_COLOR_TEXTURE, normals in texcoord).
                                      0 is treated as 7. */
} MeshPlyCapture;

typedef struct {
    int   weld;            /* 1: dedup vertices by (quantized pos, 8-bit color) */
    float weld_eps;        /* position quantization grid; ~1e-4 matches ortho
                              precision at R=1000. <= 0 falls back to a default. */
    int   smooth_normals;  /* 1: average face normals across welded verts.
                              Smoothing requires welding, so welding is only
                              applied when both weld and smooth_normals are set;
                              otherwise every face keeps its own (flat) verts. */
    int   triangulate;     /* reserved; n-gons are always fan-triangulated */
    int   srgb_decode;     /* 1: treat the captured RGB as display-referred
                              (sRGB-encoded) and decode it to linear light
                              before quantizing to 8-bit. The REPL has no color
                              management, so glColor values are effectively
                              sRGB-encoded; color-managed viewers (Blender,
                              real-time engines) that read PLY vertex colors as
                              linear otherwise render them washed out. Applies
                              to R/G/B only; alpha stays linear. Default 0 =
                              raw pass-through (matches sRGB-naive viewers and
                              the on-screen look). */
    float primitive_radius_scale;
                           /* > 0: also turn points into octahedra and line
                              segments into capped six-sided tubes so mesh-only
                              PLY viewers display them. This is the tube radius
                              as a fraction of the captured scene's largest
                              world-space span; point radius is twice that.
                              0 preserves loose-vertex / edge-only output. */
} MeshPlyOptions;

/* Per-primitive counts of what a mesh_ply_write call emitted, for status
 * reporting. `points` counts loose point vertices (already included in
 * `verts`); `edges` counts line segments written to the edge element. */
typedef struct {
    int verts, tris, edges, points;
} MeshPlyStats;

/* Parse feedback[0 .. float_count) and write an ASCII PLY mesh to `out`.
 * When `stats` is non-NULL it receives the emitted per-primitive counts
 * (zeroed on error).
 *
 * Returns the number of triangles written (>= 0) on success, or a NEGATIVE
 * value on a parse error (unknown / misaligned / truncated token) or an I/O
 * error (fprintf/ferror). An empty buffer is success with 0 triangles and
 * still writes a valid 0-vertex / 0-face header. On a mid-stream I/O failure
 * the file may be partially written; the caller surfaces an error status. */
int mesh_ply_write(FILE *out, const float *feedback, int float_count,
                   const MeshPlyCapture *cap, const MeshPlyOptions *opts,
                   MeshPlyStats *stats);

#endif /* MESH_PLY_H */
