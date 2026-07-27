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
 * Script format: one event per line, `#` comments. There are two forms:
 *
 * - Untimed lines (`verb ...`) run sequentially. The next step starts after
 *   the previous glide, click release, paced key, ring, or pause has
 *   completed. Immediate steps advance on the next rendered frame. This is
 *   the preferred form for live guided tours because stalls and frame-rate
 *   variation cannot make later steps overtake unfinished work.
 * - Timestamped lines (`seconds verb ...`) retain the absolute rendered-frame
 *   clock used by offline capture (frame N <-> t = N/60). Keep timestamps
 *   sorted. Do not mix timed and untimed lines in one script.
 *
 * `pause <seconds>` delays the next event by the requested duration in either
 * form. A *point* is either literal window pixels
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
 *   helptab:commands      tab of the open F1 help overlay, by label
 *                         (unresolvable while the overlay is closed)
 *   code:glrotatef        code-panel row, by normalized prefix of its
 *                         canonical text, or by 0-based line index
 *                         (code:3); the row must be scrolled into view
 *   shell:new             Emscripten shell's DOM New button (web only)
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
 *   5.5  key@18 glEnd()       # same, paced at N chars/sec on the frame
 *                             # clock so the text types out like a person
 *                             # (bare `key` lands the whole payload on one
 *                             # frame; a following key/skey event flushes
 *                             # any unfinished paced text first, so a tight
 *                             # schedule never drops or interleaves it)
 *   6.0  key \cT              # e.g. Ctrl+T toggles animation
 *   7.0  skey f12             # special key: f1..f12, up/down/left/right,
 *                             # home, end, pageup, pagedown
 *   7.5  chord ctrl+shift c   # one modified key press: <mods> is a
 *                             # `+`-joined subset of ctrl/shift/alt, <key> is
 *                             # a skey name (chord shift f12) or a single
 *                             # printable char (ctrl folds to its control
 *                             # byte). Reaches Shift shortcuts `key`/`skey`
 *                             # can't (a bare byte carries no Shift bit).
 *   8.0  echo scene:0.25,0.76 18 2 Ctrl+K   # caption text (point, cap
 *                             # height px, on-screen seconds). Echo does not
 *                             # delay the next event; use `pause` when a
 *                             # caption needs an exclusive reading beat.
 *                             # The px size picks the nearest fixed GLUT
 *                             # bitmap font (10/12/18/24)
 *
 * The same actions can be completion-driven by omitting timestamps:
 *
 *   glide menu:scene 0.8
 *   click
 *   pause 0.5
 *   glide item:all 0.7
 *
 * `key` + `skey` let a script type a whole demo (commit lines with `key ;`,
 * navigate with `skey up`), so demos can be re-recorded from scripts; `chord`
 * reaches the modified shortcuts (Ctrl+Shift+*, Shift+F*) neither can express.
 */

/* --- controlled tours (transport + backstep) ---------------------------- */

/* A controlled tour is one of two pointer-script run kinds, alongside
 * env-driven capture (glr_pointer_script_load_env). It layers replay-style
 * transport controls on top of an untimed script: play/pause, immediate
 * forward step, backstep (via one whole-app baseline plus prefix replay), a
 * discrete speed ladder, and a persistent Done state at the end (no
 * auto-stop). The env-capture kind is never canceled, never auto-stops, and
 * never gets a HUD or transport. */
typedef enum {
    GLR_TOUR_OFF = 0,          /* no controlled tour                       */
    GLR_TOUR_BASELINE_PENDING, /* start_tour called; capture on next frame */
    GLR_TOUR_PLAYING,          /* virtual clock advancing under speed      */
    GLR_TOUR_PAUSED,           /* held; Right steps one event at a time    */
    GLR_TOUR_STEPPING,         /* transient: executing one immediate step  */
    GLR_TOUR_SEEKING,          /* resumable backstep prefix replay          */
    GLR_TOUR_DONE              /* reached the end; awaits restart/exit      */
} GlrTourPlaybackState;

/* Read-only playback view for the HUD / tests. `active` is nonzero only for a
 * controlled tour (the legacy + env kinds always report inactive here, so the
 * HUD never shows for them). `source_line` follows the plan's rule: the active
 * event's line while playing/stepping, the next event's line while paused
 * before it, the final event's line in Done, and -1 with no controlled tour. */
typedef struct {
    int                  active;
    GlrTourPlaybackState state;
    const char          *name;
    const char          *file;
    int                  hud_expanded;
    float                speed;
    int                  completed_events;
    int                  current_event;
    int                  total_events;
    int                  source_line;
} GlrTourPlaybackView;

