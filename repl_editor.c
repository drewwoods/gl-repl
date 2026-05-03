/*
 * repl_editor.c - Special / mouse / motion / mousewheel / timer dispatch
 *                 + transitional public wrappers.
 *
 * Phase J1 commit 44 migrated keyboard dispatch + editor-owned helpers
 * to editor_input.c. The bodies that remain here:
 *  - special_func + handle_*_special_route (commit 45 will move)
 *  - mouse_func / motion_func / passive_motion_func / mousewheel_func
 *    (commit 46 will move)
 *  - timer_func (commit 48b will inline into imrepl_ctrl_timer)
 *  - editor_*_code_panel mouse-side helpers (commit 46 will move)
 *  - editor_special_restores_hidden_code_panel (commit 45 will move)
 *  - Public repl_*_func wrappers used by imrepl_ctrl + tests
 *    (commit 49a will delete the wrappers; tests migrate to
 *    editor_handle_*).
 *  - repl_editor_active_modifiers + repl_set_modifier_provider_for_test
 *    legacy forwarders (commit 49a will rename).
 */
#include "sample.h"
#include "repl_state.h"
#include "repl_parser.h"
#include "repl_actions.h"
#include "repl_core_internal.h"
#include "editor_commit.h"
#include "repl_debug.h"
#include "repl_command_store.h"
#include "repl_source_scope.h"
#include "repl_camera_controls.h"
#include "editor_clipboard.h"
#include "editor_undo.h"
#include "editor_input.h"
#include "replay.h"
#include "replay_state.h"
#include "editor_help_session.h"
#include "editor_completion.h"
#include "repl_keys.h"
#include "ui_panels.h"
#include "ui_layout.h"
#include "ui_menu_bar.h"
#include "ui_state.h"
#include "ui_variable_panel.h"
#include "variable_panel_drag.h"
#include "variable_panel.h"
#include "editor_inline_rename.h"
#include "repl_audio.h"

/* Editor-input helpers exported transitionally so the special / mouse /
 * motion bodies in this file can keep building until commits 45 and 46
 * migrate them. Once those bodies move into editor_input.c the
 * declarations collapse to file-private. */
int  editor_input_code_panel_layout(void);
int  editor_input_code_panel_hidden(void);
int  editor_input_restore_hidden_code_panel(void);
int  editor_input_router_handle_save_key(unsigned char key);
int  editor_input_router_handle_debug_dump_key(unsigned char key);
int  editor_input_router_handle_quit_key(unsigned char key);

/* ========================================================================= */
/* Forward declarations                                                      */
/* ========================================================================= */
static void mouse_func(int button, int state, int x, int y);
#ifndef USE_GLUT
static void mousewheel_func(int wheel, int direction, int x, int y);
#endif
static void passive_motion_func(int x, int y);
static void motion_func(int x, int y);
static void timer_func(int value);

void repl_set_modifier_provider_for_test(ReplModifierProvider provider) {
    editor_input_set_modifier_provider_for_test(provider);
}

int repl_editor_active_modifiers(void) {
    return editor_input_active_modifiers();
}

static int editor_point_in_code_panel(int x, int y) {
    int cp_x, cp_y, cp_w, cp_h;
    int gl_y = ui_state_viewport().window_h - y;

    repl_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    return x >= cp_x && x < cp_x + cp_w &&
           gl_y >= cp_y && gl_y < cp_y + cp_h;
}

static int editor_point_on_code_panel_divider(int x, int y) {
    int cp_x, cp_y, cp_w, cp_h;
    int gl_y = ui_state_viewport().window_h - y;
    int layout = editor_input_code_panel_layout();

    if (layout == CODE_PANEL_LAYOUT_HIDDEN)
        return 0;
    repl_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (layout == CODE_PANEL_LAYOUT_TOP)
        return abs(gl_y - cp_y) < 10;
    if (layout == CODE_PANEL_LAYOUT_BOTTOM)
        return abs(gl_y - (cp_y + cp_h)) < 10;
    return abs(x - (cp_x + cp_w)) < 10;
}

static int editor_code_panel_resize_cursor(void) {
    return editor_input_code_panel_layout() == CODE_PANEL_LAYOUT_LEFT
         ? GLUT_CURSOR_LEFT_RIGHT
         : GLUT_CURSOR_UP_DOWN;
}

