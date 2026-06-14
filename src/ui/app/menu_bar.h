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

#include "ui/app/hit.h"
#include "ui/app/snapshot.h"

/* Pinned button identifiers (right side of menu bar). Indices MUST stay
 * aligned with the internal PIN_* enum in menu_bar.c — the pin hit-test
 * emits its internal index as item_idx and the router switches on these.
 * Search opens search, View-mode toggles 2D/3D, Replay toggles replay. */
enum {
    UI_MENU_BAR_PIN_SEARCH = 0,
    UI_MENU_BAR_PIN_VIEW_MODE,
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
void ui_menu_bar_render_search_overlay(const UiRenderSnapshot *snap);

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



int  ui_menu_bar_scene_parent_row_for_tag(int tag_idx);
int  ui_menu_bar_tutorial_parent_row_for_tag(int tag_idx);

/* Test helper: generic flyout-submenu rect for (menu_id, parent_row)
 * while that menu is open. Ignores hover/open state. GL-space coords.
 * Used by Config-flyout tests. */
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

/* Scroll the open flyout when the pointer (mx, my) is over it and it is
 * taller than the viewport (e.g. the Config "All" list). `delta` is a
 * row offset, positive reveals lower rows (same sign convention as
 * editor_input_code_panel_scroll). Returns 1 if the pointer was over the
 * open flyout — in which case the wheel event is consumed even when the
 * flyout is too short to scroll, so it never leaks to the code panel or
 * camera behind the menu. Returns 0 otherwise. */
int  ui_menu_bar_handle_wheel_scroll(int mx, int my, int delta);

/* --- State queries --- */

/* Query whether a top-level menu dropdown is currently open (File, Scene, or
 * Config menu). Returns 1 if any menu dropdown is visible, 0 if not. Used to
 * prioritize input routing in ui_panels.c. */
int  ui_menu_bar_menu_dropdown_is_open(void);



#endif /* UI_MENU_BAR_H */
