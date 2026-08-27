/* Browser-style source-location history. */
#include "navigation_history.h"

#include <string.h>

#define EDITOR_NAV_HISTORY_CAPACITY 64

static EditorNavigationLocation g_locations[EDITOR_NAV_HISTORY_CAPACITY];
static int g_location_count;
static int g_location_idx = -1;

static int locations_equal(EditorNavigationLocation a,
                           EditorNavigationLocation b) {
    return a.line_idx == b.line_idx && a.char_idx == b.char_idx;
}

void editor_navigation_history_clear(void) {
    g_location_count = 0;
    g_location_idx = -1;
}

static void append_location(EditorNavigationLocation location) {
    if (g_location_count == EDITOR_NAV_HISTORY_CAPACITY) {
        memmove(&g_locations[0], &g_locations[1],
                (EDITOR_NAV_HISTORY_CAPACITY - 1) * sizeof(g_locations[0]));
        g_location_count--;
        if (g_location_idx > 0)
            g_location_idx--;
    }
    g_locations[g_location_count++] = location;
    g_location_idx = g_location_count - 1;
}

void editor_navigation_history_record_jump(EditorNavigationLocation source,
                                           EditorNavigationLocation destination) {
    if (source.line_idx < 0 || source.char_idx < 0 ||
        destination.line_idx < 0 || destination.char_idx < 0 ||
        locations_equal(source, destination))
        return;

    if (g_location_idx + 1 < g_location_count)
        g_location_count = g_location_idx + 1;

    if (g_location_count == 0 ||
        !locations_equal(g_locations[g_location_count - 1], source))
        append_location(source);
    append_location(destination);
}

int editor_navigation_history_can_step(int direction) {
    if (direction < 0)
        return g_location_idx > 0;
    if (direction > 0)
        return g_location_idx >= 0 && g_location_idx + 1 < g_location_count;
    return 0;
}

int editor_navigation_history_step(int direction,
                                   EditorNavigationLocation *out_location) {
    if (!out_location || !editor_navigation_history_can_step(direction))
        return 0;
    g_location_idx += direction < 0 ? -1 : 1;
    *out_location = g_locations[g_location_idx];
    return 1;
}
