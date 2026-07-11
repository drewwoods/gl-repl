/*
 * glr_mesh_export.c -- GL_FEEDBACK capture for PLY mesh export.
 *
 * Installs a fixed capture transform (identity modelview, a containing
 * orthographic projection, a known viewport + depth range), disables
 * lighting / culling and forces fill mode, runs the live flat program in
 * glRenderMode(GL_FEEDBACK), then hands the raw float buffer to the pure
 * writer (src/support/mesh_ply.c). All GL state is saved/restored so the
 * visible frame is undisturbed (feedback produces no fragments).
 *
 * See docs/plans/.../ply-feedback-export.md.
 */

#include "app/glr_mesh_export.h"

#include "gl_includes.h"
#include "c_compat.h"  /* STATIC_ASSERT */

#include <stdio.h>
#include <stdlib.h>

#include "source_document.h"               /* source_document_view */
#include "support/mesh_ply.h"
#include "repl/host_effects.h"
#include "repl/pipeline.h"
#include "repl/executor.h"                  /* ReplExecutionOptions, repl_execute_program */
#include "repl/state_views.h"               /* repl_state_flat_program_* */
#include "editor/state.h"                   /* editor_state_edit_line */
#include "subsystems/replay/replay.h"       /* replay_exec_limit */
#include "subsystems/replay/replay_state.h" /* replay_active */

/* The pure writer redefines the feedback token markers so it needs no GL
 * header; pin them to the real GL values here so any drift is a compile
 * error rather than a silent runtime misparse. */
STATIC_ASSERT(MESH_PLY_TOK_PASS_THROUGH == GL_PASS_THROUGH_TOKEN, "passthrough token drift");
STATIC_ASSERT(MESH_PLY_TOK_POINT == GL_POINT_TOKEN, "point token drift");
STATIC_ASSERT(MESH_PLY_TOK_LINE == GL_LINE_TOKEN, "line token drift");
STATIC_ASSERT(MESH_PLY_TOK_POLYGON == GL_POLYGON_TOKEN, "polygon token drift");
STATIC_ASSERT(MESH_PLY_TOK_BITMAP == GL_BITMAP_TOKEN, "bitmap token drift");
STATIC_ASSERT(MESH_PLY_TOK_DRAW_PIXEL == GL_DRAW_PIXEL_TOKEN, "draw-pixel token drift");
STATIC_ASSERT(MESH_PLY_TOK_COPY_PIXEL == GL_COPY_PIXEL_TOKEN, "copy-pixel token drift");
STATIC_ASSERT(MESH_PLY_TOK_LINE_RESET == GL_LINE_RESET_TOKEN, "line-reset token drift");

/* Capture cube half-extent. Geometry beyond +-R world units is clipped by
 * feedback; R=1000 contains any hand-typed scene at ~1e-4 float precision. */
#define MESH_EXPORT_ORTHO_R 1000.0f
/* Capture viewport. Size is cosmetic for precision (world precision ~ R*2^-23
 * regardless of viewport), but must be installed so the inversion matches. */
#define MESH_EXPORT_VP      1024
#define MESH_FB_INITIAL     (1 << 20)   /* 1M floats (~43k triangles) */
#define MESH_FB_MAX         (64 << 20)  /* 64M float cap before giving up */

/* Run one feedback capture into buf[0..cap_floats). Returns the value count
 * written (>= 0) or < 0 on overflow, per glRenderMode(GL_RENDER). */
static int capture_feedback(float *buf, int cap_floats, int flat_count) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    /* World space: identity modelview (no camera), containing ortho. */
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(-MESH_EXPORT_ORTHO_R, MESH_EXPORT_ORTHO_R,
            -MESH_EXPORT_ORTHO_R, MESH_EXPORT_ORTHO_R,
            -MESH_EXPORT_ORTHO_R, MESH_EXPORT_ORTHO_R);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    /* Identity texture matrix: the executor encodes per-vertex world-space
     * normals into the texcoord channel, and feedback returns texcoords
     * transformed by the texture matrix — identity keeps them verbatim.
     * (glPushAttrib does not save matrix stacks, so push/pop explicitly.) */
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);

    glViewport(0, 0, MESH_EXPORT_VP, MESH_EXPORT_VP);
    glDepthRange(0.0, 1.0);

    /* Feedback honors lighting/polygon-mode/cull; force a known state so the
     * stream is polygon tokens with raw glColor for every face. */
    glDisable(GL_LIGHTING);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_CULL_FACE);

    /* GL_3D_COLOR_TEXTURE records x y z + RGBA + 4 texcoords per vertex; the
     * executor's encode pass stuffs the world-space normal into the texcoord
     * (s, t, r), bracketed by glPassThrough markers (decision: out-of-band). */
    glFeedbackBuffer(cap_floats, GL_3D_COLOR_TEXTURE, buf);
    glRenderMode(GL_FEEDBACK);

    repl_execute_program(&(ReplExecutionOptions){
        .flat_cmd_count          = flat_count,
        .program                 = repl_state_flat_program_view(),
        .text                    = source_document_view(),
        .encode_feedback_normals = 1,
    });

    int written = glRenderMode(GL_RENDER);

    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
    return written;
}