static void editor_update_panel_frac_from_mouse(int x, int y) {
    ReplCodePanelRuntimeState *code_panel_state = ui_state_code_panel_mut();
    int layout = editor_input_code_panel_layout();

    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        return;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        int win_h = ui_state_viewport().window_h;
        if (win_h > 0)
            code_panel_state->panel_frac = (float)y / (float)win_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        int win_h = ui_state_viewport().window_h;
        if (win_h > 0)
            code_panel_state->panel_frac = (float)(win_h - y) / (float)win_h;
    } else {
        int win_w = ui_state_viewport().window_w;
        if (win_w > 0)
            code_panel_state->panel_frac = (float)x / (float)win_w;
    }

    if (code_panel_state->panel_frac < 0.1f)
        code_panel_state->panel_frac = 0.1f;
    if (code_panel_state->panel_frac > 0.9f)
        code_panel_state->panel_frac = 0.9f;
}

static void mouse_func(int button, int state, int x, int y) {
    if (state == GLUT_UP) {
        ui_panels_handle_mouse_release();
        if (variable_panel_drag_active()) {
            variable_panel_handle_drag_reset();
            editor_request_redraw();
            return;
        }
        if (ui_state_code_panel().resizing_panel) {
            ui_state_code_panel_mut()->resizing_panel = 0;
            editor_set_cursor(GLUT_CURSOR_INHERIT);
            editor_request_redraw();
            return;
        }
    }

    if (button == GLUT_LEFT_BUTTON && state == GLUT_DOWN) {
        if (variable_panel_visible()) {
            int row_idx;
            if (ui_variable_panel_hit(x, y, &row_idx)) {
                if (replay_active())
                    repl_replay_stop();
                variable_panel_handle_drag_begin(row_idx, 0, x);
                editor_request_redraw();
                return;
            }
        }

        /* The example dropdown can extend outside the code panel bounds (e.g.
         * below the panel in vertical layout).  Handle it before the
         * panel-area gate so clicks on any part of the dropdown register. */
        if (ui_menu_bar_example_dropdown_is_open()) {
            int cursor_pos = -1;
            int panel_actions = ui_panels_handle_code_panel_press(x, y, &cursor_pos);
            if (cursor_pos >= 0)
                editor_cursor_pos_set(cursor_pos);
            if (panel_actions & UI_PANEL_PRESS_OPENED_COLOR_PICKER)
                repl_undo_push_snapshot();
            editor_request_redraw();
            return;
        }

        if (editor_point_on_code_panel_divider(x, y)) {
            ui_state_code_panel_mut()->resizing_panel = 1;
            editor_set_cursor(editor_code_panel_resize_cursor());
            return;
        }
        if (editor_point_in_code_panel(x, y)) {
            int cursor_pos = -1;
            int panel_actions = ui_panels_handle_code_panel_press(x, y, &cursor_pos);
            if (cursor_pos >= 0)
                editor_cursor_pos_set(cursor_pos);
            if (panel_actions & UI_PANEL_PRESS_OPENED_COLOR_PICKER)
                repl_undo_push_snapshot();
            editor_request_redraw();
            return;
        }
        /* Scene-area click: let the color picker intercept before camera. */
        if (ui_panels_handle_scene_press(x, y)) {
            editor_request_redraw();
            return;
        }
    }

    /* Right-click inside the Config dropdown cycles the item backward;
     * missed clicks leave the menu open so the user can keep seeking. */
    if (button == GLUT_RIGHT_BUTTON && state == GLUT_DOWN) {
        if (ui_panels_handle_right_press(x, y)) {
            editor_request_redraw();
            return;
        }
    }

    /* Right-click on var panel: logarithmic drag mode. */
    if (button == GLUT_RIGHT_BUTTON && state == GLUT_DOWN && variable_panel_visible()) {
        int row_idx;
        if (ui_variable_panel_hit(x, y, &row_idx)) {
            if (replay_active())
                repl_replay_stop();
            variable_panel_handle_drag_begin(row_idx, 1, x);
            editor_request_redraw();
            return;
        }
    }

    repl_camera_mouse_event(button, state, x, y, editor_get_modifiers());

#ifdef USE_GLUT
    if (button == 3 && state == GLUT_DOWN) {
        if (ui_state_help().visible) {
            ui_state_help_mut()->scroll--;
        } else {
            if (editor_point_in_code_panel(x, y))
                editor_scroll_set(editor_scroll() - 1);
            else
                repl_camera_add_zoom_velocity(-0.3f);
        }
        editor_request_redraw();
    } else if (button == 4 && state == GLUT_DOWN) {
        if (ui_state_help().visible) {
            ui_state_help_mut()->scroll++;
        } else {
            if (editor_point_in_code_panel(x, y))
                editor_scroll_set(editor_scroll() + 1);
            else
                repl_camera_add_zoom_velocity(0.3f);
        }
        editor_request_redraw();
    }
#endif
}

