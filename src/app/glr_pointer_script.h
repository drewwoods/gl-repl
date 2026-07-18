#ifndef GLR_POINTER_SCRIPT_H
#define GLR_POINTER_SCRIPT_H

/*
 * Scripted synthetic pointer/keyboard input — the capture affordance behind
 * GLR_POINTER_SCRIPT=<file>. A recording run (scripts/record-video.sh) uses
 * it to drive menu navigation headlessly: timed pointer glides open dropdown
 * menus and hover-highlight rows exactly as a real mouse would (all events
 * route through the normal glr_ctrl_* GLUT entry points), while an overlay
 * cursor + click ripple + highlight ring make the pointer visible in the
 * captured video.
 *
 * Script format: one event per line, `#` comments. Times are seconds on the
 * rendered-frame clock (frame N <-> t = N/60 — the hook implies
 * GLR_TICK_PER_FRAME). A *point* is either literal window pixels
 * "<x> <y>" (origin top-left, the GLUT callback convention) or a symbolic
 * target token resolved against the LIVE layout when the event fires — so
 * symbolic scripts are window-size- and catalog-order-independent:
 *
 *   menu:scene            top-level menu-bar button, by label
 *   item:new_scene        row of the currently open dropdown, by label
 *   subenter:overlays     horizontal entry point into a parent row's flyout
 *                         (enter at the parent's y — a diagonal glide would
 *                         cross sibling rows and swap the flyout)
 *   sub:overlays:vertex_points   flyout row: parent label + row label
 *   pin:replay            pinned button: replay / search / view (2D-3D)
 *   scene:0.55,0.30       fraction of the scene viewport rect (x,y from
 *                         its top-left)
 *
 * Labels match by normalized prefix: case-insensitive, '_' matches ' '
 * ("torus_knot" matches "Torus knot (animated)"); chrome rows (dividers,
 * headers, flyout subheadings) never match. Row targets need their menu
 * open — click it open first, exactly as a user would. An unresolvable
 * target aborts a capture run (exit 1) and stops a tour with a status
 * message; a silently mis-aimed script is worse than a failed one.
 *
 *   0.5  move 100 50          # jump the pointer (literal pixels)
 *   0.8  move scene:0.5,0.4   # jump to the scene-viewport center-ish
 *   1.0  glide menu:scene 0.8 # ease the pointer onto the Scene button
 *   2.0  click                # left press at the pointer (+ release ~0.1s on)
 *   2.2  click item:all       # optional point: move, then click there
 *   2.6  rightclick           # right press + release
 *   3.0  down / 3.4 up        # explicit press / release (drags; while held,
 *                             # moves route through glr_ctrl_motion)
 *   3.8  wheel -1             # mouse wheel, +1/-1
 *   4.0  ring sub:3d:torus_knot 1.5   # pulsing highlight ring for 1.5s
 *   5.0  key glColor3f(       # feed text through the keyboard dispatch
 *                             # (escapes: \n = Enter, \e = Esc, \t = Tab,
 *                             #  \cX = Ctrl+X control byte, \\ = backslash)
 *   6.0  key \cT              # e.g. Ctrl+T toggles animation
 *   7.0  skey f12             # special key: f1..f12, up/down/left/right,
 *                             # home, end, pageup, pagedown
 *   8.0  echo scene:0.25,0.76 18 2 Ctrl+K   # caption text (point, cap
 *                             # height px, seconds) — labels how the next
 *                             # action was triggered (GLUT stroke text)
 *
 * Events fire in file order once their time is reached; keep them sorted.
 * `key` + `skey` let a script type a whole demo (commit lines with `key ;`,
 * navigate with `skey up`), so demos can be re-recorded from scripts.
 */

/* Read GLR_POINTER_SCRIPT and parse the script it names. Returns nonzero
 * when a script is active (so main() can imply GLR_TICK_PER_FRAME). A parse
 * error reports the offending line on stderr and exits nonzero — a capture
 * run silently recording the wrong interaction is worse than failing. */
int glr_pointer_script_load_env(void);

/* Nonzero once a script has loaded. */
int glr_pointer_script_active(void);

/* Start a script from an in-memory array of grammar lines (the guided-tour
 * path — the Tours menu plays a built-in catalog entry through the same
 * engine the capture hook uses). Unlike the env loader this runs mid-session:
 * the frame clock, glide, and overlay state reset, events fire on subsequent
 * glr_pointer_script_frame() calls, and the script auto-stops after its last
 * event and overlay effect finish. Events must be time-sorted. Returns 1 on
 * success; a malformed or out-of-order line logs to stderr and returns 0
 * without activating (built-in tours are validated by tests, so this is an
 * authoring backstop, not a user-facing error path). */
int glr_pointer_script_start_lines(const char *const *lines, int count);

/* Stop a running script now: release any scripted-held mouse button so the
 * app never sees a stuck drag, clear the overlay, and deactivate. Used by
 * the tour cancel path (real user input hands control back) and by the
 * auto-stop; safe to call when inactive. */
void glr_pointer_script_stop(void);

/* Nonzero while a runtime-started (tour) script is playing — as opposed to
 * the env-driven capture mode, which is never canceled by user input. */
int glr_pointer_script_tour_active(void);

/* Resolve one symbolic target token (e.g. "item:new_scene", "scene:0.5,0.5")
 * against the live layout, filling (*mx, *my) in mouse space. Returns 1 on
 * success, 0 when the target is unknown or currently unresolvable. The same
 * resolver every scripted point goes through — exposed so tests can pin the
 * target vocabulary to the real hit-test geometry. */
int glr_pointer_script_resolve_target(const char *target, int *mx, int *my);

/* Advance the script one frame: fire due events through the glr_ctrl_*
 * input entry points and step any active glide. Call once per display
 * callback, before glr_ctrl_display_frame(), so the frame reflects the
 * new pointer state. No-op when inactive. */
void glr_pointer_script_frame(void);

/* Draw the overlay cursor (+ click ripple, highlight ring) over the
 * composited frame; call between glr_ctrl_display_frame() and
 * glutSwapBuffers(), like splash_render(). No-op when inactive. */
void glr_pointer_script_render_overlay(int win_w, int win_h);

#endif /* GLR_POINTER_SCRIPT_H */
