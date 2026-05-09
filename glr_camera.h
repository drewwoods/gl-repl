/*
 * glr_camera.h - Viewport camera state, accessors, and orbit/pan/zoom controls.
 *
 * Owns the scene-camera struct (`ReplCameraState`) and the entirety of
 * its read/write surface. Drag handling, momentum decay, orbit/pan/zoom
 * math, and per-frame tick all live here too — what used to be split
 * between `src/ui/state.{c,h}` (storage + accessors) and
 * `repl_camera_controls.{c,h}` (drag math) is now one module.
 *
 * Pointer tracking: the module keeps its own internal pointer cache
 * for drag-delta computation. Callers that also want the global
 * `ReplPointerState` updated for snapshot consumers must call
 * `ui_state_pointer_set_*` themselves — `glr_camera` does not reach
 * into UI state.
 */
#ifndef GLR_CAMERA_H
#define GLR_CAMERA_H

/* The 3D scene-camera pose. Render builds the modelview from these
 * fields each frame; drag/pan/zoom controls mutate them. The name
 * preserves the legacy `Repl` prefix until a broader type-rename
 * sweep lands. */
typedef struct {
    float rx;
    float ry;
    float dist;
    float tx;
    float ty;
    float tz;
    float motion_glow;
    int   auto_rotate;
} ReplCameraState;

/* ---- Accessors ------------------------------------------------------ */

ReplCameraState  glr_camera(void);
ReplCameraState *glr_camera_mut(void);
void             glr_camera_set(float rx, float ry, float dist,
                                float tx, float ty, float tz,
                                float motion_glow);
void             glr_camera_set_orbit(float rx, float ry);
void             glr_camera_set_pan(float tx, float ty, float tz);
void             glr_camera_set_distance(float dist);
void             glr_camera_set_motion_glow(float motion_glow);
void             glr_camera_reset_default(void);

/* Capture/restore for state round-trip tests and undo paths. The
 * snapshot is a value copy of the camera struct; restoring overwrites
 * the live state. Pointer cache and momentum velocities are NOT part
 * of the snapshot — those are transient session state. */
void glr_camera_capture(ReplCameraState *out);
void glr_camera_restore(const ReplCameraState *snapshot);

/* ---- Controls (drag, momentum) -------------------------------------- */

/* Reset camera + drag state to defaults: clears any drag, momentum,
 * and the internal pointer/button cache. */
void glr_camera_controls_reset(void);

/* Update the internal pointer cache (for drag deltas). Window
 * coordinates. Does not touch UiState.pointer. */
void glr_camera_pointer_set(int x, int y);

/* Mouse button event. button is GLUT_LEFT/MIDDLE/RIGHT_BUTTON; state
 * is GLUT_DOWN/UP. Updates the internal pointer cache + button + drag
 * state. */
void glr_camera_mouse_event(int button, int state, int x, int y, int mods);

/* Add zoom velocity from scroll wheel or keyboard. Accumulates into
 * momentum; applied in glr_camera_tick(). */
void glr_camera_add_zoom_velocity(float delta);

/* Update camera during drag — computes delta from internal pointer
 * cache, applies rotation/pan/zoom, then advances the pointer cache. */
void glr_camera_drag_motion(int x, int y);

/* Per-frame tick: applies momentum, decays velocities, advances the
 * auto-rotate animation. */
void glr_camera_tick(void);

/* Step 4a of feature/decouple-repl-from-gl-repl-alt.md: install the
 * default ReplExportCameraBridge so repl_export.c can emit/parse the
 * `// camera` block + `static float g_angle = N.NNNNf;` preamble
 * without referencing glr_camera_*. The bridge implementation lives
 * in glr_camera_export.c. Called once at app startup from
 * glr_app_install_app_services. */
void glr_camera_export_install_bridge(void);

#endif /* GLR_CAMERA_H */
