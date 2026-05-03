/*
 * repl_editor.c — Timer dispatch + transitional public wrappers.
 *
 * Phase J1 commits 44–46 migrated keyboard / special / mouse / motion /
 * mousewheel dispatch into editor_input.c. What remains:
 *  - timer_func body (commit 48b will inline into imrepl_ctrl_timer)
 *  - repl_*_func public wrappers used by imrepl_ctrl + tests
 *    (commit 49a deletes the wrappers; tests migrate to
 *    editor_handle_*)
 *  - repl_editor_active_modifiers + repl_set_modifier_provider_for_test
 *    legacy forwarders (commit 49a will rename)
 */
#include "sample.h"
#include "repl_state.h"
#include "repl_core_internal.h"
#include "editor_input.h"
#include "replay.h"
#include "replay_state.h"
#include "ui_state.h"
#include "repl_audio.h"
#include "repl_camera_controls.h"

#include <stdio.h>
#include <string.h>

static void timer_func(int value);

void repl_set_modifier_provider_for_test(ReplModifierProvider provider) {
    editor_input_set_modifier_provider_for_test(provider);
}

int repl_editor_active_modifiers(void) {
    return editor_input_active_modifiers();
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
    return editor_handle_mouse(button, state, x, y);
}

ReplInputDispatchEffects repl_motion_func(int x, int y) {
    return editor_handle_motion(x, y);
}

ReplInputDispatchEffects repl_passive_motion_func(int x, int y) {
    return editor_handle_passive_motion(x, y);
}

#ifndef USE_GLUT
ReplInputDispatchEffects repl_mousewheel_func(int wheel, int direction, int x, int y) {
    return editor_handle_mousewheel(wheel, direction, x, y);
}
#endif

ReplInputDispatchEffects repl_timer_func(int value) {
    editor_reset_input_effects();
    timer_func(value);
    return editor_take_input_effects();
}
