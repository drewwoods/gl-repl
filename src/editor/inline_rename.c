/*
 * editor_inline_rename.c -- Input buffer for the inline scene-rename overlay.
 *
 * The rename "overlay" has no dedicated render pass: the buffered
 * text is surfaced through set_status() into the regular status
 * strip.  This module owns the buffer, the filter for filesystem-
 * unsafe characters, and the Enter/Escape commit path.  The menu
 * item that enters rename mode lives in src/app/glr_actions.c /
 * src/ui/app/menu_bar.c; the editor's key dispatcher forwards keystrokes
 * here ahead of its own routing while rename is active.
 */
#include "inline_rename.h"
#include "inline_file_prompt.h"   /* mutual-exclusion cancel */
#include "keys.h"

#include <stdio.h>
#include <string.h>
#include "repl/host_effects.h"
#include "repl/scenes.h"
#include "config.h"               /* REPL_DIAG_TEXT_MAX */

/* `g_rename_active` is the active predicate (matches the
 * inline_file_prompt.c shape - separate flag rather than overloading
 * `g_rename_slot` with a -1 sentinel). When inactive, `g_rename_slot`
 * is reset to -1 but only `g_rename_active` is the source of truth
 * for the predicate readers. */
static int  g_rename_active = 0;
static int  g_rename_slot   = -1;
static char g_rename_buf[USER_SCENE_NAME_MAX];
static int  g_rename_len    = 0;

int editor_inline_rename_active(void) {
    return g_rename_active;
}

const char *editor_inline_rename_buffer(void) {
    return g_rename_active ? g_rename_buf : "";
}

int editor_inline_rename_begin(int slot) {
    if (g_rename_active) return 0;
    if (slot < 0 || slot >= MAX_USER_SCENES) return 0;
    if (!repl_user_scene_slot_used(slot))    return 0;
    /* Mutual exclusion: at most one inline modal at a time. See the
     * symmetric note in editor_inline_file_prompt_begin. */
    editor_inline_file_prompt_cancel();
    g_rename_active = 1;
    g_rename_slot = slot;
    const char *cur = repl_user_scene_name(slot);
    snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", cur ? cur : "");
    g_rename_len = (int)strlen(g_rename_buf);
    return 1;
}

void editor_inline_rename_cancel(void) {
    g_rename_active = 0;
    g_rename_slot = -1;
    g_rename_buf[0] = '\0';
    g_rename_len = 0;
}

/* Filesystem-unsafe chars and non-printables are rejected at input time
 * because scene names become file slugs on workspace export. */
static int rename_char_ok(unsigned char c) {
    if (!key_is_printable_ascii(c)) return 0;
    if (c == '/' || c == '\\' || c == ':') return 0;
    return 1;
}

int editor_inline_rename_handle_key(unsigned char key) {
    if (!g_rename_active) return 0;

    if (key == KEY_ESC) {
        editor_inline_rename_cancel();
        return 1;
    }
    if (key == '\r' || key == '\n') {
        /* repl_user_scene_rename trims whitespace and rejects empty
         * names at the API boundary.  A 0 return means reject-and-retry
         * (keep the overlay open); non-zero means success and we close. */
        if (!repl_user_scene_rename(g_rename_slot, g_rename_buf)) {
            repl_set_status_error("Scene name cannot be empty");
            return 1;
        }
        char msg[REPL_DIAG_TEXT_MAX];
        snprintf(msg, sizeof(msg), "Renamed to: %s",
                 repl_user_scene_name(g_rename_slot));
        repl_set_status(msg);
        editor_inline_rename_cancel();
        return 1;
    }
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (g_rename_len > 0)
            g_rename_buf[--g_rename_len] = '\0';
        return 1;
    }
    if (rename_char_ok(key) && g_rename_len < (int)sizeof(g_rename_buf) - 1) {
        g_rename_buf[g_rename_len++] = (char)key;
        g_rename_buf[g_rename_len] = '\0';
        return 1;
    }
    /* Swallow everything else so no stray character hits the editor. */
    return 1;
}

int editor_inline_rename_handle_special(int key) {
    if (!g_rename_active) return 0;
    (void)key;
    return 1;  /* swallow all specials while renaming */
}
