/*
 * tools/editor_demo/menu.c - Minimal demo-local menu bar.
 *
 * See menu.h for the public API. Implementation is intentionally
 * tiny: state lives in two static ints, geometry is hard-coded,
 * dropdowns render as a vertical text list with a background
 * rectangle. Each File menu item has a one-line callback (Load /
 * Save print a status message to stderr; Quit calls exit).
 */

#include "menu.h"

#include "input.h"          /* demo_input_commit_to_buffer */
#include "editor/state.h"   /* editor_buffer_* and editor_input_* */
#include "ui/core/gl_2d.h"

#include "gl_includes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Layout constants ------------------------------------------- */

#define FILE_LABEL_X       10
#define FILE_LABEL_W       40   /* hit-test width */
#define DROPDOWN_X         FILE_LABEL_X
#define DROPDOWN_W         140
#define DROPDOWN_ITEM_H    20
#define DROPDOWN_PAD_X     12

/* ---- Menu items ------------------------------------------------- */

typedef void (*DemoMenuAction)(void);

/* Demo's persistence path. Relative to the cwd the demo was
 * launched from — keeps the demo self-contained (no /tmp pollution,
 * no platform-specific user-data dirs). Round-trip workflow:
 * "Save" writes here; later runs "Load" the same file. The status
 * messages print the full path the user typed so it's obvious
 * where the file landed. */
#define DEMO_SAVE_PATH "editor_demo.txt"

static void demo_action_save(void) {
    /* Flush in-progress input first so the on-disk content matches
     * what the user sees (the input row is the live edit-line; it
     * only lands in the buffer on Enter / Up / Down navigation or
     * an explicit commit). */
    demo_input_commit_to_buffer();

    FILE *f = fopen(DEMO_SAVE_PATH, "w");
    if (!f) {
        fprintf(stderr, "[demo-menu] Save: could not open '%s' for write\n",
                DEMO_SAVE_PATH);
        return;
    }
    EditorBufferView buf = editor_buffer_view();
    int n = buf.line_count;
    for (int i = 0; i < n; i++) {
        const char *text = editor_buffer_view_line(buf, i);
        fprintf(f, "%s\n", text ? text : "");
    }
    fclose(f);
    fprintf(stderr, "[demo-menu] Saved %d line(s) to %s\n", n, DEMO_SAVE_PATH);
}

static void demo_action_load(void) {
    FILE *f = fopen(DEMO_SAVE_PATH, "r");
    if (!f) {
        fprintf(stderr, "[demo-menu] Load: could not open '%s'\n",
                DEMO_SAVE_PATH);
        return;
    }
    /* Static storage so the const char *[] passed to
     * editor_buffer_load_lines stays valid for the duration of the
     * call. MAX_COMMANDS is the buffer's row capacity; any
     * additional lines in the file are dropped with a warning. */
    static char lines[MAX_COMMANDS][MAX_LINE_LEN];
    const char *ptrs[MAX_COMMANDS];
    int n = 0;
    while (n < MAX_COMMANDS && fgets(lines[n], MAX_LINE_LEN, f) != NULL) {
        size_t len = strlen(lines[n]);
        while (len > 0 &&
               (lines[n][len - 1] == '\n' || lines[n][len - 1] == '\r'))
            lines[n][--len] = '\0';
        ptrs[n] = lines[n];
        n++;
    }
    int truncated = (n == MAX_COMMANDS) &&
                    (fgetc(f) != EOF);  /* probe one past the limit */
    fclose(f);

    (void)editor_buffer_load_lines(ptrs, n);
    /* Park the cursor on the virtual past-the-end row so the next
     * keystroke types into a fresh line below the loaded content.
     * Clear the input buffer so stale typed text from the previous
     * session doesn't survive the load. */
    editor_state_edit_line_set(n);
    editor_input_clear();
    editor_cursor_pos_set(0);

    if (truncated) {
        fprintf(stderr,
                "[demo-menu] Loaded %d line(s) from %s (truncated at MAX_COMMANDS=%d)\n",
                n, DEMO_SAVE_PATH, MAX_COMMANDS);
    } else {
        fprintf(stderr, "[demo-menu] Loaded %d line(s) from %s\n",
                n, DEMO_SAVE_PATH);
    }
}

static void demo_action_quit(void) {
    exit(0);
}

typedef struct {
    const char    *label;
    DemoMenuAction action;
} DemoMenuItem;

