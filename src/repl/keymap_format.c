/*
 * keymap_format.c - User-facing labels for keymap.h binding pairs.
 *
 * The dispatch side stores every shortcut as a `(key, modifiers)` pair.
 * This file is the matching display side, so help/menu/tutorial text can
 * render those pairs without re-spelling "Ctrl+Shift+..." by hand.
 */
#include "keymap.h"
#include "gl_includes.h"
#include "keys.h"

#include <stdio.h>

static int key_implies_ctrl(int key) {
    if (key == KEY_CTRL_BACKSLASH || key == KEY_CTRL_DASH)
        return 1;
    if (key < 1 || key > 26)
        return 0;
    if (key == KEY_BACKSPACE || key == '\t' ||
        key == '\n' || key == '\r')
        return 0;
    return 1;
}

static void append_label_part(char *out, int out_size,
                              int *used, const char *part) {
    int avail;
    int written;

    if (!out || out_size <= 0 || !used || !part)
        return;
    if (*used < 0)
        *used = 0;
    if (*used >= out_size)
        return;

    avail = out_size - *used;
    written = snprintf(out + *used, (size_t)avail, "%s", part);
    if (written < 0)
        return;
    if (written >= avail)
        *used = out_size - 1;
    else
        *used += written;
}

static const char *base_key_name(int key, int is_special,
                                 char *buf, int buf_size) {
    if (is_special) {
        if (key >= GLUT_KEY_F1 && key <= GLUT_KEY_F12) {
            snprintf(buf, (size_t)buf_size, "F%d", key - GLUT_KEY_F1 + 1);
            return buf;
        }

        switch (key) {
        case GLUT_KEY_LEFT:      return "Left";
        case GLUT_KEY_RIGHT:     return "Right";
        case GLUT_KEY_UP:        return "Up";
        case GLUT_KEY_DOWN:      return "Down";
        case GLUT_KEY_HOME:      return "Home";
        case GLUT_KEY_END:       return "End";
        case GLUT_KEY_PAGE_UP:   return "Page Up";
        case GLUT_KEY_PAGE_DOWN: return "Page Down";
        case GLUT_KEY_INSERT:    return "Insert";
#ifndef USE_GLUT
        case GLUT_KEY_DELETE:    return "Delete";
#endif
        default:                 break;
        }
    }

    if (key == KEY_ESC)
        return "Escape";
    if (key == KEY_BACKSPACE)
        return "Backspace";
    if (key == KEY_DELETE)
        return "Delete";
    if (key == '\t')
        return "Tab";
    if (key == '\n' || key == '\r')
        return "Enter";
    if (key == ' ')
        return "Space";
    if (key == KEY_CTRL_BACKSLASH)
        return "\\";
    if (key == KEY_CTRL_DASH)
        return "-";
    if (key >= 1 && key <= 26) {
        snprintf(buf, (size_t)buf_size, "%c", 'A' + key - 1);
        return buf;
    }
    if (key_is_printable_ascii(key)) {
        snprintf(buf, (size_t)buf_size, "%c", key);
        return buf;
    }

    snprintf(buf, (size_t)buf_size, "Key%d", key);
    return buf;
}

const char *keymap_binding_to_string(char *out, int out_size,
                                     int binding_key, int binding_mods,
                                     int is_special) {
    char key_buf[16];
    const char *name;
    int used = 0;

    if (!out || out_size <= 0)
        return "";

    out[0] = '\0';

    if ((!is_special && key_implies_ctrl(binding_key)) ||
        (binding_mods & GLUT_ACTIVE_CTRL))
        append_label_part(out, out_size, &used, "Ctrl+");
    if (binding_mods & GLUT_ACTIVE_SHIFT)
        append_label_part(out, out_size, &used, "Shift+");
    if (binding_mods & GLUT_ACTIVE_ALT)
        append_label_part(out, out_size, &used, "Alt+");

    name = base_key_name(binding_key, is_special, key_buf, (int)sizeof(key_buf));
    append_label_part(out, out_size, &used, name);
    return out;
}
