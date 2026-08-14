/*
 * glr_camera_export.c -- ReplExportCameraBridge implementation.
 *
 * The camera-block *format* owner: it formats live camera state into the
 * tagged transform rows saved files carry, and it applies a resolved pose
 * back onto the live camera. It no longer parses anything - reading a
 * camera out of a file is src/repl/camera_header.c's job, from the file's
 * own `@camera` role tags.
 *
 * One formatter, two projections that differ by one optional row:
 *   - with_anim_hook = 1 ("save"): emits the `@camera spin` row,
 *     `glRotatef(g_angle, 0,1,0)`, so the exported C animates through its
 *     file-scope g_angle. g_angle's initializer stays 0.0f - the yaw itself
 *     rides on the numeric `@camera ry` row.
 *   - with_anim_hook = 0: four pose rows, no hook. Used by the code-panel
 *     preview (ReplImportExportState.cam_lines) and the .glr writer, neither of which may show
 *     a g_angle the user cannot type.
 */
#include "app/glr_camera_export.h"
#include "repl/export.h"
#include "app/glr_camera.h"
#include "app/glr_ctrl.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ----- Formatting ------------------------------------------------------- */

static void cam_fill_block(ReplExportCameraBlock *block, int with_anim_hook) {
    GlrCameraState cam = glr_camera_destination();

    memset(block, 0, sizeof(*block));
    snprintf(block->lines[0], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(0.0000f, 0.0000f, %.4ff);   // @camera dist",
             -cam.dist);
    snprintf(block->lines[1], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);   // @camera rx", cam.rx);
    snprintf(block->lines[2], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(%.4ff, 0.0f, 1.0f, 0.0f);   // @camera ry", cam.ry);
    if (with_anim_hook)
        snprintf(block->lines[3], REPL_EXPORT_CAMERA_LINE_MAX,
                 "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);   // @camera spin");
    snprintf(block->lines[4], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(%.4ff, %.4ff, %.4ff);   // @camera pan",
             -cam.tx, -cam.ty, -cam.tz);
    block->present = 1;
}

/* ----- Pose transfer ---------------------------------------------------- */

static void cam_capture_pose(ReplCameraPose *out) {
    GlrCameraState cam = glr_camera_destination();

    if (!out)
        return;
    out->dist = cam.dist;
    out->rx   = cam.rx;
    out->ry   = cam.ry;
    out->tx   = cam.tx;
    out->ty   = cam.ty;
    out->tz   = cam.tz;
}

/* EXAMPLE also has to keep the controller's saved-3D snapshot aligned with
 * the example the user is now looking at: without it a 2D->3D restoration
 * lands on the pose captured at 2D entry instead of the newly loaded
 * example's angle. glr_ctrl_view_record_external_3d_pose has a frame-timing
 * precondition ("only after the camera modelview is loaded in the display
 * frame") that the example-load action path does not satisfy, so the record
 * is *enqueued* here and drained by the controller at its frame-safe point.
 * The neutral interface in src/repl/export.h never learns this exists. */
static int   g_pending_3d_record;
static float g_pending_3d_rx, g_pending_3d_ry, g_pending_3d_tz;

int glr_camera_export_has_pending_3d_pose(void) {
    return g_pending_3d_record;
}

int glr_camera_export_take_pending_3d_pose(float *rx, float *ry, float *tz) {
    if (!g_pending_3d_record)
        return 0;
    if (rx) *rx = g_pending_3d_rx;
    if (ry) *ry = g_pending_3d_ry;
    if (tz) *tz = g_pending_3d_tz;
    g_pending_3d_record = 0;
    return 1;
}

static void cam_apply_pose(const ReplCameraPose *pose,
                           ReplCameraApplyMode mode) {
    GlrCameraState target;

    if (!pose)
        return;

    glr_camera_capture(&target);
    target.rx   = pose->rx;
    target.ry   = pose->ry;
    target.dist = pose->dist;
    target.tx   = pose->tx;
    target.ty   = pose->ty;
    target.tz   = pose->tz;

    if (mode == REPL_CAMERA_APPLY_EXAMPLE) {
        glr_camera_set_scene_default(&target);
        glr_camera_ease_to(target.rx, target.ry, target.dist,
                           target.tx, target.ty, target.tz);
        g_pending_3d_record = 1;
        g_pending_3d_rx     = target.rx;
        g_pending_3d_ry     = target.ry;
        g_pending_3d_tz     = target.tz;
        return;
    }

    /* IMPORT and RESTORE both snap: an ease would leave the live camera
     * unchanged until the next tick, so an export taken in between would
     * write the OLD pose. They differ only in whether the pose becomes what
     * "Reset camera" returns to - a snapshot restore must not redefine it. */
    glr_camera_set_orbit(target.rx, target.ry);
    glr_camera_set_distance(target.dist);
    glr_camera_set_pan(target.tx, target.ty, target.tz);
    if (mode == REPL_CAMERA_APPLY_IMPORT)
        glr_camera_set_scene_default(&target);
}

/* ----- Bridge install ------------------------------------------------- */

static const ReplExportCameraBridge g_glr_export_camera_bridge = {
    .fill_block   = cam_fill_block,
    .capture_pose = cam_capture_pose,
    .apply_pose   = cam_apply_pose,
};

void glr_camera_export_install_bridge(void) {
    repl_export_install_camera_bridge(&g_glr_export_camera_bridge);
}
