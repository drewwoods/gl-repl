#ifndef GLPROBE_H
#define GLPROBE_H

#include <stdio.h>  /* FILE */

/* glprobe -- GL_FEEDBACK geometry probe for standalone fixed-function samples.
 *
 * Drop-in debugging for "I drew something and nothing (or the wrong thing)
 * showed up". Runs a draw callback through glRenderMode(GL_FEEDBACK), which
 * returns the post-transform vertex stream WITHOUT rasterizing, then reports
 * what the pipeline actually saw. No pixels, no window, no guessing from a
 * screenshot.
 *
 * The point of the tool is the two capture modes, which split the one question
 * everybody actually has ("why can't I see it?") into two answerable ones:
 *
 *   glprobe_geometry()  -- captures under a KNOWN transform (identity
 *                          modelview + containing ortho) with lighting off.
 *                          Coordinates come back in the callback's own emitted
 *                          space and colors are raw glColor, both independent
 *                          of the camera. Answers: does the geometry exist, is
 *                          it where I think it is, is it degenerate?
 *
 *   glprobe_shading()   -- captures under the CURRENT matrices and CURRENT
 *                          lighting/material state. Feedback colors are
 *                          post-lighting, so this answers: is the geometry
 *                          actually lit, and does it land inside the viewport?
 *
 * Geometry clean + shading black => the mesh is fine and the lighting is
 * wrong. Geometry empty => the mesh is wrong (or clipped, or never issued).
 * That single comparison is what the tool exists for.
 *
 * glprobe calls only GL 1.1 + GLU (gluUnProject) and reuses the project's pure
 * PLY writer (src/support/mesh_ply.c) for mesh dumps, so a sample only needs
 * to compile glprobe.c + mesh_ply.c alongside it. See tools/glprobe/README.md.
 */

/* The geometry a probe run should exercise. Called once per capture, between
 * glRenderMode(GL_FEEDBACK) and glRenderMode(GL_RENDER). It may push/pop
 * matrices and change state freely; glprobe brackets the whole call in
 * glPushAttrib/glPopAttrib and matrix push/pop, so the visible frame is
 * undisturbed (feedback generates no fragments). */
typedef void (*GlProbeDrawFn)(void *user);

/* What the pipeline saw. Positions are always unprojected back out of window
 * space, so they read in whichever space the capture's transform defined:
 * geometry mode installs an identity modelview, so coordinates come back in
 * the draw callback's OWN emitted space (model space, plus whatever transforms
 * the callback applies itself); shading mode keeps the caller's matrices, so
 * coordinates come back in the space that was current at probe entry (world
 * space when probed after the camera is loaded). */
typedef struct GlProbeReport {
    /* Primitive census. `verts` counts every vertex in the stream; a polygon
     * of n corners contributes n verts and (n - 2) fan triangles. */
    int points, lines, polygons, verts, tris;

    /* Vertices whose position or color came back non-finite (NaN/Inf) -- the
     * usual fingerprint of a divide-by-zero or an uninitialized array. */
    int nonfinite;

    /* Polygons whose fan triangles are all (near) zero-area. A mesh that is
     * present but invisible from every angle usually reports these. */
    int degenerate;

    /* Axis-aligned bounds over all finite vertices. Valid when verts > 0. */
    float bbox_min[3], bbox_max[3];

    /* Per-vertex color statistics over the captured stream. In shading mode
     * these are POST-LIGHTING, which is what makes a black mesh diagnosable:
     * lum_max near 0 means the lighting math produced nothing, no matter how
     * correct the positions are. Luminance is 0.2126 R + 0.7152 G + 0.0722 B
     * on the raw (display-referred) channel values. */
    float lum_min, lum_max, lum_mean;
    float alpha_min, alpha_max;

    /* Vertices below GLPROBE_DARK_LUM -- "would read as black on screen". */
    int dark_verts;

    /* Shading mode only (0 in geometry mode, which installs its own containing
     * ortho and so never clips): vertices whose window coordinates fell outside
     * the viewport, and whose window z fell outside [0,1]. Non-zero `offscreen`
     * with a healthy vertex count means the camera is pointed elsewhere; a
     * non-zero `depth_clipped` means the near/far planes ate the geometry. */
    int offscreen, depth_clipped;

    /* 1 when the feedback buffer overflowed and the capture is TRUNCATED --
     * every count above is a lower bound. Raise the buffer via GlProbeOptions
     * and re-run before drawing conclusions. */
    int overflow;

    /* Which capture produced this report, for the printed header. */
    int mode;  /* GLPROBE_MODE_GEOMETRY | GLPROBE_MODE_SHADING | ..._AS_DRAWN | ..._NEUTRAL */

    /* Sub-reports, one per glPassThrough marker in the captured stream. The
     * in-process API emits no markers, so these stay empty; the LD_PRELOAD
     * front-end injects one whenever the app changes material, texture or
     * lighting state, which segments a whole-frame capture into per-object
     * batches without knowing anything about the app's source. Points into a
     * caller-owned array (see glprobe_analyze); never freed by glprobe. */
    struct GlProbeReport *batches;
    int batch_count;

    /* Set on a sub-report only: the marker value that opened this batch, and a
     * caller-supplied description of the state change it stood for. */
    float batch_id;
    char  batch_label[72];
} GlProbeReport;