#ifndef USE_GLUT
static void mousewheel_func(int wheel, int direction, int x, int y) {
    (void)wheel;
    if (ui_state_help().visible) {
        editor_help_session_scroll_by(-direction);
    } else {
        if (editor_point_in_code_panel(x, y))
            editor_scroll_set(editor_scroll() - direction);
        else
            repl_camera_add_zoom_velocity(-(float)direction * 0.1f);
    }
    editor_request_redraw();
}
#endif

static void passive_motion_func(int x, int y) {
    repl_camera_pointer_set(x, y);

    if (editor_point_on_code_panel_divider(x, y))
        editor_set_cursor(editor_code_panel_resize_cursor());
    else
        editor_set_cursor(GLUT_CURSOR_INHERIT);
}

static void motion_func(int x, int y) {
    if (ui_panels_handle_motion(x, y)) {
        repl_camera_pointer_set(x, y);
        editor_request_redraw();
        return;
    }

    if (ui_state_code_panel().resizing_panel) {
        editor_update_panel_frac_from_mouse(x, y);
        editor_request_redraw();
        return;
    }

    if (variable_panel_drag_active()) {
        variable_panel_handle_drag_motion(x);
        repl_camera_pointer_set(x, y);
        editor_request_redraw();
        return;
    }

    if (ui_panels_handle_code_panel_drag(x, y)) {
        repl_camera_pointer_set(x, y);
        editor_request_redraw();
        return;
    }

    repl_camera_drag_motion(x, y);
}

static void timer_func(int value) {
    (void)value;

    /* Advance the audio playlist if the current song reached its end
     * (no-op under loop=Song; see repl_audio_tick). */
    repl_audio_tick();

    /* When the playing track changes (either auto-advance from tick
     * or manual next/prev), surface the song name in the status bar.
     * Tracking by generation avoids needing a callback hook into
     * the audio module. */
    {
        static unsigned int last_track_gen = 0;
        unsigned int gen = repl_audio_track_generation();
        if (gen != last_track_gen) {
            last_track_gen = gen;
            const char *path = repl_audio_get_current_track();
            if (path && *path) {
                const char *base = strrchr(path, '/');
                base = base ? base + 1 : path;
                char msg[128];
                snprintf(msg, sizeof(msg), "Now playing: %s", base);
                set_status(msg);
            }
        }
    }

    repl_advance_time(0.016f);

    {
        ReplReplayRuntimeState *replay = replay_state_mut();

        if (replay->active)
            repl_replay_tick_fade_batches(0.016f);

        if (replay->active && replay->state == REPLAY_PLAYING) {
            replay->accum += replay->speed * 0.016f;
            while (replay->accum >= 1.0f &&
                   replay->state == REPLAY_PLAYING) {
                replay->accum -= 1.0f;
                repl_replay_advance();
            }
        }
    }

    repl_camera_tick();

    {
        ReplCodePanelRuntimeState *code_panel_state = ui_state_code_panel_mut();
        (code_panel_state->blink_tick)++;
        if (code_panel_state->blink_tick >= 30) {
            code_panel_state->blink_tick = 0;
            code_panel_state->cursor_visible = !code_panel_state->cursor_visible;
        }
    }

    {
        ReplStatusState *status = ui_state_status_mut();
        if (status->ttl > 0)
            status->ttl--;
    }

    editor_request_redraw();
    editor_schedule_timer(16, 0);
}

ReplInputDispatchEffects repl_keyboard_func(unsigned char key, int x, int y) {
    return editor_handle_key(key, x, y);
}

ReplInputDispatchEffects repl_special_func(int key, int x, int y) {
    return editor_handle_special(key, x, y);
}

ReplInputDispatchEffects repl_mouse_func(int button, int state, int x, int y) {
    editor_reset_input_effects();
    editor_input_notify_audio_gesture_once();
    mouse_func(button, state, x, y);
    return editor_take_input_effects();
}

ReplInputDispatchEffects repl_motion_func(int x, int y) {
    editor_reset_input_effects();
    motion_func(x, y);
    return editor_take_input_effects();
}

ReplInputDispatchEffects repl_passive_motion_func(int x, int y) {
    editor_reset_input_effects();
    passive_motion_func(x, y);
    return editor_take_input_effects();
}

#ifndef USE_GLUT
ReplInputDispatchEffects repl_mousewheel_func(int wheel, int direction, int x, int y) {
    editor_reset_input_effects();
    mousewheel_func(wheel, direction, x, y);
    return editor_take_input_effects();
}
#endif

ReplInputDispatchEffects repl_timer_func(int value) {
    editor_reset_input_effects();
    timer_func(value);
    return editor_take_input_effects();
}
