/*
 * glr_camera.h - Scene-camera state and orbit/pan/zoom controls.
 *
 * Owns the 3D camera pose used by scene rendering and the full mutation
 * surface around it: direct setters, easing, drag handling, wheel/keyboard
 * zoom velocity, momentum decay, and the per-frame tick.
 *
 * Pointer tracking stays local to this module. Callers that also want the
 * frame snapshot's pointer state updated for UI consumers must write that
 * separately through ui_state; glr_camera intentionally does not reach into
 * UI state.
 *
 * Older storage/accessor and drag-math splits are now secondary history;
 * callers should treat this as the single camera owner.
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
} GlrCameraState;

typedef enum {
    GLR_CAMERA_CONTROL_3D = 0,
    GLR_CAMERA_CONTROL_2D = 1
} GlrCameraControlMode;

/* ---- Accessors ------------------------------------------------------ */

GlrCameraState  glr_camera(void);
GlrCameraState *glr_camera_mut(void);
GlrCameraControlMode glr_camera_control_mode(void);
int              glr_camera_target_active(void);
void             glr_camera_set_control_mode(GlrCameraControlMode mode);
void             glr_camera_set(float rx, float ry, float dist,
                                float tx, float ty, float tz,
                                float motion_glow);
void             glr_camera_set_orbit(float rx, float ry);
void             glr_camera_set_pan(float tx, float ty, float tz);
void             glr_camera_set_distance(float dist);
void             glr_camera_set_motion_glow(float motion_glow);
void             glr_camera_ease_to(float rx, float ry, float dist,
                                    float tx, float ty, float tz);
void             glr_camera_reset_default(void);

/* Smoothly recenter the orbit target on the world origin, keeping the
 * current orbit angles and zoom. Uses the same easing as
 * glr_camera_ease_to so it is not an instant jump. */
void             glr_camera_focus_origin(void);

/* Smoothly return the whole camera pose to its built-in default and
 * restore 3D control mode. The eased counterpart of
 * glr_camera_reset_default (which snaps instantly). */
void             glr_camera_ease_to_default(void);

/* Capture/restore for state round-trip tests and undo paths. The
 * snapshot is a value copy of the camera struct; restoring overwrites
 * the live state. Pointer cache, target easing, and momentum velocities
 * are NOT part of the snapshot — those are transient session state. */
void glr_camera_capture(GlrCameraState *out);
void glr_camera_restore(const GlrCameraState *snapshot);

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

/* Install the default ReplExportCameraBridge so src/repl/export.c can
 * emit/parse the `// camera` block and `g_angle` preamble without
 * referencing glr_camera_* directly. The bridge implementation lives
 * in glr_camera_export.c and is installed once during app-service setup.
 * The decouple-plan step number is historical detail, not the primary
 * reason this API exists. */
void glr_camera_export_install_bridge(void);

#endif /* GLR_CAMERA_H */
