/*
 * inline_file_prompt.c -- Inline status-bar file picker (item 19).
 *
 * Parallel to inline_rename.c: owns the modal text buffer, the
 * input filter, and the Enter/Escape commit path. The renderer is
 * shared with rename via the snapshot field in src/ui/snapshot.h;
 * the controller routes keystrokes here ahead of normal dispatch
 * while the prompt is active.
 *
 * The Enter path clears the undo ring (wholesale state replacement,
 * same invariant as Load Workspace / F12 cycle) and runs the existing
 * file-import path via repl_load_scene_runtime — i.e., the same
 * pipeline `./sample <file>` uses at startup.
 */
#include "inline_file_prompt.h"
#include "keys.h"
#include "undo.h"

#include "repl/core.h"

#include <sys/stat.h>

/* Capacity holds full workspace paths comfortably (PATH_MAX is 4096 on
 * Linux). The status-bar render truncates by pixel width so any
 * realistic typed path fits visually. */
#define FILE_PROMPT_BUF_MAX 256
#define FILE_PROMPT_ERR_MAX 192

static int  g_prompt_active = 0;
static char g_prompt_buf[FILE_PROMPT_BUF_MAX];
static int  g_prompt_len = 0;
/* Most-recent commit error. Lives next to the buffer so the renderer
 * can show it WITHIN the prompt strip (the strip occludes the regular
 * status bar, so repl_set_status alone would be invisible while the
 * prompt is up). Cleared on any keystroke that mutates the buffer. */
static char g_prompt_err[FILE_PROMPT_ERR_MAX];

int editor_inline_file_prompt_active(void) {
    return g_prompt_active;
}

const char *editor_inline_file_prompt_buffer(void) {
    return g_prompt_active ? g_prompt_buf : "";
}

const char *editor_inline_file_prompt_error(void) {
    return g_prompt_active ? g_prompt_err : "";
}

int editor_inline_file_prompt_begin(const char *default_name) {
    if (g_prompt_active) return 0;
    g_prompt_active = 1;
    if (default_name && default_name[0]) {
        snprintf(g_prompt_buf, sizeof(g_prompt_buf), "%s", default_name);
        g_prompt_len = (int)strlen(g_prompt_buf);
    } else {
        g_prompt_buf[0] = '\0';
        g_prompt_len = 0;
    }
    g_prompt_err[0] = '\0';
    return 1;
}

void editor_inline_file_prompt_cancel(void) {
    g_prompt_active = 0;
    g_prompt_buf[0] = '\0';
    g_prompt_len = 0;
    g_prompt_err[0] = '\0';
}

/* Filename input filter. Reject control chars (printable-ASCII gate),
 * quotes (shell paste guard), and the small set of shell metacharacters
 * a typed path should not contain. `.` and `/` are explicitly allowed
 * — unlike rename, where they would corrupt scene-name slugs, here
 * they are required to address subdirectories like `workspace/x.c`. */
static int prompt_char_ok(unsigned char c) {
    if (!key_is_printable_ascii(c)) return 0;
    if (c == '"' || c == '\'' || c == '`')      return 0;
    if (c == '|' || c == '<' || c == '>')       return 0;
    if (c == '\\')                              return 0;
    return 1;
}

/* Set the in-prompt error string and ALSO publish via repl_set_status
 * so that callers (e.g. tests, future renderers) that key off status
 * see the same message. */
static void prompt_set_err(const char *msg) {
    snprintf(g_prompt_err, sizeof(g_prompt_err), "%s", msg ? msg : "");
    if (msg && msg[0]) repl_set_status(msg);
}

/* Run the load. On success, set a confirmation status and close the
 * prompt; on failure, set the in-prompt error and keep the prompt
 * open so the user can correct the path without re-typing from
 * scratch. */
static void prompt_commit_path(void) {
    if (g_prompt_len == 0) {
        prompt_set_err("File path is empty");
        return;
    }

    /* stat() before touching state so a missing-file or directory
     * commit cannot wipe the user's current document. The generic
     * "cannot open" branch covers permission errors and other stat()
     * oddities. */
    struct stat st;
    if (stat(g_prompt_buf, &st) != 0) {
        char msg[FILE_PROMPT_ERR_MAX];
        snprintf(msg, sizeof(msg),
                 "File not found: %s", g_prompt_buf);
        prompt_set_err(msg);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        char msg[FILE_PROMPT_ERR_MAX];
        snprintf(msg, sizeof(msg),
                 "Path is a directory — use Load Workspace: %s",
                 g_prompt_buf);
        prompt_set_err(msg);
        return;
    }

    /* Load the file as a NEW user-scene slot. The previously-active
     * scene is preserved in its own slot (still accessible via scene
     * tabs / F12); the loaded file becomes the active scene. Drop the
     * undo ring — a post-load Ctrl+Z must not pull a snapshot from
     * the previous scene into the newly loaded one (same invariant
     * as Load Workspace / F12 in src/app/glr_actions.c). */
    editor_undo_clear();

    int new_slot = repl_load_scene_as_new_slot(g_prompt_buf);
    if (new_slot < 0) {
        char msg[FILE_PROMPT_ERR_MAX];
        snprintf(msg, sizeof(msg),
                 "Failed to load (parse error or slots full): %s",
                 g_prompt_buf);
        prompt_set_err(msg);
        return;
    }

    char msg[FILE_PROMPT_ERR_MAX];
    snprintf(msg, sizeof(msg),
             "Loaded scene from: %s (slot %d)",
             g_prompt_buf, new_slot);
    repl_set_status(msg);
    editor_inline_file_prompt_cancel();
}

int editor_inline_file_prompt_handle_key(unsigned char key) {
    if (!g_prompt_active) return 0;

    if (key == KEY_ESC) {
        editor_inline_file_prompt_cancel();
        return 1;
    }
    if (key == '\r' || key == '\n') {
        prompt_commit_path();
        return 1;
    }
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (g_prompt_len > 0)
            g_prompt_buf[--g_prompt_len] = '\0';
        g_prompt_err[0] = '\0';   /* user is editing; stale error gone */
        return 1;
    }
    if (prompt_char_ok(key) &&
        g_prompt_len < (int)sizeof(g_prompt_buf) - 1) {
        g_prompt_buf[g_prompt_len++] = (char)key;
        g_prompt_buf[g_prompt_len] = '\0';
        g_prompt_err[0] = '\0';   /* user is editing; stale error gone */
        return 1;
    }
    /* Swallow everything else so no stray character hits the editor. */
    return 1;
}

int editor_inline_file_prompt_handle_special(int key) {
    if (!g_prompt_active) return 0;
    (void)key;
    return 1;  /* swallow all specials while the prompt is up */
}
