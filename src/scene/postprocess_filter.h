/*
 * postprocess_filter.h - Experimental scene-viewport post-processing.
 *
 * Self-contained scene-layer effect invoked at the very end of
 * scene_render_3d_scene(), after the accumulation resolve, on the fully
 * resolved scene image. Pure fixed-function GL (no shaders, no FBOs):
 * the resolved scene rect is copied into one texture and redrawn.
 *
 * Experimental: not in the Config menu, not persisted via @cfg, no
 * GlrConfigKey. Controlled only by the hidden Ctrl+N shortcut. The
 * selected mode flows through SceneRenderConfig so the effect also
 * works for the non-REPL scene_demo binary and src/scene/ stays
 * REPL-independent. The effect only touches the scene viewport rect;
 * the code panel and other 2D UI stay crisp.
 */
#ifndef SCENE_POSTPROCESS_FILTER_H
#define SCENE_POSTPROCESS_FILTER_H

typedef enum ScenePostFilterMode {
    SCENE_POST_FILTER_OFF = 0,
    SCENE_POST_FILTER_CHROMATIC_ABERRATION,
    SCENE_POST_FILTER_COUNT
} ScenePostFilterMode;

/* Human-readable name for status text. Out-of-range -> "Off". */
const char *scene_postprocess_filter_mode_name(int mode);

/* Invalidate the cached texture handle / dimensions. Deletes the live
 * texture if a context still owns it. Called from scene_render_init_gl()
 * so a fresh GL context never reuses a stale texture name. */
void scene_postprocess_filter_reset(void);

/* Apply the selected filter to the scene rect (GL bottom-left window
 * coords: the same rect scene_render_3d_scene() rendered into via
 * glViewport). No-op for SCENE_POST_FILTER_OFF or invalid rects. */
void scene_postprocess_filter_render(int mode, int sx, int sy,
                                     int sw, int sh);

#endif /* SCENE_POSTPROCESS_FILTER_H */
