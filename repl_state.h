#ifndef REPL_STATE_H
#define REPL_STATE_H

#include "repl_eval.h"
#include "repl_state_views.h"
#include "repl_state_owners.h"

typedef ReplDocumentState ReplDocumentRuntimeState;

typedef ReplFlatProgramState ReplFlatProgramRuntimeState;

typedef ReplVariableState ReplVariableRuntimeState;

typedef ReplEditorInputState ReplEditorInputRuntimeState;

typedef ReplSelectionState ReplSelectionRuntimeState;

typedef ReplClipboardState ReplClipboardRuntimeState;

typedef ReplCodePanelRuntimeState ReplCodePanelRuntimeStorage;

typedef ReplHelpState ReplHelpRuntimeState;

typedef ReplVariablePanelState ReplVariablePanelRuntimeState;

typedef ReplVariableDragState ReplVariableDragRuntimeState;

typedef ReplProfilePanelState ReplProfilePanelRuntimeState;

typedef ReplStatusState ReplStatusRuntimeState;

typedef ReplSearchState ReplSearchRuntimeState;

typedef ReplAutocompleteState ReplAutocompleteRuntimeState;

typedef ReplCameraState ReplCameraRuntimeState;

typedef ReplPointerState ReplPointerRuntimeState;

typedef ReplViewportState ReplViewportRuntimeState;

typedef ReplPresentationState ReplPresentationRuntimeState;

typedef ReplRenderState ReplRenderRuntimeState;

typedef ReplReplayRuntimeState ReplReplayRuntimeStateStore;

typedef ReplSceneRuntimeState ReplSceneRuntimeStateStore;

typedef ReplImportExportState ReplImportExportRuntimeState;

typedef struct {
    ReplDocumentRuntimeState         document;
    ReplFlatProgramRuntimeState      flat_program;
    ReplVariableRuntimeState         variables;
    ReplEditorInputRuntimeState      editor_input;
    ReplSelectionRuntimeState        selection;
    ReplClipboardRuntimeState        clipboard;
    ReplCodePanelRuntimeStorage      code_panel;
    ReplHelpRuntimeState             help;
    ReplVariablePanelRuntimeState    variable_panel;
    ReplVariableDragRuntimeState     variable_drag;
    ReplProfilePanelRuntimeState     profile_panel;
    ReplStatusRuntimeState           status;
    ReplSearchRuntimeState           search;
    ReplAutocompleteRuntimeState     autocomplete;
    ReplCameraRuntimeState           camera;
    ReplPointerRuntimeState          pointer;
    ReplViewportRuntimeState         viewport;
    ReplPresentationRuntimeState     presentation;
    ReplRenderRuntimeState           render;
    ReplReplayRuntimeStateStore      replay;
    ReplSceneRuntimeStateStore       scenes;
    ReplImportExportRuntimeState     import_export;
} ReplRuntimeState;

void repl_state_capture(ReplRuntimeState *snapshot);
void repl_state_restore(const ReplRuntimeState *snapshot);


#endif /* REPL_STATE_H */
