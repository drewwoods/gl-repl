/*
 * src/repl/state.h - Aggregate REPL runtime snapshot API.
 *
 * Includes the slice-level read/write accessors plus whole-state capture and
 * restore helpers for callers that need to snapshot REPL-owned runtime data.
 */
#ifndef REPL_STATE_H
#define REPL_STATE_H

#include "repl/state_views.h"
#include "repl/state_owners.h"

/* Snapshot of the REPL-owned runtime slices.
 *
 * `repl_state_capture()` / `_restore()` copy just the REPL halves: document,
 * flat program, predefined variables, runtime-mutated render state, scene
 * bookkeeping, and import/export buffers. App-owned presentation state now
 * lives in glr_state.{c,h}, and replay runtime state lives in
 * src/widgets/replay_state.c; callers that need a full-world snapshot pair
 * those owners' capture helpers with this one. The slice split came from step
 * 7a of feature/decouple-repl-from-gl-repl-alt.md and the replay peer move.
 */
typedef struct {
    ReplDocumentState         document;
    ReplFlatProgramState      flat_program;
    ReplVariableState         variables;
    ReplRenderState           render;
    ReplSceneRuntimeState     scenes;
    ReplImportExportState     import_export;
} ReplRuntimeState;

/* Copy the live REPL-owned runtime state into or out of a caller-supplied
 * snapshot buffer. Neither function touches controller/editor/UI peer state. */
void repl_state_capture(ReplRuntimeState *snapshot);
void repl_state_restore(const ReplRuntimeState *snapshot);


#endif /* REPL_STATE_H */
