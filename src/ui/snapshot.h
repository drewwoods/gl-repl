/*
 * ui_snapshot.h - frame-frozen UI render snapshot.
 *
 * The controller (`imrepl_ctrl.c`) builds one `UiRenderSnapshot` per frame
 * from `ReplRuntimeState` and passes it to every `ui_*_render()` entry
 * point. UI render code reads only from the snapshot; it does not call
 * `repl_state_*()` directly. This is the symmetric counterpart of
 * `SceneRenderConfig` for the 2D editor chrome.
 *
 * Render-discovered state (cursor pixel position, hit rects) flows back to
 * the controller through `Ui*Output` structs; renderers never call
 * `_mut()` accessors directly.
 *
 * The snapshot embeds existing by-value `Repl*State` slices verbatim plus
 * pointer-style views (`ReplEditorInputView`, `ReplImportExportView`) whose
 * stable storage is owned by `src/repl/state.c`. Embedded pointers are valid for
 * the duration of the frame; the snapshot itself is `const` to the renderer.
 */
#ifndef UI_SNAPSHOT_H
#define UI_SNAPSHOT_H

#include "editor/state.h"  /* ReplEditorInputView (Phase 1 commit 5) */
#include "editor/help_session.h"
#include "glr_state.h"     /* GlrRenderState (step 7a) */
#include "repl/state_views.h"
#include "repl/eval.h"
#include "editor.h"
#include "widgets/color_picker_state.h"

#include "state_types.h"

/* Forward decl: snapshot only carries a pointer; the full type lives
 * in ui_tabbed_overlay.h and is included by the controller (which
 * builds the value) and the renderer (which reads it). */
struct UiOverlayContent;

enum { UI_VARIABLE_NAME_MAX = 16 };

typedef struct {
    char         name[UI_VARIABLE_NAME_MAX];
    const float *value;
} UiVariable;

typedef struct {
    const UiVariable *vars;
    int               count;
} UiVariableList;

typedef struct UiRenderSnapshot {
    /* By-value state slices */
    ReplViewportState           viewport;
    ReplCodePanelRuntimeState   code_panel;
    ReplHelpState               help;
    EditorHelpSession           help_session;
    ReplVariablePanelState      variable_panel;
    ReplProfilePanelState       profile_panel;
    ReplStatusState             status;
    ReplSearchState             search;
    ReplAutocompleteState       autocomplete;
    ReplPointerState            pointer;
    GlrRenderState              render;
    ReplReplayRuntimeState      replay;
    ReplSceneRuntimeState       scenes;
    EditorScrollState           scroll;
    ColorPickerView             color_picker;

    /* Pointer-shaped read-only views (storage owned by src/repl/state.c) */
    ReplEditorInputView         editor_input;
    ReplImportExportView        import_export;

    /* UI-facing variable rows. Names are copied into the snapshot; values
     * point at the live source values for this frame. */
    UiVariable                  variable_panel_var_storage[MAX_PREDEF_VARS];
    UiVariableList              variable_panel_vars;

    /* Document */
    const GLCmd                *document_cmds;
    int                         document_count;
    int                         edit_line;

    /* Convenience scalars (mirror editor_input/code_panel for terse access) */
    int                         flat_program_count;
    float                       anim_time;

    /* User scenes */
    int                         user_scene_active_idx;

    /* F1 help overlay text content (controller-adapted from REPL help
     * text; the renderer is tabbed-overlay-shaped and feature-agnostic). */
    const struct UiOverlayContent *help_content;

    /* Per-frame editor overlay snapshots (controller-pushed). */
    const EditorTransformerList *editor_transformers;
    const EditorHighlightList   *editor_highlights;
    const EditorVirtualLineList *editor_virtual_lines;

    /* Selection range materialized once so the per-row branch in the
     * code panel does not call back into clipboard helpers. */
    int                         selection_active;
    int                         selection_lo;
    int                         selection_hi;

    /* Indent + statusbar derived state. The controller computes these
     * once per frame from the depth cache so the render path does not
     * re-derive them per row or call into repl_source_scope_*. */
    int                         active_indent_chars;
    int                         trailing_indent_chars;
    int                         in_begin_block;
    GLenum                      current_begin_mode;
} UiRenderSnapshot;

#endif /* UI_SNAPSHOT_H */
