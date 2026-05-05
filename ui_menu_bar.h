/*
 * ui_menu_bar.h - Top menu bar with File/Scene/Config dropdowns and pinned buttons.
 *
 * Renders the top row of the UI: File/Scene/Config top-level menus on
 * the left, pinned action buttons (Search, Replay) on the right.
 * Implements dropdown menus with hierarchical item lists (e.g., File
 * menu has Export/Import/Workspace operations; Scene menu has New
 * Scene/Save/Rename plus user scene list; Config has toggles/cycles
 * for overlays and rendering features).
 *
 * Target contract (Phase E onward):
 *
 *   UI renders the menu bar and reports `UiHit` results from
 *   ui_menu_bar_hit_test() (UI_HIT_MENU_ITEM for top-level buttons
 *   and open-dropdown rows; UI_HIT_PIN_BUTTON for pinned buttons).
 *   `imrepl_ctrl` routes the hit; repl_actions.c performs the action.
 *   This module does not dispatch actions itself — `activate_dropdown_item`
 *   stays as a transitional helper that the controller calls.
 *
 * Menu structure: Three top-level menus. Only one dropdown is open at
 * a time. Search overlay is a special full-width text input below the
 * menu bar (Ctrl+F). Example dropdown is the F12 cycle menu.
 *
 * Pinned buttons: Search and Replay on the right side toggle their
 * overlays. They are always visible; they don't open dropdowns the
 * way top-level menu items do.
 *
 * Hit-test: ui_menu_bar_hit_test() classifies (mx, my). When a
 * dropdown is open, hits inside that dropdown win; otherwise hits
 * resolve to the top-level menu button or the pin button. The router
 * disambiguates top-level vs dropdown row via ui_menu_bar_open_menu_id().
 *
 * Legacy imperative path (transitional): `activate_dropdown_item`
 * still calls into repl_actions.c. That call site is tracked by
 * `check-ui-returns-hits-only`; the target is for `imrepl_ctrl` to
 * own activation once it routes by UiHit.kind.
 *
 * Search integration: note_search_opened() notifies the menu bar that
 * the search overlay is active so the Search pin highlights.
 * render_search_overlay() draws the search text input and matches.
 */
#ifndef UI_MENU_BAR_H
#define UI_MENU_BAR_H

#include "ui_hit.h"
#include "ui_snapshot.h"

/* Pinned button identifiers (right side of menu bar). Search and Replay
 * buttons that toggle their respective overlays. */
enum {
    REPL_MENU_BAR_PIN_SEARCH = 0,
    REPL_MENU_BAR_PIN_REPLAY,
    REPL_MENU_BAR_PIN_COUNT
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

/* Query the currently open top-level menu ID (REPL_MENU_FILE, REPL_MENU_SCENE,
 * REPL_MENU_CONFIG from repl_actions.h), or -1 if no menu is open. Used by
 * ui_panels.c to prioritize input routing. */
int  ui_menu_bar_open_menu_id(void);

/* Close all menus and dropdowns. Called after menu item selection or when user
 * clicks outside menus. */
void ui_menu_bar_close(void);

/* Open a specific top-level menu by ID (REPL_MENU_FILE, etc.). Called by menu
 * button click or keyboard dispatch. */
void ui_menu_bar_set_open_menu(int menu_id);

/* Open the Config dropdown specifically (convenience for keyboard shortcut). */
void ui_menu_bar_open_config(void);

/* Notify menu bar that search overlay became active (used to highlight Search
 * pin button). Called by search.c when search is opened. */
void ui_menu_bar_note_search_opened(void);

/* --- Hit-testing and interaction --- */

/* Hit-test a click on a top-level menu button (File/Scene/Config). Returns the
 * menu ID if hit, -1 if no menu button was clicked. mx, my are window
 * coordinates. Called by ui_panels.c on left-click. */
int  ui_menu_bar_menu_hit(int mx, int my);

/* Hit-test a click on a pinned button (Search/Replay). Returns the button ID
 * (REPL_MENU_BAR_PIN_*) if hit, -1 if no button was clicked. mx, my are window
 * coordinates. Called by ui_panels.c on left-click. */
int  ui_menu_bar_pin_hit(int mx, int my);

/* Hit-test a click within an open dropdown menu. Returns the item index if a
 * menu item was clicked, -1 if not. mx, my are window coordinates. Called by
 * ui_panels.c on left-click while a dropdown is open. */
int  ui_menu_bar_dropdown_item_hit(int mx, int my);

/* Pure hit-test: classify (mx, my) as a UiHit for menu-bar-related regions.
 *
 * Phase E commit 29 entry. Reports the menu-bar slice of the hit-test in
 * priority order: open dropdown row > top-level menu button > pin button.
 * The result kind is one of:
 *   UI_HIT_MENU_ITEM   — top-level menu button or open-dropdown row.
 *                        item_idx carries the menu_id (top-level button)
 *                        OR the dropdown row index (when a dropdown is
 *                        already open). The router can disambiguate via
 *                        ui_menu_bar_open_menu_id() — when -1 the hit is
 *                        a top-level button, otherwise it is a dropdown
 *                        row inside that menu.
 *   UI_HIT_PIN_BUTTON  — pinned right-side button (Search/Replay).
 *                        item_idx carries the pin id.
 *   UI_HIT_NONE        — pointer is outside every menu-bar region.
 *
 * Reads layout / state only; never mutates. */
UiHit ui_menu_bar_hit_test(int mx, int my);

/* Handle right-click on menu bar region: open Config menu if clicked on menu
 * button area, otherwise no-op. Returns 1 if Config menu was opened, 0 if
 * right-click was in pinned button area or elsewhere. mx, my are window
 * coordinates. Called by ui_panels.c on right-click. */
int  ui_menu_bar_handle_config_right_press(int mx, int my);

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
