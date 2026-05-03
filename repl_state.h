#ifndef REPL_STATE_H
#define REPL_STATE_H

#include "editor_state.h"  /* ReplEditorBuffer typedef + EditorState API */
#include "repl_state_views.h"
#include "repl_state_owners.h"
#include "ui_editor.h"

typedef struct {
    ReplDocumentState         document;
    ReplFlatProgramState      flat_program;
    ReplVariableState         variables;
    ReplCameraState           camera;
    ReplPresentationState     presentation;
    ReplRenderState           render;
    ReplReplayRuntimeState    replay;
    ReplSceneRuntimeState     scenes;
    ReplImportExportState     import_export;
} ReplRuntimeState;

void repl_state_capture(ReplRuntimeState *snapshot);
void repl_state_restore(const ReplRuntimeState *snapshot);


#endif /* REPL_STATE_H */