/* Start a controlled tour from an in-memory grammar array (the Tours menu
 * path). `name`/`file` are borrowed metadata for the HUD (the catalog holds
 * static strings that outlive the run). The script must be untimed and
 * completion-driven; a timestamped or mixed script is rejected (logs to
 * stderr, returns 0 without activating). On success the tour enters
 * GLR_TOUR_BASELINE_PENDING and captures its rewind baseline on the next
 * glr_pointer_script_frame(). Returns 1 on success, 0 on a parse/mode error. */
int glr_pointer_script_start_tour(const char *name, const char *file,
                                  const char *const *lines, int line_count);

/* Snapshot of the controlled-tour transport for the HUD and tests. Safe to
 * call any time; reports active == 0 when no controlled tour is running. */
GlrTourPlaybackView glr_pointer_script_tour_view(void);

/* Toggle the controlled-tour HUD between its compact name strip and expanded
 * transport details. Returns 1 when a controlled tour consumed the request,
 * 0 when no tour is active. This presentation bit deliberately lives outside
 * the rewind baseline so Back does not unexpectedly collapse/expand the HUD. */
int glr_pointer_script_toggle_tour_hud(void);

/* True through the controller-tick phase of a rendered seek frame, including
 * the frame in which prefix replay reaches its target and enters Paused. The
 * controller uses this to keep animation time, REPL replay, view transitions,
 * and camera motion from contaminating deterministic reconstruction. */
int glr_pointer_script_tour_suppresses_app_tick(void);

/* Host transport-key handlers, dispatched by gl_repl.c BEFORE the tour-cancel
 * intercept. Only consume keys while a controlled tour is running: Space
 * (play/pause/resume/restart), +/=/- (speed), Esc (stop). Return 1 when the
 * key was consumed as a transport action, 0 to let the caller fall through to
 * cancellation / normal input. Never consume anything for the legacy or
 * env-capture kinds. */
int glr_pointer_script_handle_tour_key(unsigned char key);

/* Special-key transport handler (Left = backstep, Right = forward step).
 * Same consume/fallthrough contract as glr_pointer_script_handle_tour_key. */
int glr_pointer_script_handle_tour_special(int key);

/* Caption (echo) alpha for a given age/duration in frames. Eases in over the
 * first ~9 frames and out over the last ~30 during playback; returns full
 * opacity when `clock_frozen` (a paused/stepped tour is an inspection view, and
 * the frozen virtual clock can't advance a fade). Pure; exposed for the
 * overlay-alpha guard test. */
float glr_pointer_script_echo_alpha(int age, int dur, int clock_frozen);

/* Read GLR_POINTER_SCRIPT and parse the script it names. Returns nonzero
 * when a script is active (so main() can imply GLR_TICK_PER_FRAME). A parse
 * error reports the offending line on stderr and exits nonzero — a capture
 * run silently recording the wrong interaction is worse than failing. */
int glr_pointer_script_load_env(void);

/* Nonzero once a script has loaded. */
int glr_pointer_script_active(void);

/* Stop a running script now: release any scripted-held mouse button so the
 * app never sees a stuck drag, clear the overlay, and deactivate. Used by
 * the tour cancel path (real user input hands control back) and by the
 * auto-stop; safe to call when inactive. */
void glr_pointer_script_stop(void);

/* Nonzero while a runtime-started (tour) script is playing — as opposed to
 * the env-driven capture mode, which is never canceled by user input. */
int glr_pointer_script_tour_active(void);

/* Resolve one symbolic target token (e.g. "item:new_scene", "scene:0.5,0.5",
 * or the Emscripten-only "shell:new")
 * against the live layout, filling (*mx, *my) in mouse space. Returns 1 on
 * success, 0 when the target is unknown or currently unresolvable. The same
 * resolver every scripted point goes through — exposed so tests can pin the
 * target vocabulary to the real hit-test geometry. */
int glr_pointer_script_resolve_target(const char *target, int *mx, int *my);

/* Advance the script one frame: fire a due timed event or the next completed-
 * sequence step through the glr_ctrl_* input entry points, and step any
 * active glide. Call once per display
 * callback, before glr_ctrl_display_frame(), so the frame reflects the
 * new pointer state. No-op when inactive. */
void glr_pointer_script_frame(void);

/* Draw the overlay cursor (+ click ripple, highlight ring) over the
 * composited frame; call between glr_ctrl_display_frame() and
 * glutSwapBuffers(), like splash_render(). No-op when inactive. */
void glr_pointer_script_render_overlay(int win_w, int win_h);

#endif /* GLR_POINTER_SCRIPT_H */
