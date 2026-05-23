/*
 * ui_menu_bar.h - Top menu bar with dropdowns and pinned buttons.
 *
 * Renders the File/Scene/Config menus on the left, Search/Replay pins on the
 * right, and any open dropdown beneath them. This module owns menu-bar display
 * state and hit classification; action execution happens outside the UI layer,
 * after the controller routes the returned `UiHit` to the actions or scene code.
 *
 * Search integration lives here too: the search pin highlight and the search
 * overlay text field are part of the menu-bar chrome even though the search
 * model itself lives elsewhere.
 */
#ifndef UI_MENU_BAR_H
#define UI_MENU_BAR_H

#include "ui/core/hit.h"
#include "ui/app/snapshot.h"

/* Pinned button identifiers (right side of menu bar). Search and Replay
 * buttons that toggle their respective overlays. */
enum {
    UI_MENU_BAR_PIN_SEARCH = 0,
    UI_MENU_BAR_PIN_REPLAY,
    UI_MENU_BAR_PIN_COUNT
};

/* --- Rendering --- */

/* Render the top menu bar: File/Scene/Config menus on left, Search/Replay
 * buttons on right, and any open dropdown. Reads only from the supplied
 * UI snapshot. Called once per frame. */
void ui_menu_bar_render(const UiRenderSnapshot *snap);

/* Render the search overlay text input and matches (below menu bar, full width).
 * cp_x is code-panel x-coordinate; panel_w is width; panel_top is y-coordinate
 * of menu bar bottom. Reads from the supplied snapshot. */
void ui_menu_bar_render_search_overlay(const UiRenderSnapshot *snap,
                                       int cp_x, int panel_w, int panel_top);

/* Render the example dropdown (F12 cycle menu showing built-in examples + user
 * scenes). Reads from the supplied snapshot. */
void ui_menu_bar_render_example_dropdown(const UiRenderSnapshot *snap);

/* --- Menu state --- */

/* Query the currently open top-level menu ID (GLR_MENU_FILE, GLR_MENU_SCENE,
 * GLR_MENU_CONFIG from repl_actions.h), or -1 if no menu is open. Used by
 * ui_panels.c to prioritize input routing. */
int  ui_menu_bar_open_menu_id(void);

/* Close all menus and dropdowns. Called after menu item selection or when user
 * clicks outside menus. */
void ui_menu_bar_close(void);

/* Open a specific top-level menu by ID (GLR_MENU_FILE, etc.). Called by menu
 * button click or keyboard dispatch. `now` is the current animation clock
 * (anim_time), used to seed dropdown fade-in animation. */
void ui_menu_bar_set_open_menu(int menu_id, float now);

/* Open the Config dropdown specifically (convenience for keyboard shortcut). */
void ui_menu_bar_open_config(float now);

/* Notify menu bar that search overlay became active (used to highlight Search
 * pin button). `now` seeds the highlight fade-in. */
void ui_menu_bar_note_search_opened(float now);

/* --- Hit-testing and interaction --- */

/* Hit-test a click on a top-level menu button (File/Scene/Config). Returns the
 * menu ID if hit, -1 if no menu button was clicked. mx, my are window
 * coordinates. Called by ui_panels.c on left-click. */
int  ui_menu_bar_menu_hit(int mx, int my);

/* Hit-test a click on a pinned button (Search/Replay). Returns the button ID
 * (UI_MENU_BAR_PIN_*) if hit, -1 if no button was clicked. mx, my are window
 * coordinates. Called by ui_panels.c on left-click. */
int  ui_menu_bar_pin_hit(int mx, int my);

/* Hit-test a click within an open dropdown menu. Returns the item index if a
 * menu item was clicked, -1 if not. mx, my are window coordinates. Called by
 * ui_panels.c on left-click while a dropdown is open. */
int  ui_menu_bar_dropdown_item_hit(int mx, int my);

/* Test helper: return the Scene example submenu rect for a visible tag while
 * the Scene menu is open. Ignores the current hover/open-tag state. Rect
 * coordinates are in GL space (origin at bottom-left). */
int  ui_menu_bar_scene_example_submenu_rect_for_test(int tag_idx,
                                                     int *sx, int *sy,
                                                     int *sw, int *sh);

/* Test helper: return the Tutorials submenu rect for a visible tutorial
 * tag while the Tutorials menu is open. Same shape as the Scene helper
 * above; ignores hover/open-tag state. */
int  ui_menu_bar_tutorial_submenu_rect_for_test(int tag_idx,
                                                int *sx, int *sy,
                                                int *sw, int *sh);

/* Test helper: generic flyout-submenu rect for (menu_id, parent_row)
 * while that menu is open. Ignores hover/open state. GL-space coords.
 * Used by Config-flyout tests; the Scene helper above is the
 * tag-indexed convenience wrapper over the same engine. */
int  ui_menu_bar_submenu_rect_for_test(int menu_id, int parent_row,
                                       int *sx, int *sy,
                                       int *sw, int *sh);

/* Refresh open-dropdown hover state from the current pointer. Returns 1 when
 * the hovered parent row or Scene example submenu changed. */
int  ui_menu_bar_update_pointer_hover(int mx, int my, float now);

/* Pure hit-test: classify (mx, my) as a UiHit for menu-bar-related regions.
 * Reports the menu-bar slice of the hit-test in priority order: submenu row >
 * open dropdown row > pin button > top-level menu button.
 * The result kind is one of:
 *   UI_HIT_MENU_BUTTON — top-level menu button.
 *   UI_HIT_MENU_ITEM   — open-dropdown parent row.
 *   UI_HIT_SUBMENU_ITEM — flyout submenu row (Scene example / Config item).
 *   UI_HIT_PIN_BUTTON  — pinned right-side button (Search/Replay).
 *                        item_idx carries the pin id.
 *   UI_HIT_NONE        — pointer is outside every menu-bar region.
 *
 * Reads layout / state only; never mutates. */
UiHit ui_menu_bar_hit_test(int mx, int my);

UiHit ui_menu_bar_handle_config_right_press(int mx, int my);

/* --- State queries --- */

/* Query whether a top-level menu dropdown is currently open (File, Scene, or
 * Config menu). Returns 1 if any menu dropdown is visible, 0 if not. Used to
 * prioritize input routing in ui_panels.c. */
int  ui_menu_bar_menu_dropdown_is_open(void);

/* Query whether the example dropdown (F12 cycle menu) is currently open. Returns
 * 1 if open, 0 if not. Used to prioritize input routing and determine if
 * keyboard input should go to example menu instead of code panel. */
int  ui_menu_bar_example_dropdown_is_open(void);

#endif /* UI_MENU_BAR_H */
