/*
 * repl_keys.h - ASCII and control key code constants.
 *
 * Symbolic names for keyboard shortcuts used throughout the REPL. GLUT delivers
 * Ctrl+<letter> as a single byte (Ctrl+A = 1, Ctrl+B = 2, ..., Ctrl+Z = 26),
 * backspace and delete as special codes, and Escape as 27. These #defines make
 * keyboard dispatch readable and provide a single place to check for conflicts
 * when adding new bindings.
 *
 * Shared by repl_editor.c (main input dispatcher), repl_search.c (search
 * overlay input), and other modules that need to recognize key codes. Comments
 * describe the action each key triggers (e.g., KEY_CTRL_S = save to output.c).
 *
 * Adding a binding: Define the key code, add the action to the appropriate
 * handler in repl_editor.c, and update the help overlay (ui_help_overlay.c)
 * and F-key table if applicable.
 *
 * Platform notes: Some platforms (Apple GLUT, terminal emulators) deliver
 * backspace or delete in unexpected codes. KEY_BACKSPACE (8) and KEY_DELETE (127)
 * are defined for portability. Ctrl+Dash (31) is an Apple GLUT quirk for
 * keyboard zoom control.
 */
#ifndef REPL_KEYS_H
#define REPL_KEYS_H

/* Navigation and editing */
#define KEY_CTRL_A    1    /* jump to line start */
#define KEY_CTRL_E    5    /* jump to line end */
#define KEY_BACKSPACE 8    /* backspace (some platforms deliver here) */
#define KEY_DELETE    127  /* delete key (macOS terminal) */

/* Editing operations */
#define KEY_CTRL_C    3    /* copy (selection or current line) */
#define KEY_CTRL_X    24   /* cut (selection or current line) */
#define KEY_CTRL_V    22   /* paste (from clipboard) */
#define KEY_CTRL_D    4    /* delete current line or exit insert mode */

/* Undo/redo */
#define KEY_CTRL_Z    26   /* undo (Shift+Ctrl+Z or Ctrl+Y for redo) */
#define KEY_CTRL_Y    25   /* redo */

/* Code panel and formatting */
#define KEY_CTRL_L    12   /* clear all commands (Ctrl+L) */
#define KEY_CTRL_BACKSLASH 28 /* reformat all commands (Ctrl+\, 0x1c) */
#define KEY_CTRL_B    2    /* cycle code panel layout modes */

/* File operations */
#define KEY_CTRL_S    19   /* save to default output (output.c) */
#define KEY_CTRL_Q    0x11 /* save and quit (Ctrl+Q, == 17) */

/* Search and navigation */
#define KEY_CTRL_F    6    /* open search overlay (Ctrl+F) */
#define KEY_CTRL_K    11   /* jump replay PC to cursor line (Ctrl+K) */

/* Visualization and overlays */
#define KEY_CTRL_R    18   /* toggle replay mode (Ctrl+R) */
#define KEY_CTRL_T    20   /* toggle animated 't' time variable (Shift: reset to 0) */
#define KEY_CTRL_U    21   /* toggle MSAA (Ctrl+U) */
#define KEY_CTRL_O    15   /* cycle grid major spacing (Ctrl+O) */
#define KEY_CTRL_W    23   /* cycle CPU profile panel (Ctrl+W) */
#define KEY_CTRL_DASH 31   /* decrement accum AA samples (Apple GLUT quirk, Ctrl+-) */

/* Diagnostics */
#define KEY_CTRL_P    16   /* dump editor state + flat commands to stdout (Ctrl+P) */

/* Other */
#define KEY_ESC       27   /* escape key (cancel overlays, etc.) */

#endif /* REPL_KEYS_H */
