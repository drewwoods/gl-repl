/*
 * src/repl/core.h - REPL compatibility facade.
 *
 * Temporarily reexports APIs that have moved to their natural owner headers.
 * New code should include the owner header directly.
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
#include "repl/pipeline.h"    /* Compatibility: live pipeline entry points moved out of core. */
#include "repl/program_query.h" /* Compatibility: program queries moved out of core. */
#include "repl/reformat.h"   /* Compatibility: reformatter moved out of core. */
#include "repl/scenes.h"     /* Compatibility: scene/workspace APIs moved out of core. */
#include "repl/state_notify.h" /* Compatibility: dirty-state notifications moved out of core. */
#include "repl/time.h"       /* Compatibility: timekeeping moved out of core. */
#include "source_document.h" /* SourceTextView document view */

#endif /* REPL_CORE_H */
