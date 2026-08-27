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
#include "gl_includes.h"        /* GLenum for Render3dLight.id */
#include <math.h>

#if !defined(APIENTRY)
#define APIENTRY
#endif

/* GL_NV_fog_distance tokens - present in glext.h (Linux/Homebrew and the
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

/* Defined in render.h, which includes this header - the buffer-inspection
 * hooks below only pass it by pointer, so a tag declaration is enough and
 * the include direction stays render.h -> render_types.h. */
struct Render3dProjectionDesc;

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
     * The runtime submits these positions with an identity modelview, and the
     * exporter places their POSITION line in display() before the camera
     * transform. Set by render3d_lights_apply_theme based on the theme. */
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
    RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES, /* hidden-line effect: visible edges */
    RENDER3D_EXEC_WINDING                  /* winding view: two-sided front/back fill */
} Render3dExecutePurpose;

#define WIREFRAME_LIST(X) \
    X(OFF)                \
    X(PLAIN)              \
    X(HIDDEN)

typedef enum Render3dWireframeMode {
#define WIREFRAME_ENUM_ENTRY(name) WIREFRAME_##name,
    WIREFRAME_LIST(WIREFRAME_ENUM_ENTRY)
#undef WIREFRAME_ENUM_ENTRY
    WIREFRAME_COUNT
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
 * The callback gets only a renderer-supplied opaque context plus the user_data
 * the caller stashed when building the config - any program, replay, or
 * overlay state is the caller's responsibility, carried through user_data. */
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

    /* --- Optional scene-bounds query ---
     * Answers "where is the user's geometry, and how big is it?" in world
     * space. A helper that has to place something AROUND the scene - the
     * drone and fairy backdrops' flight paths, light falloff and spotlight
     * aim - calls this; every other helper ignores it.
     *
     * A hook rather than a plain min/max field for two reasons. Measuring
     * the scene is not free (the caller walks its whole program), and only
     * some backdrops in some frames want the answer, so pulling it keeps
     * the cost off every other frame. And a caller with no program to
     * measure - render3d_demo, which links no REPL at all - leaves it NULL
     * rather than being obliged to invent a box.
     *
     * Returns 1 having filled out_min/out_max, or 0 when the scene cannot
     * be measured (nothing drawn yet, degenerate geometry). Callers must
     * treat NULL and a 0 return identically: fall back to a fixed scale.
     * May be invoked more than once per frame - a backdrop asks in the pass
     * setup phase, when it places its lights, and again when it draws the
     * bodies, and accumulation repeats the pass - so an expensive
     * implementation should memoize per frame.
     */
    /* clang-format off - keep on one line so the flat-view pointer guard
     * skips this function pointer (it ignores lines containing "(*"). */
    int (*geometry_bounds_fn)(void *user_data, float out_min[3], float out_max[3]);
    /* clang-format on */
    void *geometry_bounds_user_data;

    /* --- Optional post-fill hook ---
     * Invoked once per pass between the main user-geometry fill and the
     * scene helpers (grid / axes / backdrop / overlays). A caller may install
     * this to add geometry on top of the main fill; callers that do not need
     * that phase leave it NULL. */
    void (*post_fill_fn)(void *user_data);
    void  *post_fill_user_data;

    /* --- Optional post-overlays hook ---
     * Invoked at the end of the pass (after lights_render, before the
     * outermost glPopAttrib). A caller may install this for geometry-reporting
     * overlays; callers that do not need it leave it NULL. */
    void (*post_overlays_fn)(void *user_data);
    void  *post_overlays_user_data;

    /* --- Optional post-resolve overlays hook ---
     * Invoked ONCE per frame on the fully resolved scene image - after
     * the accumulation resolve (glAccum GL_RETURN), or after the single
     * pass when accumulation is off - with the canonical jitter-free
     * projection re-applied and the caller's camera modelview live.
     * Bitmap-text annotation (vertex-number labels) draws here: inside
     * the accum loop each AA/blur sub-pass would stamp the pixel-snapped
     * glyphs at a slightly different position, ghosting the text. A caller
     * that needs resolved-image annotations installs this; otherwise it is
     * left NULL. */
    void (*post_resolve_overlays_fn)(void *user_data);
    void  *post_resolve_overlays_user_data;

    /* --- Optional buffer-inspection hooks ---
     * Three neutral "here is a point where you may read or composite"
     * slots. render3d fires them and knows nothing about what for; the
     * embedding caller subscribes whatever wants to look at the framebuffer
     * (today the depth/stencil visualizations). All three fire
     * unconditionally when installed - the subscriber no-ops when its
     * own modes are off - which is what lets one hook serve a reader
     * that wants a single read per frame and one that composites per
     * accumulation pass. */

    /* Fires at the fill/helpers boundary of every accumulation pass:
     * user geometry and the post_fill_fn hook have written their
     * buffers, the backdrop/grid/axes helpers (which write depth of
     * their own) have not, so a read here sees geometry only.
     * is_final_pass is 0 on every pass but the last; subscribers that
     * want one read per frame gate on it, subscribers that composite
     * per pass ignore it. */
    void (*buffer_read_fn)(void *user_data, int is_final_pass,
                           int sx, int sy, int sw, int sh);
    void  *buffer_read_user_data;

    /* Fires after the helper passes, before the geometry-reporting edit
     * overlays, once per accumulation pass. For subscribers compositing
     * a sparse overlay that must stay UNDER the outlines/points/guides. */
    void (*buffer_pass_overlay_fn)(void *user_data, int is_final_pass,
                                   int sx, int sy, int sw, int sh);
    void  *buffer_pass_overlay_user_data;

    /* Fires ONCE per frame on the resolved image: after
     * post_resolve_overlays_fn, before the scene post-filter, so Post FX
     * applies uniformly across a Split seam. `proj` is the canonical
     * jitter-free active projection for this frame. For full-rect
     * replacements that belong on the resolved image. */
    void (*buffer_resolve_overlay_fn)(void *user_data,
                                      const struct Render3dProjectionDesc *proj,
                                      int sx, int sy, int sw, int sh);
    void  *buffer_resolve_overlay_user_data;

    /* --- Background colors (RGBA) ---
     * Two distinct jobs, deliberately not one field. Both arrive pre-resolved:
     * the render3d module receives colors, never a program model to inspect.
     *
     * `baseline_clear_color` is the GL clear-color state established before the
     * geometry walk - what a glClear with no preceding glClearColor uses. It is
     * the caller's fixed configuration default and must not track what previous
     * frames ended up showing, or host presentation history would change the
     * pixels the program's OWN clear writes (most visibly on a scene switch).
     *
     * `presentation_rgba` is the background the scene is understood to sit on:
     * what the grid / axes recede fog fades toward, and what the caller derived
     * `alpha_scale` from. A caller that observes its program's real clears feeds
     * that observation here, so the two colors legitimately differ. */
    float baseline_clear_color[4];
    float presentation_rgba[4];

    /* --- Animation --- */
    float anim_time;

    /* --- Scene viewport rectangle (the region the scene helpers
     *     render into; the window viewport may be larger). --- */
    int render3d_x;
    int render3d_y;
    int render3d_w;
    int render3d_h;

    /* --- Camera state (read-only inputs) ---
     * The render3d module does not apply the modelview transform; callers
     * populate GL_MODELVIEW before render3d_draw_scene(). These fields are
     * still passed in because grid/axes themes orient themselves to camera
     * angle, the orbit-target gizmo is sized by cam_dist, and the gizmo
     * position comes from cam_tx/ty/tz. */
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
    int use_accum;          /* accumulation buffer usable for this caller */
    int accum_effect;       /* Render3dAccumEffect: OFF / AA / BLUR */
    int accum_passes;       /* resolved sample count: 1..MAX_ACCUM_SAMPLES */
    int use_accum_aa_scissors; /* scissor the accum loop to the scene rect,
                                * skipping pixels outside it; caller policy. */

    /* --- Optional per-pass blur hook ---
     * Invoked once before each accumulation pass when accum_effect == BLUR
     * (pass_idx in [0, pass_count)). The caller may install this to load an
     * interpolated camera modelview and/or adjust per-pass animation or
     * program state, and may overwrite the cam_* fields of the passed-in
     * per-pass config copy so scene helpers blur with the camera. The scene
     * renders these passes with jitter 0. NULL for AA / OFF or for callers
     * that do not provide blur sampling (BLUR then degrades to the AA jitter
     * path). */
    /* clang-format off - keep on one line so the flat-view pointer guard
     * skips this function pointer (it ignores lines containing "(*"). */
    void (*setup_subframe_fn)(void *user_data, int pass_idx, int pass_count, struct Render3dRenderConfig *pass_config);
    void  *setup_subframe_user_data;

    /* --- Lighting --- */
    Render3dLight lights[MAX_LIGHTS];
    int        show_light_indicators;
    /* Slot 0..MAX_LIGHTS-1 of the light whose indicator to emphasize
     * (the cursor is on its glEnable/glDisable(GL_LIGHTn) line), or -1
     * for none. Only consulted when show_light_indicators is set. */
    int        highlight_light_slot;

    /* --- Environment --- */
    Render3dBackdropMode backdrop_mode;
    /* Runtime point-parameter capability plus the loaded entry point,
     * supplied by the caller's GL capability probe. The backdrop uses the
     * proc to reset GL_POINT_DISTANCE_ATTENUATION. Callers that have not
     * confirmed support leave both zero - the safe default never calls the
     * entry point without an explicit capability and function pointer. */
    int point_parameter_supported;
    void (APIENTRY *point_parameter_proc)(GLenum pname, const GLfloat *params);
    /* Runtime GL_NV_fog_distance capability, detected by the caller and
     * mirrored here. When set, the passes whose
     * distance fog wraps geometry around the camera - the city backdrop
     * and the ocean/radar grid themes - switch to true radial eye distance
     * (GL_EYE_RADIAL_NV) so their fringes stop swimming as the camera
     * orbits. Scoped per-pass and confined by each pass's
     * GL_ALL_ATTRIB_BITS (GL_FOG_BIT) push/pop, so the eye-plane-tuned
     * themes are untouched. 0 for callers that did not detect the extension
     * is the safe default. */
    int nv_fog_distance_supported;
    /* Scene-viewport post-processing selected by the caller. */
    Render3dPostFilterMode post_filter_mode;
    Render3dWireframeMode wireframe;
    /* Winding-visualization view: replace the normal user-color fill with a
     * single two-sided-lighting pass that paints front-facing polygons green
     * and back-facing (inside-out / mis-wound) polygons red, so winding
     * mistakes are visible. The caller's execute_fn must install a state
     * filter that suppresses the program's own material/lighting/cull
     * commands. The callback owns that filtering policy. 0 = off. */
    int winding_view;

    /* --- Grid and axes ---
     * grid_theme/axes_theme are the *effective* (machine `current`)
     * theme to draw; *_opacity is the transition fade (0..1, applied
     * after alpha_scale so OUT stays authoritative).
     *
     * `*_xn_phase` is an advisory direction hint (FADE_IN /
     * FADE_OUT / STEADY) the caller fills in from its transition
     * machine. The render3d renderer does not read these values at runtime;
     * keep them populated so tests can verify the forwarded contract. */
    Render3dGridTheme grid_theme;
    float        grid_opacity;
    Render3dXnPhase grid_xn_phase;   /* RESERVED - see comment above */
    int          grid_extent_idx;
    int          grid_major_idx;
    Render3dAxesTheme axes_theme;
    float        axes_opacity;
    Render3dXnPhase axes_xn_phase;   /* RESERVED - see comment above */
    float grid_major_steps[GRID_MAJOR_COUNT];
    float grid_extents[GRID_EXTENT_COUNT];

    /* --- Focus marker (forwarded in the frame snapshot; no renderer reads
     * it yet - tests only, reserved for future grid use) --- */
    Render3dFocusVertex focus;

    /* --- Visual scaling --- */
    /* Alpha boost to counter dark-bg crush; 1.0 = no change. Derived by the
     * caller from presentation_rgba above - the derivation is caller policy,
     * so render3d takes the scale and never recomputes it from the color. */
    float alpha_scale;
    float grid_brightness; /* user grid-line alpha multiplier (Grid brightness cfg); 1.0 = no change */
    /* The same Grid brightness cfg as the *setting* rather than as the derived
     * line multiplier above (Render3dGridBrightness; peer of grid_extent_idx /
     * grid_major_idx). A theme wanting its own per-step response - the
     * Checkerboard floor picks a fill and ink opacity per step - cannot get
     * there from grid_brightness, whose factors run past 4.0 and are shaped for
     * scaling faint lines. Out-of-range guards to NORMAL, so a zero-initialized
     * config reads as DIM by index, not as an error. */
    int grid_brightness_idx;
} Render3dRenderConfig;

