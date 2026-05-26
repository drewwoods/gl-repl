/*
 * glr_camera_export.h - Camera bridge installer for the REPL export subsystem.
 *
 * Exposes the registration hook to connect the controller/camera subsystem
 * to the neutral export module.
 */
#ifndef GLR_CAMERA_EXPORT_H
#define GLR_CAMERA_EXPORT_H

/* Install the default ReplExportCameraBridge so src/repl/export.c can
 * emit/parse the `// camera` block and `g_angle` preamble without
 * referencing glr_camera_* directly. The bridge implementation lives
 * in glr_camera_export.c and is installed once during app-service setup. */
void glr_camera_export_install_bridge(void);

#endif /* GLR_CAMERA_EXPORT_H */
