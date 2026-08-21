/*
 * glr_ctrl_cycle.c - scene / tutorial cycling policy.
 *
 * Carved out of glr_ctrl_router.c: F12 walks the example catalog and the user
 * scene slots, F11 walks the tutorial catalog, and the menu bar's Prev/Next
 * steppers drive the same entry points. None of it inspects an input event -
 * it is skip-on-load-failure policy, origin restore, the parked example place
 * (ReplSceneRuntimeState.example_place_idx) and the retained tutorial index -
 * so it lives beside the load actions it calls rather than in the router. The
 * router keeps only the two F11/F12 key handlers.
 *
 * glr_ctrl_cycle_peek() predicts where a step would land for the stepper
 * tooltip and must stay in this file: it mirrors the first candidate each
 * cycle below picks, and the two drift apart the moment they are separated.
 */
#include "app/glr_ctrl.h"

#include "config.h"                 /* MAX_USER_SCENES */
#include "editor/input.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "repl/example_loader.h"
#include "repl/host_effects.h"
#include "repl/examples.h"
#include "repl/scenes.h"
#include "repl/state_views.h"
#include "repl/tutorials.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include <stdio.h>


/* Try every example in one leg of the F12 cycle. A malformed runtime
 * catalog entry must not turn a single failed load into a permanent stop at
 * that index: the next example is still a valid destination for this key
 * press. The loader emits the detailed diagnostic for each failure. */
static int cycle_try_examples(int start, int direction, int count,
                              int stop_before, int *skipped_out) {
    int skipped = 0;

    for (int example_idx = start;
         example_idx >= 0 && example_idx < count &&
         example_idx != stop_before;
         example_idx += direction) {
        int edit_line = repl_load_example(example_idx);
        if (edit_line <= 0) {
            skipped++;
            continue;
        }
        editor_state_edit_line_set(edit_line);
        if (skipped_out)
            *skipped_out = skipped;
        return 1;
    }

    if (skipped_out)
        *skipped_out = skipped;
    return 0;
}

static int cycle_restore_origin(int active_scene, int active_example) {
    if (active_scene >= 0) {
        if (!repl_load_user_scene_idx(active_scene))
            return 0;
        editor_load_line_to_input(editor_state_edit_line());
        return 1;
    }

    if (active_example >= 0) {
        int edit_line = repl_load_example(active_example);
        if (edit_line <= 0)
            return 0;
        editor_state_edit_line_set(edit_line);
        return 1;
    }
    return 0;
}

static void cycle_report_skipped_examples(int skipped, int keep_error,
                                          int restored_origin) {
    if (skipped <= 0)
        return;

    char msg[REPL_DIAG_TEXT_MAX];
    const char *plural = skipped == 1 ? "" : "s";
    if (keep_error) {
        if (restored_origin) {
            snprintf(msg, sizeof(msg),
                     "F12 cycle failed: skipped %d unavailable example%s; "
                     "previous scene was restored; see stderr for details",
                     skipped, plural);
        } else {
            snprintf(msg, sizeof(msg),
                     "F12 cycle failed: skipped %d unavailable example%s; "
                     "no scene could be loaded; see stderr for details",
                     skipped, plural);
        }
        repl_set_status_error(msg);
        return;
    }

    int active_scene = repl_active_user_scene();
    if (active_scene >= 0) {
        snprintf(msg, sizeof(msg),
                 "Loaded scene: %s (skipped %d unavailable example%s; "
                 "see stderr for details)",
                 repl_user_scene_name(active_scene), skipped, plural);
    } else {
        int active_example = repl_state_scenes().active_example_idx;
        if (active_example >= 0 && active_example < repl_example_count()) {
            snprintf(msg, sizeof(msg),
                     "Example %d/%d: %s (F12 for next; skipped %d "
                     "unavailable example%s; see stderr for details)",
                     active_example + 1, repl_example_count(),
                     repl_example_name(active_example), skipped, plural);
        } else {
            snprintf(msg, sizeof(msg),
                     "Scene cycle skipped %d unavailable example%s; "
                     "see stderr for details", skipped, plural);
        }
    }
    repl_set_status(msg);
}

