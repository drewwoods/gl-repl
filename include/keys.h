/*
 * keys.h - ASCII and control key code constants.
 *
 * Pure portability header. GLUT delivers Ctrl+<letter> as a single byte
 * (Ctrl+A = 1, Ctrl+B = 2, ..., Ctrl+Z = 26), backspace and delete as
 * special codes, and Escape as 27. These #defines are the physical
 * byte/key layer; the action->key bindings built on top of them (which
 * action each key triggers, and where to reassign one) live in keymap.h
 * at the repo root.
 *
 * Lives under include/ alongside the other zero-dependency portability
 * headers (c_compat.h, gl_includes.h) so any layer (editor input, ui_*,
 * controller) can include it via `#include <keys.h>` without crossing
 * boundaries. Comments below describe what each key triggers in the
 * current app (e.g., KEY_CTRL_S = save to output.c) but the constants
 * themselves are app-neutral.
 *
 * Platform notes: Some platforms (Apple GLUT, terminal emulators) deliver
 * backspace or delete in unexpected codes. KEY_BACKSPACE (8) and KEY_DELETE (127)
 * are defined for portability. Ctrl+Dash (31) is an Apple GLUT quirk for
 * keyboard zoom control.
 */
#ifndef KEYS_H
#define KEYS_H

/* Navigation and editing */
#define KEY_CTRL_A    1    /* jump to line start; Ctrl+Shift+A: toggle audio */
#define KEY_CTRL_E    5    /* jump to line end */
#define KEY_CTRL_H    8    /* Ctrl+H aliases Backspace; the Shift chord may be
                              claimed by a pre-editor shortcut route */
#define KEY_CTRL_I    9    /* Ctrl+I aliases Tab; Ctrl+Shift+I: Look down Z */
#define KEY_CTRL_J    10   /* Ctrl+J aliases LF / Enter; Ctrl+Shift+J: Call depth */
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
#define KEY_CTRL_F    6    /* open search overlay (Ctrl+F; Shift: toggle
                              code focus - hidden session toggle, see
                              the g_cfg_items[] note in glr_actions.c) */
#define KEY_CTRL_K    11   /* jump replay PC to cursor line (Ctrl+K) */

/* Visualization and overlays */
#define KEY_CTRL_G    7    /* toggle wireframe (Ctrl+G; byte 7 = BEL, no
                              whitespace collision, the one free plain Ctrl
                              slot) */
#define KEY_CTRL_R    18   /* toggle replay mode (Ctrl+R) */
#define KEY_CTRL_T    20   /* toggle animated 't' time variable (Shift: reset to 0) */
#define KEY_CTRL_U    21   /* toggle MSAA (Ctrl+U) */
#define KEY_CTRL_O    15   /* cycle grid major spacing (Ctrl+O) */
#define KEY_CTRL_N    14   /* plain Ctrl+N is unbound; Ctrl+Shift+N toggles
                              normal vectors (GLR_NORMAL_VECTORS). The
                              Post FX Scope cycle lives on F10 / Shift+F10 */
#define KEY_CTRL_W    23   /* cycle CPU profile panel (Ctrl+W) */
#define KEY_CTRL_DASH 31   /* decrement accum AA samples (Apple GLUT quirk, Ctrl+-) */

/* Diagnostics */
#define KEY_CTRL_P    16   /* toggle polygon highlight (Ctrl+P);
                              Ctrl+Shift+P: toggle vertex points */

/* Other */
#define KEY_ESC       27   /* escape key (cancel overlays, etc.) */

/* Printable 7-bit ASCII: space (32) through '~' (126), excluding DEL
 * (127). Single source of truth for the "is this a typeable character"
 * test in editor input / search / inline-rename so the raw 32/127
 * bounds don't drift apart. App-neutral, like the rest of this header. */
static inline int key_is_printable_ascii(int key) {
    return key >= 32 && key < 127;
}

#endif /* KEYS_H */