enum {
    GLPROBE_MODE_GEOMETRY = 0,  /* known ortho, no camera, lighting off       */
    GLPROBE_MODE_SHADING  = 1,  /* live camera + lights                       */
    GLPROBE_MODE_AS_DRAWN = 2,  /* preload: whole frame, app state untouched  */
    GLPROBE_MODE_NEUTRAL  = 3   /* preload: whole frame, culling/clip/light off */
};

/* A vertex this dark reads as black against any normal backdrop. Chosen to sit
 * just above the ambient-only term of a typical dim material, so an unlit mesh
 * trips it and a merely dark-but-lit one does not. */
#define GLPROBE_DARK_LUM 0.05f

/* Batch rows glprobe_report_print emits before summarizing the rest. A scene
 * that changes material per tile produces hundreds; a report that lists them
 * all is unreadable, and the diagnostic question ("which object is unlit?") is
 * answered by the first few plus the verdict. */
#define GLPROBE_REPORT_BATCH_ROWS 32

typedef struct {
    /* Feedback buffer size in floats. 0 = 1M floats (~150k vertices), grown
     * x2 and re-run on overflow up to 64M. A capture that still overflows
     * reports overflow = 1 rather than failing. */
    int buffer_floats;

    /* Half-extent of the capture cube for GEOMETRY mode. 0 = 1000, which
     * contains any hand-authored scene at ~1e-4 precision. Geometry outside
     * the cube is clipped away, so raise this for genuinely huge scenes. */
    float ortho_r;

    /* Print the first N primitives vertex-by-vertex in glprobe_report_print.
     * 0 prints the summary only. Negative prints every primitive. */
    int dump_primitives;
} GlProbeOptions;

/* Capture `draw` under a known identity-modelview + ortho transform with
 * lighting, culling and polygon-mode forced to a neutral state.
 *
 * When `ply_path` is non-NULL the capture is also written there as an ASCII
 * PLY mesh (welded, smooth-normalled, with points and lines given triangle
 * proxies so mesh-only viewers show them). PLY output is geometry-mode only:
 * it depends on the known ortho inverse that shading mode does not install.
 *
 * Returns 0 on success, -1 if the capture or the PLY write failed. `out` may
 * be NULL if only the file is wanted. `opts` may be NULL for defaults. */
int glprobe_geometry(GlProbeDrawFn draw, void *user, const GlProbeOptions *opts,
                     GlProbeReport *out, const char *ply_path);

/* Capture `draw` under the caller's live matrices, viewport, lighting and
 * material state -- i.e. exactly what the visible frame would produce. Call it
 * from inside the display callback, after the camera is set up, so the report
 * describes the frame the user is looking at.
 *
 * Returns 0 on success, -1 on capture failure. `opts` may be NULL. */
int glprobe_shading(GlProbeDrawFn draw, void *user, const GlProbeOptions *opts,
                    GlProbeReport *out);

/* The transform a capture ran under, used to unproject window coordinates back
 * into a readable space. The in-process API fills this itself; the LD_PRELOAD
 * front-end assembles one from the live projection plus the view matrix it
 * snoops from gluLookAt, so its coordinates come out in world space. */
typedef struct {
    double mv[16], proj[16];
    int    vp[4];
} GlProbeXform;

/* Analyze an already-captured GL_3D_COLOR feedback stream -- the entry point
 * for front-ends that run their own glRenderMode(GL_FEEDBACK) pass, which the
 * two capture calls above cannot do for them (a whole-frame capture must not
 * touch the app's matrices, and only the caller knows what its markers mean).
 *
 * `feedback[0 .. float_count)` is parsed with 7 floats per vertex. When
 * `batches` is non-NULL the stream is also split at every GL_PASS_THROUGH
 * token into at most `max_batches` sub-reports, and out->batches/batch_count
 * are pointed at that caller-owned array. Marker values land in
 * batch_id; labeling them is the caller's job.
 *
 * `mode` is stamped onto the report (and every sub-report) for the printed
 * header. Returns 0, or -1 on a malformed/truncated stream. */
int glprobe_analyze(const float *feedback, int float_count,
                    const GlProbeXform *xf, int mode, GlProbeReport *out,
                    GlProbeReport *batches, int max_batches);

/* Print a human-readable report to `f` (NULL = stderr), tagged with `label`.
 * Lines that indicate a problem are prefixed with "!!" so a long log can be
 * grepped. `opts` supplies dump_primitives; NULL prints the summary only. */
void glprobe_report_print(const GlProbeReport *r, const char *label,
                          const GlProbeOptions *opts, FILE *f);

/* Convenience: run BOTH captures over the same draw callback and print the
 * pair, which is the comparison the tool is built around. Writes the geometry
 * capture to `ply_path` when non-NULL. Returns 0 if both captures succeeded.
 *
 * Must be called with the live camera already loaded (the shading half depends
 * on it). Returns -1 if either capture failed. */
int glprobe_diagnose(GlProbeDrawFn draw, void *user, const char *label,
                     const GlProbeOptions *opts, const char *ply_path, FILE *f);

#endif /* GLPROBE_H */