int glr_export_mesh_ply(const char *path, int srgb_decode) {
    if (!path) {
        repl_set_status_error("No .ply path");
        return -1;
    }

    /* Export runs outside the per-frame flatten-if-dirty in display_frame, so
     * make the flat program current first (decision 7: freshness). */
    if (repl_state_flat_program_dirty())
        repl_flatten_commands(editor_state_edit_line());

    /* Match "what's on screen": clamp to the replay PC when replaying. */
    int flat_count = replay_active() ? replay_exec_limit()
                                     : repl_state_flat_program_count();
    if (flat_count <= 0) {
        repl_set_status_error("Nothing to export");
        return -1;
    }

    /* Capture into a feedback buffer, growing x2 and re-running on overflow
     * (the buffer is only filled while rendering, so each retry re-runs). */
    int cap_floats = MESH_FB_INITIAL;
    float *buf = NULL;
    int written = -1;
    for (;;) {
        float *nb = realloc(buf, (size_t)cap_floats * sizeof *nb);
        if (!nb) {
            free(buf);
            repl_set_status_error("Out of memory capturing .ply");
            return -1;
        }
        buf = nb;

        written = capture_feedback(buf, cap_floats, flat_count);
        if (written >= 0)
            break;                         /* fit */
        if (cap_floats >= MESH_FB_MAX) {
            free(buf);
            repl_set_status_error("Scene too large to export to .ply");
            return -1;
        }
        cap_floats <<= 1;
        if (cap_floats > MESH_FB_MAX)
            cap_floats = MESH_FB_MAX;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(buf);
        repl_set_status_error("Could not open .ply file for writing");
        return -1;
    }

    MeshPlyCapture cap = {
        .ortho_r = MESH_EXPORT_ORTHO_R,
        .vp_x = 0, .vp_y = 0, .vp_w = MESH_EXPORT_VP, .vp_h = MESH_EXPORT_VP,
        .depth_near = 0.0f, .depth_far = 1.0f,
        .floats_per_vertex = MESH_PLY_FLOATS_PER_VERTEX_TEX,  /* GL_3D_COLOR_TEXTURE */
    };
    MeshPlyOptions opts = {
        .weld = 1, .weld_eps = 1e-3f, .smooth_normals = 1, .triangulate = 1,
        .srgb_decode = srgb_decode,
        /* Xcode/Quick Look ignore PLY loose vertices and edge elements. Keep
         * those records, but also emit small face meshes for broad viewers. */
        .primitive_radius_scale = 0.0025f,
    };
    MeshPlyStats stats;
    int ntris = mesh_ply_write(fp, buf, written, &cap, &opts, &stats);
    int close_err = (fclose(fp) != 0);
    free(buf);

    if (ntris < 0 || close_err) {
        repl_set_status_error("Failed to write .ply mesh");
        return -1;
    }

    /* "Exported 12 triangles[, 8 edges][, 3 points] to <path>" — edge/point
     * counts appear only when present; a triangle-less capture still leads
     * with "0 triangles" unless lines/points carried the scene. */
    char prims[96];
    int off = 0;
    if (stats.tris > 0 || (stats.edges == 0 && stats.points == 0))
        off += snprintf(prims + off, sizeof prims - (size_t)off,
                        "%d triangles", stats.tris);
    if (stats.edges > 0)
        off += snprintf(prims + off, sizeof prims - (size_t)off, "%s%d edges",
                        off > 0 ? ", " : "", stats.edges);
    if (stats.points > 0)
        snprintf(prims + off, sizeof prims - (size_t)off, "%s%d points",
                 off > 0 ? ", " : "", stats.points);

    char msg[224];
    snprintf(msg, sizeof msg, "Exported %s to %s", prims, path);
    repl_set_status(msg);
    return ntris;
}
