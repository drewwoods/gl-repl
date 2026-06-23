/*
 * render_types.h - Shared scene render context types.
 *
 * Defines the config snapshot render.c and its helper renderers exchange:
 * execute callbacks, light descriptors, camera/environment state, theme/fade
 * inputs, and the per-frame derived context wrapper.
 */
#ifndef RENDER3D_RENDER_TYPES_H
#define RENDER3D_RENDER_TYPES_H

#include "themes.h"
#include "render3d_transition.h"   /* Render3dXnPhase for the overlay fade fields */
#include "postprocess_filter.h" /* Render3dPostFilterMode */
#include "gl_includes.h"        /* GLenum for Render3dLight.id */

#if !defined(APIENTRY)
#define APIENTRY
#endif

/* GL_NV_fog_distance tokens — present in glext.h (Linux/Homebrew and the
 * macOS SDK) but not on the Apple-GLUT framework path (gl_includes.h pulls
 * only OpenGL/gl.h there) nor in the bundled GL stubs. Define the registry
 * values when absent; guarded so a real glext.h still wins. Consumed by the
 * scene passes that opt into radial fog via
 * Render3dRenderConfig.nv_fog_distance_supported (city backdrop, ocean/radar
 * grids). */
#ifndef GL_FOG_DISTANCE_MODE_NV
#define GL_FOG_DISTANCE_MODE_NV 0x855A
#endif
#ifndef GL_EYE_RADIAL_NV
#define GL_EYE_RADIAL_NV 0x855B
#endif

#define MAX_LIGHTS 4

/* Glacial blue-white shared by the Frozen Lake grid (under-ice
 * viewport tint + ice-mist fog colour) and the Polar Day backdrop's
 * horizon stop, so the grid's mist dissolves seamlessly into the sky
 * with no colour seam at the grid extent. */
#define RENDER3D_GLACIAL_TINT_R 0.58f
#define RENDER3D_GLACIAL_TINT_G 0.74f
#define RENDER3D_GLACIAL_TINT_B 0.86f

typedef struct {
    GLenum   id;         /* GL_LIGHT0 .. GL_LIGHT3 */
    int      enabled;
    /* True when pos[] is interpreted in eye (camera-relative) space.
     * The runtime pushes POSITION for these slots once at identity
     * modelview, and the exporter routes their POSITION line to init()
     * instead of display(). Set by render3d_lights_apply_theme based on
     * the chosen theme. */
    int      pos_is_eye_space;
    float    pos[4];     /* xyz + w (0=directional, 1=positional) */
    float    diffuse[4];
    float    ambient[4];
    float    specular[4];
} Render3dLight;

typedef struct Render3dRgba {
    float r, g, b, a;
} Render3dRgba;

/* Why the scene is invoking the execute callback this frame.
 * Side-effecting callbacks (audio, RNG advance, dirty-flag writes)
 * should run only on MAIN_FILL; non-fill purposes are scaffolding
 * passes whose output is consumed before the frame is presented and
 * whose side effects would corrupt the frame's main state.
 *
 * Renamed from the unused_ placeholder so callers can branch on
 * purpose without changing the function-pointer signature. */
typedef enum Render3dExecutePurpose {
    RENDER3D_EXEC_MAIN_FILL = 0,   /* the rendered geometry pass */
    RENDER3D_EXEC_DEPTH_PROBE,     /* render3d_probe_eye_dist feedback walk */
    RENDER3D_EXEC_WIREFRAME_HIDDEN_LINES, /* hidden-line effect: all edges */
    RENDER3D_EXEC_WIREFRAME_DEPTH_FILL,   /* hidden-line effect: depth-only fill */
    RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES /* hidden-line effect: visible edges */
} Render3dExecutePurpose;

typedef enum Render3dWireframeMode {
    RENDER3D_WIREFRAME_OFF = 0,
    RENDER3D_WIREFRAME_PLAIN,
    RENDER3D_WIREFRAME_HIDDEN,
    RENDER3D_WIREFRAME_COUNT
} Render3dWireframeMode;

/* Accumulation-buffer effect mode. OFF = single pass (no accum); AA =
 * jitter-the-frustum antialiasing across accum_passes samples; BLUR /
 * BLUR_CAMERA = motion blur (the caller supplies a per-pass setup callback
 * that varies the camera / animation time across the samples). BLUR blurs
 * camera motion when the camera moves, else the animation time; BLUR_CAMERA
 * blurs only camera motion and falls back to AA when the camera is still.
 * For any blur mode the scene renders with jitter 0 (blur and AA jitter are
 * never combined). */
