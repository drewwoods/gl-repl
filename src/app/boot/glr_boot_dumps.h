/*
 * glr_boot_dumps.h - CLI dump-and-exit boot path.
 *
 * The `--dump-*` / `--flat-histogram` flags select a GL-free bootstrap that
 * runs a diagnostic dump to stdout and exits instead of opening a window.
 * This is a boot-band concern: it consumes the parsed GlrCliOptions (glr_cli)
 * before any frame runs and never returns to the main loop. It reuses the
 * controller-band diagnostic formatters in glr_debug (a downward call), which
 * is why the dispatch lives here rather than in glr_debug itself - that keeps
 * the reusable dump primitives free of any boot (glr_cli) dependency.
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

#endif /* GLR_BOOT_DUMPS_H */
