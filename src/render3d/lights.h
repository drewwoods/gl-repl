/*
 * lights.h - Scene light setup and light-indicator rendering.
 *
 * Owns the fixed-function light setup helpers render.c uses around the
 * main geometry pass. The per-frame inputs come from Render3dFrameRenderContext: which
 * lights are enabled, their colors/positions, and whether indicator geometry
 * should be drawn.
 */
#ifndef RENDER3D_LIGHTS_H
#define RENDER3D_LIGHTS_H

#include "render_types.h"

/* Initialize the global ambient light model the scene uses as its baseline. */
void render3d_lights_init_global_ambient(void);

/* Apply the frame's light state before executing user geometry. */
void render3d_lights_setup(const Render3dFrameRenderContext *frame_ctx);

/* Render light-indicator overlays after the main geometry pass when enabled. */
void render3d_lights_render(const Render3dFrameRenderContext *frame_ctx);

/* Overwrite `out` with the Render3dLightTheme preset. `.enabled` is left at
 * 0 — the program's glEnable(GL_LIGHTn) decides which slots light up.
 * `.pos_is_eye_space` is set per-slot per-theme, so downstream readers
 * (render3d_lights_setup, the exporter, etc.) don't need to know about
 * theme enums. */
void render3d_lights_apply_theme(Render3dLight out[MAX_LIGHTS], int theme);

/* User-facing label table for the cfg menu / status text, indexed by
 * Render3dLightTheme. Length is LIGHT_THEME_COUNT. Pointer type matches
 * GlrConfigItem.state_names (non-const pointers) so the cfg row can
 * reference it without a qualifier-discarding cast. */
extern const char *render3d_light_theme_names[];

#endif /* RENDER3D_LIGHTS_H */
