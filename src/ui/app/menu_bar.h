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

/* Hit-test the find bar's interactive parts, in paint order:
 *   - the replace row hanging below the bar (Replace / All buttons, the
 *     whole-word chip, and the field itself → UI_HIT_SEARCH_REPLACE,
 *     UI_HIT_SEARCH_WORD_TOGGLE, UI_HIT_SEARCH_FOCUS),
 *   - the match stepper at the right of the search box (UI_HIT_SEARCH_NAV,
 *     item_idx = +1 next / -1 previous),
 *   - the search box itself, which takes focus back from the replace row.
 * Returns UI_HIT_NONE when the point misses all of them. Checked before
 * the generic menu-bar and code-panel hit-tests, since the row overlaps
 * the panel and the stepper overlaps the search pin slot. */
UiHit ui_menu_bar_search_hit_test(const UiRenderSnapshot *snap,
                                  int mx, int my);

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

/* Open the File dropdown with its "Open Workspace" flyout already expanded and
 * its parent row hovered — the state a hover would have produced. Backs the
 * tab strip's workspace chip, whose whole job is to lead here. Toggles closed
 * when that flyout is already the open one. */
void ui_menu_bar_open_workspace_list(float now);

/* Frozen open-dropdown state (which menu, its fade clock, the hovered row).
 * Capture it before an action that runs glr_ctrl_reset_transients() — which
 * closes the menu — and restore it afterward to keep the dropdown open
 * across the action WITHOUT restarting the fade or dropping the hover
 * highlight. Used by the Scene menu's Next/Previous cycle rows so repeated
 * clicks step through examples with the menu (and its highlight) intact. */
typedef struct {
    int   menu_id;     /* open top-level menu, or -1 if none */
    float open_time;   /* anim_time the menu opened (fade clock) */
    int   item_hover;  /* hovered dropdown row, or -1 */
} UiMenuBarOpenState;

UiMenuBarOpenState ui_menu_bar_open_state_capture(void);
void ui_menu_bar_open_state_restore(UiMenuBarOpenState state);

/* Complete menu-bar runtime snapshot: the open top-level menu, the hovered
 * dropdown row, the open flyout's parent/menu/open-clock/scroll, and the menu +
 * search fade clocks. Broader than UiMenuBarOpenState (which frames only the
 * top-level dropdown for the Scene cycle rows). The dropdown geometry cache is
 * derived and intentionally excluded — it rebuilds from these on the next
 * frame. Used by the tour baseline so Back / Done-restart put an open menu (and
 * any flyout) back exactly as the tour began. */
typedef struct {
    int   open_menu;
    int   item_hover;
    int   submenu_menu_id;
    int   submenu_parent_row;
    float submenu_open_time;
    int   submenu_scroll;
    float menu_open_time;
    float search_open_time;
} UiMenuBarRuntimeSnapshot;

void ui_menu_bar_runtime_capture(UiMenuBarRuntimeSnapshot *out);
void ui_menu_bar_runtime_restore(const UiMenuBarRuntimeSnapshot *snapshot);

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
const char *ui_menu_bar_menu_item_shortcut_for_test(int menu_id,
                                                   int item_idx);
int  ui_menu_bar_menu_item_enabled_for_test(int menu_id, int item_idx);
/* Raw dropdown row label, "### "/"---" chrome markers included. Returns
 * static storage for the rows that compose their label (the File menu's
 * workspace header, Audio's loop row). */
const char *ui_menu_bar_menu_item_label_for_test(int menu_id, int item_idx);

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

/* Scroll the open flyout when the pointer (mx, my) is over it and it is
 * taller than the viewport (e.g. the Config "All" list). `delta` is a
 * row offset, positive reveals lower rows (same sign convention as
 * editor_input_code_panel_scroll). Returns 1 if the pointer was over the
 * open flyout — in which case the wheel event is consumed even when the
 * flyout is too short to scroll, so it never leaks to the code panel or
 * camera behind the menu. Returns 0 otherwise. */
int  ui_menu_bar_handle_wheel_scroll(int mx, int my, int delta);

/* --- Symbolic-target geometry queries (pointer-script resolution) ---
 *
 * Resolve a named menu-bar element to a point in MOUSE space (window px,
 * origin top-left — the GLUT event convention) against the live layout.
 * Labels match by normalized prefix: case-insensitive, with '_' in the
 * needle matching ' ' in the label; chrome rows ("---", "### " headers,
 * flyout subheadings) never match. Each returns 1 and fills (*mx, *my)
 * with the element's center, or 0 when the element does not exist, is
 * hidden on this platform, or (for row queries) its menu is not open.
 * Used by src/app/glr_pointer_script.c to resolve `menu:` / `item:` /
 * `sub:` / `subenter:` / `pin:` script targets at event-fire time, so
 * scripts stay window-size- and catalog-order-independent. */

/* Top-level menu button by label ("file", "scene", ...). */
int  ui_menu_bar_target_menu(const char *name, int *mx, int *my);

/* Pinned right-side button: "search", "view" (the 2D/3D swatch), "replay". */
int  ui_menu_bar_target_pin(const char *name, int *mx, int *my);

/* Row of the currently open dropdown by label ("new_scene", "3d", ...). */
int  ui_menu_bar_target_open_row(const char *name, int *mx, int *my);

/* Horizontal entry point into `parent`'s flyout: just inside the flyout
 * edge adjacent to the dropdown, at the parent row's y — the safe path
 * into a hover-opened flyout (a diagonal glide would cross sibling parent
 * rows and swap the flyout). Requires the parent's menu to be open. */
int  ui_menu_bar_target_flyout_entry(const char *parent, int *mx, int *my);

/* Row of `parent`'s flyout by label. Requires the parent's menu to be
 * open (the flyout itself need not be — geometry is derivable); fails if
 * the row is scrolled out of the flyout's visible window. */
int  ui_menu_bar_target_flyout_row(const char *parent, const char *name,
                                   int *mx, int *my);

/* --- State queries --- */

/* Query whether a top-level menu dropdown is currently open (File, Scene, or
 * Config menu). Returns 1 if any menu dropdown is visible, 0 if not. Used to
 * prioritize input routing in ui_panels.c. */
int  ui_menu_bar_menu_dropdown_is_open(void);



#endif /* UI_MENU_BAR_H */
