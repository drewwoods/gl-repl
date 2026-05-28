/*
 * replay_ui_hud.h - replay subsystem's UI surface (status HUD overlay).
 *
 * Feature-owned UI: the replay peer subsystem's screen-space HUD. Lives
 * under the `replay_ui_*` prefix instead of generic `ui_*` because the
 * module legitimately knows replay concepts (mode, PC, speed, play /
 * paused / done state) and reads from the replay peer subsystem's
 * snapshot. It still must not own unrelated editor / REPL state and
 * must not call parser / compile / apply.
 *
 * See MODULES.md "2D UI rendering and hit-test" for the prefix
 * discipline rule and `scripts/check-replay-ui-isolation.sh` for the
 * lighter feature-UI guard.
 */
#ifndef REPLAY_UI_HUD_H
#define REPLAY_UI_HUD_H

/* Replay HUD layout shared by replay_ui_hud.c and other UI helpers that need
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

/* Render the replay HUD once per frame. Reads replay state (mode / pc /
 * total / play state / speed / expand-args) from snap->replay and the
 * scene rect from ui_layout_scene_rect — matching the
 * (const UiRenderSnapshot *) signature shared by the other panel
 * renderers. No-op when snap->replay.active is 0. */
struct UiRenderSnapshot;
void replay_ui_hud_render(const struct UiRenderSnapshot *snap);

#endif /* REPLAY_UI_HUD_H */