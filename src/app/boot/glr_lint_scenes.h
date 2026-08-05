/*
 * glr_lint_scenes.h - `--lint-scenes <dir>` : validate .glr files, no window.
 *
 * The canonical document order and the `@camera` role tags are enforced by
 * rejection, so migrating a corpus means reading rejection messages. A loader
 * that stopped at the first error would turn that into one edit-reload cycle
 * per violation; this mode walks a directory, reports every violation in every
 * file in one pass, and never opens a GL context.
 *
 * It streams: the reader's REPL_CAMERA_MAX_DIAGS cap is a per-load inspection
 * cache and does not bound this report.
 */
#ifndef GLR_LINT_SCENES_H
#define GLR_LINT_SCENES_H

#include <stdio.h>

/* Lint every `*.glr` under `dir`. Returns the number of files with at least
 * one violation (0 = the whole directory is canonical). */
int glr_lint_scenes_dir(const char *dir, FILE *out);

#endif /* GLR_LINT_SCENES_H */
