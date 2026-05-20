/*
 * inline_file_prompt.h - Inline status-bar file picker for runtime scene load.
 *
 * Parallel to editor_inline_rename: a modal status-bar text input that
 * prompts for a single file path, then runs the existing
 * `repl_export_load_from_file` import path on commit (Enter). Triggered
 * by the File-menu "Load Scene…" row (GLR_FILE_ITEM_LOAD_SCENE) so the
 * user can swap in a saved scene at runtime instead of having to
 * relaunch with `./sample <file>`.
 *
 * Lifecycle: editor_inline_file_prompt_begin(default_name) seeds the
 * buffer with the suggested filename and enters modal mode. While
 * active, the controller's keyboard route gates keystrokes through
 * editor_inline_file_prompt_handle_key() / _handle_special() ahead of
 * normal editor dispatch. On Enter, the buffer is run through the
 * file-load pipeline (wholesale state replacement, so the undo ring is
 * cleared first). On Escape or a click outside the status bar the
 * buffer is discarded.
 *
 * Input filtering: filenames legitimately need '.' and '/' (for paths
 * like `workspace/scene.c`), so the rename module's '/' and ':'
 * blacklist does not apply. '\\' stays rejected — it's not a POSIX
 * path separator and is almost certainly user error or shell paste.
 * The filter additionally rejects quotes (", ', `) and shell
 * redirect characters (|, <, >) as a small defense-in-depth measure
 * against pasted shell snippets — the import path itself does not
 * invoke a shell, but a buffer containing those characters is almost
 * certainly user error.
 *
 * Status reporting: errors (file missing, parse failure, path is a
 * directory) set a status line and keep the prompt open so the user
 * can correct the path. Success sets a status line and closes the
 * prompt. Both terminal cases publish status via repl_set_status().
 */
#ifndef EDITOR_INLINE_FILE_PROMPT_H
#define EDITOR_INLINE_FILE_PROMPT_H

/* Query whether the file prompt is currently the input focus. Used by
 * the controller's key-priority routing and by the status-bar renderer
 * to display the prompt banner. */
int  editor_inline_file_prompt_active(void);

/* Open the file prompt and seed it with `default_name` (e.g.
 * DEFAULT_SCENE_FILE from config.h). Returns 1 on success; 0 only if the module is already
 * active (begin while open is a no-op rather than a reset, mirroring
 * the conservative rename idiom). */
int  editor_inline_file_prompt_begin(const char *default_name);

/* Modal keystroke handlers. Both consume every key while active and
 * return 1; the controller routes specials (arrows, F-keys) through
 * the _special variant ahead of normal dispatch so modal text entry
 * cannot leak actions into the editor. Returns 0 only when the prompt
 * is inactive. */
int  editor_inline_file_prompt_handle_key(unsigned char key);
int  editor_inline_file_prompt_handle_special(int key);

/* Discard pending input and close the prompt. Called by Escape, by a
 * click outside the modal status bar, and after a successful load. */
void editor_inline_file_prompt_cancel(void);

/* Current buffer contents (the typed-so-far path). Valid only while
 * editor_inline_file_prompt_active(); returns "" otherwise. The
 * renderer reads this directly so the prompt owns its own display
 * state instead of riding the shared transient status line. */
const char *editor_inline_file_prompt_buffer(void);

/* Most-recent failed-commit error string. Returns "" when there is no
 * error (including when the prompt is inactive). Cleared on any
 * keystroke that mutates the buffer (the user is editing, so the
 * stale error no longer applies) and on cancel. Read by the renderer
 * to display the error WITHIN the prompt strip since that strip
 * occludes the regular status bar. */
const char *editor_inline_file_prompt_error(void);

#endif /* EDITOR_INLINE_FILE_PROMPT_H */
