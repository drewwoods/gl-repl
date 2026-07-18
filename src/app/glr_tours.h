/*
 * glr_tours.h - built-in guided tours, played through the pointer-script
 * engine (src/app/glr_pointer_script.h).
 *
 * A tour is a named, compiled-in pointer script: timed synthetic pointer
 * glides, clicks, keystrokes, and stroke-text captions that walk the user
 * through a slice of the app (menus, editing, camera) using the exact same
 * dispatch path as live input. The Tours menu lists the catalog; activating
 * an entry hands the script to glr_pointer_script_start_lines. Any real
 * key press, click, or wheel event cancels a running tour (intercepted in
 * gl_repl.c's GLUT callbacks — scripted events bypass those, so only the
 * user can trigger the cancel), and a tour that runs to completion stops
 * itself.
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

/* Start tour `idx`: load its script into the pointer-script engine and set
 * the status line. Returns 1 on success, 0 on a bad index or script error. */
int glr_tours_start(int idx);

#endif /* GLR_TOURS_H */
