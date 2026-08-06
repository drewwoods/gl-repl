/*
 * glr_boot_dumps.h - CLI dump/export-and-exit boot paths.
 *
 * The `--dump-*` / `--flat-histogram` / `--export-c` / `--export-glr` flags
 * select a GL-free bootstrap that runs a diagnostic dump to stdout (or writes
 * the exported scene file) and exits instead of opening a window. This is a
 * boot-band concern:
 * it consumes the parsed GlrCliOptions (glr_cli) before any frame runs and
 * never returns to the main loop. It reuses the controller-band diagnostic
 * formatters in glr_debug (a downward call), which is why the dispatch lives
 * here rather than in glr_debug itself - that keeps the reusable dump
 * primitives free of any boot (glr_cli) dependency.
 *
 * `--export-ply` is deliberately NOT here: feedback capture needs a live GL
 * context, so it is consumed on the first display callback instead.
 */
#ifndef GLR_BOOT_DUMPS_H
#define GLR_BOOT_DUMPS_H

#include <stdio.h>

#include "app/boot/glr_cli.h" /* GlrCliOptions */

/* CLI dump dispatch. If opts selects any --dump-* / --flat-histogram flag,
 * bootstrap the REPL (GL-free: no context, no window), load --example if
 * given, run the requested dumps to `out` (NULL -> stdout), and return 1 so
 * the caller exits 0. Returns 0 when no dump flag is set (proceed to the
 * windowed run). */
int glr_boot_run_dumps(const GlrCliOptions *opts, FILE *out);

/* CLI --export-c / --export-glr dispatch. If either path is set, bootstrap
 * the REPL the same GL-free way glr_boot_run_dumps does, load --example if
 * given, and write the session out: --export-c as a standalone C program (the
 * Ctrl+S format), --export-glr as the .glr authoring format (File → Save
 * .glr). Both may be given at once - the session loads once and is written
 * twice.
 *
 * The exported C's window size comes from --window: no viewport exists on
 * this path, so the requested size is seeded into the UI layout first,
 * keeping the exported aspect ratio the same as a windowed save's. The .glr
 * format has no such scaffold and ignores it.
 *
 * Returns 1 when either flag was present (caller exits with *exit_code: 0
 * when every requested write succeeded, 1 otherwise), 0 when neither was. */
int glr_boot_run_exports(const GlrCliOptions *opts, int *exit_code);

#endif /* GLR_BOOT_DUMPS_H */
