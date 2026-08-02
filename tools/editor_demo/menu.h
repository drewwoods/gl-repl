/*
 * tools/editor_demo/menu.h - Minimal demo-local menu bar.
 *
 * A standalone File menu for the generic text editor demo. Not
 * reused from src/ui/app/menu_bar.c - that one is heavily coupled to
 * REPL (config items, scenes, tutorials, examples) and would drag
 * those deps into the demo's link set. The demo menu is ~80 lines
 * of plain rendering + hit-test; no point sharing.
 *
 * Layout: a 24-pixel band along the top of the window. Single
 * "File" label at the left; clicking it opens a dropdown with
 * Load / Save / Quit entries. Click outside, or click any entry,
 * to close.
 *
 * Coordinates throughout are bottom-left origin (OpenGL); the
 * caller is responsible for converting GLUT's top-left mouse
 * coordinates before calling demo_menu_handle_click.
 */
#ifndef EDITOR_DEMO_MENU_H
#define EDITOR_DEMO_MENU_H

#define DEMO_MENU_BAR_H 24

/* Render the menu bar (always) plus the active dropdown (if open).
 * Assumes a 2D ortho projection is already pushed (caller wraps
 * the call with gl2d_begin / gl2d_end). */
void demo_menu_render(int vp_w, int vp_h);

/* Handle a left-button click at (mx, my) in bottom-left
 * coordinates. Returns 1 if the click was consumed (menu/dropdown
 * region), 0 if it should fall through to the code panel.
 * Invokes the appropriate item callback when a dropdown entry is
 * hit. */
int demo_menu_handle_click(int mx, int my, int vp_w, int vp_h);

/* Returns non-zero while the dropdown is open. */
int demo_menu_is_open(void);

/* Force the menu closed (e.g. on Escape). */
void demo_menu_close(void);

#endif /* EDITOR_DEMO_MENU_H */
