#ifndef REPL_STATE_H
#define REPL_STATE_H

#include "repl_state_views.h"
#include "repl_state_owners.h"

typedef struct {
    ReplDocumentState         document;
    ReplFlatProgramState      flat_program;
    ReplVariableState         variables;
    ReplPresentationState     presentation;
    ReplRenderState           render;
    /* replay moved to replay_state.c (Phase F commit 33); peer-owned. */
    ReplSceneRuntimeState     scenes;
    ReplImportExportState     import_export;
} ReplRuntimeState;

void repl_state_capture(ReplRuntimeState *snapshot);
void repl_state_restore(const ReplRuntimeState *snapshot);


#endif /* REPL_STATE_H */
