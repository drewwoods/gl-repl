/*
 * postprocess_filter.h - Scene-viewport post-processing.
 *
 * Self-contained scene-layer effect invoked at the very end of
 * render3d_draw_scene(), after the accumulation resolve, on the fully
 * resolved scene image. Pure fixed-function GL (no shaders, no FBOs):
 * the resolved scene rect is copied into one texture and redrawn.
 *
 * Exposed in the Config menu, persisted via @cfg. The selected mode flows
 * through Render3dRenderConfig so the effect also works for the non-REPL
 * render3d_demo binary and src/scene/ stays REPL-independent. The effect only
 * touches the scene viewport rect; the code panel and other 2D UI stay crisp.
 */
#ifndef RENDER3D_POSTPROCESS_FILTER_H
#define RENDER3D_POSTPROCESS_FILTER_H

typedef enum Render3dPostFilterMode {
    RENDER3D_POST_FILTER_OFF = 0,
    RENDER3D_POST_FILTER_CHROMATIC_ABERRATION,
    RENDER3D_POST_FILTER_VIGNETTE,
    RENDER3D_POST_FILTER_SCANLINES,
    RENDER3D_POST_FILTER_FILM_GRAIN,
    RENDER3D_POST_FILTER_COUNT
} Render3dPostFilterMode;

/* Unified macro list driving the post-processing configuration cycle.
 * Contains: (suffix, view3d_mode, frame_mode, name_str, symbol_str) */
#define POST_FILTER_LIST(X) \
    X(OFF, RENDER3D_POST_FILTER_OFF, RENDER3D_POST_FILTER_OFF, "Off", "GLR_POST_FILTER_OFF") \
    X(CHROMATIC_ABERRATION_SCENE, RENDER3D_POST_FILTER_CHROMATIC_ABERRATION, RENDER3D_POST_FILTER_OFF, "Chromatic aberration (scene)", "GLR_POST_FILTER_CHROMATIC_ABERRATION_SCENE") \
    X(CHROMATIC_ABERRATION_FRAME, RENDER3D_POST_FILTER_OFF, RENDER3D_POST_FILTER_CHROMATIC_ABERRATION, "Chromatic aberration (frame)", "GLR_POST_FILTER_CHROMATIC_ABERRATION_FRAME") \
    X(VIGNETTE_SCENE, RENDER3D_POST_FILTER_VIGNETTE, RENDER3D_POST_FILTER_OFF, "Vignette (scene)", "GLR_POST_FILTER_VIGNETTE_SCENE") \
    X(VIGNETTE_FRAME, RENDER3D_POST_FILTER_OFF, RENDER3D_POST_FILTER_VIGNETTE, "Vignette (frame)", "GLR_POST_FILTER_VIGNETTE_FRAME") \
    X(SCANLINES_SCENE, RENDER3D_POST_FILTER_SCANLINES, RENDER3D_POST_FILTER_OFF, "Scanlines (scene)", "GLR_POST_FILTER_SCANLINES_SCENE") \
    X(SCANLINES_FRAME, RENDER3D_POST_FILTER_OFF, RENDER3D_POST_FILTER_SCANLINES, "Scanlines (frame)", "GLR_POST_FILTER_SCANLINES_FRAME") \
    X(FILM_GRAIN_SCENE, RENDER3D_POST_FILTER_FILM_GRAIN, RENDER3D_POST_FILTER_OFF, "Film grain (scene)", "GLR_POST_FILTER_FILM_GRAIN_SCENE") \
    X(FILM_GRAIN_FRAME, RENDER3D_POST_FILTER_OFF, RENDER3D_POST_FILTER_FILM_GRAIN, "Film grain (frame)", "GLR_POST_FILTER_FILM_GRAIN_FRAME")

typedef enum {
#define POST_FILTER_ENUM_ENTRY(suffix, view3d_mode, frame_mode, name_str, symbol_str) GLR_POST_FILTER_##suffix,
    POST_FILTER_LIST(POST_FILTER_ENUM_ENTRY)
#undef POST_FILTER_ENUM_ENTRY
    POST_FILTER_COUNT
} GlrPostFilterConfigState;

/* Human-readable name for status text. Out-of-range -> "Off". */
const char *render3d_postprocess_filter_mode_name(Render3dPostFilterMode mode);

/* Invalidate the cached texture handle / dimensions. Deletes the live
 * texture if a context still owns it. Called from render3d_init_gl()
 * so a fresh GL context never reuses a stale texture name. */
void render3d_postprocess_filter_reset(void);

/* Apply the selected filter to the scene rect (GL bottom-left window
 * coords: the same rect render3d_draw_scene() rendered into via
 * glViewport). No-op for RENDER3D_POST_FILTER_OFF or invalid rects. */
void render3d_postprocess_filter_render(Render3dPostFilterMode mode,
                                     int sx, int sy, int sw, int sh);

#endif /* RENDER3D_POSTPROCESS_FILTER_H */
