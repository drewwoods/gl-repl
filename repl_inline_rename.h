/*
 * repl_inline_rename.h - Inline scene-rename input overlay.
 *
 * Modal rename mode for editing the active user scene's name in place. When
 * activated via the Scene menu ("Rename active scene"), a temporary input buffer
 * overlays the status bar, capturing keyboard input. The user types the new name,
 * presses Enter to commit (via repl_user_scene_rename), or Escape to cancel.
 *
 * Lifecycle: repl_inline_rename_begin() initializes rename mode on a user scene
 * slot, copying the current name into the rename buffer. While active, the editor's
 * key dispatcher (in repl_editor.c) routes keystrokes to repl_inline_rename_handle_key()
 * and repl_inline_rename_handle_special() before normal code-panel input. These
 * handlers edit the rename buffer and detect Enter/Escape. On Enter, the new name
 * is committed via repl_user_scene_rename() (which validates, trims, and de-duplicates).
 * On Escape or explicit cancel, the rename buffer is discarded.
 *
 * Input filtering: Path-unsafe characters (/, \, :) and non-printables are filtered
 * at input time since scene names become filesystem slugs on workspace export. The
 * rename mode status-bar prompt ("New name: ") is rendered by the UI.
 *
 * State queries: repl_inline_rename_active() returns 1 if rename mode is active.
 * Used by the key dispatcher priority queue (repl_editor.c) to route input and by
 * the status-bar renderer (ui_panels.c) to show the rename prompt.
 */
#ifndef REPL_INLINE_RENAME_H
#define REPL_INLINE_RENAME_H

/* Query whether rename mode is active. Returns 1 if the user is editing a scene
 * name, 0 otherwise. Used by the key dispatcher (repl_editor.c) to prioritize
 * rename input over code-panel input, and by the status-bar renderer (ui_panels.c)
 * to display the rename prompt. */
int  repl_inline_rename_active(void);

/* Enter rename mode on a user scene slot. Initializes the rename buffer with the
 * slot's current name and returns 1 on success. Returns 0 if the slot is invalid
 * (not in use or out of bounds). Triggered by the Scene menu "Rename active scene"
 * item (via repl_action_menu_item_activate in repl_actions.c). */
int  repl_inline_rename_begin(int slot);

/* Handle an ASCII keystroke while rename mode is active. Returns 1 if the key was
 * consumed (whether it updated the buffer or committed/cancelled rename), 0 if the
 * key should be processed normally. Handles printable characters (added to buffer),
 * backspace (delete previous char), Enter (commit via repl_user_scene_rename),
 * and Escape (cancel). Filters path-unsafe characters (/, \, :) at input time.
 * Called by the editor's key dispatcher (repl_editor.c). */
int  repl_inline_rename_handle_key(unsigned char key);

/* Handle a special key (F-key, arrow, etc.) while rename mode is active. Returns 1
 * if consumed, 0 otherwise. Currently most special keys are not used in rename mode
 * (user types text, presses Enter or Escape). Called by the editor's key dispatcher. */
int  repl_inline_rename_handle_special(int key);

/* Cancel rename mode and discard pending name changes. Called by Escape key handler
 * or explicitly by the editor. Clears the rename buffer and deactivates rename mode. */
void repl_inline_rename_cancel(void);

#endif /* REPL_INLINE_RENAME_H */
