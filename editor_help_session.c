/*
 * editor_help_session.c - Read-only editor session for the help overlay.
 */
#include "editor_help_session.h"

#define EDITOR_HELP_SESSION_INITIAL { .tab_idx = 0, .scroll = 0 }

static EditorHelpSession       g_help_session = EDITOR_HELP_SESSION_INITIAL;
static const EditorHelpSession g_help_session_defaults = EDITOR_HELP_SESSION_INITIAL;

void editor_help_session_capture(EditorHelpSession *snapshot) {
    if (!snapshot) return;
    *snapshot = g_help_session;
}

void editor_help_session_restore(const EditorHelpSession *snapshot) {
    if (!snapshot) return;
    g_help_session = *snapshot;
}

void editor_help_session_reset(void) {
    g_help_session = g_help_session_defaults;
}

EditorHelpSession editor_help_session_view(void) {
    return g_help_session;
}

EditorHelpSession *editor_help_session_mut(void) {
    return &g_help_session;
}

int editor_help_session_tab_idx(void) {
    return g_help_session.tab_idx;
}

int editor_help_session_scroll(void) {
    return g_help_session.scroll;
}

void editor_help_session_set_tab(int tab_idx) {
    g_help_session.tab_idx = tab_idx;
}

void editor_help_session_set_scroll(int scroll) {
    g_help_session.scroll = scroll;
}

void editor_help_session_scroll_by(int delta) {
    g_help_session.scroll += delta;
}
