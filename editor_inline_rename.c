/*
 * editor_inline_rename.c -- Input buffer for the inline scene-rename overlay.
 *
 * The rename "overlay" has no dedicated render pass: the buffered
 * text is surfaced through set_status() into the regular status
 * strip.  This module owns the buffer, the filter for filesystem-
 * unsafe characters, and the Enter/Escape commit path.  The menu
 * item that enters rename mode lives in repl_actions.c /
 * ui_menu_bar.c; the editor's key dispatcher forwards keystrokes
 * here ahead of its own routing while rename is active.
 */
#include "repl_core.h"
#include "keys.h"
#include "editor_inline_rename.h"

static int  g_rename_slot = -1;
static char g_rename_buf[USER_SCENE_NAME_MAX];
static int  g_rename_len  = 0;

static void rename_refresh_status(void) {
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Rename scene (Enter to save, Esc to cancel): %s", g_rename_buf);
    set_status(msg);
}

int editor_inline_rename_active(void) {
    return g_rename_slot >= 0;
}

int editor_inline_rename_begin(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return 0;
    if (!repl_user_scene_slot_used(slot))    return 0;
    g_rename_slot = slot;
    const char *cur = repl_user_scene_name(slot);
    snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", cur ? cur : "");
    g_rename_len = (int)strlen(g_rename_buf);
    rename_refresh_status();
    return 1;
}

void editor_inline_rename_cancel(void) {
    g_rename_slot = -1;
    g_rename_buf[0] = '\0';
    g_rename_len = 0;
}

/* Filesystem-unsafe chars and non-printables are rejected at input time
 * because scene names become file slugs on workspace export. */
static int rename_char_ok(unsigned char c) {
    if (c < 32 || c >= 127) return 0;
    if (c == '/' || c == '\\' || c == ':') return 0;
    return 1;
}

int editor_inline_rename_handle_key(unsigned char key) {
    if (g_rename_slot < 0) return 0;

    if (key == KEY_ESC) {
        editor_inline_rename_cancel();
        return 1;
    }
    if (key == '\r' || key == '\n') {
        /* repl_user_scene_rename trims whitespace and rejects empty
         * names at the API boundary.  A 0 return means reject-and-retry
         * (keep the overlay open); non-zero means success and we close. */
        if (!repl_user_scene_rename(g_rename_slot, g_rename_buf)) {
            set_status("Scene name cannot be empty");
            return 1;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "Renamed to: %s",
                 repl_user_scene_name(g_rename_slot));
        set_status(msg);
        editor_inline_rename_cancel();
        return 1;
    }
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (g_rename_len > 0) {
            g_rename_buf[--g_rename_len] = '\0';
            rename_refresh_status();
        }
        return 1;
    }
    if (rename_char_ok(key) && g_rename_len < (int)sizeof(g_rename_buf) - 1) {
        g_rename_buf[g_rename_len++] = (char)key;
        g_rename_buf[g_rename_len] = '\0';
        rename_refresh_status();
        return 1;
    }
    /* Swallow everything else so no stray character hits the editor. */
    return 1;
}

int editor_inline_rename_handle_special(int key) {
    if (g_rename_slot < 0) return 0;
    (void)key;
    return 1;  /* swallow all specials while renaming */
}
