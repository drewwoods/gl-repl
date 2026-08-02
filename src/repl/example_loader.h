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

/* Maximum post-metadata body lines a `.glr` example can load. This is an
 * example-authoring cap, separate from the larger source document /
 * command-store capacity, so callers can surface the real example limit in
 * editor or catalog diagnostics. */
#define EXAMPLE_BODY_LINES_MAX 512

/* Drive example loading from unit tests / bench drivers without going
 * through the GLUT example dropdown. `lines` is a NULL-terminated
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
int repl_load_example_lines_for_test(const char *const *lines);

/* If `lines` starts with a valid 5-line camera preset block (the marker may
 * be compact `// camera` or decorated `// --- Camera ---`), validate it,
 * apply it through the camera bridge, and return the number of lines consumed
 * (5). Returns 0 - applying nothing - when the block is absent or malformed,
 * so a truncated header falls through to ordinary line parsing instead of
 * being silently swallowed. Shared with the tutorial setup-scaffold loader,
 * which honors the same header vocabulary. */
int repl_example_consume_camera_header(const char *const *lines);

#endif /* REPL_EXAMPLE_LOADER_H */