static void cycle_example_or_user_scene_dir(int direction) {
    glr_ctrl_reset_transients();
    editor_undo_note_wholesale_replacement();
    int count = repl_example_count();
    int active_scene = repl_active_user_scene();
    int active_example = repl_state_scenes().active_example_idx;
    int skipped = 0;

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
        if (count > 0) {
            /* A promotion parks the example the promoted document came from
             * (ReplSceneRuntimeState.example_place_idx) so this leg resumes
             * one step past it rather than restarting the catalog. The step
             * wraps, so a promotion off the last example still has somewhere
             * to go. Without a parked place the leg starts at the end the
             * direction implies, as before. */
            int place = repl_state_scenes().example_place_idx;
            int example_start = (place >= 0 && place < count)
                                    ? (place + direction + count) % count
                                    : ((direction > 0) ? 0 : count - 1);
            if (cycle_try_examples(example_start, direction, count, -1,
                                   &skipped)) {
                cycle_report_skipped_examples(skipped, 0, 0);
                return;
            }
            /* Resuming mid-catalog leaves the entries before the resume point
             * unvisited, so they get the same wrap leg an active example
             * gets - otherwise a parked place could shrink what this key
             * press can reach when loads fail. */
            if (example_start != ((direction > 0) ? 0 : count - 1)) {
                int wrap_skipped = 0;
                if (cycle_try_examples((direction > 0) ? 0 : count - 1,
                                       direction, count, example_start,
                                       &wrap_skipped)) {
                    skipped += wrap_skipped;
                    cycle_report_skipped_examples(skipped, 0, 0);
                    return;
                }
                skipped += wrap_skipped;
            }
        }
        if (skipped > 0) {
            int restored = cycle_restore_origin(active_scene, active_example);
            cycle_report_skipped_examples(skipped, 1, restored);
        }
        return;
    }

    int attempted_examples = 0;
    if (count > 0) {
        int next = active_example + direction;
        if (next >= 0 && next < count) {
            attempted_examples = 1;
            if (cycle_try_examples(next, direction, count, -1, &skipped)) {
                cycle_report_skipped_examples(skipped, 0, 0);
                return;
            }
        }
    }

    int start = (direction > 0) ? 0 : MAX_USER_SCENES - 1;
    int end = (direction > 0) ? MAX_USER_SCENES : -1;
    for (int scene_idx = start; scene_idx != end; scene_idx += direction) {
        if (repl_user_scene_slot_used(scene_idx)) {
            if (repl_load_user_scene_idx(scene_idx)) {
                editor_load_line_to_input(editor_state_edit_line());
                cycle_report_skipped_examples(skipped, 0, 0);
            }
            return;
        }
    }
    /* With no active example, the first leg can already cover the entire
     * catalog (notably from transient state with F12). Do not run that same
     * catalog a second time as a wrap leg. An active example still needs a
     * wrap leg because it must scan the entries before the origin. */
    if (count > 0 && (!attempted_examples || active_example >= 0)) {
        int wrap_skipped = 0;
        int stop_before = active_example >= 0 ? active_example : -1;
        if (cycle_try_examples((direction > 0) ? 0 : count - 1,
                               direction, count, stop_before,
                               &wrap_skipped)) {
            skipped += wrap_skipped;
            cycle_report_skipped_examples(skipped, 0, 0);
            return;
        }
        skipped += wrap_skipped;
    }

    /* Failed loads reset the document before reporting the error. Preserve a
     * known-good origin when every destination in this leg is unavailable. */
    if (skipped > 0) {
        int restored = cycle_restore_origin(active_scene, active_example);
        cycle_report_skipped_examples(skipped, 1, restored);
    }
}

int glr_ctrl_cycle_selects_tutorials(void) {
    return tutorial_active() || tutorial_state_view().tutorial_idx >= 0;
}

/* Where a step in `direction` would land, for the menu bar's stepper
 * tooltip. This mirrors the *first candidate* each cycle above picks; the
 * cycles then skip destinations that fail to load, so a broken catalog entry
 * can make the real landing differ from the prediction. That is the error
 * path, and it announces itself in the status bar - a tooltip that opened
 * the scene file to find out would cost a load per hovered frame. */
GlrCycleTarget glr_ctrl_cycle_peek(int direction) {
    GlrCycleTarget target;
    target.kind = GLR_CYCLE_TARGET_NONE;
    target.name = NULL;

    if (direction == 0)
        return target;

    /* Same rule the stepper click uses: an active or completed tutorial with
     * a retained index steps lessons. Explicit exit clears the index and
     * falls through to the scene cycle. */
    if (glr_ctrl_cycle_selects_tutorials()) {
        int count = repl_tutorial_count();
        int cur, idx;
        if (count <= 0)
            return target;
        cur = tutorial_state_view().tutorial_idx;
        idx = (cur >= 0) ? (cur + direction + count) % count
                         : ((direction > 0) ? 0 : count - 1);
        target.kind = GLR_CYCLE_TARGET_TUTORIAL;
        target.name = repl_tutorial_name(idx);
        return target;
    }

    {
        int count = repl_example_count();
        int active_scene = repl_active_user_scene();
        int active_example = repl_state_scenes().active_example_idx;
        int end = (direction > 0) ? MAX_USER_SCENES : -1;
        int start, i;

        if (active_scene >= 0) {
            /* In the user scenes: the next used slot, else back out to the
             * catalog at the parked example place. */
            for (i = active_scene + direction; i != end; i += direction) {
                if (repl_user_scene_slot_used(i)) {
                    target.kind = GLR_CYCLE_TARGET_SCENE;
                    target.name = repl_user_scene_name(i);
                    return target;
                }
            }
            if (count > 0) {
                int place = repl_state_scenes().example_place_idx;
                int idx = (place >= 0 && place < count)
                              ? (place + direction + count) % count
                              : ((direction > 0) ? 0 : count - 1);
                target.kind = GLR_CYCLE_TARGET_EXAMPLE;
                target.name = repl_example_name(idx);
            }
            return target;
        }

        /* In the catalog (or transient): the adjacent example while one is
         * left, then the user scenes, then the catalog wrap. */
        if (count > 0) {
            int next = active_example + direction;
            if (next >= 0 && next < count) {
                target.kind = GLR_CYCLE_TARGET_EXAMPLE;
                target.name = repl_example_name(next);
                return target;
            }
        }
        start = (direction > 0) ? 0 : MAX_USER_SCENES - 1;
        for (i = start; i != end; i += direction) {
            if (repl_user_scene_slot_used(i)) {
                target.kind = GLR_CYCLE_TARGET_SCENE;
                target.name = repl_user_scene_name(i);
                return target;
            }
        }
        if (count > 0) {
            target.kind = GLR_CYCLE_TARGET_EXAMPLE;
            target.name = repl_example_name((direction > 0) ? 0 : count - 1);
        }
    }
    return target;
}

/* Public scene-cycle entry points: the F12 / Shift+F12 key path and the
 * Scene-menu "Next" / "Previous" rows both funnel through these. */
void glr_ctrl_scene_cycle_next(void) {
    cycle_example_or_user_scene_dir(1);
}

void glr_ctrl_scene_cycle_prev(void) {
    cycle_example_or_user_scene_dir(-1);
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
