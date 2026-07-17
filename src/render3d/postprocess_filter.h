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

#include "gl_includes.h"   /* GLint for the 2D-bracket matrix-mode snapshot */

typedef enum Render3dPostFilterMode {
    RENDER3D_POST_FILTER_OFF = 0,
    RENDER3D_POST_FILTER_CHROMATIC_ABERRATION,
    RENDER3D_POST_FILTER_VIGNETTE,
    RENDER3D_POST_FILTER_SCANLINES,
    RENDER3D_POST_FILTER_FILM_GRAIN,
    RENDER3D_POST_FILTER_COUNT
} Render3dPostFilterMode;

/* App config vocabulary for the two-row Post FX control:
 * scope answers where the effect is applied; effect answers which operation
 * is applied when the scope is not Off. */
#define POST_FX_SCOPE_LIST(X) \
    X(OFF,     "Off",     "GLR_POST_FX_SCOPE_OFF") \
    X(VIEW_3D, "3D View", "GLR_POST_FX_SCOPE_3D_VIEW") \
    X(FRAME,   "Frame",   "GLR_POST_FX_SCOPE_FRAME")

typedef enum {
#define POST_FX_SCOPE_ENUM_ENTRY(suffix, name_str, symbol_str) GLR_POST_FX_SCOPE_##suffix,
    POST_FX_SCOPE_LIST(POST_FX_SCOPE_ENUM_ENTRY)
#undef POST_FX_SCOPE_ENUM_ENTRY
    GLR_POST_FX_SCOPE_COUNT
} GlrPostFxScope;

#define POST_FX_EFFECT_LIST(X) \
    X(CHROMATIC_ABERRATION, RENDER3D_POST_FILTER_CHROMATIC_ABERRATION, "Chromatic aberration", "GLR_POST_FX_EFFECT_CHROMATIC_ABERRATION") \
    X(VIGNETTE,             RENDER3D_POST_FILTER_VIGNETTE,             "Vignette",             "GLR_POST_FX_EFFECT_VIGNETTE") \
    X(SCANLINES,            RENDER3D_POST_FILTER_SCANLINES,            "Scanlines",            "GLR_POST_FX_EFFECT_SCANLINES") \
    X(FILM_GRAIN,           RENDER3D_POST_FILTER_FILM_GRAIN,           "Film grain",           "GLR_POST_FX_EFFECT_FILM_GRAIN")

typedef enum {
#define POST_FX_EFFECT_ENUM_ENTRY(suffix, render_mode, name_str, symbol_str) GLR_POST_FX_EFFECT_##suffix,
    POST_FX_EFFECT_LIST(POST_FX_EFFECT_ENUM_ENTRY)
#undef POST_FX_EFFECT_ENUM_ENTRY
    GLR_POST_FX_EFFECT_COUNT
} GlrPostFxEffect;

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

/* Scene-layer screen-space 2D bracket shared by the post filters and the
 * depth-viz quad (src/render3d/ must not depend on ui/gl_2d.h). begin
 * pushes GL_ALL_ATTRIB_BITS + all three matrix stacks, sets a pixel
 * ortho over the rect with texturing on (GL_REPLACE) and depth/light/
 * blend off; end pops it all back. The matrix mode is snapshot/restored
 * through the out-param (it is not covered by glPushAttrib), so the
 * pair isn't coupled by a file-static. */
void render3d_post_2d_begin(int sx, int sy, int sw, int sh,
                            GLint *saved_matrix_mode_out);
void render3d_post_2d_end(GLint saved_matrix_mode);

#endif /* RENDER3D_POSTPROCESS_FILTER_H */
