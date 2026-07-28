/*
 * glr_ctrl_router.c - GLUT input dispatch + UiHit routing for the controller.
 *
 * Carved out of glr_ctrl.c (the ~1450-line input half): keyboard / special /
 * mouse / motion / wheel entry points, the glr_ctrl_router_* handlers, the
 * UiHit code-panel route_* dispatch + drag/double-click tracking, help-overlay
 * and scene-cycle routing, and the SIGINT-quit lifecycle. Pure routing — it
 * calls back into glr_ctrl.c only through the seams in glr_ctrl_internal.h
 * (drag snapshot, input-effect apply) and the public glr_ctrl.h surface, and
 * reaches no scene/ headers. It includes ui/ headers, so it is on the
 * check-controller-boundaries ui allowlist.
 */
#include "app/glr_ctrl.h"
#include "app/glr_ctrl_export.h"

#include "app/glr_ctrl_replay_annotations.h"
#include "app/glr_pointer_script.h"
#include "subsystems/replay/replay_render.h"
#include "subsystems/edit_overlays/edit_overlays.h"
#include "c_compat.h"  /* STATIC_ASSERT (C99/C11 portable) */
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include "gl_includes.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include "app/glr_audio.h"
#include "subsystems/color_picker/color_picker_state.h"
#include "ui/subsystems/color_picker.h"   /* UI_HIT_COLOR_SWATCH routing */
#include "editor/clipboard.h"
#include "app/glr_completion.h"
#include "app/glr_defaults.h"        /* CFG_DEFAULT_* */
#include "app/glr_state.h"
#include "editor/commit.h"
#include "editor/completion.h"
#include "editor/help_session.h"
#include "editor/inline_file_prompt.h"
#include "editor/inline_rename.h"
#include "editor/input.h"
#include "editor/replace.h"
#include "editor/search.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "app/glr_actions.h"
#include "app/glr_config.h"
#include "app/glr_camera.h"
#include "app/glr_camera_export.h"
#include "app/glr_debug.h"
#include "keys.h"
#include "support/memprof.h"
#include "support/cpuprof.h"
#include <string.h>
#include "repl/example_loader.h"
#include "repl/host_effects.h"
#include "repl/scenes.h"
#include "source_document.h"
#include "repl/examples.h"          /* REPL_EXAMPLE_TAG_* */
#include "repl/command_descriptions.h"
#include "repl/eval.h"
#include "repl/parser.h"
#include "repl/executor.h"
#include "repl/export.h"
#include "repl/help_text.h"
#include "repl/pipeline.h"
#include "subsystems/replay/replay_annotations.h"
#include "repl/source_scope.h"
#include "repl/state_views.h"
#include "repl/tutorials.h"
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "ui/subsystems/replay_hud.h"
#include "ui/subsystems/tour_hud.h"
#include "ui/app/autocomplete_panel.h"
#include "ui/app/editor.h"
#include "ui/app/layout.h"
#include "ui/app/menu_bar.h"
#include "ui/core/metrics.h"
#include "ui/support/memprof.h"
#include "ui/app/numeric_swatch.h"
#include "ui/app/panels.h"
#include "ui/app/repl_code_panel.h"
#include "ui/support/cpuprof.h"
#include "ui/app/snapshot.h"
#include "ui/app/state.h"
#include "ui/app/state_types.h" /* UI-chrome typedefs (CodePanel/Camera/Help/etc.) */
#include "ui/core/tabbed_overlay.h"
#include "ui/subsystems/variable_panel.h"
#include "ui/app/variable_panel_view.h"
#include "app/glr_color_picker_bridge.h"
#include "app/glr_ctrl_internal.h"
#include "subsystems/variable_panel/variable_panel_drag.h"

/* GLR_WHEEL_ZOOM_STEP lives with the camera owner in glr_camera.h. */

/* ---- Keyboard router helpers ------------------------------------------ */

