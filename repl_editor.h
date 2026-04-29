#ifndef REPL_EDITOR_H
#define REPL_EDITOR_H

/*
 * repl_editor.h - Keyboard/mouse input dispatch and GLUT callback wrappers.
 *
 * Central hub for editor input. sample.c forwards keyboard, mouse, special,
 * and reshape callbacks here; this module routes keystrokes to the active
 * search overlay, rename mode, code panel, camera controls, replay, and global
 * shortcuts, and exposes modifier-key state for drag interactions.
 */

/* Query currently active modifier keys (GLUT_ACTIVE_CTRL, GLUT_ACTIVE_SHIFT,
 * GLUT_ACTIVE_ALT). Returns a bitmask of active modifiers. Used by drag/click
 * handlers to select interaction mode (e.g., Ctrl = pan, Shift = orbit alt-style
 * for camera controls). */
int repl_editor_active_modifiers(void);

#endif
