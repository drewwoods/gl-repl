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
 * GLR_TICK_PER_FRAME), coordinates are window pixels, origin top-left (the
 * GLUT callback convention).
 *
 *   0.5  move 100 50          # jump the pointer
 *   1.0  glide 300 200 0.8    # ease the pointer to (300,200) over 0.8s
 *   2.0  click                # left press at the pointer (+ release ~0.1s on)
 *   2.2  click 310 44         # optional coords: move, then click there
 *   2.6  rightclick           # right press + release
 *   3.0  down / 3.4 up        # explicit press / release (drags)
 *   3.8  wheel -1             # mouse wheel, +1/-1
 *   4.0  ring 300 200 1.5     # pulsing highlight ring for 1.5s
 *   5.0  key glColor3f(       # feed text through the keyboard dispatch
 *                             # (escapes: \n = Enter, \e = Esc, \t = Tab,
 *                             #  \cX = Ctrl+X control byte, \\ = backslash)
 *   6.0  key \cT              # e.g. Ctrl+T toggles animation
 *   7.0  skey f12             # special key: f1..f12, up/down/left/right,
 *                             # home, end, pageup, pagedown
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
