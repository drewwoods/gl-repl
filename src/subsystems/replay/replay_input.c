#include "subsystems/replay/replay_internal.h"
#include "keys.h"
#include <stdio.h>

static int replay_cancel_on_unrecognized(void) {
    repl_set_status("Replay: cancelled (key)");
    replay_stop();
    return 1;
}

int replay_handle_key(unsigned char key) {
    ReplayRuntimeState *state = replay_state_mut();
    if (!state->active) {
        /* GLR_REPLAY_JUMP binds with no modifier, so a bare-key compare
         * is exact here (matching the `case KM_KEY(GLR_REPLAY_JUMP)`
         * below) and keeps this peer subsystem free of the editor-layer
         * keymap_event_is matcher, which repl_demo does not link. */
        if (key == KM_KEY(GLR_REPLAY_JUMP)) {
            int target_line = repl_dispatch_edit_line_get();
            replay_start();
            if (replay_active()) {
                int landed = replay_seek_to_src_line(target_line);
                if (landed < 0) {
                    repl_set_status("Jump: no geometry at or after cursor");
                } else {
                    char msg[REPLAY_STATUS_MSG_LEN];
                    replay_state_mut()->state = REPLAY_PAUSED;
                    snprintf(msg, sizeof(msg), "Jump: paused at line %d", landed + 1);
                    repl_set_status(msg);
                }
            }
            return 1;
        }
        return 0;
    }

    switch (key) {
    case KM_KEY(GLR_REPLAY_JUMP): {
        int landed = replay_seek_to_src_line(repl_dispatch_edit_line_get());
        if (landed < 0) {
            repl_set_status("Jump: no geometry at or after cursor");
        } else {
            char msg[REPLAY_STATUS_MSG_LEN];
            state->state = REPLAY_PAUSED;
            snprintf(msg, sizeof(msg), "Jump: paused at line %d", landed + 1);
            repl_set_status(msg);
        }
        return 1;
    }
    case ' ': {
        if (state->state == REPLAY_PLAYING) {
            state->state = REPLAY_PAUSED;
            repl_set_status("Replay: paused");
        } else if (state->state == REPLAY_PAUSED) {
            state->state = REPLAY_PLAYING;
            repl_set_status("Replay: playing");
        } else if (state->state == REPLAY_DONE) {
            replay_restart_from_beginning();
            repl_set_status("Replay: restarted");
        }
        return 1;
    }
    case '+':
    case '=':
        replay_speed_adjust(REPLAY_SPEED_STEP_UP);
        return 1;
    case '-':
        replay_speed_adjust(REPLAY_SPEED_STEP_DOWN);
        return 1;
    case 'm':
    case 'M': {
        int was_playing = (state->state == REPLAY_PLAYING);
        state->mode = (state->mode == REPLAY_MODE_VERTEX)
                    ? REPLAY_MODE_POLYGON
                    : REPLAY_MODE_VERTEX;
        replay_seek(state->pc);
        if (was_playing && state->state != REPLAY_DONE)
            state->state = REPLAY_PLAYING;
        repl_set_status(state->mode == REPLAY_MODE_VERTEX
                 ? "Replay: vertex mode"
                 : "Replay: polygon mode");
        return 1;
    }
    case 'e':
    case 'E':
        /* Route Replay expand toggle through the config bridge */
        repl_cfg_set_int("replay_expand", !repl_cfg_get_int("replay_expand", 0));
        repl_set_status(repl_cfg_get_int("replay_expand", 0)
                 ? "Replay: expand args ON"
                 : "Replay: expand args OFF");
        return 1;
    case KEY_ESC:
        replay_stop();
        repl_set_status("Replay: off");
        return 1;
    default:
        /* Set status and stop replay on unrecognized key press */
        return replay_cancel_on_unrecognized();
    }
}

static int replay_modifier_special_key(int key) {
#ifdef USE_GLUT
    return 0;
#else
    return key == GLUT_KEY_NUM_LOCK ||
           key == GLUT_KEY_SHIFT_L || key == GLUT_KEY_SHIFT_R ||
           key == GLUT_KEY_CTRL_L || key == GLUT_KEY_CTRL_R ||
           key == GLUT_KEY_ALT_L || key == GLUT_KEY_ALT_R ||
           key == GLUT_KEY_SUPER_L || key == GLUT_KEY_SUPER_R;
#endif
}

int replay_handle_special(int key) {
    if (!replay_active())
        return 0;

    if (key == GLUT_KEY_LEFT) {
        replay_step_back();
        return 1;
    }
    if (key == GLUT_KEY_RIGHT) {
        replay_advance(repl_state_flat_program_view());
        return 1;
    }
    if (key == GLUT_KEY_UP) {
        replay_speed_adjust(REPLAY_SPEED_STEP_UP);
        return 1;
    }
    if (key == GLUT_KEY_DOWN) {
        replay_speed_adjust(REPLAY_SPEED_STEP_DOWN);
        return 1;
    }

    if (!replay_modifier_special_key(key)) {
        /* Set status and stop replay on unrecognized key press */
        return replay_cancel_on_unrecognized();
    }
    return 0;
}
