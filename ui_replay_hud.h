/*
 * ui_replay_hud.h - replay status HUD overlay.
 *
 * Screen-space replay HUD rendered from a SceneRenderConfig snapshot. This is
 * a UI-layer overlay, not part of the 3D scene renderer.
 */
#ifndef UI_REPLAY_HUD_H
#define UI_REPLAY_HUD_H

#include "scene_render_types.h"

/* Render the replay HUD once per frame. Uses only the supplied config snapshot
 * and no live REPL state. No-op when replaying is disabled. */
void ui_replay_hud_render(const SceneRenderConfig *config);

#endif /* UI_REPLAY_HUD_H */