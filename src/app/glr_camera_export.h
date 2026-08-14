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

/* An example-mode pose apply enqueues a record because the controller must
 * consume it at a specific display-frame point, after the camera modelview is
 * loaded. The take-pending call returns 1 (and clears the pending flag) when a
 * record was waiting. The non-consuming query lets the animation timer defer
 * a view transition until that newer pose reaches its frame-safe drain point. */
int  glr_camera_export_has_pending_3d_pose(void);
int  glr_camera_export_take_pending_3d_pose(float *rx, float *ry, float *tz);

#endif /* GLR_CAMERA_EXPORT_H */
