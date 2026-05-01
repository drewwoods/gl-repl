#ifndef REPL_STATE_H
#define REPL_STATE_H

#include "repl_state_views.h"
#include "repl_state_owners.h"

typedef struct {
    ReplDocumentState         document;
    ReplFlatProgramState      flat_program;
    ReplVariableState         variables;
    ReplEditorInputState      editor_input;
    ReplEditorBuffer          editor_buffer;
    ReplSelectionState        selection;
    ReplClipboardState        clipboard;
    ReplCodePanelRuntimeState code_panel;
    ReplHelpState             help;
    ReplVariablePanelState    variable_panel;
    ReplVariableDragState     variable_drag;
    ReplProfilePanelState     profile_panel;
    ReplStatusState           status;
    ReplSearchState           search;
    ReplAutocompleteState     autocomplete;
    ReplCameraState           camera;
    ReplPointerState          pointer;
    ReplViewportState         viewport;
    ReplPresentationState     presentation;
    ReplRenderState           render;
    ReplReplayRuntimeState    replay;
    ReplSceneRuntimeState     scenes;
    ReplImportExportState     import_export;
} ReplRuntimeState;

void repl_state_capture(ReplRuntimeState *snapshot);
void repl_state_restore(const ReplRuntimeState *snapshot);


#endif /* REPL_STATE_H */
