/*
 * postprocess_filter.h - Experimental scene-viewport post-processing.
 *
 * Self-contained scene-layer effect invoked at the very end of
 * render3d_draw_scene(), after the accumulation resolve, on the fully
 * resolved scene image. Pure fixed-function GL (no shaders, no FBOs):
 * the resolved scene rect is copied into one texture and redrawn.
 *
 * Experimental: not in the Config menu, not persisted via @cfg, no
 * GlrConfigKey. Controlled only by the hidden Ctrl+N shortcut. The
 * selected mode flows through Render3dRenderConfig so the effect also
 * works for the non-REPL render3d_demo binary and src/scene/ stays
 * REPL-independent. The effect only touches the scene viewport rect;
 * the code panel and other 2D UI stay crisp.
 */
#ifndef RENDER3D_POSTPROCESS_FILTER_H
#define RENDER3D_POSTPROCESS_FILTER_H

typedef enum Render3dPostFilterMode {
    RENDER3D_POST_FILTER_OFF = 0,
    RENDER3D_POST_FILTER_CHROMATIC_ABERRATION,
    RENDER3D_POST_FILTER_VIGNETTE,
    RENDER3D_POST_FILTER_SCANLINES,
    RENDER3D_POST_FILTER_COUNT
} Render3dPostFilterMode;

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
