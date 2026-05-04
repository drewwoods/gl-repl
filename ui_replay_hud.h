/*
 * ui_replay_hud.h - replay status HUD overlay.
 *
 * Screen-space replay HUD rendered from a UI-local snapshot. This is a
 * UI-layer overlay, not part of the 3D scene renderer.
 */
#ifndef UI_REPLAY_HUD_H
#define UI_REPLAY_HUD_H

#include "ui_metrics.h"

/* Replay HUD layout shared by ui_replay_hud.c and other UI helpers that need
 * to position themselves relative to the HUD footprint. */
#define REPLAY_HUD_MARGIN_X      18
#define REPLAY_HUD_MARGIN_Y      18
#define REPLAY_HUD_MIN_WIDTH     220
#define REPLAY_HUD_HEIGHT        56
/* y positions measured from hud_y (bottom edge), top-to-bottom:
 *   line1 (status)   @ 36 - icon row, above progress
 *   progress groove  @ 22
 *   line2 (kbd)      @  4 */
#define REPLAY_HUD_PROGRESS_Y    22
#define REPLAY_HUD_PROGRESS_H     6
#define REPLAY_HUD_TEXT_PAD_X    10
#define REPLAY_HUD_TEXT_LINE1_Y  36
#define REPLAY_HUD_TEXT_LINE2_Y   4
#define REPLAY_HUD_BOTTOM_Y (REPLAY_HUD_MARGIN_Y + REPLAY_HUD_HEIGHT)

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
