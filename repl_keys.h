/*
 * repl_keys.h - ASCII/control key codes used by keyboard_func callbacks.
 *
 * GLUT delivers Ctrl+<letter> as a single byte (Ctrl+A = 1, ... Ctrl+Z = 26).
 * These names make the keyboard dispatch readable and give a single place to
 * check for conflicts when adding a new binding. Shared by repl_editor.c
 * (main dispatch) and repl_search.c (search-overlay dispatch).
 */
#ifndef REPL_KEYS_H
#define REPL_KEYS_H

#define KEY_CTRL_A    1    /* jump to line start */
#define KEY_CTRL_B    2    /* toggle code panel layout */
#define KEY_CTRL_C    3    /* copy */
#define KEY_CTRL_D    4    /* delete current line / exit insert */
#define KEY_CTRL_E    5    /* jump to line end */
#define KEY_CTRL_F    6    /* open search */
#define KEY_BACKSPACE 8    /* some platforms deliver backspace here */
#define KEY_CTRL_K    11   /* jump replay to cursor line */
#define KEY_CTRL_L    12   /* clear all commands */
#define KEY_CTRL_O    15   /* cycle grid major spacing */
#define KEY_CTRL_P    16   /* dump editor + flat cmds to stdout */
#define KEY_CTRL_Q    0x11 /* save & quit (== 17) */
#define KEY_CTRL_R    18   /* toggle replay */
#define KEY_CTRL_S    19   /* save to default output */
#define KEY_CTRL_T    20   /* toggle animated t (Shift: reset to 0) */
#define KEY_CTRL_U    21   /* toggle MSAA */
#define KEY_CTRL_V    22   /* paste */
#define KEY_CTRL_W    23   /* cycle CPU profile panel */
#define KEY_CTRL_X    24   /* cut */
#define KEY_CTRL_Y    25   /* redo */
#define KEY_CTRL_Z    26   /* undo (Shift: redo) */
#define KEY_ESC       27
#define KEY_CTRL_BACKSLASH 28 /* reformat all commands (0x1c) */
#define KEY_CTRL_DASH 31   /* decrement accum samples (Apple GLUT quirk) */
#define KEY_DELETE    127  /* macOS terminal backspace */

#endif /* REPL_KEYS_H */