typedef enum Render3dAccumEffect {
    RENDER3D_ACCUM_EFFECT_OFF = 0,
    RENDER3D_ACCUM_EFFECT_AA,
    RENDER3D_ACCUM_EFFECT_BLUR,
    RENDER3D_ACCUM_EFFECT_BLUR_CAMERA,
} Render3dAccumEffect;

/* True for the motion-blur effect modes (both drive the per-pass hook). */
#define RENDER3D_ACCUM_EFFECT_IS_BLUR(e) \
    ((e) == RENDER3D_ACCUM_EFFECT_BLUR || (e) == RENDER3D_ACCUM_EFFECT_BLUR_CAMERA)

/* Per-call context the scene passes back to the user's geometry
 * callback. Currently a single purpose enum; more frame-derived
 * metadata can land here without changing the function-pointer
 * signature. */
typedef struct Render3dExecuteContext {
    Render3dExecutePurpose purpose;
} Render3dExecuteContext;

/* Called by render.c to emit user geometry. May be NULL (geometry
 * is silently skipped; scene background/grid/axes/lights still render).
 * The callback gets only a scene-supplied opaque context plus the user_data
 * the caller stashed when building the config — any REPL state (flat
 * program, current PC, alpha overrides for fade passes, etc.) is the
 * caller's responsibility, carried through user_data. */
typedef void (*Render3dExecuteProgramFn)(const Render3dExecuteContext *ctx,
                                      void *user_data);

typedef struct Render3dFocusVertex {
    float pos[3];
    int valid;
} Render3dFocusVertex;

/* Snapshot of all per-frame inputs that helper renderers need to read
 * without sampling globals again.  render.c fills this once at frame
 * start, then passes it to grid/axes/overlay helpers. */
