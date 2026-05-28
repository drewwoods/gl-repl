/*
 * lights.h - Scene light setup and light-indicator rendering.
 *
 * Owns the fixed-function light setup helpers render.c uses around the
 * main geometry pass. The per-frame inputs come from SceneFrameRenderContext: which
 * lights are enabled, their colors/positions, and whether indicator geometry
 * should be drawn.
 */
#ifndef SCENE_LIGHTS_H
#define SCENE_LIGHTS_H

#include "render_types.h"

/* Initialize the global ambient light model the scene uses as its baseline. */
void scene_lights_init_global_ambient(void);

/* Apply the frame's light state before executing user geometry. */
void scene_lights_setup(const SceneFrameRenderContext *frame_ctx);

/* Render light-indicator overlays after the main geometry pass when enabled. */
void scene_lights_render(const SceneFrameRenderContext *frame_ctx);

/* Overwrite `out` with the SceneLightTheme preset. `.enabled` is left at
 * 0 — the program's glEnable(GL_LIGHTn) decides which slots light up.
 * `.pos_is_eye_space` is set per-slot per-theme, so downstream readers
 * (scene_lights_setup, the exporter, etc.) don't need to know about
 * theme enums. */
void scene_lights_apply_theme(SceneLight out[MAX_LIGHTS], int theme);

/* User-facing label table for the cfg menu / status text, indexed by
 * SceneLightTheme. Length is LIGHT_THEME_COUNT. Pointer type matches
 * GlrConfigItem.state_names (non-const pointers) so the cfg row can
 * reference it without a qualifier-discarding cast. */
extern const char *scene_light_theme_names[];

#endif /* SCENE_LIGHTS_H */
