/*
 * ui_replay_hud.h - replay status HUD overlay.
 *
 * Screen-space replay HUD rendered from a UI-local snapshot. This is a
 * UI-layer overlay, not part of the 3D scene renderer.
 */
#ifndef UI_REPLAY_HUD_H
#define UI_REPLAY_HUD_H

typedef struct UiReplayHudState {
    int scene_x;
    int scene_y;
    int scene_w;
    int scene_h;
    int viewport_w;
    int viewport_h;
    int code_panel_layout;
    int replay_mode;
    int replay_pc;
    int replay_total_cmds;
    int replay_state_val;
    float replay_speed;
    int replay_expand_args;
    int replaying;
} UiReplayHudState;

/* Render the replay HUD once per frame. Uses only the supplied UI snapshot
 * and no live REPL state. No-op when replaying is disabled. */
void ui_replay_hud_render(const UiReplayHudState *state);

#endif /* UI_REPLAY_HUD_H */