typedef struct Render3dRenderConfig {
    /* --- Execute hook --- */
    Render3dExecuteProgramFn execute_fn;          /* NULL = no geometry */
    void                 *execute_user_data;

    /* --- Optional post-fill hook ---
     * Invoked once per pass between the main user-geometry fill and the
     * scene helpers (grid / axes / backdrop / overlays). The REPL
     * controller installs this to overlay fading-replay geometry on top
     * of the main fill; non-REPL callers leave it NULL. */
    void (*post_fill_fn)(void *user_data);
    void  *post_fill_user_data;

    /* --- Optional post-overlays hook ---
     * Invoked at the end of the pass (after lights_render, before the
     * outermost glPopAttrib). The REPL controller uses this for
     * vertex-number / normal-vector overlays that walk the user's
     * program; non-REPL callers leave it NULL. */
    void (*post_overlays_fn)(void *user_data);
    void  *post_overlays_user_data;

    /* --- Background clear color (RGBA) ---
     * Populated by the controller from the user's CMD_CLEAR_COLOR (or a
     * default if none was set). The scene module no longer scans the
     * flat program for this; it's a pre-resolved float[4]. */
    float clear_color[4];

    /* --- Animation --- */
    float anim_time;

    /* --- Scene viewport rectangle (the region the scene helpers
     *     render into; the window viewport may be larger). --- */
    int render3d_x;
    int render3d_y;
    int render3d_w;
    int render3d_h;

    /* --- Camera state (read-only inputs) ---
     * The actual modelview transform is no longer applied by the scene module —
     * callers populate GL_MODELVIEW themselves before render3d_draw_scene()
     * (the controller uses glr_camera_load_modelview from src/app/glr_camera.h;
     * render3d_demo inlines the matrix calls). These fields are still passed in
     * because grid/axes themes orient themselves to camera angle, the
     * orbit-target gizmo is sized by cam_dist, and the gizmo position comes
     * from cam_tx/ty/tz. */
    float cam_dist;
    float cam_rx;
    float cam_ry;
    float cam_tx;
    float cam_ty;
    float cam_tz;
    float cam_motion_glow; /* 0 = orbit-target gizmo hidden */

    /* Projection blend: 0 = orthographic XY view, 1 = perspective 3D.
     * Intermediate values load a blended projection matrix for the
     * 2D<->3D transition. */
    float projection_mix;

    /* --- Rendering quality --- */
    int multisample_enabled;
    int line_smooth_enabled;
    int use_accum;          /* accumulation buffer available (--noaccum gate) */
    int accum_effect;       /* Render3dAccumEffect: OFF / AA / BLUR */
    int accum_passes;       /* resolved sample count: 1,2,4,8,12,16 */
    int use_accum_aa_scissors; /* scissor the accum loop to the scene rect
                                * (skip the dead region under the code panel).
                                * Off by default — see CFG_DEFAULT_USE_ACCUM_AA_SCISSORS. */

    /* --- Optional per-pass blur hook ---
     * Invoked once before each accumulation pass when accum_effect == BLUR
     * (pass_idx in [0, pass_count)). The controller installs this to load an
     * interpolated camera modelview (camera blur) and/or set the predef-t
     * sub-step + reflatten (time blur), and may overwrite the cam_* fields of
     * the passed-in per-pass config copy so scene helpers blur with the
     * camera. The scene renders these passes with jitter 0. NULL for AA / OFF
     * and for non-REPL callers (BLUR then degrades to the AA jitter path). */
    /* clang-format off — keep on one line so the flat-view pointer guard
     * skips this function pointer (it ignores lines containing "(*"). */
    void (*setup_subframe_fn)(void *user_data, int pass_idx, int pass_count, struct Render3dRenderConfig *pass_config);
    void  *setup_subframe_user_data;

    /* --- Lighting --- */
    int        user_lighting_enabled;
    Render3dLight lights[MAX_LIGHTS];
    int        show_light_indicators;

    /* --- Environment --- */
    Render3dBackdropMode backdrop_mode;
    /* Runtime point-parameter capability plus the loaded entry point,
     * mirrored from glr_ctrl_init_gl via the REPL executor. The
     * backdrop uses the proc to reset GL_POINT_DISTANCE_ATTENUATION
     * without taking a scene-layer dependency on REPL/controller code.
     * Non-REPL callers (render3d_demo) leave both zero via memset — the
     * safe default: never call the entry point unless a caller has
     * confirmed support and supplied a callable proc. */
    int point_parameter_supported;
    void (APIENTRY *point_parameter_proc)(GLenum pname, const GLfloat *params);
    /* Runtime GL_NV_fog_distance capability, detected once in
     * glr_ctrl_init_gl and mirrored here. When set, the passes whose
     * distance fog wraps geometry around the camera — the city backdrop
     * and the ocean/radar grid themes — switch to true radial eye distance
     * (GL_EYE_RADIAL_NV) so their fringes stop swimming as the camera
     * orbits. Scoped per-pass and confined by each pass's
     * GL_ALL_ATTRIB_BITS (GL_FOG_BIT) push/pop, so the eye-plane-tuned
     * themes are untouched. 0 for non-detecting callers (render3d_demo) via
     * memset — the safe default. */
    int nv_fog_distance_supported;
    /* Experimental scene-viewport post-processing.
     * Runtime-only (Ctrl+N); never persisted via @cfg. */
    Render3dPostFilterMode post_filter_mode;
    Render3dWireframeMode wireframe;

    /* --- Grid and axes ---
     * grid_theme/axes_theme are the *effective* (machine `current`)
     * theme to draw; *_opacity is the transition fade (0..1, applied
     * after alpha_scale so OUT stays authoritative).
     *
     * `*_xn_phase` is an advisory direction hint (FADE_IN /
     * FADE_OUT / STEADY) the controller fills in from the transition
     * machine, intended as a future input for fog-direction or
     * other phase-aware effects. RESERVED — read by tests only
     * today (verifying the controller forwards the value); no scene
     * renderer reads them at runtime. Keep populated so the contract
     * stays observable. */
    int          grid_theme;
    float        grid_opacity;
    Render3dXnPhase grid_xn_phase;   /* RESERVED — see comment above */
    int          grid_extent_idx;
    int          grid_major_idx;
    int          axes_theme;
    float        axes_opacity;
    Render3dXnPhase axes_xn_phase;   /* RESERVED — see comment above */
    float grid_major_steps[GRID_MAJOR_COUNT];
    float grid_extents[GRID_EXTENT_COUNT];

    /* --- Focus marker (currently forwarded for tests / future grid use) --- */
    Render3dFocusVertex focus;

    /* --- Visual scaling --- */
    float alpha_scale; /* alpha boost to counter dark-bg crush; 1.0 = no change */
    float grid_brightness; /* user grid-line alpha multiplier (Grid brightness cfg); 1.0 = no change */
} Render3dRenderConfig;

/* Derived state that helper renderers should consume instead of recomputing
 * from globals. Wraps the snapshot Render3dRenderConfig so helpers can be
 * extended with frame-derived fields here without changing every
 * helper's parameter list. */
typedef struct Render3dFrameRenderContext {
    Render3dRenderConfig config;
} Render3dFrameRenderContext;

#endif /* RENDER3D_RENDER_TYPES_H */
