/*
 * src/repl/core.h - REPL pipeline facade.
 *
 * Exposes the few command-pipeline entry points that still lack narrower
 * owners: flatten/autonormal recompute, source-dirty invalidation, and the
 * default output save wrapper. The includes below temporarily compatibility-
 * reexport APIs that have moved to their natural owner headers.
 *
 * Sibling headers callers may need to include directly:
 *   src/repl/state.h           — read-only state views
 *   src/repl/state_owners.h    — mutable accessors + reset helpers
 *   src/repl/example_loader.h  — example loading
 *   src/repl/export.h          — save/load file I/O
 *   src/repl/flatten.h         — flatten primitives
 *   src/repl/scenes.h          — scene/workspace APIs
 *   src/subsystems/replay/replay.h — replay state machine
 *   src/editor/input.h         — editor commit / navigation entry points
 *
 * Many functions formerly declared here moved out as the pipeline grew its
 * own narrower owners; this file no longer re-declares or re-documents them.
 */
#ifndef REPL_CORE_H
#define REPL_CORE_H

#include <stdio.h>

#include "repl/bootstrap.h"   /* Compatibility: startup bootstrap moved out of core. */
#include "repl/example_loader.h" /* Compatibility: example loading moved out of core. */
#include "repl/export.h"     /* ReplExportLayout and save/load helpers */
#include "repl/flatten.h"
#include "repl/geometry_query.h" /* Compatibility: cursor/feed queries moved out of core. */
#include "repl/host_effects.h" /* Compatibility: host bridge moved out of core. */
#include "repl/program_query.h" /* Compatibility: program queries moved out of core. */
#include "repl/reformat.h"   /* Compatibility: reformatter moved out of core. */
#include "repl/scenes.h"     /* Compatibility: scene/workspace APIs moved out of core. */
#include "repl/time.h"       /* Compatibility: timekeeping moved out of core. */
#include "source_document.h" /* SourceTextView document view */

/* --- Save / load ------------------------------------------------------- */

/* Write the active user scene (or current example/transient buffer) to
 * ./output.c. This is the default single-file export path behind Ctrl+S when no
 * named scene-specific save target takes over. `layout` is the controller-built
 * ReplExportLayout passed through to the exporter as opaque integers. */
void repl_save_default_output(const ReplExportLayout *layout);

/* --- Command pipeline -------------------------------------------------- */

/* Rebuild the live flat program from the current source commands (idempotent).
 * Expansion honors the laziness flag set by mark_normals_dirty(); call this
 * once per frame before execution if the source array changed. */
void repl_flatten_commands(int edit_line_idx);

/* Recompute auto-normals for every glBegin/glEnd batch in the source
 * array. Called automatically when source commands are modified.
 *
 * `autonormal_enabled` gates the recompute. The toggle lives on
 * `GlrState.presentation` (step 7a of
 * feature/decouple-repl-from-gl-repl-alt.md); callers pass the value
 * explicitly because `src/repl/autonormal.c` is a REPL pipeline TU and
 * cannot reach into glr_state. Pass 0 for an unconditional no-op. */
void repl_recompute_autonormals(int autonormal_enabled,
                                int *edit_line_inout);

/* Mark the source program dirty: invalidates the auto-normal pass, the
 * flat program, and the source-scope depth cache. Every source mutation
 * goes through here. Was named repl_mark_normals_dirty until the audit
 * pointed out it does much more than that. */
void        repl_mark_source_dirty(void);

#endif /* REPL_CORE_H */
