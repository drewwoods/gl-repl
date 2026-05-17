/*
 * src/repl/example_loader.h - Lower-level example loader for tests and benches.
 *
 * Production code normally calls `repl_load_example(int)` from src/repl/core.h,
 * which layers on scene promotion, presentation reset, and editor cleanup. This
 * smaller header exposes the in-memory line-array entry point used by tests and
 * bench drivers that want the same example body loading without the surrounding
 * controller choreography.
 *
 * Phase 5 of feature/source-document-port.md split this out of
 * src/repl/core_internal.h so the catch-all internal header could shrink toward
 * parse-only helpers.
 */
#ifndef REPL_EXAMPLE_LOADER_H
#define REPL_EXAMPLE_LOADER_H

/* Drive example loading from unit tests / bench drivers without going
 * through the GLUT example dropdown. `lines` is a NULL-terminated
 * array of source lines; the loader runs the same body-emission
 * pipeline as the production path (parse + insert + auto-normal +
 * flatten) without the controller-side cleanup (presentation reset,
 * editor input wipe). */
void repl_load_example_lines_for_test(const char *const *lines);

#endif /* REPL_EXAMPLE_LOADER_H */
