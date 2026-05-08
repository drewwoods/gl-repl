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
 * The snapshot embeds existing by-value `Repl*State` slices verbatim plus a
 * handful of pointer-style views (`FlatProgramView`, `ReplPredefView`,
 * `ReplVariableView`, `ReplEditorInputView`, `ReplImportExportView`) whose
 * stable storage is owned by `repl_state.c`. Embedded pointers are valid
 * for the duration of the frame; the snapshot itself is `const` to the
 * renderer.
 */
#ifndef UI_SNAPSHOT_H
#define UI_SNAPSHOT_H

#include "editor_state.h"  /* ReplEditorInputView (Phase 1 commit 5) */
#include "editor_help_session.h"
#include "repl_state_views.h"
#include "repl_eval.h"
#include "repl_core.h"
#include "repl_flatten.h"
#include "editor.h"
#include "color_picker.h"

/* Forward decl: snapshot only carries a pointer; the full type lives
 * in ui_tabbed_overlay.h and is included by the controller (which
 * builds the value) and the renderer (which reads it). */
struct UiOverlayContent;

typedef struct UiRenderSnapshot {
    /* By-value state slices */
    ReplViewportState           viewport;
    ReplPresentationState       presentation;
    ReplCodePanelRuntimeState   code_panel;
    ReplHelpState               help;
    EditorHelpSession           help_session;
    ReplVariablePanelState      variable_panel;
    ReplProfilePanelState       profile_panel;
    ReplStatusState             status;
    ReplSearchState             search;
    ReplAutocompleteState       autocomplete;
    ReplCameraState             camera;
    ReplPointerState            pointer;
    ReplRenderState             render;
    ReplReplayRuntimeState      replay;
    ReplSceneRuntimeState       scenes;
    ReplVariableDragState       variable_drag;
    ReplSelectionState          selection;
    EditorScrollState           scroll;
    ColorPickerView             color_picker;

    /* Pointer-shaped read-only views (storage owned by repl_state.c) */
    ReplVariableView            variables;
    ReplEditorInputView         editor_input;
    ReplImportExportView        import_export;
    FlatProgramView             flat_program;
    ReplPredefView              predef;

    /* Document */
    const GLCmd                *document_cmds;
    int                         document_count;
    int                         edit_line;
    int                         normals_dirty;

    /* Convenience scalars (mirror editor_input/code_panel for terse access) */
    int                         insert_mode;
    int                         cursor_pos;
    int                         input_len;
    int                         pending_newline_len;
    int                         flat_program_count;

    /* Grid arrays */
    const float                *grid_major_steps;
    const float                *grid_extents;

    /* User scenes */
    int                         user_scene_count;
    int                         user_scene_active_idx;
    int                         user_scene_slot_used[MAX_USER_SCENES];
    const char                 *user_scene_names[MAX_USER_SCENES];

    /* Workspace dir convenience */
    const char                 *workspace_dir;

    /* F1 help overlay text content (REPL-built; the renderer is
     * tabbed-overlay-shaped and feature-agnostic). */
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
