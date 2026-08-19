/*
 * glr_tours.c - the built-in guided-tour catalog (see glr_tours.h).
 *
 * Tour content is file-backed like the example scenes: each tour is a
 * pointer-script file under tours/ (grammar: src/app/glr_pointer_script.h)
 * named by tours/catalog.ini (catalog-emscripten.ini for web builds), compiled
 * in by scripts/gen_tours.py as the generated include below. Shared scripts
 * can isolate web-only targets with the __EMSCRIPTEN__ conditional grammar.
 * Catalog section order is the Tours menu row
 * order. Author points as SYMBOLIC targets (menu:/item:/subenter:/sub:/
 * pin:/scene:, plus web-only shell:) resolved against the live layout when each event fires, so
 * tours play at any window size and follow catalog/label reordering; the
 * .pointer files also load directly via GLR_POINTER_SCRIPT= /
 * record-video.sh --script for previewing a tour before shipping it.
 */
#include "app/glr_tours.h"
#include "app/glr_pointer_script.h"
#include "repl/host_effects.h"
#include "subsystems/replay/replay.h"

#include <stdio.h>

typedef struct {
    const char        *name;
    const char        *file;   /* catalog-relative .pointer filename */
    const char *const *lines;
    int                line_count;
} TourEntry;

/* Defines g_tours[] plus one g_tour_<id>[] line array per catalog entry. */
#include "../../build/generated/glr_tours_data.inc"

int glr_tours_count(void) {
    return (int)(sizeof(g_tours) / sizeof(g_tours[0]));
}

const char *glr_tours_name(int idx) {
    if (idx < 0 || idx >= glr_tours_count()) return NULL;
    return g_tours[idx].name;
}

int glr_tours_start(int idx) {
    if (idx < 0 || idx >= glr_tours_count()) return 0;
    if (!glr_pointer_script_start_tour(g_tours[idx].name, g_tours[idx].file,
                                       g_tours[idx].lines,
                                       g_tours[idx].line_count)) {
        repl_set_status_error("Tour script failed to load");
        return 0;
    }
    /* A tour owns the same scene-execution lane as REPL replay. End an
     * existing replay before the next frame captures the tour baseline, so
     * the tour never inherits a narrowed replay render or restores one on
     * Back. Do this only after parsing/loading succeeds: a bad catalog entry
     * must leave the user's active replay untouched. */
    if (replay_active())
        replay_stop();
    char msg[96];
    snprintf(msg, sizeof(msg),
             "Tour: %s  (Space play/pause, arrows step, Esc exit)",
             g_tours[idx].name);
    repl_set_status(msg);
    return 1;
}
