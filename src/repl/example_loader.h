/*
 * src/repl/example_loader.h - Lower-level example loader for tests and benches.
 *
 * Production code normally calls `repl_load_example(int)`, declared here,
 * which layers on scene promotion, presentation reset, and editor cleanup. This
 * smaller header exposes the in-memory line-array entry point used by tests and
 * bench drivers that want the same example body loading without the surrounding
 * controller choreography.
 *
 * This was split out of the former src/repl/core_internal.h umbrella,
 * which has since been removed entirely so callers include the focused
 * owner headers they need directly.
 */
#ifndef REPL_EXAMPLE_LOADER_H
#define REPL_EXAMPLE_LOADER_H

/* Drive example loading from unit tests / bench drivers without going
 * through the GLUT example dropdown. `lines` is a NULL-terminated
 * array of source lines; the loader runs the same body-emission
 * pipeline as the production path (parse + insert + auto-normal +
 * flatten) without the controller-side cleanup (presentation reset,
 * editor input wipe).
 *
 * Returns the post-load cursor target (= document line count after
 * the example body emits). Callers that care about cursor placement
 * apply the value via editor_state_edit_line_set() above the β
 * boundary; tests that just want the body loaded can ignore the
 * return (implemented in phase 3.6.4; see the edit-line-ownership
 * plan doc). */
int repl_load_example(int idx);
int repl_load_example_lines_for_test(const char *const *lines);

#endif /* REPL_EXAMPLE_LOADER_H */
