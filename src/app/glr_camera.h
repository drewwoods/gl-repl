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
/* The camera's current ease *destination*: the active ease target when a
 * target ease is in flight, otherwise the live pose. Use this (not
 * glr_camera()) when a freshly-issued ease_to must win over the stale live
 * pose - e.g. the view-mode 3D->2D transition fires the frame after an
 * example's `// camera` block eases toward a new dist, before that ease has
 * ticked into the live pose. Reading glr_camera() there would flatten the
 * previous example's pose into 2D. */
GlrCameraState   glr_camera_destination(void);
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
/* Override the per-ease decay for the current target. Each call to
 * glr_camera_ease_to resets it to the global GLR_CAMERA_TARGET_DECAY,
 * so this must be called AFTER the ease_to that it should affect.
 * Clamped to [0, 1); lower = faster ease. A decay of 0.0f snaps to the
 * target on the next tick (no interpolation). */
void             glr_camera_set_target_decay(float decay);
void             glr_camera_reset_default(void);

/* Scoped deterministic-reconstruction policy used while a tour restores its
 * baseline and fast-replays an event prefix. New ease requests resolve
 * immediately to their destination, and glr_camera_tick is frozen, so a scene
 * load cannot expose the restored baseline pose and then visibly replay its
 * camera ease. Scopes may nest; every begin must have a matching end. Existing
 * target/momentum state restored from a snapshot remains untouched unless the
 * reconstructed prefix explicitly replaces it. */
void             glr_camera_reconstruction_begin(void);
void             glr_camera_reconstruction_end(void);

/* Smoothly recenter the orbit target on the world origin, keeping the
 * current orbit angles and zoom. Uses the same easing as
 * glr_camera_ease_to so it is not an instant jump. */
void             glr_camera_focus_origin(void);

/* Smoothly return the camera to its scene default pose (set by example
 * camera blocks), or the built-in default if no scene pose is set.
 * Restores 3D control mode. The eased counterpart of
 * glr_camera_reset_default (which snaps instantly). */
void             glr_camera_ease_to_default(void);

/* Record / clear the per-scene camera default. When set,
 * glr_camera_ease_to_default uses this pose instead of the built-in
 * default. Set by example camera blocks; cleared on scene transitions
 * and full resets. */
void glr_camera_set_scene_default(const GlrCameraState *pose);
void glr_camera_clear_scene_default(void);

/* Capture/restore for state round-trip tests and undo paths. The
 * snapshot is a value copy of the camera struct; restoring overwrites
 * the live state. Pointer cache, target easing, and momentum velocities
 * are NOT part of the snapshot - those are transient session state. */
void glr_camera_capture(GlrCameraState *out);
void glr_camera_restore(const GlrCameraState *snapshot);

/* Complete camera-runtime snapshot: everything glr_camera owns, unlike the
 * pose-only glr_camera_capture above. Covers the live pose, the ease target +
 * active flag + per-ease decay, the control mode, the scene-default pose + its
 * set flag, the internal pointer/button cache, the modifier mask, and all six
 * momentum velocities. Used by the tour baseline so Back / Done-restart put the
 * camera (and any in-flight ease/momentum) back exactly as the tour began. */
typedef struct {
    GlrCameraState       camera;
    GlrCameraState       target;
    int                  target_active;
    float                target_decay;
    GlrCameraControlMode control_mode;
    GlrCameraState       scene_default;
    int                  scene_default_set;
    int                  pointer_x, pointer_y, pointer_button;
    int                  mouse_mods;
    float                vel_rx, vel_ry, vel_tx, vel_ty, vel_tz, vel_zoom;
} GlrCameraRuntimeSnapshot;

void glr_camera_runtime_capture(GlrCameraRuntimeSnapshot *out);
void glr_camera_runtime_restore(const GlrCameraRuntimeSnapshot *snapshot);

/* ---- Modelview projection (camera apply) --------------------------- */

/* Minimal {pose} subset of GlrCameraState that scene callers need:
 * orbit pitch/yaw degrees, distance to target, and target world
 * position. Used as the input to glr_camera_load_modelview so the C
 * type system can tell six adjacent floats apart at the boundary.
 *
 * The audit's #11 finding was that render3d_apply_camera lived in
 * src/scene/render.c but the renderer refused to call it - a
 * scene-namespaced public function that scene code wouldn't touch
 * was hidden temporal coupling enforced by convention. The fix is
 * to move both the type and the helper into glr_camera (the app's
 * camera owner) so the scene module is purely the renderer and
 * callers are responsible for populating GL_MODELVIEW before each
 * render3d_draw_scene call. */
typedef struct GlrCameraPose {
    float rx, ry;       /* orbit pitch and yaw, degrees */
    float dist;         /* distance from target along the local Z axis */
    float tx, ty, tz;   /* target world position */
} GlrCameraPose;

/* Project the full camera state into the modelview-specific pose the
 * renderer consumes. This keeps call sites from open-coding the field
 * copy and drifting if the camera types grow new members later. */
GlrCameraPose glr_camera_pose_from_state(const GlrCameraState *state);

/* Load the modelview matrix with the orbit-camera transform: clear,
 * translate by distance, rotate by pitch/yaw, translate by target.
 * Pure GL state mutation; reads no globals. Callers invoke this once
 * per frame before render3d_draw_scene(). */
void glr_camera_load_modelview(const GlrCameraPose *pose);

/* Interpolate between two poses (f in [0,1]; f=0 -> a, f=1 -> b). Orbit
 * angles rx/ry use the shortest angular path (so a wrap across +-180
 * blurs the short way); dist/tx/ty/tz are linear. Used by the
 * accumulation motion-blur loop to sample intermediate camera poses. */
GlrCameraPose glr_camera_pose_lerp(const GlrCameraPose *a,
                                   const GlrCameraPose *b, float f);

/* True when two poses differ by more than the camera-ease epsilons on
 * any axis (angles via shortest delta). The motion-blur driver uses this
 * to decide camera-blur vs. time-blur for the frame. */
int glr_camera_pose_changed(const GlrCameraPose *prev,
                            const GlrCameraPose *cur);

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

/* Update camera during drag - computes delta from internal pointer
 * cache, applies rotation/pan/zoom, then advances the pointer cache. */
void glr_camera_drag_motion(int x, int y);

/* Per-frame tick: applies momentum, decays velocities, advances the
 * auto-rotate animation. */
void glr_camera_tick(void);

#endif /* GLR_CAMERA_H */