static const DemoMenuItem g_file_items[] = {
    { "Load", demo_action_load },
    { "Save", demo_action_save },
    { "Quit", demo_action_quit },
};

#define DEMO_FILE_ITEM_COUNT \
    (int)(sizeof(g_file_items) / sizeof(g_file_items[0]))

/* ---- State ------------------------------------------------------ */

static int g_menu_open = 0;

int demo_menu_is_open(void) { return g_menu_open; }
void demo_menu_close(void)   { g_menu_open = 0; }

/* ---- Rendering -------------------------------------------------- */

static void draw_rect(float x, float y, float w, float h,
                      float r, float g, float b) {
    glColor3f(r, g, b);
    glRectf(x, y, x + w, y + h);
}

void demo_menu_render(int vp_w, int vp_h) {
    /* Menu bar strip across the top. */
    float bar_y = (float)(vp_h - DEMO_MENU_BAR_H);
    draw_rect(0, bar_y, (float)vp_w, (float)DEMO_MENU_BAR_H,
              0.18f, 0.18f, 0.22f);

    /* "File" label. */
    glColor3f(0.90f, 0.92f, 0.95f);
    gl2d_draw_string((float)FILE_LABEL_X, bar_y + 7, "File",
                     GLUT_BITMAP_9_BY_15);

    if (!g_menu_open) return;

    /* Dropdown panel. */
    float drop_h = (float)(DROPDOWN_ITEM_H * DEMO_FILE_ITEM_COUNT);
    float drop_y_top = bar_y;                 /* top edge sits at bar bottom */
    float drop_y_bot = drop_y_top - drop_h;

    draw_rect((float)DROPDOWN_X, drop_y_bot, (float)DROPDOWN_W, drop_h,
              0.22f, 0.22f, 0.28f);

    /* Items: top item rendered first. y_top of item i is
     * drop_y_top - i*DROPDOWN_ITEM_H. */
    glColor3f(0.92f, 0.94f, 0.96f);
    for (int i = 0; i < DEMO_FILE_ITEM_COUNT; i++) {
        float item_y_top = drop_y_top - (float)(i * DROPDOWN_ITEM_H);
        gl2d_draw_string((float)(DROPDOWN_X + DROPDOWN_PAD_X),
                         item_y_top - 14,
                         g_file_items[i].label,
                         GLUT_BITMAP_9_BY_15);
    }
}

/* ---- Hit testing ------------------------------------------------ */

/* Return 0..N-1 if (mx, my) lands on a dropdown item, -1 otherwise.
 * Caller already verified the dropdown is open. */
static int dropdown_item_at(int mx, int my, int vp_h) {
    if (mx < DROPDOWN_X || mx >= DROPDOWN_X + DROPDOWN_W)
        return -1;
    float bar_y = (float)(vp_h - DEMO_MENU_BAR_H);
    /* Item i occupies y range [bar_y - (i+1)*H, bar_y - i*H). */
    for (int i = 0; i < DEMO_FILE_ITEM_COUNT; i++) {
        float item_top    = bar_y - (float)(i * DROPDOWN_ITEM_H);
        float item_bottom = item_top - (float)DROPDOWN_ITEM_H;
        if ((float)my < item_top && (float)my >= item_bottom)
            return i;
    }
    return -1;
}

int demo_menu_handle_click(int mx, int my, int vp_w, int vp_h) {
    (void)vp_w;
    float bar_y = (float)(vp_h - DEMO_MENU_BAR_H);

    /* Click on the "File" label toggles the dropdown. */
    if ((float)my >= bar_y && my <= vp_h) {
        if (mx >= FILE_LABEL_X && mx < FILE_LABEL_X + FILE_LABEL_W) {
            g_menu_open = !g_menu_open;
            return 1;
        }
        /* Click elsewhere on the menu bar is "outside the dropdown"
         * per menu.h — close any open dropdown. Consume the click
         * either way so it doesn't fall through to the code panel. */
        g_menu_open = 0;
        return 1;
    }

    /* Click on an open dropdown item invokes its action. */
    if (g_menu_open) {
        int idx = dropdown_item_at(mx, my, vp_h);
        if (idx >= 0) {
            g_menu_open = 0;
            g_file_items[idx].action();
            return 1;
        }
        /* Click anywhere outside the dropdown closes it; consume
         * so the underlying code panel doesn't also react. */
        g_menu_open = 0;
        return 1;
    }

    return 0;
}
