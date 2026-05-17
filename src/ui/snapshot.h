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
#include "app/glr_state.h"     /* GlrRenderState (step 7a) */
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

/* Scene tab strip view. Both constants are hardcoded here, NOT derived from
 * repl macros — snapshot.h must stay free of repl-layer includes (UI-layer
 * purity). Equivalence to the repl source-of-truth is enforced by
 * STATIC_ASSERT in glr_ctrl.c (which already includes repl/core.h). */
enum { UI_SCENE_TAB_NAME_MAX = 64 };   /* == USER_SCENE_NAME_MAX */
enum { UI_SCENE_TAB_CAP = 9 };         /* == MAX_USER_SCENES + 1 */

/* Resolved reshape() projection block, frozen into the snapshot once per
 * frame by the controller so the code panel's row-count and render
 * passes (which run on opposite sides of scene_render_3d_scene) always
 * agree. Dimensions hardcoded for UI-layer purity — equivalence with
 * REPL_EXPORT_PROJ_LINES / _LINE_MAX is STATIC_ASSERTed in glr_ctrl.c. */
enum { UI_RESHAPE_PROJ_LINES = 4 };    /* == REPL_EXPORT_PROJ_LINES */
enum { UI_RESHAPE_PROJ_LINE_MAX = 96 };/* == REPL_EXPORT_PROJ_LINE_MAX */

typedef enum { UI_SCENE_TAB_USER = 0, UI_SCENE_TAB_EXAMPLE } UiSceneTabKind;
typedef struct {
    char           name[UI_SCENE_TAB_NAME_MAX];
    UiSceneTabKind kind;
    int            slot;    /* user-scene slot, or -1 for the example tab */
    int            active;  /* this tab is the active scene */
} UiSceneTab;
typedef struct {
    UiSceneTab tabs[UI_SCENE_TAB_CAP];
    int        count;
    int        active_idx;  /* display idx of the active tab, or -1 */
} UiSceneTabList;

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

    /* Scene tab strip — derived each frame, no persistent model */
    UiSceneTabList              scene_tabs;

    /* Inline scene-rename prompt. Owns its own display state (sourced
     * from editor_inline_rename_buffer) instead of riding the shared
     * transient status line, so other repl_set_status() writers can't
     * overwrite the prompt mid-rename. rename_text reuses the scene-tab
     * name cap (== USER_SCENE_NAME_MAX). */
    int                         rename_active;
    char                        rename_text[UI_SCENE_TAB_NAME_MAX];

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

    /* Reshape() projection body, resolved once per frame by the
     * controller from the scene's nearest-steady projection (see
     * scene_get_active_projection). The code panel expands the
     * REPL_EXPORT_RESHAPE_PROJ_SENTINEL footer slot from this, so its
     * row-count and render passes agree even when a 2D/3D transition
     * changes the line count mid-frame. */
    char                        reshape_proj_lines[UI_RESHAPE_PROJ_LINES]
                                                  [UI_RESHAPE_PROJ_LINE_MAX];
    int                         reshape_proj_count;
} UiRenderSnapshot;

#endif /* UI_SNAPSHOT_H */
