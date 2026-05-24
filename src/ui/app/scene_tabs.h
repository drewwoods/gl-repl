/*
 * ui_scene_tabs.h - Scene tab strip below the code-panel menu bar.
 *
 * A VS Code-style tab strip: one tab per occupied user-scene slot (slot
 * order) plus exactly one "example" tab while an example is active. It is a
 * *pure view* of existing scene state plus a second way to invoke the
 * existing scene-switch code paths — the behavioral model of user scenes /
 * examples does not change here. Click-to-switch and double-click-to-rename
 * are routed by the controller (route_scene_tab_hit in glr_ctrl.c).
 *
 * Contract (mirrors src/ui/app/menu_bar.h):
 *
 *   UI renders the strip and reports `UiHit` results from
 *   ui_scene_tabs_hit_test():
 *     UI_HIT_CODE_PANEL_TAB    — a clickable scene tab; item_idx = display
 *                                index into snapshot.scene_tabs.
 *     UI_HIT_CODE_PANEL_CHROME — in-band but off-tab (gaps / right of the
 *                                last tab); inert, consumes the click so it
 *                                does not fall through to a code-text hit.
 *     UI_HIT_NONE              — pointer is outside the band.
 *   The module mutates nothing and never reads or writes REPL/editor state.
 *   Tab/scene content and the hit-test y-flip come from the frozen snapshot;
 *   panel geometry comes through the shared ui_layout_code_panel_rect()
 *   helper, exactly as menu_bar.c does (so the band stays consistent with
 *   the rest of the panel chrome by construction).
 *
 * Shape/kind cues: every tab has a rounded-top outline (no bottom edge)
 * so the strip reads as tabs, not flat menu buttons. The active tab is
 * shaded across its whole rect and its outline keys off that hue —
 * green for a user scene, amber for the ephemeral example tab (built-ins
 * vanish when a user scene is selected and are not renamable unless
 * promoted). Inactive tabs get a neutral outline; idle example labels
 * stay muted amber vs the user tabs' neutral so the kinds differ even
 * with no active tab.
 *
 * Dirty dot: deferred — there is no reachable per-scene modified flag
 * (editor dirty state is global), so no per-tab unsaved indicator is drawn.
 */
#ifndef UI_SCENE_TABS_H
#define UI_SCENE_TABS_H

#include "ui/app/hit.h"
#include "ui/app/snapshot.h"
#include "ui/core/metrics.h"

#define TAB_STRIP_H LINE_H

/* Render the scene tab strip. No-op when the derived tab set is empty or
 * the panel is hidden. Draws into the caller's open gl2d block (same as
 * ui_menu_bar_render). Reads only from the supplied snapshot. */
void ui_scene_tabs_render(const UiRenderSnapshot *snap);

/* Hit-test (mx, my) window coords against the band. Consumes the entire
 * band (TAB on a tab rect, CHROME in-band off-tab) so blank-strip clicks
 * never fall through to the code panel. */
UiHit ui_scene_tabs_hit_test(const UiRenderSnapshot *snap, int mx, int my);

/* Vertical reserve the strip occupies: TAB_STRIP_H when a non-empty tab
 * set is shown, else 0. The code panel threads this into its top-chrome
 * reserve so rows / scrollbar / scroll math clear the band. */
int ui_scene_tabs_band_h(const UiRenderSnapshot *snap);

#endif /* UI_SCENE_TABS_H */
