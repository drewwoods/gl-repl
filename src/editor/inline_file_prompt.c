/*
 * inline_file_prompt.c -- Inline status-bar file picker (item 19).
 *
 * Parallel to inline_rename.c: owns the modal text buffer, the
 * input filter, and the Enter/Escape commit path. The renderer is
 * shared with rename via the snapshot field in src/ui/app/snapshot.h;
 * the controller routes keystrokes here ahead of normal dispatch
 * while the prompt is active.
 *
 * The Enter path clears the undo ring (wholesale state replacement,
 * same invariant as Load Workspace / F12 cycle) and runs the existing
 * file-import path via repl_load_scene_runtime — i.e., the same
 * pipeline `./sample <file>` uses at startup.
 */
#include "inline_file_prompt.h"
#include "inline_rename.h"        /* mutual-exclusion cancel */
#include "input.h"                /* editor_load_line_to_input */
#include "state.h"                /* editor_state_edit_line */
#include <keys.h>
#include "undo.h"

#include "repl/core.h"

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
    /* At most one inline modal is up at a time. If the user had a
     * scene-rename in progress and somehow triggered Load Scene (via
     * a future shortcut, etc.), cancel rename rather than have both
     * modals quietly active — the controller's keyboard route checks
     * rename first, so the file prompt would otherwise be invisible
     * to keystrokes while still drawing in the snapshot. */
    editor_inline_rename_cancel();
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
    if (msg && msg[0]) repl_set_status_error(msg);
}

/* Translate a load-error reason from repl_load_scene_as_new_slot
 * into a user-visible string. Keeps the editor module free of
 * filesystem probing (stat() lives in repl/scenes.c) and centralizes
 * the message wording so the renderer's truncation math stays
 * predictable. */
static void format_load_err(ReplSceneLoadStatus reason,
                            const char *path,
                            char *out, int out_sz) {
    switch (reason) {
    case REPL_SCENE_LOAD_ERR_EMPTY_PATH:
        snprintf(out, (size_t)out_sz, "File path is empty");
        break;
    case REPL_SCENE_LOAD_ERR_NOT_FOUND:
        snprintf(out, (size_t)out_sz, "File not found: %s", path);
        break;
    case REPL_SCENE_LOAD_ERR_IS_DIR:
        snprintf(out, (size_t)out_sz,
                 "Path is a directory — use Load Workspace: %s", path);
        break;
    case REPL_SCENE_LOAD_ERR_PARSE:
        snprintf(out, (size_t)out_sz,
                 "Failed to load (parse error): %s", path);
        break;
    case REPL_SCENE_LOAD_ERR_NO_SLOT:
        snprintf(out, (size_t)out_sz,
                 "All scene slots full — save workspace to free a slot");
        break;
    default:
        snprintf(out, (size_t)out_sz, "Failed to load: %s", path);
        break;
    }
}

/* Run the load. On success, set a confirmation status and close the
 * prompt; on failure, set the in-prompt error and keep the prompt
 * open so the user can correct the path without re-typing from
 * scratch. */
static void prompt_commit_path(void) {
    /* Note: do NOT call editor_undo_clear before the load. The REPL
     * function preserves the live document on any failure, but the
     * undo ring is the user's edit history — clearing it preemptively
     * would discard that history even when the load fails. Clear it
     * only after a confirmed successful slot allocation, mirroring
     * how Load Workspace / F12 sequence the clear right next to the
     * commit point. */
    ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
    int new_slot = repl_load_scene_as_new_slot(g_prompt_buf, &reason);
    if (new_slot < 0) {
        char msg[FILE_PROMPT_ERR_MAX];
        format_load_err(reason, g_prompt_buf, msg, (int)sizeof(msg));
        prompt_set_err(msg);
        return;
    }

    editor_undo_clear();
    /* Loaded scene's edit_line points to its end; refresh the input
     * buffer from the new line so any stale typed text the user had
     * is dropped — matches glr_scene_load_user_slot in glr_actions.c
     * which is the sibling 'switch active slot' path. */
    editor_load_line_to_input(editor_state_edit_line());

    /* Sized to fit the full path (FILE_PROMPT_BUF_MAX) plus the format
     * chrome. The literal below mirrors the format string verbatim with
     * INT_MIN substituted for %d, so sizeof() yields the worst-case
     * chrome width at compile time — edit the format, edit the literal. */
    char msg[FILE_PROMPT_BUF_MAX + sizeof("Loaded scene from:  (slot -2147483648)")];
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
