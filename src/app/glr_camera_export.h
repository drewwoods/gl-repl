/*
 * glr_camera_export.h - Camera bridge installer for the REPL export subsystem.
 *
 * Exposes the registration hook to connect the controller/camera subsystem
 * to the neutral export module.
 */
#ifndef GLR_CAMERA_EXPORT_H
#define GLR_CAMERA_EXPORT_H

/* Install the default ReplExportCameraBridge so src/repl/ can format the
 * tagged `@camera` transform rows and apply a resolved pose without
 * referencing glr_camera_* directly. The bridge implementation lives in
 * glr_camera_export.c and is installed once during app-service setup. */
void glr_camera_export_install_bridge(void);

/* Precondition: glr_ctrl_view_record_external_3d_pose must be called only from
 * a specific point in the display frame, after the camera modelview is loaded.
 * An example-mode pose apply therefore only *enqueues* the record; the
 * controller drains it at that point via the take-pending call below, which
 * returns 1 (and clears the pending flag) when a record was waiting. */
void glr_ctrl_view_record_external_3d_pose(float rx, float ry, float tz);
int  glr_camera_export_take_pending_3d_pose(float *rx, float *ry, float *tz);

#endif /* GLR_CAMERA_EXPORT_H */
