/*
 * glr_tours.h - built-in guided tours, played through the pointer-script
 * engine (src/app/glr_pointer_script.h).
 *
 * A tour is a named, completion-driven pointer script: each synthetic
 * glide, click, paced keystroke, or caption completes before the next step
 * starts, with explicit `pause` steps for intentional dwell time. The events
 * walk the user through a slice of the app (menus, editing, camera) using the
 * exact same dispatch path as live input. Tour content is file-backed like the
 * example scenes — .pointer files under tours/ listed by tours/catalog.ini
 * (or catalog-emscripten.ini for the web-safe set) and compiled in by
 * scripts/gen_tours.py. The Tours menu lists the
 * catalog; activating an entry hands the script to
 * glr_pointer_script_start_tour, which plays it as a controlled tour with
 * replay-style transport (Space play/pause, arrows step/back, +/- speed,
 * Esc exit; see src/app/glr_pointer_script.h). A real key/click/wheel event
 * that is not a transport control cancels the tour (intercepted in gl_repl.c's
 * GLUT callbacks — scripted events bypass those, so only the user can trigger
 * the cancel), and a tour that plays to the end enters a persistent Done
 * rather than stopping.
 *
 * Tour points are symbolic targets (menu:/item:/sub:/pin:/scene: — grammar
 * in glr_pointer_script.h) resolved against the live layout when each
 * event fires, so tours work at any window size and follow catalog/label
 * reordering.
 */
#ifndef GLR_TOURS_H
#define GLR_TOURS_H

/* Catalog queries for the Tours menu. */
int         glr_tours_count(void);
const char *glr_tours_name(int idx);

/* Start tour `idx`: load its script into the pointer-script engine, stop any
 * active REPL replay before the tour baseline is captured, and set the status
 * line. Returns 1 on success, 0 on a bad index or script error. */
int glr_tours_start(int idx);

#endif /* GLR_TOURS_H */
