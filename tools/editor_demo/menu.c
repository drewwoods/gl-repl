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

#include "ui/gl_2d.h"

#include <gl_includes.h>

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

static void demo_action_load(void) {
    fprintf(stderr, "[demo-menu] Load: not implemented yet\n");
}

static void demo_action_save(void) {
    fprintf(stderr, "[demo-menu] Save: not implemented yet\n");
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
        /* Click elsewhere on the menu bar consumes the click but
         * leaves the menu state unchanged. */
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
