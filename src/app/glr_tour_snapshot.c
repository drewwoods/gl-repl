/*
 * glr_tour_snapshot.c - whole-app tour baseline (see glr_tour_snapshot.h).
 *
 * Composes the focused per-owner capture/restore surfaces into one opaque
 * baseline. Capture heap-allocates the whole struct plus the two opaque
 * sub-snapshots (scene catalog, undo history); restore replays the owner
 * restores in the plan's order (steps 3-10). Derived state (flat program,
 * renderer resources, controller frame caches) is intentionally not stored.
 */
#include "app/glr_tour_snapshot.h"

#include "app/glr_camera.h"
#include "app/glr_ctrl_internal.h"   /* GlrViewTransitionSnapshot + capture */
#include "app/glr_state.h"
#include "editor/help_session.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "repl/scenes.h"
#include "repl/state.h"
#include "subsystems/color_picker/color_picker_state.h"
#include "subsystems/replay/replay_state.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "subsystems/variable_panel/variable_panel_state.h"
#include "ui/app/menu_bar.h"
#include "ui/app/overlay_layout.h"
#include "ui/app/state.h"

#include <stdlib.h>

struct GlrTourSnapshot {
    /* By-value slices (live inside this heap allocation). */
    ReplCheckpointState       repl;
    EditorSessionSnapshot     editor;
    GlrState                  glr_state;
    UiState                   ui_state;
    ReplayRuntimeState        replay;
    TutorialRuntimeState      tutorial;
    VariablePanelState        variable_panel;
    EditorHelpSession         help_session;
    GlrCameraRuntimeSnapshot  camera;
    GlrViewTransitionSnapshot view_transition;
    UiMenuBarRuntimeSnapshot  menu_bar;
    ColorPickerRuntimeSnapshot color_picker;
    UiOverlayLayoutSnapshot   overlay_layout;
    /* Separately heap-allocated (large / opaque). NULL until captured. */
    EditorUndoHistorySnapshot *undo;
    ReplScenesSnapshot        *scenes;
};

GlrTourSnapshot *glr_tour_snapshot_capture(void) {
    GlrTourSnapshot *s = (GlrTourSnapshot *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    /* Opaque heap sub-snapshots first, so a failure aborts cheaply. */
    s->undo = editor_undo_history_capture();
    if (!s->undo) {
        glr_tour_snapshot_destroy(s);
        return NULL;
    }
    s->scenes = repl_scenes_snapshot_capture();
    if (!s->scenes) {
        glr_tour_snapshot_destroy(s);
        return NULL;
    }

    repl_state_checkpoint_capture(&s->repl);
    editor_state_session_capture(&s->editor);
    glr_state_capture(&s->glr_state);
    ui_state_capture(&s->ui_state);
    replay_state_capture(&s->replay);
    s->tutorial = tutorial_state_view();
    variable_panel_state_capture(&s->variable_panel);
    editor_help_session_capture(&s->help_session);
    glr_camera_runtime_capture(&s->camera);
    glr_ctrl_view_transition_capture(&s->view_transition);
    ui_menu_bar_runtime_capture(&s->menu_bar);
    color_picker_runtime_capture(&s->color_picker);
    ui_overlay_layout_capture(&s->overlay_layout);
    return s;
}

int glr_tour_snapshot_restore(const GlrTourSnapshot *s) {
    if (!s)
        return 0;

    /* Step 3: scene catalog (before the live document so an active-slot
     * restore observes the catalog it belongs to). */
    repl_scenes_snapshot_restore(s->scenes);
    /* Step 4: app-side presentation/render/grid state. */
    glr_state_restore(&s->glr_state);
    /* Step 5: REPL checkpoint (leaves the flat program dirty). */
    repl_state_checkpoint_restore(&s->repl);
    /* Step 6: editor session (buffer restores in lockstep with the doc). */
    editor_state_session_restore(&s->editor);
    /* Step 7: undo/redo history. */
    editor_undo_history_restore(s->undo);
    /* Step 8: camera + view transition. */
    glr_camera_runtime_restore(&s->camera);
    glr_ctrl_view_transition_restore(&s->view_transition);
    /* Step 9: replay, tutorial, variable-panel, color-picker, help session. */
    replay_state_restore(&s->replay);
    *tutorial_state_mut() = s->tutorial;
    variable_panel_state_restore(&s->variable_panel);
    color_picker_runtime_restore(&s->color_picker);
    editor_help_session_restore(&s->help_session);
    /* Step 10: UI chrome, menu state, overlay layout. */
    ui_state_restore(&s->ui_state);
    ui_menu_bar_runtime_restore(&s->menu_bar);
    ui_overlay_layout_restore(&s->overlay_layout);
    return 1;
}

void glr_tour_snapshot_destroy(GlrTourSnapshot *s) {
    if (!s)
        return;
    editor_undo_history_destroy(s->undo);   /* NULL-safe */
    repl_scenes_snapshot_destroy(s->scenes); /* NULL-safe */
    free(s);
}