/* Common rendering quality configuration (MSAA + line smooth). */
static inline void render3d_apply_quality_config(const Render3dRenderConfig *config) {
    if (config->multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (config->line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
}

/* Derived state that helper renderers should consume instead of recomputing
 * from config/globals. Wraps the snapshot Render3dRenderConfig and carries
 * derived frame-level pose (camera position and basis). */
typedef struct Render3dFrameRenderContext {
    Render3dRenderConfig config;

    /* Derived camera world-space pose (computed once in render3d_prepare_frame_context). */
    float camera_world_pos[3]; /* world-space eye position (x, y, z); y is camera height */

    /* 3x3 eye-to-world orientation basis mapping eye vectors to world vectors:
     * world_dir = basis * eye_dir.
     *   basis[0] = world vector for eye +X (right)
     *   basis[1] = world vector for eye +Y (up)
     *   basis[2] = world vector for eye +Z (back) */
    float camera_basis[3][3];
} Render3dFrameRenderContext;

/* Initialize a Render3dFrameRenderContext from a Render3dRenderConfig,
 * computing derived camera world-space position and 3x3 eye-to-world basis. */
static inline void render3d_prepare_frame_context(Render3dFrameRenderContext *ctx,
                                                 const Render3dRenderConfig *config) {
    if (!ctx || !config) return;
    ctx->config = *config;

    const float deg = 3.14159265358979323846f / 180.0f;
    float cx = cosf(config->cam_rx * deg), sx = sinf(config->cam_rx * deg);
    float cy = cosf(config->cam_ry * deg), sy = sinf(config->cam_ry * deg);

    /* Camera world position */
    ctx->camera_world_pos[0] = config->cam_tx - config->cam_dist * cx * sy;
    ctx->camera_world_pos[1] = config->cam_ty + config->cam_dist * sx;
    ctx->camera_world_pos[2] = config->cam_tz + config->cam_dist * cx * cy;

    /* 3x3 eye-to-world basis: world = Ry(-ry) * Rx(-rx) * eye */
    /* Basis vector 0: eye +X (right) */
    ctx->camera_basis[0][0] = cy;
    ctx->camera_basis[0][1] = 0.0f;
    ctx->camera_basis[0][2] = sy;

    /* Basis vector 1: eye +Y (up) */
    ctx->camera_basis[1][0] = sy * sx;
    ctx->camera_basis[1][1] = cx;
    ctx->camera_basis[1][2] = -cy * sx;

    /* Basis vector 2: eye +Z (back) */
    ctx->camera_basis[2][0] = -sy * cx;
    ctx->camera_basis[2][1] = sx;
    ctx->camera_basis[2][2] = cy * cx;
}

#endif /* RENDER3D_RENDER_TYPES_H */
