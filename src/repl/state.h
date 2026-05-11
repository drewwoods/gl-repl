#ifndef REPL_STATE_H
#define REPL_STATE_H

#include "repl/state_views.h"
#include "repl/state_owners.h"

// presentation slice moved to glr_state.{c,h} (step 7a of
// feature/decouple-repl-from-gl-repl-alt.md). Storage lives on the
// app-side GlrState; capture/restore use glr_state_capture /
// _restore in lockstep with the REPL halves below.
// replay moved to replay_state.c (Phase F commit 33); peer-owned.
typedef struct {
    ReplDocumentState         document;
    ReplFlatProgramState      flat_program;
    ReplVariableState         variables;
    ReplRenderState           render;
    ReplSceneRuntimeState     scenes;
    ReplImportExportState     import_export;
} ReplRuntimeState;

void repl_state_capture(ReplRuntimeState *snapshot);
void repl_state_restore(const ReplRuntimeState *snapshot);


#endif /* REPL_STATE_H */
