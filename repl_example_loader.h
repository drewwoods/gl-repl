/*
 * repl_example_loader.h - Example loader public surface.
 *
 * Production code calls `repl_load_example(int)` (declared in
 * repl_core.h) — the public entry point with all side effects (scene
 * promotion, presentation reset, editor cleanup dispatch). This header
 * exposes the lower-level test/bench entry that drives example loading
 * directly from in-memory line arrays.
 *
 * Phase 5 of feature/source-document-port.md split this out of
 * repl_core_internal.h so the catch-all "core internal" header can
 * shrink toward parse-only.
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