int glr_ctrl_router_handle_save_key(unsigned char key) {
    if (keymap_event_is(key, GLR_SAVE)) {
        /* Ctrl+S == File > Save Scene: active named scene ->
         * <workspace>/<slug>.c; example/transient -> ./output.c. */
        ReplExportLayout layout;
        glr_ctrl_fill_export_layout(&layout);
        repl_save_active_scene(&layout);
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_debug_dump_key(unsigned char key) {
    if (keymap_event_is(key, GLR_DEBUG_DUMP)) {
        glr_debug_dump_flat_commands_sync(stdout, source_document_view());
        glr_debug_dump_editor(stdout, source_document_view());
        repl_set_status("Dumped editor + flat commands to stdout");
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_time_reset_key(unsigned char key) {
    if (!keymap_event_is(key, GLR_TIME_RESET))
        return 0;
    glr_action_reset_time_to_zero();
    return 1;
}

/* Recovery safeguard: Ctrl+Q, File > Quit, and SIGINT (Ctrl+C), plus
 * Load Workspace (which replaces every in-memory slot), write a recovery
 * copy to a DISTINCT, findable file — never the active scene/workspace. The point
 * is to rescue an unintended exit / forgotten save / discarded scene
 * without silently clobbering the user's real scene; reload it with
 * `./gl-repl recovery.c`. (Not /tmp — the user would never find it; not
 * the scene file — that would defeat the safeguard.) The filename lives
 * in config.h as QUIT_RECOVERY_FILE. */

static volatile sig_atomic_t g_quit_requested = 0;

/* Async-signal-safe: only sets a sig_atomic_t flag. The actual save +
 * exit runs on the normal path in glr_ctrl_tick(). */
void glr_ctrl_request_quit(void) {
    g_quit_requested = 1;
}

/* Write the live scene to the recovery file. Shared by the quit
 * safeguard and Load Workspace. Returns 1 on success, 0 on failure. */
int glr_ctrl_save_recovery_file(void) {
    ReplExportLayout layout;
    glr_ctrl_fill_export_layout(&layout);
    return repl_export_save_output(QUIT_RECOVERY_FILE, source_document_view(),
                                   &layout) ? 1 : 0;
}

static void glr_ctrl_save_quit_recovery(void) {
    if (glr_ctrl_save_recovery_file()) {
        printf("Saved recovery copy to %s (reload: ./%s %s)\n",
               QUIT_RECOVERY_FILE, glr_ctrl_program_name(),
               QUIT_RECOVERY_FILE);
    }
}

int glr_ctrl_router_handle_quit_key(unsigned char key) {
    if (keymap_event_is(key, GLR_QUIT)) {
        glr_ctrl_save_quit_recovery();
        exit(0);
    }
    return 0;
}

/* Showcase-step ack: while a tutorial SET or NOTE step is waiting,
 * Enter / Tab / Space advances. Scoped strictly to those kinds inside
 * tutorial_handle_ack_key so REQUIRE / COMMAND steps never have their
 * keys swallowed here. Runs AFTER the existing controller routes (so
 * Ctrl-keys, replay, cfg shortcuts, save, quit keep priority) and
 * BEFORE editor_handle_key — see the chain in glr_ctrl_keyboard. */
int glr_ctrl_router_handle_tutorial_ack_key(unsigned char key) {
    return tutorial_handle_ack_key(key);
}

int glr_ctrl_router_handle_config_menu_key(unsigned char key) {
    if (!editor_state_search()->active && keymap_event_is(key, GLR_CONFIG_MENU)) {
        if (replay_active())
            replay_stop();
        glr_ctrl_restore_hidden_code_panel();
        ui_menu_bar_open_config(repl_state_variables().anim_time);
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_replay_key(unsigned char key) {
    return replay_handle_key(key);
}

int glr_ctrl_router_handle_cfg_shortcut_key(unsigned char key) {
    return glr_cfg_handle_ascii_shortcut(key);
}

int glr_ctrl_router_handle_audio_key(unsigned char key) {
    if (!keymap_event_is(key, GLR_AUDIO))
        return 0;
    glr_action_toggle_audio_play_pause();
    return 1;
}

/* Ctrl+= / Ctrl+- step the Config "Accum passes" cycle (1/2/4/8/12/16,
 * clamped, no wrap), so the keyboard and the menu are one coherent model.
 * Gated on use_accum (no accumulation buffer under --no-accum) and on the
 * effect being active — adjusting the sample count is meaningless while the
 * effect is Off. */
int glr_ctrl_router_handle_accum_samples_key(unsigned char key) {
    GlrRenderState *rs = glr_state_render_mut();
    int is_up = (key == '=' || key == '+');
    int is_down = (key == KEY_CTRL_DASH ||
                   (key == '-' &&
                    (editor_input_active_modifiers() & GLUT_ACTIVE_CTRL)));

    if (is_up && !(editor_input_active_modifiers() & GLUT_ACTIVE_CTRL))
        return 0;
    if (!is_up && !is_down)
        return 0;

    if (rs->use_accum && rs->accum_effect != RENDER3D_ACCUM_EFFECT_OFF) {
        int n   = glr_config_state_count(GLR_CONFIG_ACCUM_PASSES);
        int idx = glr_config_get(GLR_CONFIG_ACCUM_PASSES);
        int next = idx + (is_up ? 1 : -1);
        if (next < 0)     next = 0;
        if (next > n - 1) next = n - 1;
        if (next != idx)
            glr_config_set(GLR_CONFIG_ACCUM_PASSES, next);
        char msg[64];
        snprintf(msg, sizeof(msg), "Accum passes: %s",
                 glr_config_state_name(GLR_CONFIG_ACCUM_PASSES, next));
        repl_set_status(msg);
    }
    return 1;
}

/* Shared by the Ctrl+Shift+F shortcut and the status-bar keycap click.
 * Toggling collapses the chrome header rows from ~20 to 0 (and back);
 * the follow-scroll request keeps the active edit row on screen, and
 * the sync mirrors the new flag into the code-panel UI snapshot. */
void glr_ctrl_toggle_code_focus(void) {
    GlrPresentationState *p = glr_state_presentation_mut();
    p->code_focus = !p->code_focus;
    glr_ctrl_sync_ui_chrome();
    editor_scroll_follow_cursor_set(1);
    repl_set_status(p->code_focus ? "Code focus: ON" : "Code focus: OFF");
}

/* Ctrl+Shift+F: toggle the code-panel focus view. Ctrl+F (no Shift)
 * stays the search shortcut handled downstream in editor_handle_key; the Shift
 * modifier disambiguates, and this router runs ahead of the editor so search
 * never sees the shifted form. Hidden shortcut only — no Config row, no @cfg. */
int glr_ctrl_router_handle_code_focus_key(unsigned char key) {
    if (!keymap_event_is(key, GLR_CODE_FOCUS))
        return 0;
    glr_ctrl_toggle_code_focus();
    return 1;
}

/* The Ctrl+Shift camera shortcuts (Ctrl+Shift+C reset / +O focus-origin
 * / +V view mode) have no dedicated router: they are ordinary Config
 * rows carrying a GLUT_ACTIVE_SHIFT modifier, exactly dispatched by
 * glr_cfg_handle_ascii_shortcut (see glr_actions.c). */

/* ---- Special-key router helpers --------------------------------------- */

int glr_ctrl_router_handle_replay_special(int key) {
    return replay_handle_special(key);
}

int glr_ctrl_router_handle_cfg_special_shortcut(int key) {
    return glr_cfg_handle_special_shortcut(key);
}

int glr_ctrl_router_handle_horizontal_audio_special(int key) {
    if (keymap_event_is(key, GLR_AUDIO_PREV))
        glr_audio_prev_track();
    else if (keymap_event_is(key, GLR_AUDIO_NEXT))
        glr_audio_next_track();
    else
        return 0;
    return 1;
}

/* Snapshot of the live help overlay for pure geometry queries
 * (scroll clamp, click hit-test). Mirrors the per-frame state the
 * renderer is handed in glr_ctrl_display_frame. */
static UiOverlayState glr_ctrl_help_overlay_state(void) {
    UiViewportState vp = ui_state_viewport();
    EditorHelpSession s = editor_help_session_view();
    UiOverlayState st = {
        .visible    = ui_state_help().visible,
        .tab_idx    = s.tab_idx,
        .scroll     = s.scroll,
        .viewport_w = vp.window_w,
        .viewport_h = vp.window_h,
        .content    = glr_ctrl_help_overlay_content(),
    };
    return st;
}

/* Scroll the help session, then clamp to the active tab's real
 * bounds so the offset can't run past the end (which would otherwise
 * have to be "unwound" before an upward scroll did anything). */
void glr_ctrl_help_scroll_by(int delta) {
    editor_help_session_scroll_by(delta);
    UiOverlayState st = glr_ctrl_help_overlay_state();
    int max_scroll = ui_tabbed_overlay_max_scroll(&st);
    int scroll = editor_help_session_scroll();
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;
    editor_help_session_set_scroll(scroll);
}

/* Left-click routing while the modal help overlay is up: tab bar
 * selects a tab, anywhere outside the panel dismisses, body clicks
 * are swallowed (the overlay is modal). */
int glr_ctrl_router_handle_help_click(int button, int state, int x, int y) {
    if (button != GLUT_LEFT_BUTTON || state != GLUT_DOWN)
        return 0;
    if (!ui_state_help().visible)
        return 0;
    UiOverlayState st = glr_ctrl_help_overlay_state();
    UiOverlayHit hit = ui_tabbed_overlay_hit_test(&st, x, y);
    if (hit.kind == UI_OVERLAY_HIT_OUTSIDE) {
        glr_ctrl_toggle_help();          /* click-away dismiss */
        editor_request_redraw();
        return 1;
    }
    if (hit.kind == UI_OVERLAY_HIT_TAB) {
        editor_help_session_set_tab(hit.tab);
        editor_help_session_set_scroll(0);
        editor_request_redraw();
        return 1;
    }
    return 1;                            /* modal: swallow body clicks */
}

/* Normalized prefix match for pointer-script target labels, same rule as
 * menu_bar's: case-insensitive, '_' in the needle stands in for ' ' (script
 * targets are single tokens). */
static int glr_ctrl_target_label_matches(const char *label,
                                         const char *needle) {
    if (!label || !needle || !*needle)
        return 0;
    while (*needle) {
        char n = (char)tolower((unsigned char)*needle);
        char l = (char)tolower((unsigned char)*label);
        if (n == '_') n = ' ';
        if (n != l)
            return 0;
        needle++;
        label++;
    }
    return 1;
}

int glr_ctrl_help_tab_point(const char *label, int *mx, int *my) {
    UiOverlayState st = glr_ctrl_help_overlay_state();
    if (!st.visible || !st.content)
        return 0;
    for (int i = 0; i < st.content->tab_count; i++) {
        if (!glr_ctrl_target_label_matches(st.content->tabs[i].label, label))
            continue;
        return ui_tabbed_overlay_tab_point(&st, i, mx, my);
    }
    return 0;
}

int glr_ctrl_code_line_point(const char *spec, int *mx, int *my) {
    UiRenderSnapshot snap;
    int count = repl_state_document_count();

    if (!spec || !*spec || count <= 0)
        return 0;
    glr_ctrl_build_ui_snapshot(&snap);

    if (spec[0] >= '0' && spec[0] <= '9') {
        int line = atoi(spec);
        if (line >= count)
            return 0;
        return ui_repl_code_panel_source_line_point(&snap, line, mx, my);
    }

    /* Text targets take the first *visible* match: a long document repeats a
     * command many times, and only an on-screen row can be clicked. */
    for (int i = 0; i < count; i++) {
        const char *text = editor_buffer_line(i);
        while (text && (*text == ' ' || *text == '\t'))
            text++;
        if (!glr_ctrl_target_label_matches(text, spec))
            continue;
        if (ui_repl_code_panel_source_line_point(&snap, i, mx, my))
            return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_help_tab_special(int key) {
    if (!ui_state_help().visible)
        return 0;
    if (key == GLUT_KEY_LEFT) {
        glr_action_help_tab_prev();
        return 1;
    }
    if (key == GLUT_KEY_RIGHT) {
        glr_action_help_tab_next();
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_help_scroll_special(int key) {
    if (!ui_state_help().visible)
        return 0;
    switch (key) {
    case GLUT_KEY_UP:        glr_ctrl_help_scroll_by(-1); return 1;
    case GLUT_KEY_DOWN:      glr_ctrl_help_scroll_by(1);  return 1;
    case GLUT_KEY_PAGE_UP:   glr_ctrl_help_scroll_by(-5); return 1;
    case GLUT_KEY_PAGE_DOWN: glr_ctrl_help_scroll_by(5);  return 1;
    default: return 0;
    }
}

/* Shared by the F1 key and the status-bar "F1 help" keycap click. */
void glr_ctrl_toggle_help(void) {
    UiHelpState *help = ui_state_help_mut();
    help->visible = !help->visible;
    editor_help_session_set_tab(0);
    editor_help_session_set_scroll(0);
}

void glr_ctrl_close_help(void) {
    UiHelpState *help = ui_state_help_mut();
    help->visible = 0;
    editor_help_session_set_tab(0);
    editor_help_session_set_scroll(0);
}

int glr_ctrl_router_handle_escape_key(unsigned char key) {
    if (!keymap_event_is(key, GLR_ESCAPE))
        return 0;
    if (color_picker_active_line() >= 0) {
        color_picker_stop();
        editor_request_redraw();
        return 1;
    }
    if (ui_state_help().visible) {
        glr_ctrl_close_help();
        editor_request_redraw();
        return 1;
    }
    return 0;
}

int glr_ctrl_router_handle_help_toggle_special(int key) {
    if (keymap_event_is(key, GLR_HELP)) {
        glr_ctrl_toggle_help();
        return 1;
    }
    return 0;
}

static void cycle_example_or_user_scene_dir(int direction) {
    glr_ctrl_reset_transients();
    editor_undo_note_wholesale_replacement();
    int count = repl_example_count();
    int active_scene = repl_active_user_scene();

    if (active_scene >= 0) {
        int start = active_scene + direction;
        int end = (direction > 0) ? MAX_USER_SCENES : -1;
        for (int scene_idx = start; scene_idx != end; scene_idx += direction) {
            if (repl_user_scene_slot_used(scene_idx)) {
                if (repl_load_user_scene_idx(scene_idx))
                    editor_load_line_to_input(editor_state_edit_line());
                return;
            }
        }
        if (count > 0)
            editor_state_edit_line_set(repl_load_example((direction > 0) ? 0 : count - 1));
        return;
    }

    if (count > 0) {
        int next = repl_state_scenes().active_example_idx + direction;
        if (next >= 0 && next < count) {
            editor_state_edit_line_set(repl_load_example(next));
            return;
        }
    }

    int start = (direction > 0) ? 0 : MAX_USER_SCENES - 1;
    int end = (direction > 0) ? MAX_USER_SCENES : -1;
    for (int scene_idx = start; scene_idx != end; scene_idx += direction) {
        if (repl_user_scene_slot_used(scene_idx)) {
            if (repl_load_user_scene_idx(scene_idx))
                editor_load_line_to_input(editor_state_edit_line());
            return;
        }
    }
    if (count > 0)
        editor_state_edit_line_set(repl_load_example((direction > 0) ? 0 : count - 1));
}

/* Public scene-cycle entry points: the F12 / Shift+F12 key path and the
 * Scene-menu "Next" / "Previous" rows both funnel through these. */
void glr_ctrl_scene_cycle_next(void) {
    cycle_example_or_user_scene_dir(1);
}

void glr_ctrl_scene_cycle_prev(void) {
    cycle_example_or_user_scene_dir(-1);
}

int glr_ctrl_router_handle_scene_cycle_special(int key) {
    if (keymap_event_is(key, GLR_NEXT_EXAMPLE)) {
        glr_ctrl_scene_cycle_next();
        return 1;
    }
    if (keymap_event_is(key, GLR_PREV_EXAMPLE)) {
        glr_ctrl_scene_cycle_prev();
        return 1;
    }
    return 0;
}

static void cycle_tutorial_dir(int direction) {
    int count = repl_tutorial_count();
    if (count <= 0) return;

    glr_ctrl_reset_transients();
    editor_undo_note_wholesale_replacement();

    /* A completed tutorial is inactive but retains its index so F11 can
     * continue to the following lesson. Explicit exits and fresh state use
     * the default -1 index and retain the first/last tutorial behavior. */
    int active_idx = tutorial_state_view().tutorial_idx;
    int next_idx = 0;
    if (active_idx >= 0) {
        next_idx = (active_idx + direction + count) % count;
    } else {
        next_idx = (direction > 0) ? 0 : count - 1;
    }

    tutorial_start(next_idx);
}

void glr_ctrl_tutorial_cycle_next(void) {
    cycle_tutorial_dir(1);
}

void glr_ctrl_tutorial_cycle_prev(void) {
    cycle_tutorial_dir(-1);
}

int glr_ctrl_router_handle_tutorial_cycle_special(int key) {
    if (keymap_event_is(key, GLR_NEXT_TUTORIAL)) {
        glr_ctrl_tutorial_cycle_next();
        return 1;
    }
    if (keymap_event_is(key, GLR_PREV_TUTORIAL)) {
        glr_ctrl_tutorial_cycle_prev();
        return 1;
    }
    return 0;
}

/* ---- Mouse / motion router helpers ------------------------------------ */

static int glr_ctrl_shift_fine_modifier_active(void) {
    return (editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT) != 0;
}

static float glr_ctrl_adjustment_modifier_scale(int coarse, int fine) {
    float scale = 1.0f;
    if (coarse)
        scale *= GLR_ADJUST_COARSE_SCALE;
    if (fine)
        scale *= GLR_ADJUST_FINE_SCALE;
    return scale;
}

int glr_ctrl_router_handle_variable_panel_drag_begin(int button, int state, int x, int y) {
    if (state != GLUT_DOWN) return 0;
    if (!variable_panel_visible()) return 0;
    if (button != GLUT_LEFT_BUTTON && button != GLUT_RIGHT_BUTTON)
        return 0;
    int row_idx;
    UiVariablePanelView var_view =
        ui_app_variable_panel_view_live(repl_eval_predef_view().count);
    if (!ui_variable_panel_hit_row(&var_view, x, y, &row_idx))
        return 0;
    if (replay_active())
        replay_stop();
    int coarse = (button == GLUT_RIGHT_BUTTON) ? 1 : 0;
    variable_panel_handle_drag_begin(row_idx, coarse, x);
    editor_request_redraw();
    return 1;
}

/* Mouse-up: write the value the drag settled on back into the variable's
 * declaration, once. Motion applied the live value only (see
 * glr_ctrl_apply_variable_panel_value_change), so this is the drag's single
 * source mutation — one dirty mark, one re-flatten from cold state, and the
 * saved source matches what the scene renders.
 *
 * No undo snapshot: the first motion already captured one at the drag-start
 * value. No tutorial notify: the change carries no predef op, and the live
 * value it persists was notified when it was applied. A drag with no motion,
 * or a variable with no declaration row, does no work at all. */
static void glr_ctrl_persist_variable_panel_drag_value(
        const VariablePanelDragState *drag) {
    ReplCompiledChange compiled;
    ReplCompileContext ctx;
    char err[REPL_STATUS_TEXT_MAX] = "";

    if (!drag->value_changed || !drag->name[0])
        return;

    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ctx.insert_mode = editor_insert_mode();
    if (repl_compile_persist_predef_value(drag->name, drag->final_value,
                                          &ctx, &compiled,
                                          err, sizeof(err)) != REPL_COMPILE_OK) {
        repl_set_status_error(err[0] ? err : "Variable update failed");
        return;
    }
    if (compiled.kind == REPL_COMPILED_NO_CHANGE)
        return;   /* no declaration row to rewrite */

    /* On failure the already-applied live value stands; Undo still restores
     * the drag-start snapshot. */
    if (!editor_commit_apply_external_change(&compiled, /*capture_undo=*/0,
                                             /*publish_status=*/0))
        repl_set_status_error("Command buffer full!");
}

int glr_ctrl_router_handle_variable_panel_drag_release(int state) {
    VariablePanelDragState drag;

    if (state != GLUT_UP) return 0;
    if (!variable_panel_drag_active()) return 0;
    drag = variable_panel_drag();
    glr_ctrl_persist_variable_panel_drag_value(&drag);
    variable_panel_handle_drag_reset();
    editor_request_redraw();
    return 1;
}

int glr_ctrl_router_handle_scene_press(int button, int state, int x, int y) {
    if (state != GLUT_DOWN || button != GLUT_LEFT_BUTTON)
        return 0;
    /* The scene region is owned by the camera, but the floating color
     * picker can overlap it. Give the picker first crack on press; if
     * the click lands outside its rects the peer dismisses itself
     * (closed=1) and we redraw but fall through so the camera sees
     * the event. */
    ColorPickerInputResult r = color_picker_handle_press(x, y);
    if (r.closed || r.changed)
        editor_request_redraw();
    if (r.consumed)
        return 1;
    return 0;
}

int glr_ctrl_router_handle_camera_mouse(int button, int state, int x, int y) {
    glr_ctrl_sync_camera_control_mode();
    if (state == GLUT_DOWN)
        ui_state_pointer_set(x, y, button);
    else if (state == GLUT_UP)
        ui_state_pointer_set(x, y, -1);
    else
        ui_state_pointer_set_pos(x, y);
    glr_camera_mouse_event(button, state, x, y, editor_input_active_modifiers());
    return 1;
}

static void glr_ctrl_apply_variable_panel_value_change(
        const VariablePanelValueChange *value_change) {
    ReplCompiledChange compiled;
    ReplCompileContext ctx;
    VariablePanelDragState drag;
    char err[REPL_STATUS_TEXT_MAX] = "";
    int var_idx;
    int capture_undo;

    if (!value_change || !value_change->name[0])
        return;

    drag = variable_panel_drag();
    var_idx = drag.var_idx;
    ReplPredefView predef = repl_eval_predef_view();
    if (var_idx < 0 || var_idx >= predef.count)
        return;
    if (strcmp(predef.vars[var_idx].name, value_change->name) != 0) {
        var_idx = repl_eval_find_predef_var_idx(value_change->name);
        if (var_idx < 0)
            return;
    }
    if (predef.vars[var_idx].value == value_change->value)
        return;

    ctx = repl_compile_context_from_live(editor_state_edit_line());
    /* repl_compile_context_from_live(editor_state_edit_line()) defaults insert_mode to 0
     * (non-editor convention); the editor-side caller knows the live
     * value and overrides. */
    ctx.insert_mode = editor_insert_mode();
    /* Live-only: motion updates the variable's value, never its declaration
     * text. Rewriting the source on every pointer event would mark the source
     * dirty hundreds of times per drag; the release handler persists the
     * settled value once instead. */
    if (repl_compile_set_predef_value_live(value_change->name,
                                           value_change->value,
                                           &ctx, &compiled,
                                           err, sizeof(err)) != REPL_COMPILE_OK) {
        repl_set_status_error(err[0] ? err : "Variable update failed");
        return;
    }

    capture_undo = !variable_panel_drag_undo_snapshot_pushed();
    if (!editor_commit_apply_external_change(&compiled, capture_undo, 0)) {
        repl_set_status_error("Command buffer full!");
        return;
    }
    if (capture_undo)
        variable_panel_drag_mark_undo_snapshot_pushed();
    variable_panel_drag_note_applied_value(value_change->value);
}

int glr_ctrl_router_handle_variable_panel_motion(int x, int y) {
    VariablePanelValueChange value_change;

    (void)y;
    if (!variable_panel_drag_active())
        return 0;
    if (variable_panel_handle_drag_motion(x, &value_change)) {
        VariablePanelDragState drag = variable_panel_drag();
        /* Shift fine-scaling only applies to the normal left-click scrub; the
         * coarse (right-click) scrub is a flat 10x. */
        if (!drag.coarse) {
            float scale = glr_ctrl_adjustment_modifier_scale(
                0, glr_ctrl_shift_fine_modifier_active());
            value_change.value = drag.start_value +
                (value_change.value - drag.start_value) * scale;
        }
        glr_ctrl_apply_variable_panel_value_change(&value_change);
    }
    editor_request_redraw();
    return 1;
}

int glr_ctrl_router_handle_camera_motion(int x, int y) {
    glr_ctrl_sync_camera_control_mode();
    glr_camera_drag_motion(x, y);
    return 1;
}

int glr_ctrl_router_handle_camera_pointer_set(int x, int y) {
    glr_camera_pointer_set(x, y);
    ui_state_pointer_set_pos(x, y);
    return 1;
}

/* ---- Code-panel UiHit dispatch --------------------------------------- */

/* Code-panel selection drag tracking. Press handlers set the anchor
 * to the clicked source-cmd row; motion re-runs ui_panels_hit_test
 * to derive the drag target and extends the editor selection.
 * Release on UP clears the active flag. The state lives here (not in
 * ui_panels.c) because UI input files report hit-test data only. */
static int g_code_panel_drag_active = 0;
static int g_code_panel_drag_anchor = -1;
static int g_code_panel_drag_moved  = 0;
/* Press char column inside the active-edit-row drag. -1 when the
 * press wasn't on a code-text row (gutter / insert-line / non-text
 * hits don't arm an input-row drag). The drag motion handler reads
 * this to decide between per-char input-buffer selection (drag stays
 * on the active edit row) and the existing line-range path. */
static int g_code_panel_drag_char_anchor = -1;

/* Double-click detection: previous UI_HIT_CODE_TEXT press timestamp
 * and target. A second press at the same (line, char) within
 * DOUBLE_CLICK_MS reads as a double-click and selects the word under
 * the cursor. Using line/char instead of pixel coords means small
 * mouse jitter between presses doesn't break the gesture. */
#define DOUBLE_CLICK_MS 400u
static unsigned int g_last_text_press_ms     = 0;
static int          g_last_text_press_line   = -1;
static int          g_last_text_press_char   = -1;
/* Scene-tab double-click: a 2nd press on the same tab display index
 * within DOUBLE_CLICK_MS triggers inline rename (user tabs only). */
static unsigned int g_last_tab_press_ms      = 0;
static int          g_last_tab_press_idx     = -1;
/* Test clock seam mirrors editor_input_set_modifier_provider_for_test:
 * tests that drive a double-click without a live GLUT context replace
 * the clock with a deterministic source. Production code falls back to
 * glutGet(GLUT_ELAPSED_TIME). */
static unsigned int (*g_double_click_clock_ms_for_test)(void) = NULL;

void glr_ctrl_router_set_double_click_clock_for_test(
    unsigned int (*clock_ms)(void)) {
    g_double_click_clock_ms_for_test = clock_ms;
    /* Tests typically arrange a fresh clock per case — wipe the
     * stored press so prior real-time history can't leak across. */
    g_last_text_press_ms   = 0;
    g_last_text_press_line = -1;
    g_last_text_press_char = -1;
    g_last_tab_press_ms    = 0;
    g_last_tab_press_idx   = -1;
}

static unsigned int current_double_click_ms(void) {
    if (g_double_click_clock_ms_for_test)
        return g_double_click_clock_ms_for_test();
    return (unsigned int)glutGet(GLUT_ELAPSED_TIME);
}

void glr_ctrl_router_reset_code_panel_drag(void) {
    g_code_panel_drag_active = 0;
    g_code_panel_drag_anchor = -1;
    g_code_panel_drag_moved  = 0;
    g_code_panel_drag_char_anchor = -1;
}

/* Apply an input-row drag motion: if the press char-anchor is armed
 * and the drag still hits the active edit row, grow the input-buffer
 * selection toward target_char. Returns 1 if handled (caller should
 * stop processing), 0 otherwise (caller falls through to line-range).
 * Exposed in glr_ctrl.h so tests can drive the per-char logic without
 * computing pixel coordinates that match the live panel layout. */
int glr_ctrl_router_apply_input_row_drag(int target_line, int target_char) {
    if (!g_code_panel_drag_active || g_code_panel_drag_anchor < 0)
        return 0;
    if (g_code_panel_drag_char_anchor < 0)
        return 0;
    if (target_line != g_code_panel_drag_anchor)
        return 0;
    if (target_line != editor_state_edit_line())
        return 0;
    if (target_char < 0)
        return 0;
    editor_cursor_pos_extend_selection(target_char);
    g_code_panel_drag_moved = 1;
    glr_action_cursor_blink_reset();
    editor_request_redraw();
    return 1;
}

/* Extend the in-progress line-range selection from the press anchor to
 * target. Shared by the plain line-range drag path and the char-drag
 * promotion below. */
static void extend_line_range_to(int target) {
    g_code_panel_drag_moved = 1;
    editor_selection_start(g_code_panel_drag_anchor);
    editor_selection_set_end(target);
    editor_navigate_to_line(target);
    glr_action_cursor_blink_reset();
    editor_request_redraw();
}

int glr_ctrl_router_promote_char_drag_to_line_range(int target_line) {
    if (!g_code_panel_drag_active || g_code_panel_drag_anchor < 0)
        return 0;
    if (g_code_panel_drag_char_anchor < 0)
        return 0;
    if (target_line < 0 || target_line == g_code_panel_drag_anchor)
        return 0;
    /* Drop the press row's character span: the two selection models are
     * mutually exclusive, and from here the drag is line-range. */
    editor_input_anchor_clear();
    g_code_panel_drag_char_anchor = -1;
    extend_line_range_to(target_line);
    return 1;
}

/* Common epilog for clicks that move the editor cursor: blink reset,
 * autocomplete clear, selection clear, redraw. Mirrors the legacy
 * ui_panels_handle_code_panel_click tail. */
static void route_code_click_epilog(void) {
    if (editor_state_search()->active)
        editor_search_clear_all();
    glr_action_cursor_blink_reset();
    /* On a click that landed the cursor on the tutorial's expected
     * commit line, refresh autocomplete so the shadow ghost
     * reappears; anywhere else, clear to drop stale completions. */
    if (tutorial_active() &&
        editor_state_edit_line() == tutorial_expected_commit_line())
        editor_completion_update();
    else
        editor_completion_clear();
    editor_selection_clear_line_range();
    editor_request_redraw();
}

/* Select the word containing (line_idx, char_idx) as an input-buffer
 * selection. Navigates to the line first (reloads input) so the word
 * walk runs against the row's actual canonical text. char_idx outside
 * a word leaves the buffer unselected. Public so tests can drive
 * double-click selection without faking GLUT_ELAPSED_TIME. */
void glr_ctrl_router_select_word_at(int line_idx, int char_idx) {
    if (line_idx < 0)
        return;
    editor_navigate_to_line(line_idx);

    const char *text = editor_input_text();
    int len = editor_input_len();
    int word_start = char_idx;
    int word_end   = char_idx;
    editor_input_word_bounds_at(text, len, char_idx, &word_start, &word_end);
    if (word_end <= word_start) {
        /* Clicked between words / on whitespace: leave cursor placed
         * but no selection. */
        editor_cursor_pos_set(char_idx);
        return;
    }
    /* Place cursor at word_start, then atomically pin and extend to
     * word_end. Doing it as two steps with editor_input_anchor_set
     * would collapse immediately (anchor == cursor) — that's the
     * footgun the extend helper was added to avoid. */
    editor_cursor_pos_set(word_start);
    editor_cursor_pos_extend_selection(word_end);
}

/* UI_HIT_CODE_TEXT: navigate to clicked line, set cursor column, arm
 * the selection drag anchor. A press that hits the same (line, char)
 * as the previous press within DOUBLE_CLICK_MS reads as a double-click
 * and selects the word at the cursor instead of placing a bare
 * cursor. */
static int route_code_text_hit(const UiHit *hit) {
    /* A non-swatch click on the code panel closes any open color picker
     * (matches legacy ui_panels_handle_code_panel_press behaviour). */
    color_picker_stop();

    /* Shift+click extends a selection from the current cursor to the
     * clicked location, mirroring shift+arrow keys:
     *   - same row as the cursor → per-character input-buffer selection
     *     (the substring between the two columns), via the same anchor
     *     machinery as shift+Left/Right and click-drag;
     *   - a different row → whole-line range selection from the cursor's
     *     line to the clicked line (the shift+Up/Down model). Partial
     *     first/last lines across a multi-line span are not represented
     *     by either selection model, so the cross-line case selects full
     *     lines. */
    if ((editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT) &&
        hit->line_idx >= 0 && hit->char_idx >= 0) {
        if (hit->line_idx == editor_state_edit_line() &&
            editor_state_edit_line() < repl_state_document_count()) {
            editor_selection_clear_line_range();
            editor_cursor_pos_extend_selection(hit->char_idx);
            /* Arm the drag so a shift-drag keeps growing the same
             * per-character selection on this row. */
            g_code_panel_drag_active = 1;
            g_code_panel_drag_anchor = hit->line_idx;
            g_code_panel_drag_moved  = 0;
            g_code_panel_drag_char_anchor = hit->char_idx;
        } else {
            if (!editor_clipboard_sel_active())
                editor_selection_start(editor_state_edit_line());
            int anchor_line = editor_state_selection_anchor();
            editor_selection_set_end(hit->line_idx);
            editor_navigate_to_line(hit->line_idx);
            /* Arm a line-range drag anchored at the selection's fixed
             * start so a follow-up shift-drag extends the whole-line
             * range about the same pivot (drag handler re-runs
             * editor_selection_start(g_code_panel_drag_anchor)). */
            g_code_panel_drag_active = 1;
            g_code_panel_drag_anchor = anchor_line;
            g_code_panel_drag_moved  = 1;
            g_code_panel_drag_char_anchor = -1;
        }
        if (editor_state_search()->active)
            editor_search_clear_all();
        glr_action_cursor_blink_reset();
        editor_request_redraw();
        return 1;
    }

    unsigned int now_ms = current_double_click_ms();
    int is_double_click = (g_last_text_press_line == hit->line_idx &&
                           g_last_text_press_char == hit->char_idx &&
                           hit->line_idx >= 0 &&
                           hit->char_idx >= 0 &&
                           now_ms - g_last_text_press_ms < DOUBLE_CLICK_MS);
    g_last_text_press_ms   = now_ms;
    g_last_text_press_line = hit->line_idx;
    g_last_text_press_char = hit->char_idx;

    if (is_double_click) {
        glr_ctrl_router_select_word_at(hit->line_idx, hit->char_idx);
        route_code_click_epilog();
        /* Double-click also arms the drag in case the user starts to
         * drag-extend the word selection. */
        if (hit->line_idx >= 0 && hit->line_idx < repl_state_document_count()) {
            g_code_panel_drag_active = 1;
            g_code_panel_drag_anchor = hit->line_idx;
            g_code_panel_drag_moved  = 0;
            g_code_panel_drag_char_anchor = hit->char_idx;
        }
        return 1;
    }

    if (hit->line_idx >= 0)
        editor_navigate_to_line(hit->line_idx);
    if (hit->char_idx >= 0)
        editor_cursor_pos_set(hit->char_idx);
    route_code_click_epilog();

    /* Arm drag anchor for committed lines only. */
    if (hit->line_idx >= 0 && hit->line_idx < repl_state_document_count()) {
        g_code_panel_drag_active = 1;
        g_code_panel_drag_anchor = hit->line_idx;
        g_code_panel_drag_moved  = 0;
        /* Record the press column so a drag that stays on the active
         * edit row can build a per-character input-buffer selection.
         * The press itself moved the cursor to hit->char_idx (and
         * cleared any anchor as the Phase B default of the
         * editor-input-selection plan), so on the first drag-motion
         * the extend helper pins this column. */
        g_code_panel_drag_char_anchor = hit->char_idx;
    } else {
        glr_ctrl_router_reset_code_panel_drag();
    }
    return 1;
}

/* UI_HIT_CODE_INSERT_LINE: insertion virtual row in insert mode. Set
 * cursor column but do not navigate (matches legacy on_insert_line=1
 * behaviour). No drag anchor — the insert row is virtual. */
static int route_code_insert_line_hit(const UiHit *hit) {
    color_picker_stop();
    if (hit->char_idx >= 0)
        editor_cursor_pos_set(hit->char_idx);
    route_code_click_epilog();
    glr_ctrl_router_reset_code_panel_drag();
    return 1;
}

/* UI_HIT_CODE_GUTTER: clicking the line-number column selects the
 * row. Same dispatch as CODE_TEXT minus the cursor-column move. */
static int route_code_gutter_hit(const UiHit *hit) {
    color_picker_stop();
    if (hit->line_idx >= 0)
        editor_navigate_to_line(hit->line_idx);
    route_code_click_epilog();
    if (hit->line_idx >= 0 && hit->line_idx < repl_state_document_count()) {
        g_code_panel_drag_active = 1;
        g_code_panel_drag_anchor = hit->line_idx;
        g_code_panel_drag_moved  = 0;
    } else {
        glr_ctrl_router_reset_code_panel_drag();
    }
    return 1;
}

/* UI_HIT_CODE_PANEL_CHROME: inert code-panel chrome (e.g. statusbar).
 * Consume the press so scene/camera handlers do not see it, but do not
 * move the cursor or selection. */
static int route_code_panel_chrome_hit(void) {
    color_picker_stop();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_CODE_FOCUS_TOGGLE: the statusbar "focus" keycap. Same action
 * as the Ctrl+Shift+F shortcut — go through the shared toggle so both
 * paths sync chrome, request follow-scroll, and post the status line. */
static int route_code_focus_toggle_hit(void) {
    glr_ctrl_toggle_code_focus();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_CODE_CLEAR_ALL: the statusbar trash chip. Same action as
 * Ctrl+L, including tutorial guard, undo capture, and status text. */
static int route_code_clear_all_hit(void) {
    editor_clear_all_cmds();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

/* Statusbar edit-action chips. These call the same editor primitives as
 * the keyboard routes, so line selections, input selections, guards, undo
 * capture, and status text stay centralized in editor/. */
static int route_code_copy_hit(void) {
    editor_clipboard_copy_current();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

static int route_code_cut_hit(void) {
    editor_clipboard_cut_current();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

static int route_code_paste_hit(void) {
    editor_clipboard_paste_current();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

static int route_code_undo_hit(void) {
    editor_undo_pop_snapshot();
    editor_scroll_follow_cursor_set(0);
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

static int route_code_redo_hit(void) {
    editor_undo_do_redo();
    editor_scroll_follow_cursor_set(0);
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_HELP_TOGGLE: the statusbar "F1 help" keycap. Same action as
 * the F1 key — go through the shared toggle so the overlay tab/scroll
 * reset identically. */
static int route_help_toggle_hit(void) {
    glr_ctrl_toggle_help();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_INLINE_COLOR_SWATCH: toggle / open the floating color picker
 * for the swatch's source line. Undo capture is owned by the picker's
 * writeback path (color_picker_write_cmd → editor_commit_apply_external_change
 * with capture_undo on the first slider edit per session), so
 * a session that opens and closes without editing leaves the undo ring
 * untouched. */
static int route_inline_color_swatch_hit(const UiHit *hit, int my) {
    if (hit->line_idx < 0)
        return 0;
    if (color_picker_active_line() == hit->line_idx) {
        color_picker_stop();
    } else {
        color_picker_start(hit->line_idx, my);
    }
    editor_request_redraw();
    return 1;
}

/* Adjustment magnitude for an inline-swatch press. Mirrors the variable
 * slider's coarse/fine modifiers: a right-click steps x10 (coarse), and
 * holding Shift steps x1/5 (fine). They combine (right+Shift = x2). */
static float numeric_swatch_scale(int is_right_button) {
    return glr_ctrl_adjustment_modifier_scale(
        is_right_button, glr_ctrl_shift_fine_modifier_active());
}

static int route_numeric_swatch_hit(const UiHit *hit, float scale) {
    int edit_line = editor_state_edit_line();

    if (editor_insert_mode() ||
        edit_line < 0 || edit_line >= repl_state_document_count())
        return 1;

    if (tutorial_active() &&
        !tutorial_guard_source_change(edit_line, 1, 1))
        return 1;

    editor_commit_apply_swatch_change(edit_line, hit->item_idx > 0 ? 1 : -1,
                                      scale);
    return 1;
}

static int glr_ctrl_router_point_in_gl_state_popup(int x, int y) {
    UiGlStatePanelView view;
    if (!ui_state_gl_state_inspector().visible)
        return 0;
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    return view.visible && ui_gl_state_panel_hit_test(&view, x, y);
}

/* The inspector describes state at a fixed source boundary. Once an event is
 * actually handed to the editor that boundary is no longer the user's active
 * context, so remove the overlay before the editor processes the event. Keep
 * this at the controller/editor routing seam: editor itself does not own UI
 * popup state, and controller-owned shortcuts should not dismiss it. */
static void glr_ctrl_router_dismiss_gl_state_for_editor_input(void) {
    if (!ui_state_gl_state_inspector().visible)
        return;
    ui_state_gl_state_inspector_close();
    editor_request_redraw();
}

/* Left press vs the floating OpenGL-state popup: a click inside the open
 * popup is consumed (the click must not fall through to the code-panel rows
 * behind it); on the header's "[+]"/"[-]" chip it also expands/collapses the
 * default/source detail columns. A click anywhere else dismisses the popup
 * and is NOT consumed, so it still routes normally afterwards (the color
 * picker's click-away convention). Skipped while a menu dropdown is open —
 * dropdowns render above the popup, so they win their clicks. */
static int glr_ctrl_router_handle_gl_state_popup_left_press(int x, int y) {
    UiGlStatePanelView view;
    if (!ui_state_gl_state_inspector().visible)
        return 0;
    if (ui_menu_bar_menu_dropdown_is_open())
        return 0;
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    if (!view.visible)
        return 0;
    if (ui_gl_state_panel_hit_test(&view, x, y)) {
        if (ui_gl_state_panel_hit_test_setup_toggle(&view, x, y))
            ui_state_gl_state_inspector_toggle_setup();
        else if (ui_gl_state_panel_hit_test_details_toggle(&view, x, y))
            ui_state_gl_state_inspector_toggle_details();
        editor_request_redraw();
        return 1;
    }
    ui_state_gl_state_inspector_close();
    editor_request_redraw();
    return 0;
}

/* Wheel over the floating OpenGL-state popup scrolls its report rows.
 * Consumed whenever the pointer is over the open popup — even when there
 * is nothing left to scroll — so the wheel never leaks to the code panel /
 * camera behind it (menu-flyout convention). delta > 0 scrolls toward
 * later rows. */
static int glr_ctrl_router_handle_gl_state_popup_wheel(int x, int y,
                                                       int delta) {
    UiGlStatePanelView view;
    UiGlStateInspectorState inspector = ui_state_gl_state_inspector();
    int max_scroll, scroll;

    if (!inspector.visible)
        return 0;
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    if (!view.visible || !ui_gl_state_panel_hit_test(&view, x, y))
        return 0;
    max_scroll = ui_gl_state_panel_max_scroll(&view);
    scroll = inspector.scroll_rows + delta;
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;
    ui_state_gl_state_inspector_set_scroll(scroll);
    editor_request_redraw();
    return 1;
}

/* A blank live input row is a state boundary even though it is not in the
 * source document yet. UI_HIT_CODE_INSERT_LINE identifies an insert/end row;
 * a non-insert active row still reports UI_HIT_CODE_TEXT, so it must prefer
 * the visible input over the committed command hidden underneath it. In
 * insert mode the committed row sharing the same source index remains a
 * normal command hit. */
static int glr_ctrl_router_hit_is_blank_gl_state_anchor(const UiHit *hit) {
    const GLCmd *cmd;

    if (!hit)
        return 0;
    if (hit->kind == UI_HIT_CODE_INSERT_LINE)
        return glr_ctrl_gl_state_live_input_is_blank_at_line(hit->line_idx);
    if (hit->kind != UI_HIT_CODE_TEXT)
        return 0;
    if (!editor_insert_mode() &&
        glr_ctrl_gl_state_live_input_is_blank_at_line(hit->line_idx))
        return 1;
    cmd = repl_state_document_cmd_at(hit->line_idx);
    return cmd && cmd->type == CMD_EMPTY;
}

/* Right-click over a committed GL-family command opens its authored help card;
 * glEnable/glDisable resolve that card by capability argument. A visually
 * empty committed or live input row toggles the OpenGL-state inspector. Every
 * other chrome right-click is consumed inert so it never forwards to the
 * scene camera, where right-drag starts pan. */
static void route_right_code_panel_hit(const UiHit *hit, int x, int y) {
    const GLCmd *cmd;
    ReplCommandDescription description;

    cmd = hit->kind == UI_HIT_CODE_TEXT
        ? repl_state_document_cmd_at(hit->line_idx) : NULL;
    if (glr_ctrl_router_hit_is_blank_gl_state_anchor(hit)) {
        UiGlStateInspectorState inspector = ui_state_gl_state_inspector();
        ui_state_command_description_close();
        if (inspector.visible &&
            inspector.source_line_idx == hit->line_idx)
            ui_state_gl_state_inspector_close();
        else
            ui_state_gl_state_inspector_open(hit->line_idx, x, y);
    } else if (cmd && repl_command_description_lookup(cmd, &description)) {
        ui_state_gl_state_inspector_close();
        ui_state_command_description_open(hit->line_idx, x, y);
    } else {
        ui_state_command_description_close();
    }

    color_picker_stop();
    glr_ctrl_router_reset_code_panel_drag();
    editor_request_redraw();
}

/* Right-press dispatch (GLUT_DOWN only — UP runs the shared release path).
 * Mirrors the left-button model: one canonical hit-test, then a switch on
 * the hit kind. Right-button owners, in the hit-test's z-order:
 *   - numeric swatch arrow → coarse (x10) step;
 *   - Config flyout row → backward-cycle, dropdown stays open;
 *   - variable slider row → log-mode drag begin;
 *   - scene / no hit → camera (right-drag pan);
 *   - code text → command help card / OpenGL-state inspector
 *     (route_right_code_panel_hit);
 *   - anything else (menu chrome, tabs, statusbar, pickers, modal help)
 *     → consumed inert so a right press on 2D chrome never pans the
 *     camera behind it. */
static void route_right_press(int x, int y) {
    UiRenderSnapshot ui_snap;
    UiHit front_hit;
    UiHit hit;

    glr_ctrl_build_ui_snapshot(&ui_snap);
    front_hit = ui_panels_hit_test_above_gl_state(
        &ui_snap, x, y, repl_eval_predef_view().count);
    if (front_hit.kind != UI_HIT_NONE) {
        hit = front_hit;
    } else {
        UiGlStateInspectorState inspector;
        hit = ui_panels_hit_test(&ui_snap, x, y,
                                 repl_eval_predef_view().count);
        /* The inspector itself is display-only chrome. With no later-rendered
         * panel claiming this pixel, consume its surface before classifying
         * the code panel / scene behind it. Preserve the anchor row's second
         * right-click toggle even when the solved panel border covers the
         * original anchor pixel. */
        if (glr_ctrl_router_point_in_gl_state_popup(x, y)) {
            inspector = ui_state_gl_state_inspector();
            if (!glr_ctrl_router_hit_is_blank_gl_state_anchor(&hit) ||
                hit.line_idx != inspector.source_line_idx) {
                editor_request_redraw();
                return;
            }
        }
    }

    switch (hit.kind) {
    case UI_HIT_NUMERIC_SWATCH:
        route_numeric_swatch_hit(&hit, numeric_swatch_scale(1));
        return;
    case UI_HIT_SUBMENU_ITEM:
        /* Backward-cycle the Config flyout item under the cursor;
         * item_idx carries the absolute g_cfg_items[] index. The
         * hit-test resolves actionable ITEM rows only — flyout chrome
         * ("### "/"---" in the "All" flyout) is skipped via
         * submenu_row_kind, and section/All parent rows own no submenu
         * row — so a right-press over the section list can never
         * mis-cycle a g_cfg_items[] index. */
        if (hit.cmd_idx == GLR_MENU_CONFIG && hit.item_idx >= 0) {
            glr_cfg_cycle_row(hit.item_idx, -1);
            editor_request_redraw();
            return;
        }
        break;                    /* non-Config flyout rows: inert */
    case UI_HIT_VARIABLE_SLIDER:
        if (glr_ctrl_router_handle_variable_panel_drag_begin(
                GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y))
            return;
        break;                    /* row rect disagreement: inert */
    case UI_HIT_SCENE:
    case UI_HIT_NONE:
        glr_ctrl_router_handle_camera_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN,
                                            x, y);
        return;
    default:
        break;
    }

    /* All hits from the front-layer pass own visible pixels. Actionable
     * right-button cases were handled above; the remaining surfaces consume
     * the press inert rather than falling through to code/camera routing. */
    if (front_hit.kind != UI_HIT_NONE)
        return;

    route_right_code_panel_hit(&hit, x, y);
}

/* UI_HIT_COLOR_SWATCH: floating picker slider control press. The
 * picker has its own internal hit-test for SV/hue/alpha regions and
 * starts a drag on press. */
static int route_color_picker_control_hit(int x, int y) {
    ColorPickerInputResult r = color_picker_handle_press(x, y);
    if (r.changed || r.closed)
        editor_request_redraw();
    return r.consumed;
}

/* UI_HIT_PIN_BUTTON: Search / Replay pinned right-side button. */
static int route_pin_button_hit(const UiHit *hit) {
    ui_menu_bar_close();
    switch (hit->item_idx) {
    case UI_MENU_BAR_PIN_REPLAY:
        replay_handle_pin_clicked();
        break;
    case UI_MENU_BAR_PIN_VIEW_MODE:
        glr_action_toggle_view_mode();
        break;
    case UI_MENU_BAR_PIN_SEARCH:
        editor_search_handle_key(KM_KEY(GLR_SEARCH));
        ui_menu_bar_note_search_opened(repl_state_variables().anim_time);
        break;
    default:
        break;
    }
    editor_request_redraw();
    return 1;
}

/* UI_HIT_SEARCH_NAV: find-bar match stepper. item_idx carries the
 * navigation direction (+1 next, -1 previous), already mapped from the
 * up/down arrow by the hit-test. */
static int route_search_nav_hit(const UiHit *hit) {
    editor_search_navigate(hit->item_idx);
    editor_request_redraw();
    return 1;
}

/* UI_HIT_SEARCH_FOCUS: click on a find-bar text field. item_idx is the
 * EditorSearchFocus the click targets. */
static int route_search_focus_hit(const UiHit *hit) {
    editor_search_set_focus((EditorSearchFocus)hit->item_idx);
    editor_request_redraw();
    return 1;
}

/* UI_HIT_SEARCH_WORD_TOGGLE: the whole-word chip. Clicking it both
 * focuses and flips it, the way a checkbox behaves; Tab + Space is the
 * keyboard twin. */
static int route_search_word_toggle_hit(void) {
    editor_search_set_focus(EDITOR_SEARCH_FOCUS_WORD);
    editor_search_toggle_whole_word();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_SEARCH_REPLACE: item_idx 1 = Replace All, 0 = the current match.
 * Both report their outcome through the status line. */
static int route_search_replace_hit(const UiHit *hit) {
    if (hit->item_idx == 1)
        (void)editor_replace_all();
    else
        (void)editor_replace_current();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_MENU_BUTTON: top-level menu-bar button. Click on the open
 * menu's button toggles it closed; click on a different button
 * switches the open dropdown. */
static int route_menu_button_hit(const UiHit *hit) {
    int menu_id = hit->cmd_idx;
    if (menu_id < 0) return 0;

    int open_menu = ui_menu_bar_open_menu_id();
    if (open_menu == menu_id)
        ui_menu_bar_close();
    else
        ui_menu_bar_set_open_menu(menu_id, repl_state_variables().anim_time);
    editor_request_redraw();
    return 1;
}

/* UI_HIT_MENU_ITEM: open dropdown row click. Activates the action
 * via glr_action_menu_item_activate using both menu_id (cmd_idx)
 * and item_idx from the hit payload. The action returns 1 if the
 * dropdown should close after activation (most action items) or 0 to
 * leave it open (cycle / toggle items). */
static int route_menu_item_hit(const UiHit *hit) {
    if (hit->cmd_idx < 0 || hit->item_idx < 0) return 0;
    int close = glr_action_menu_item_activate(hit->cmd_idx, hit->item_idx);
    if (close)
        ui_menu_bar_close();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_SUBMENU_ITEM: a flyout row. cmd_idx is the owning menu_id;
 * item_idx is the absolute target index the provider resolved.
 *  - MENU_SCENE: item_idx = global flat example index → load it and
 *    dismiss the menu (a scene pick is a one-shot action).
 *  - MENU_TUTORIALS: item_idx = global tutorial index → start it via
 *    tutorial_start (REPL-side; sidesteps glr_action_menu_item_activate,
 *    whose MENU_TUTORIALS branch only owns the top-level tag-row /
 *    Restart / Exit dispatch) and dismiss the menu.
 *  - MENU_CONFIG: item_idx = absolute g_cfg_items[] index → cycle the
 *    item forward via glr_cfg_cycle_row (NOT through
 *    glr_action_menu_item_activate, whose Config branch is the inert
 *    parent-row guard) and KEEP the dropdown + flyout open so repeated
 *    toggles work, matching the old flat Config dropdown. (Parent-row
 *    guard added in Step 5 of config-menu-submenu-sections.md.)
 *  - MENU_AUDIO: item_idx = playlist track index → request playback and
 *    KEEP the dropdown + flyout open (like Config) so the active-track
 *    highlight follows the pick and the user can switch again. */
static int route_submenu_item_hit(const UiHit *hit) {
    if (hit->item_idx < 0)
        return 0;
    if (hit->cmd_idx == GLR_MENU_CONFIG) {
        glr_cfg_cycle_row(hit->item_idx, 1);
        editor_request_redraw();
        return 1;
    }
    if (hit->cmd_idx == GLR_MENU_TUTORIALS) {
        tutorial_start(hit->item_idx);
        ui_menu_bar_close();
        editor_request_redraw();
        return 1;
    }
    if (hit->cmd_idx == GLR_MENU_AUDIO) {
        if (glr_audio_play_track_index(hit->item_idx) == 0) {
            /* Keep the dropdown open so the active-track highlight moves
             * to the picked row and the user can browse/switch again,
             * like the Config flyout's repeated toggles. */
            editor_request_redraw();
            return 1;
        }
        return 0;
    }
    glr_scene_load_example(hit->item_idx);
    ui_menu_bar_close();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_CODE_PANEL_TAB: scene tab strip click. Single click switches
 * scenes through the shared Scene-menu load helpers (reusing the exact
 * load sequence — no duplication); a 2nd click on the same user tab
 * within DOUBLE_CLICK_MS opens the existing status-bar inline rename.
 * The display index is resolved against the *live* scene shape so a
 * frame-stale click can't misroute. Always consumed. */
static int route_scene_tab_hit(const UiHit *hit) {
    int idx           = hit->item_idx;
    int user_tab_count = repl_user_scene_count();
    int example_idx   = repl_state_scenes().active_example_idx;
    int active_slot   = repl_active_user_scene();
    unsigned int now_ms = current_double_click_ms();
    int is_double = (g_last_tab_press_idx >= 0 &&
                     g_last_tab_press_idx == idx &&
                     now_ms - g_last_tab_press_ms < DOUBLE_CLICK_MS);

    g_last_tab_press_ms  = now_ms;
    g_last_tab_press_idx = idx;

    if (idx < 0)
        return 1;  /* stale — consumed no-op */

    if (idx < user_tab_count) {
        int slot = glr_scene_menu_slot_for_dense_index(idx);
        if (slot < 0)
            return 1;  /* stale */
        if (slot != active_slot)
            glr_scene_load_user_slot(slot);  /* no-op when already active */
        if (is_double)
            editor_inline_rename_begin(slot);
        editor_request_redraw();
        return 1;
    }

    if (idx == user_tab_count && example_idx >= 0) {
        /* The example tab is active iff no user slot is active. Only
         * reload when it is not already the active scene; never rename. */
        if (active_slot >= 0)
            glr_scene_load_example(example_idx);
        editor_request_redraw();
        return 1;
    }

    return 1;  /* out-of-range stale idx — consumed no-op */
}

/* UI_HIT_STATUS_HISTORY: the bottom messages button (item_idx == 1)
 * toggles the inline recent-message list; a click on the open list body
 * (item_idx == 0) is consumed so it doesn't fall through to the scene. */
static int route_status_history_hit(const UiHit *hit) {
    if (hit->item_idx == 1) {
        ui_state_status_history_toggle();
        editor_request_redraw();
    }
    return 1;
}

/* UI_HIT_PANEL_DIVIDER: start the panel-resize drag. Motion updates
 * panel_frac via editor_handle_motion's resizing-panel branch; UP
 * clears the resizing flag. */
static int route_panel_divider_hit(const UiHit *hit) {
    (void)hit;
    ui_state_code_panel_mut()->resizing_panel = 1;
    editor_set_cursor(editor_input_code_panel_resize_cursor());
    return 1;
}

/* UI_HIT_VARIABLE_SLIDER: variable-panel left-click drag begin. The
 * J1 helper handles replay-stop + drag start. */
static int route_variable_slider_hit(int x, int y) {
    return glr_ctrl_router_handle_variable_panel_drag_begin(GLUT_LEFT_BUTTON,
                                                               GLUT_DOWN, x, y);
}

/* UI_HIT_VARIABLE_COLLAPSE_TOGGLE: title-bar chip that folds the panel down
 * to just its title (or restores it). Mouse-only, left-click, no keymap
 * slot — mirrors the gl-state popup's details/setup chips. */
static int route_variable_panel_collapse_toggle_hit(void) {
    variable_panel_toggle_collapsed();
    editor_request_redraw();
    return 1;
}

/* UI_HIT_PROFILE_SECTION_TOGGLE: presentation-only DETAILS-tree state. The
 * profiler probes remain active for hidden descendants. */
static int route_profile_section_toggle_hit(const UiHit *hit) {
    UiProfilePanelState *panel = ui_state_profile_panel_mut();
    panel->collapsed_sections = ui_profile_panel_toggle_mask(
        panel->collapsed_sections, hit->item_idx);
    editor_request_redraw();
    return 1;
}

/* UI_HIT_HISTOGRAM_RESET: the histogram panel's header "[reset]" control.
 * The histograms are cumulative (unlike the section listing's decaying
 * EMAs), so this is the manual counterpart of the automatic clear on
 * example switch. */
static int route_histogram_reset_hit(void) {
    prof_histogram_reset();
    ui_state_status_set("section histograms reset");
    editor_request_redraw();
    return 1;
}

/* UI_HIT_HISTOGRAM_SERIES_TOGGLE: a click on a legend swatch/label toggles
 * that series in and out of the overlaid histogram plot. Presentation-only
 * (like the section-tree collapse); the profiler keeps sampling the hidden
 * series so re-showing it reveals the full distribution. */
static int route_histogram_series_toggle_hit(const UiHit *hit) {
    UiProfilePanelState *panel = ui_state_profile_panel_mut();
    panel->hidden_histogram_series = ui_histogram_panel_toggle_series(
        panel->hidden_histogram_series, hit->item_idx);
    editor_request_redraw();
    return 1;
}

/* Derive a code-panel target line from a hit, mirroring the legacy
 * code_panel_drag_target. Insert-line drags use edit_line (the line
 * the cursor is parked on), only falling back to the last committed
 * line when edit_line is past the document end — preserving the
 * insertion-point as the drag endpoint when the user is editing
 * mid-document. Returns -1 if the hit is not a code-panel kind. */
static int code_panel_target_from_hit(UiHit hit) {
    switch (hit.kind) {
    case UI_HIT_CODE_TEXT:
    case UI_HIT_CODE_GUTTER:
        return hit.line_idx;
    case UI_HIT_CODE_INSERT_LINE: {
        int target = hit.line_idx; /* Target line to focus on. */
        int doc_count = repl_state_document_count();
        if (target >= doc_count)
            target = doc_count - 1;
        return target;
    }
    default:
        return -1;
    }
}

static int route_tour_hud_hit(void) {
    if (!glr_pointer_script_toggle_tour_hud())
        return 0;
    editor_request_redraw();
    return 1;
}

int glr_ctrl_router_handle_code_panel_hit(UiHit hit, int x, int y) {
    /* HUD expansion is presentation-only. Route before generic click
     * side-effects so inspecting transport details cannot cancel an inline
     * rename/file prompt or dismiss a menu the authored tour is using. */
    if (hit.kind == UI_HIT_TOUR_HUD)
        return route_tour_hud_hit();

    /* Inline rename is a hard modal for keystrokes but NOT for the
     * mouse, so a click otherwise moves the cursor / switches tabs
     * while typed keys still feed the rename buffer — misleading.
     * Clicking anywhere cancels the in-progress rename (discard, like
     * Esc), then the click proceeds normally (cursor lands where the
     * user clicked, rename mode exits). Begin-rename routes through a
     * later dispatch in this same call, so rename is not yet active at
     * entry for the click that starts it — no self-cancel. */
    if (editor_inline_rename_active())
        editor_inline_rename_cancel();
    if (editor_inline_file_prompt_active())
        editor_inline_file_prompt_cancel();

    /* A click outside the menu bar (anywhere that isn't UI_HIT_MENU_BUTTON,
     * UI_HIT_MENU_ITEM, or UI_HIT_SUBMENU_ITEM) dismisses an open
     * dropdown — matches the legacy
     * "click outside dropdown closes it" behaviour from
     * ui_panels_handle_code_panel_press. */
    int dismissed_dropdown = 0;
    if (hit.kind != UI_HIT_MENU_BUTTON &&
        hit.kind != UI_HIT_MENU_ITEM &&
        hit.kind != UI_HIT_SUBMENU_ITEM &&
        hit.kind != UI_HIT_PIN_BUTTON &&
        hit.kind != UI_HIT_COLOR_SWATCH &&
        ui_menu_bar_menu_dropdown_is_open()) {
        ui_menu_bar_close();
        dismissed_dropdown = 1;
    }

    int consumed;
    switch (hit.kind) {
    case UI_HIT_COLOR_SWATCH:
        consumed = route_color_picker_control_hit(x, y); break;
    case UI_HIT_INLINE_COLOR_SWATCH:
        consumed = route_inline_color_swatch_hit(&hit, y); break;
    case UI_HIT_NUMERIC_SWATCH:
        consumed = route_numeric_swatch_hit(&hit, numeric_swatch_scale(0)); break;
    case UI_HIT_PIN_BUTTON:
        consumed = route_pin_button_hit(&hit); break;
    case UI_HIT_SEARCH_NAV:
        consumed = route_search_nav_hit(&hit); break;
    case UI_HIT_SEARCH_FOCUS:
        consumed = route_search_focus_hit(&hit); break;
    case UI_HIT_SEARCH_WORD_TOGGLE:
        consumed = route_search_word_toggle_hit(); break;
    case UI_HIT_SEARCH_REPLACE:
        consumed = route_search_replace_hit(&hit); break;
    case UI_HIT_MENU_BUTTON:
        consumed = route_menu_button_hit(&hit); break;
    case UI_HIT_MENU_ITEM:
        consumed = route_menu_item_hit(&hit); break;
    case UI_HIT_SUBMENU_ITEM:
        consumed = route_submenu_item_hit(&hit); break;
    case UI_HIT_VARIABLE_SLIDER:
        consumed = route_variable_slider_hit(x, y); break;
    case UI_HIT_VARIABLE_COLLAPSE_TOGGLE:
        consumed = route_variable_panel_collapse_toggle_hit(); break;
    case UI_HIT_PROFILE_SECTION_TOGGLE:
        consumed = route_profile_section_toggle_hit(&hit); break;
    case UI_HIT_HISTOGRAM_RESET:
        consumed = route_histogram_reset_hit(); break;
    case UI_HIT_HISTOGRAM_SERIES_TOGGLE:
        consumed = route_histogram_series_toggle_hit(&hit); break;
    case UI_HIT_OVERLAY_CHROME:
        consumed = 1; break;
    case UI_HIT_PANEL_DIVIDER:
        consumed = route_panel_divider_hit(&hit); break;
    case UI_HIT_CODE_TEXT:
        consumed = route_code_text_hit(&hit); break;
    case UI_HIT_CODE_INSERT_LINE:
        consumed = route_code_insert_line_hit(&hit); break;
    case UI_HIT_CODE_GUTTER:
        consumed = route_code_gutter_hit(&hit); break;
    case UI_HIT_CODE_PANEL_CHROME:
        consumed = route_code_panel_chrome_hit(); break;
    case UI_HIT_CODE_CLEAR_ALL:
        consumed = route_code_clear_all_hit(); break;
    case UI_HIT_CODE_COPY:
        consumed = route_code_copy_hit(); break;
    case UI_HIT_CODE_CUT:
        consumed = route_code_cut_hit(); break;
    case UI_HIT_CODE_PASTE:
        consumed = route_code_paste_hit(); break;
    case UI_HIT_CODE_UNDO:
        consumed = route_code_undo_hit(); break;
    case UI_HIT_CODE_REDO:
        consumed = route_code_redo_hit(); break;
    case UI_HIT_CODE_FOCUS_TOGGLE:
        consumed = route_code_focus_toggle_hit(); break;
    case UI_HIT_HELP_TOGGLE:
        consumed = route_help_toggle_hit(); break;
    case UI_HIT_CODE_PANEL_TAB:
        consumed = route_scene_tab_hit(&hit); break;
    case UI_HIT_STATUS_HISTORY:
        consumed = route_status_history_hit(&hit); break;
    case UI_HIT_HELP_PANEL:
    case UI_HIT_REPLAY_BUTTON:
    case UI_HIT_SCENE:
    case UI_HIT_NONE:
    default:
        consumed = 0;
    }

    /* A click that only dismissed an open dropdown is consumed by the
     * dismiss itself; do not fall through to scene-press / camera /
     * variable-slider drag for the same press. Matches legacy
     * UI_PANEL_PRESS_CONSUMED behaviour. */
    if (!consumed && dismissed_dropdown)
        return 1;
    return consumed;
}

/* Would a left press at (x, y) route to `kind`? Two layers answer ahead of
 * the canonical hit-test in mouse_dispatch, and neither is reachable through
 * it: the above-gl-state overlay pass (telemetry panels, status surfaces,
 * variable panel, an open dropdown) and the floating OpenGL-state popup's own
 * surface. Anything that has to promise what a click will do — the GLUT host's
 * tour-HUD intercept, the divider's hover cursor — must consult that same
 * order, or it speaks for pixels another panel already owns.
 *
 * `kind` is an int because UiHit.kind is: the core kinds and the app-band
 * extensions in ui/app/hit.h live in separate enums. */
static int router_press_routes_to(int x, int y, int kind) {
    UiRenderSnapshot snap;
    int variable_count = repl_eval_predef_view().count;

    glr_ctrl_build_ui_snapshot(&snap);
    if (ui_panels_hit_test_above_gl_state(&snap, x, y, variable_count).kind
            != UI_HIT_NONE)
        return 0;
    if (glr_ctrl_router_point_in_gl_state_popup(x, y))
        return 0;
    return ui_panels_hit_test(&snap, x, y, variable_count).kind == kind;
}

/* The GLUT host asks before its generic "real click cancels the tour"
 * intercept. Reuse canonical z-order classification rather than merely
 * checking the HUD rectangle: a menu or later floating overlay painted over
 * the same pixels must keep ownership and must not masquerade as a HUD click. */
int glr_ctrl_router_point_on_tour_hud(int x, int y) {
    return router_press_routes_to(x, y, UI_HIT_TOUR_HUD);
}

int glr_ctrl_router_handle_code_panel_drag(int x, int y) {
    if (!g_code_panel_drag_active || g_code_panel_drag_anchor < 0)
        return 0;

    const UiRenderSnapshot *ui_snap = glr_ctrl_drag_hit_test_snapshot();

    UiHit hit = ui_panels_hit_test(ui_snap, x, y, repl_eval_predef_view().count);

    /* Per-character input-buffer drag: as long as the drag stays on
     * the same source row as the press AND that row is the active
     * edit line, grow the input-buffer selection toward the current
     * char column. */
    if (hit.kind == UI_HIT_CODE_TEXT && hit.char_idx >= 0 &&
        glr_ctrl_router_apply_input_row_drag(hit.line_idx, hit.char_idx))
        return 1;

    int target = code_panel_target_from_hit(hit);
    if (target < 0) {
        /* Drag wandered off the code-panel kinds — clamp the pointer
         * into the code-panel rect and re-classify so the selection
         * extends to the nearest visible row. Matches legacy
         * code_panel_drag_target's [0, visible_lines-1] vis clamp. */
        int cp_x, cp_y, cp_w, cp_h;
        ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
        int win_h = ui_state_viewport().window_h;
        if (cp_w > 0 && cp_h > 0 && win_h > 0) {
            int gl_y = win_h - y;
            int cx = x;
            int cy = y;
            if (cx < cp_x + 1) cx = cp_x + 1;
            if (cx > cp_x + cp_w - 1) cx = cp_x + cp_w - 1;
            if (gl_y < cp_y + 1) cy = win_h - (cp_y + 1);
            if (gl_y > cp_y + cp_h - 1) cy = win_h - (cp_y + cp_h - 1);
            UiHit clamped = ui_panels_hit_test(ui_snap, cx, cy,
                                               repl_eval_predef_view().count);
            target = code_panel_target_from_hit(clamped);
        }
    }
    if (target < 0)
        return 1;

    /* A char-anchored drag that crosses off its press row promotes to
     * whole-line range selection: the press row's partial selection
     * widens to the full line and the range grows by rows from there.
     * The input buffer holds one row of text, so a character range
     * cannot span rows; promoting keeps the ordinary editor gesture
     * (press in the text, drag down) producing the multi-line
     * selection the user is reaching for instead of dead-ending. The
     * promotion is one-way — dragging back onto the press row narrows
     * the range to that single line rather than restoring the original
     * character span, so the gesture never flips models twice. */
    if (g_code_panel_drag_char_anchor >= 0) {
        if (target == g_code_panel_drag_anchor)
            return 1;
        glr_ctrl_router_promote_char_drag_to_line_range(target);
        return 1;
    }

    if (target != g_code_panel_drag_anchor || g_code_panel_drag_moved)
        extend_line_range_to(target);
    return 1;
}

/* Shared wheel routing for both wheel transports (freeglut's wheel
 * callback and plain GLUT's buttons-3/4 emulation). delta > 0 scrolls
 * content toward later rows / zooms out; the two entry points normalize
 * their opposite sign conventions before calling. Priority: modal help
 * overlay, surfaces painted above the OpenGL-state popup (including menu
 * flyouts and inert panel chrome), the popup itself, code panel, then camera
 * zoom velocity — each earlier owner consumes the wheel so it never reaches
 * what's behind it. */
static void route_wheel(int x, int y, int delta) {
    UiRenderSnapshot ui_snap;
    UiHit front_hit;

    if (ui_state_command_description().visible &&
        editor_input_point_in_code_panel(x, y))
        ui_state_command_description_close();
    if (ui_state_help().visible) {
        glr_ctrl_help_scroll_by(delta);
    } else {
        glr_ctrl_build_ui_snapshot(&ui_snap);
        front_hit = ui_panels_hit_test_above_gl_state(
            &ui_snap, x, y, repl_eval_predef_view().count);
        if (front_hit.kind != UI_HIT_NONE) {
            /* The only scrollable front surface is an overflowing menu
             * flyout. Every other visible panel intentionally consumes the
             * wheel inert, including a short/non-scrollable flyout. */
            if (front_hit.kind == UI_HIT_MENU_ITEM ||
                front_hit.kind == UI_HIT_SUBMENU_ITEM ||
                front_hit.kind == UI_HIT_CODE_PANEL_CHROME)
                (void)ui_menu_bar_handle_wheel_scroll(x, y, delta);
        } else if (glr_ctrl_router_handle_gl_state_popup_wheel(x, y, delta)) {
            /* Consumed by the OpenGL-state popup under the pointer. */
        } else if (editor_input_point_in_code_panel(x, y)) {
            glr_ctrl_router_dismiss_gl_state_for_editor_input();
            editor_input_code_panel_scroll(delta);
        } else {
            glr_camera_add_zoom_velocity((float)delta * GLR_WHEEL_ZOOM_STEP);
        }
    }
    editor_request_redraw();
}

/* ===========================================================================
 * Dispatch entry points
 *
 * Each GLUT entry point is a thin shim around a *_dispatch body:
 *
 *   void glr_ctrl_<event>(...) {
 *       glr_audio_on_user_gesture();                  (key/mouse only)
 *       editor_reset_input_effects();
 *       <event>_dispatch(...);                        (plain returns)
 *       glr_ctrl_apply_input_effects(editor_take_and_reset_input_effects());
 *   }
 *
 * Every route — controller helper or editor — accumulates side effects
 * into the shared editor_input pending-effects struct (via
 * editor_request_redraw etc.; editor_handle_* snapshots are folded back
 * in with editor_merge_input_effects), and the shim flushes them through
 * GLUT exactly once per event. Dispatch bodies therefore just `return`
 * when a route consumes the event — no per-branch flushing.
 *
 * Each dispatch body is a fixed sandwich:
 *   1. Rename / file-prompt modal capture FIRST (hard modal — every
 *      key/special key goes to the capture buffer).
 *   2. Controller-owned routes: routing helpers above, in priority
 *      order.
 *   3. Editor's domain: editor_handle_* fires for clicks / keys that
 *      land in the editor's text-document or code-panel UI rect.
 * ===========================================================================
 */

static void keyboard_dispatch(unsigned char key, int x, int y) {
    /* The command help card is intentionally ephemeral: the next key event
     * resumes editing and dismisses it without swallowing that key. */
    ui_state_command_description_close();

    /* Rename / file-prompt capture: hard modal. */
    if (editor_input_rename_capture_key(key)) {
        glr_ctrl_router_dismiss_gl_state_for_editor_input();
        return;
    }
    if (editor_input_file_prompt_capture_key(key)) {
        glr_ctrl_router_dismiss_gl_state_for_editor_input();
        return;
    }

    if (glr_ctrl_router_handle_escape_key(key))
        return;

    /* Controller-owned routes — config shortcuts run before replay
     * forwarding so Ctrl+R stays owned by the Replay config row while
     * non-config replay keys (Ctrl+K, space, +/- and friends) still
     * forward to replay_handle_key. */
    if (glr_ctrl_router_handle_config_menu_key(key) ||
        glr_ctrl_router_handle_audio_key(key) ||
        glr_ctrl_router_handle_cfg_shortcut_key(key) ||
        glr_ctrl_router_handle_replay_key(key) ||
        glr_ctrl_router_handle_save_key(key) ||
        glr_ctrl_router_handle_debug_dump_key(key) ||
        glr_ctrl_router_handle_time_reset_key(key) ||
        glr_ctrl_router_handle_accum_samples_key(key) ||
        glr_ctrl_router_handle_code_focus_key(key) ||
        glr_ctrl_router_handle_tutorial_ack_key(key) ||
        glr_ctrl_router_handle_quit_key(key))
        return;

    /* Ctrl+F opens search downstream in editor_handle_key. Note the
     * rising edge here so the search-overlay fade clock is driven from
     * the controller — symmetric with the menu-pin path in
     * route_pin_button_hit; the renderer no longer mutates it. */
    int search_was_active = editor_state_search()->active;
    glr_ctrl_router_dismiss_gl_state_for_editor_input();
    editor_merge_input_effects(editor_handle_key(key, x, y));
    if (!search_was_active && editor_state_search()->active)
        ui_menu_bar_note_search_opened(repl_state_variables().anim_time);
}

void glr_ctrl_keyboard(unsigned char key, int x, int y) {
    glr_audio_on_user_gesture();

    /* macOS Cmd+letter normalization happens before any dispatch so
     * the controller-owned cfg-shortcut chain (Cmd+B / Cmd+S / Cmd+T
     * etc.) compares against the control-character form (KEY_CTRL_*).
     * Without this, Cmd+B arrives here as 'b' (0x62), the chain looks
     * for KEY_CTRL_B (0x02) in g_cfg_items[].key_code, and the
     * shortcut silently misses. */
    key = editor_input_normalize_super_to_ctrl(key);

    editor_reset_input_effects();
    keyboard_dispatch(key, x, y);
    glr_ctrl_apply_input_effects(editor_take_and_reset_input_effects());
}

static void special_dispatch(int key, int x, int y) {
    ui_state_command_description_close();

    if (editor_input_rename_capture_special(key)) {
        glr_ctrl_router_dismiss_gl_state_for_editor_input();
        return;
    }
    if (editor_input_file_prompt_capture_special(key)) {
        glr_ctrl_router_dismiss_gl_state_for_editor_input();
        return;
    }

    if (glr_ctrl_router_handle_replay_special(key) ||
        glr_ctrl_router_handle_cfg_special_shortcut(key) ||
        glr_ctrl_router_handle_horizontal_audio_special(key) ||
        glr_ctrl_router_handle_help_tab_special(key) ||
        glr_ctrl_router_handle_help_scroll_special(key) ||
        glr_ctrl_router_handle_help_toggle_special(key) ||
        glr_ctrl_router_handle_scene_cycle_special(key) ||
        glr_ctrl_router_handle_tutorial_cycle_special(key))
        return;

    glr_ctrl_router_dismiss_gl_state_for_editor_input();
    editor_merge_input_effects(editor_handle_special(key, x, y));
}

/* FreeGLUT's Cocoa backend reports modifier transitions through the special
 * callback before reporting a combined shortcut through the keyboard
 * callback. Modifier state is already read through glutGetModifiers(), so
 * these transition notifications are not controller actions and must not
 * enter modal, overlay, shortcut, or editor routing. System GLUT variants
 * that do not expose the FreeGLUT modifier-key constants also do not emit
 * these events; keep the fallback build source-compatible with them. */
static int glr_ctrl_special_is_modifier_event(int key) {
    switch (key) {
#ifdef GLUT_KEY_SHIFT_L
    case GLUT_KEY_SHIFT_L:
#endif
#ifdef GLUT_KEY_SHIFT_R
    case GLUT_KEY_SHIFT_R:
#endif
#ifdef GLUT_KEY_CTRL_L
    case GLUT_KEY_CTRL_L:
#endif
#ifdef GLUT_KEY_CTRL_R
    case GLUT_KEY_CTRL_R:
#endif
#ifdef GLUT_KEY_ALT_L
    case GLUT_KEY_ALT_L:
#endif
#ifdef GLUT_KEY_ALT_R
    case GLUT_KEY_ALT_R:
#endif
#ifdef GLUT_KEY_SUPER_L
    case GLUT_KEY_SUPER_L:
#endif
#ifdef GLUT_KEY_SUPER_R
    case GLUT_KEY_SUPER_R:
#endif
#if defined(GLUT_KEY_SHIFT_L) || defined(GLUT_KEY_SHIFT_R) || \
    defined(GLUT_KEY_CTRL_L) || defined(GLUT_KEY_CTRL_R) || \
    defined(GLUT_KEY_ALT_L) || defined(GLUT_KEY_ALT_R) || \
    defined(GLUT_KEY_SUPER_L) || defined(GLUT_KEY_SUPER_R)
        return 1;
#endif
    default:
        return 0;
    }
}

void glr_ctrl_special(int key, int x, int y) {
    glr_audio_on_user_gesture();

    if (glr_ctrl_special_is_modifier_event(key))
        return;

    editor_reset_input_effects();
    special_dispatch(key, x, y);
    glr_ctrl_apply_input_effects(editor_take_and_reset_input_effects());
}

/* Scripted-chord entry points: dispatch a single key/special press while a
 * caller-declared modifier mask is authoritative, so a pointer script can
 * fire Shift/Ctrl+Shift shortcuts the physical-modifier path can't synthesize.
 * The editor override is pushed and popped here — the controller stays the
 * sole input-event mutation gate; the pointer script only calls these. */
void glr_ctrl_keyboard_with_modifiers(unsigned char key, int x, int y, int mods) {
    editor_input_push_scripted_modifiers(mods);
    glr_ctrl_keyboard(key, x, y);
    editor_input_pop_scripted_modifiers();
}

void glr_ctrl_special_with_modifiers(int key, int x, int y, int mods) {
    editor_input_push_scripted_modifiers(mods);
    glr_ctrl_special(key, x, y);
    editor_input_pop_scripted_modifiers();
}

/* Mouse routing: hit-test to decide owner before dispatching.
 *
 * UP cleanup: the editor first releases its own UP-side state
 * (panel resize end), then the variable-panel drag release fires if
 * active, then camera UP so the orbit/pan/zoom interaction releases.
 *
 * DOWN, left button: modal help overlay first, then the reverse-z pass for
 * panels painted over the OpenGL-state popup, then the popup, then the
 * canonical ui_panels_hit_test → UiHit.kind switch
 * (glr_ctrl_router_handle_code_panel_hit); non-chrome kinds fall
 * through to scene press (color picker overlay) and camera. Right
 * button: route_right_press — the same canonical hit-test with
 * right-button semantics. Buttons 3/4 (plain GLUT's wheel emulation)
 * route through the shared route_wheel. */
static void mouse_dispatch(int button, int state, int x, int y) {
    if (button == GLUT_LEFT_BUTTON || button == GLUT_MIDDLE_BUTTON ||
        button == GLUT_RIGHT_BUTTON) {
        ui_state_pointer_set(x, y, state == GLUT_DOWN ? button : -1);
    } else {
        ui_state_pointer_set_pos(x, y);
    }

    /* The release paired with the right-click that opened the card is not a
     * new editor interaction. A later press/wheel event in the editor is. */
    if (state == GLUT_DOWN && ui_state_command_description().visible &&
        (editor_input_point_in_code_panel(x, y) ||
         editor_input_point_on_code_panel_divider(x, y))) {
        ui_state_command_description_close();
        editor_request_redraw();
    }

    if (state == GLUT_UP) {
        color_picker_handle_release();
        glr_ctrl_router_reset_code_panel_drag();
        editor_merge_input_effects(editor_handle_mouse(button, state, x, y));
        if (glr_ctrl_router_handle_variable_panel_drag_release(state))
            return;
        glr_ctrl_router_handle_camera_mouse(button, state, x, y);
        return;
    }

    if (button == GLUT_LEFT_BUTTON) {
        /* Modal help overlay intercepts left clicks first: tab
         * select / click-away dismiss / swallow body. */
        if (glr_ctrl_router_handle_help_click(button, state, x, y))
            return;
        /* Later-rendered panels own overlapping pixels. Their actionable
         * controls still route normally; inert panel bodies explicitly
         * consume the click. A front panel outside the popup keeps the
         * popup's established click-away behavior (except while a menu is
         * open, matching the popup handler below). */
        UiRenderSnapshot ui_snap;
        glr_ctrl_build_ui_snapshot(&ui_snap);
        UiHit front_hit = ui_panels_hit_test_above_gl_state(
            &ui_snap, x, y, repl_eval_predef_view().count);
        if (front_hit.kind != UI_HIT_NONE) {
            if (ui_state_gl_state_inspector().visible &&
                !ui_menu_bar_menu_dropdown_is_open() &&
                !glr_ctrl_router_point_in_gl_state_popup(x, y)) {
                ui_state_gl_state_inspector_close();
                editor_request_redraw();
            }
            (void)glr_ctrl_router_handle_code_panel_hit(front_hit, x, y);
            return;
        }
        /* Floating OpenGL-state popup: swallow clicks on its surface,
         * dismiss on click-away (the handler returns 0 for the latter so
         * the click still routes below). */
        if (glr_ctrl_router_handle_gl_state_popup_left_press(x, y))
            return;
        /* Classify the click via the canonical hit-test, then route by
         * UiHit.kind to the owning subsystem. The hit-test covers
         * variable panel, color picker, menu bar, code panel (including
         * divider + inline swatch + insert line) and pin buttons. Only
         * kinds that don't apply (UI_HIT_SCENE, UI_HIT_NONE,
         * UI_HIT_HELP_PANEL) fall through to scene press / camera. */
        UiHit hit = ui_panels_hit_test(&ui_snap, x, y,
                           repl_eval_predef_view().count);
        if (glr_ctrl_router_handle_code_panel_hit(hit, x, y))
            return;
        if (glr_ctrl_router_handle_scene_press(button, state, x, y))
            return;
        glr_ctrl_router_handle_camera_mouse(button, state, x, y);
        return;
    }

    if (button == GLUT_RIGHT_BUTTON) {
        route_right_press(x, y);
        return;
    }

#ifdef USE_GLUT
    /* Plain GLUT delivers the scroll wheel as button 3 (up) / 4 (down)
     * presses; freeglut uses the glr_ctrl_mousewheel callback instead. */
    if ((button == 3 || button == 4) && state == GLUT_DOWN) {
        route_wheel(x, y, (button == 3) ? -1 : 1);
        return;
    }
#endif

    glr_ctrl_router_handle_camera_mouse(button, state, x, y);
}

void glr_ctrl_mouse(int button, int state, int x, int y) {
    glr_audio_on_user_gesture();

    editor_reset_input_effects();
    mouse_dispatch(button, state, x, y);
    glr_ctrl_apply_input_effects(editor_take_and_reset_input_effects());
}

/* Motion routing: UI overlay (color picker drag), variable-panel drag
 * motion, and camera drag motion are router-side; code-panel resize
 * tracking and code-panel selection drag are editor's.
 *
 * Pointer-state tracking: each non-camera handler updates
 * ui_state_pointer via glr_camera_pointer_set(x, y) AFTER its own
 * work so the camera's view of the pointer stays current. The camera
 * branch deliberately does NOT pre-set the pointer because
 * glr_camera_drag_motion reads the previous (px, py) to compute
 * delta and updates the pointer to (x, y) at the end. Pre-setting
 * here would zero the delta and freeze orbit/pan/zoom drag. */
static void motion_dispatch(int x, int y) {
    /* Floating color picker drag tracking (SV / hue / alpha sliders). */
    {
        ColorPickerInputResult r = color_picker_handle_motion(x, y);
        if (r.consumed) {
            glr_ctrl_router_handle_camera_pointer_set(x, y);
            editor_request_redraw();
            return;
        }
    }

    if (glr_ctrl_router_handle_variable_panel_motion(x, y)) {
        glr_ctrl_router_handle_camera_pointer_set(x, y);
        return;
    }

    /* Code-panel selection drag (controller-owned state). */
    if (glr_ctrl_router_handle_code_panel_drag(x, y)) {
        glr_ctrl_router_handle_camera_pointer_set(x, y);
        return;
    }

    /* Editor's domain: panel resize tracking. editor_handle_motion is
     * a no-op when resizing_panel is clear. */
    if (ui_state_code_panel().resizing_panel) {
        glr_ctrl_router_dismiss_gl_state_for_editor_input();
        editor_merge_input_effects(editor_handle_motion(x, y));
        glr_ctrl_router_handle_camera_pointer_set(x, y);
        return;
    }

    /* Camera drag motion reads pointer = (px, py), computes delta,
     * then calls pointer_set(x, y) itself. */
    glr_ctrl_router_handle_camera_motion(x, y);
}

void glr_ctrl_motion(int x, int y) {
    editor_reset_input_effects();
    motion_dispatch(x, y);
    glr_ctrl_apply_input_effects(editor_take_and_reset_input_effects());
}

static void passive_motion_dispatch(int x, int y) {
    /* Passive motion (no button held) just updates the pointer
     * position — there's no drag delta to preserve. */
    glr_ctrl_router_handle_camera_pointer_set(x, y);
    /* Several hover treatments (including the code-statusbar tooltips)
     * are derived purely from snapshot pointer state during rendering.
     * Redraw on motion so they appear/disappear even while animation is
     * paused and no timer-driven frame is forthcoming. */
    editor_request_redraw();
    EditorInputDispatchEffects editor_effects = editor_handle_passive_motion(x, y);
    /* The editor derives the resize cursor from divider geometry alone, but
     * anything painted over that pixel owns the click: an open menu
     * dropdown/flyout extending past the code-panel edge, the floating
     * OpenGL-state popup, a telemetry/variable panel, the modal help overlay.
     * Confirm the pixel really routes to the divider before promising a drag;
     * otherwise fall back to inherit. Only runs while the pointer is inside
     * the grab band, so the snapshot build stays off the general
     * passive-motion path. */
    if (editor_effects.set_cursor &&
        editor_effects.cursor != GLUT_CURSOR_INHERIT &&
        !router_press_routes_to(x, y, UI_HIT_PANEL_DIVIDER))
        editor_effects.cursor = GLUT_CURSOR_INHERIT;
    editor_merge_input_effects(editor_effects);
    ui_menu_bar_update_pointer_hover(x, y, repl_state_variables().anim_time);
}

void glr_ctrl_passive_motion(int x, int y) {
    editor_reset_input_effects();
    passive_motion_dispatch(x, y);
    glr_ctrl_apply_input_effects(editor_take_and_reset_input_effects());
}

void glr_ctrl_mousewheel(int wheel, int direction, int x, int y) {
#ifndef USE_GLUT
    /* freeglut wheel callback. direction is inverted relative to
     * route_wheel's delta convention (positive = toward later rows). */
    (void)wheel;
    editor_reset_input_effects();
    route_wheel(x, y, -direction);
    glr_ctrl_apply_input_effects(editor_take_and_reset_input_effects());
#else
    (void)wheel; (void)direction; (void)x; (void)y;
#endif
}

/* Deferred quit, consumed on the normal frame-tick path. SIGINT only sets
 * the flag, so no stdio/file I/O runs in handler context. Called from
 * glr_ctrl_tick in glr_ctrl.c via glr_ctrl_internal.h. */
void glr_ctrl_router_run_pending_quit(void) {
    if (g_quit_requested) {
        glr_ctrl_save_quit_recovery();
        exit(0);
    }
}
