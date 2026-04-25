#ifndef REPL_EDITOR_H
#define REPL_EDITOR_H

/*
 * repl_editor.h - Keyboard/mouse input dispatch and GLUT callback wrappers.
 *
 * Central hub for all editor input (keyboard, mouse, window reshape). Routes
 * keystrokes to the appropriate handler (search overlay, rename mode, code panel,
 * camera controls), manages modifier keys (Ctrl, Shift, Alt), and provides a
 * thin wrapper layer around GLUT callbacks (keyboard_func, mouse_func,
 * special_func, reshape_func).
 *
 * Input priority queue:
 *   1. Search overlay (Ctrl+F active) — repl_search.c handles text input
 *   2. Rename mode (Scene → Rename) — repl_inline_rename.c handles rename input
 *   3. Camera controls — mouse drag/wheel zoom
 *   4. Code panel edit — normal typing and undo/redo (Ctrl+Z/Y)
 *   5. Global shortcuts — Ctrl+S (save), Ctrl+L (clear), F1-F11 (overlays), etc.
 *
 * Commit dispatch: The `;` key and Enter trigger the commit handler chain
 * (repl_commit.c), which parses input and commits structured blocks (float
 * decl, assignments, control flow, GL commands) in declarative order.
 *
 * GLUT boundary: GLUT API calls (glutGetModifiers, glutPostRedisplay, glutSetCursor)
 * are funneled through this module's static helpers, establishing a clear
 * review boundary for platform-specific code. No GLUT calls cross into other
 * modules.
 *
 * Undo/redo hook: repl_undo_push_snapshot() is called before mutations; this
 * is where example auto-promotion (repl_promote_example_if_needed) fires.
 *
 * Only GLUT callback entry points: keyboard_func, mouse_func, special_func,
 * reshape_func in sample.c call into repl_editor.c to forward input.
 */

/* Query currently active modifier keys (GLUT_ACTIVE_CTRL, GLUT_ACTIVE_SHIFT,
 * GLUT_ACTIVE_ALT). Returns a bitmask of active modifiers. Used by drag/click
 * handlers to select interaction mode (e.g., Ctrl = pan, Shift = orbit alt-style
 * for camera controls). */
int repl_editor_active_modifiers(void);

#endif
