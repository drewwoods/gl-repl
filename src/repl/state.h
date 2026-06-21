/*
 * src/repl/state.h - Aggregate REPL runtime snapshot API.
 */
#ifndef REPL_STATE_H
#define REPL_STATE_H

#include "repl/state_views.h"

/* Snapshot of the REPL-owned runtime slices: document, flat program,
 * variables, executor-mutated render state, active scene/workspace identity,
 * and import/export buffers.
 *
 * This intentionally excludes peer owners such as editor, UI, app
 * presentation/render policy, and replay. It also excludes the user-scene
 * catalog slots in src/repl/scenes.c; scene slot payloads are managed there
 * through SceneSnapshot (src/repl/scene_snapshot.h).
 */
typedef struct {
    ReplDocumentState         document;
    ReplFlatProgramState      flat_program;
    ReplVariableState         variables;
    ReplRenderState           render;
    ReplSceneRuntimeState     scene_runtime;
    ReplImportExportState     import_export;
} ReplRuntimeState;

/* Copy the live REPL-owned runtime state into or out of a caller-supplied
 * snapshot buffer. Neither function touches controller/editor/UI peer state. */
void repl_state_capture(ReplRuntimeState *snapshot);
void repl_state_restore(const ReplRuntimeState *snapshot);


#endif /* REPL_STATE_H */
