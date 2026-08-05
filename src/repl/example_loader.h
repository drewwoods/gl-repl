/*
 * src/repl/example_loader.h - Lower-level example loader.
 *
 * The app calls `repl_load_example(int)`, declared here, which layers on scene
 * promotion, presentation reset, and editor cleanup. This smaller header
 * exposes the in-memory line-array entry point for callers that have `.glr`
 * scene text but no example index and want the same body loading without the
 * surrounding controller choreography: tests, bench drivers, and
 * tools/repl_live_demo (which watches `.glr` files edited externally).
 *
 * This was split out of the former src/repl/core_internal.h umbrella,
 * which has since been removed entirely so callers include the focused
 * owner headers they need directly.
 */
#ifndef REPL_EXAMPLE_LOADER_H
#define REPL_EXAMPLE_LOADER_H

/* Maximum post-metadata body lines a `.glr` example can load. This is an
 * example-authoring cap, separate from the larger source document /
 * command-store capacity, so callers can surface the real example limit in
 * editor or catalog diagnostics. */
#define EXAMPLE_BODY_LINES_MAX 512

/* Load `.glr` scene text without going through the example catalog / GLUT
 * example dropdown. `lines` is a NULL-terminated
 * array of source lines; the loader runs the same body-emission
 * pipeline as the production path (parse + insert + auto-normal +
 * flatten) without the controller-side cleanup (presentation reset,
 * editor input wipe).
 *
 * Returns the post-load cursor target (= document line count after
 * the example body emits). Callers that care about cursor placement
 * apply the value via editor_state_edit_line_set() at the editor boundary;
 * tests that only need the body loaded can ignore the return. */
int repl_load_example(int idx);
int repl_load_example_lines(const char *const *lines);

/* Camera rows are recognised by their `@camera` role tags, wherever they
 * sit - there is no header region and nothing to "consume N lines of".
 * Every loader offers each line to one src/repl/camera_header.c reader and
 * skips the ones it consumes; see repl_camera_header_offer(). */

#endif /* REPL_EXAMPLE_LOADER_H */
