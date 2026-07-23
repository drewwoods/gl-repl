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

/* Lean checkpoint of the REPL-owned slices EXCLUDING the derived flat
 * program. The flat program (ReplFlatProgramState, ~6.8 MB) is rebuilt from
 * the document on the next frame, so it is deliberately not stored. Used by
 * the tour-baseline snapshot (src/app/glr_tour_snapshot.c), which pairs this
 * with the editor/scene/peer captures.
 *
 * Restore rebinds evaluator predef storage, restores the special `t` binding,
 * invalidates the expression + source-scope caches, clears the flat-program
 * count, marks the flat program dirty, and drops any stale rebake / args-dirty
 * routing so no rebake runs against the just-invalidated cache. */
typedef struct {
    ReplDocumentState     document;
    ReplVariableState     variables;
    ReplRenderState       render;
    ReplSceneRuntimeState scene_runtime;
    ReplImportExportState import_export;
} ReplCheckpointState;

void repl_state_checkpoint_capture(ReplCheckpointState *out);
void repl_state_checkpoint_restore(const ReplCheckpointState *snapshot);


#endif /* REPL_STATE_H */
