#ifndef GLPROBE_EXTRACT_H
#define GLPROBE_EXTRACT_H

#include <stdio.h>

/* glprobe_extract -- turn a GL_FEEDBACK capture into geometry you can open
 * somewhere else: a PLY mesh, or a gl-repl-importable C snippet.
 *
 * This is the extraction half of glprobe. The diagnostic half (glprobe.h)
 * answers "why can't I see it?"; this one answers "give me the mesh". The
 * capture is the same feedback stream, but the pass that produces it must be
 * set up differently, and the difference is the whole reason this is a
 * separate entry point:
 *
 *   - The projection is REPLACED with a containing ortho for the pass. Feedback
 *     clips to the view volume, so a capture taken under the app's own
 *     perspective silently loses everything off-screen or beyond the far
 *     plane -- fine for a report about the visible frame, fatal for an
 *     extractor. Overriding it is possible because apps set the projection in
 *     their reshape callback, not per frame.
 *   - gluLookAt is neutralized for the pass, so the modelview carries only the
 *     app's model transforms and coordinates come back in WORLD space with no
 *     inverse to apply and no camera baked into the mesh.
 *
 * Both live in the preload front-end, which owns the interposition; this file
 * only turns the resulting float stream into files. It calls no GL.
 */

/* Where the capture's window coordinates came from, so they can be inverted.
 * Mirrors the ortho + viewport + depth range the extract pass installed.
 * (Same shape as MeshPlyCapture, which is not an accident -- PLY output hands
 * these straight through.) */
typedef struct {
    float ortho_r;                 /* glOrtho half-extent; cube is [-R, R]^3 */
    int   vp_x, vp_y, vp_w, vp_h;
    float depth_near, depth_far;
} GlProbeExtractCapture;

/* The app's camera, recovered from the gluLookAt the extractor snooped, in the
 * orbit form gl-repl's `// camera` block uses:
 *
 *     glTranslatef(0, 0, -dist)  Rx(rx)  Ry(ry)  glTranslatef(-t)
 *
 * so an extracted scene opens framed the way the original binary framed it
 * instead of at gl-repl's default pose. `present` is 0 when the app never
 * called gluLookAt (it built the view from raw transforms, or has no camera),
 * in which case no camera block is emitted and gl-repl keeps its own. */
typedef struct {
    int   present;
    float dist, rx, ry;    /* rx/ry in degrees, matching glRotatef */
    float tx, ty, tz;      /* orbit target in world space */
} GlProbeExtractCamera;

/* The lighting/material state the app had installed when one batch drew.
 *
 * Feedback carries a color per vertex, but for LIT geometry that color is
 * meaningless here: the extract pass turns lighting off so positions are not
 * distorted, and a lit object never calls glColor, so the stream comes back
 * holding whatever glColor happened to be current -- white, in practice. The
 * color of a lit surface lives in its material, which is state the extractor
 * has to shadow separately and re-emit per batch. Without this the whole
 * extraction is one flat glColor3f(1, 1, 1). */
typedef struct {
    int   lit;              /* GL_LIGHTING was on: emit material, not glColor */
    int   color_material;   /* GL_COLOR_MATERIAL was on: glColor drives diffuse,
                               so per-vertex color is meaningful again */
    int   has_material;     /* the app set a material at all */
    float diffuse[4], ambient[4], specular[4];
    float shininess;
} GlProbeExtractBatch;

typedef struct {
    /* Emit only the primitives inside the Nth glPassThrough batch (the
     * material/texture segmentation the preload front-end injects). -1 emits
     * everything. Batch selection is what makes the gl-repl path usable: one
     * object out of a frame usually fits the editor's command budget where a
     * whole frame does not. */
    int batch;

    /* Stop after this many triangles (<= 0 = no limit). Applied after batch
     * filtering. */
    int max_tris;

    /* Digits after the decimal point in emitted coordinates. 0 = 4. Lower
     * makes shorter lines; the capture's own precision is about 1e-5 of the
     * fitted cube, so past ~5 the extra digits are noise. */
    int precision;

    /* For the C emitter: a comment naming where the geometry came from. */
    const char *source_note;

    /* Emitted as a `// camera` block when .present is set. */
    GlProbeExtractCamera camera;

    /* Emitted as glClearColor when .has_clear_color is set -- snooped from the
     * app so the extracted scene keeps its background. */
    int   has_clear_color;
    float clear_color[4];

    /* Per-batch lighting/material state, indexed by the glPassThrough marker
     * value. NULL emits geometry with per-vertex color only, which is correct
     * only for a wholly unlit scene. */
    const GlProbeExtractBatch *batches;
    int batch_count;
} GlProbeExtractOptions;

typedef struct {
    int tris_written;
    int verts_written;
    int lines_written;    /* source lines in the emitted snippet (C only) */
    int tris_skipped;     /* dropped by max_tris */
} GlProbeExtractStats;

/* Write the capture as a gl-repl-importable C snippet.
 *
 * The output is the plain snippet form gl-repl's examples use -- `// @cfg`
 * directives, an optional `// camera` block, then the commands themselves --
 * not the full standalone-C export wrapper, because the importer treats a file
 * with no "Snippet start" marker as one snippet from the first line.
 *
 * Normals are NOT emitted: feedback does not carry them, and a
 * `@cfg auto_normals = 1` header makes gl-repl derive them per facet instead.
 * That is much cheaper than a glNormal3f per vertex, but it is NOT free --
 * gl-repl materializes the derived normals as real document rows at runtime,
 * so the live command count ends up above what was written to the file (312
 * triangles measured 941 emitted -> 1022 live). Treat `lines_written` as a
 * lower bound and leave headroom. glColor3f is emitted only when the color
 * actually changes, which is a straight saving with no such caveat.
 *
 * gl-repl's editor buffer holds MAX_EDITOR_COMMANDS (1024) source commands as
 * a fixed array, so `stats->lines_written` plus the auto-normal rows above is
 * what must fit; use opts->batch and opts->max_tris to stay under it.
 *
 * Returns 0 on success, -1 on a malformed stream or an I/O error. */
int glprobe_extract_write_repl_c(FILE *out, const float *feedback,
                                 int float_count,
                                 const GlProbeExtractCapture *cap,
                                 const GlProbeExtractOptions *opts,
                                 GlProbeExtractStats *stats);

#endif /* GLPROBE_EXTRACT_H */
