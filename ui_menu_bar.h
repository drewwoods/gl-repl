/*
 * ui_menu_bar.h - Top menu bar with File/Scene/Config dropdowns and pinned buttons.
 *
 * Renders the top row of the UI: File/Scene/Config top-level menus on the left,
 * pinned action buttons (Search, Replay) on the right. Implements dropdown menus
 * with hierarchical item lists (e.g., File menu has Export/Import/Workspace
 * operations; Scene menu has New Scene/Save/Rename plus user scene list; Config
 * has toggles/cycles for overlays and rendering features).
 *
 * Menu structure: Three top-level menus routed via repl_actions.h. Dropdowns
 * are modal overlays; only one menu/dropdown is open at a time. Search overlay
 * is a special full-width text input that appears below the menu bar when Ctrl+F
 * activates search. Example dropdown is the F12 cycle menu (examples + user scenes).
 *
 * Pinned buttons: Search and Replay buttons on the right side toggle their
 * respective overlays. Buttons are always visible; they don't open/close menus
 * like top-level menu items do (they toggle state directly).
 *
 * Input routing: Hit-testing (menu_hit, pin_hit, dropdown_item_hit) identifies
 * which UI element was clicked. activate_dropdown_item routes the action through
 * repl_actions.c for side effects (file I/O, config change, scene switch, etc.).
 * handle_config_right_press detects right-click on menu bar for context menu.
 *
 * State queries: open_menu_id returns the currently open menu (-1 if none);
 * dropdown_is_open checks whether a specific dropdown is visible. Used by
 * ui_panels.c to coordinate input priority (menu input takes precedence).
 *
 * Search integration: note_search_opened() notifies menu bar that search
 * overlay is active, so the Search pin button is highlighted. render_search_overlay()
 * draws the search text input and matches.
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

/* Activate a dropdown item by index (from dropdown_item_hit). Dispatches the
 * action through repl_actions.c for side effects (file I/O, config toggle,
 * scene switch, etc.). Returns 1 if menu should close after action, 0 if it
 * should stay open (for toggles/cycles). Called by ui_panels.c after a
 * dropdown item is clicked. */
int  ui_menu_bar_activate_dropdown_item(int item_idx);

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
