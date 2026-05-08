/*
 * repl_menu_bar.c -- Code-panel menu bar, dropdowns, and search slot.
 */
#include "glr_actions.h"
#include "repl_core.h"
#include "repl_config.h"
#include "keys.h"
#include "repl_state_views.h"
#include "replay.h"   /* ReplayState (PLAYING / PAUSED / DONE) enum values */
#include "state.h"
#include "menu_bar.h"
#include "metrics.h"
#include "layout.h"
#include "./include/gl_2d.h"

/* Menu bar - styled after Header Wireframes v2.
 * Left: top-level menus (File, Scene, Config).
 * Right: pinned buttons (Search, Replay) - retained in flat form until the
 * right-side redesign lands. */

enum {
    MENU_FILE = GLR_MENU_FILE,
    MENU_SCENE = GLR_MENU_SCENE,
    MENU_CONFIG = GLR_MENU_CONFIG,
    NUM_MENUS = GLR_MENU_COUNT
};

static const char *g_menu_labels[NUM_MENUS] = {
    "File", "Scene", "Config"
};

/* Right-to-left the pins render from highest index first, so PIN_REPLAY sits
 * at the far right matching the design. PIN_SEARCH fills the gap between the
 * last menu on the left and PIN_REPLAY. */
enum { PIN_SEARCH = 0, PIN_REPLAY, NUM_PIN_BTNS };
static const char *g_pin_btn_labels[NUM_PIN_BTNS] = {
    "search...", "Replay"
};

#define PIN_SEARCH_MIN_W 140

static int g_open_menu = -1;      /* index into g_menu_labels; -1 = none */

static int g_menu_item_hover = -1;
static float g_menu_open_time = -1.0f;   /* anim_time when current menu opened */
static float g_search_open_time = -1.0f; /* anim_time when search opened */
#define UI_FADE_DURATION 0.18f

static int ui_menu_bar_panel_visible(void);

int ui_menu_bar_menu_dropdown_is_open(void) {
    return g_open_menu >= 0 &&
           ui_menu_bar_panel_visible();
}
int ui_menu_bar_example_dropdown_is_open(void) { return ui_menu_bar_menu_dropdown_is_open(); }


enum {
    FILE_ITEM_EXPORT = REPL_FILE_ITEM_EXPORT,
    FILE_ITEM_IMPORT = REPL_FILE_ITEM_IMPORT,
    FILE_ITEM_SAVE_WORKSPACE = REPL_FILE_ITEM_SAVE_WORKSPACE,
    FILE_ITEM_LOAD_WORKSPACE = REPL_FILE_ITEM_LOAD_WORKSPACE,
    FILE_ITEM_COUNT = REPL_FILE_ITEM_COUNT
};

/* SCENE menu layout:
 *   [0]                      "### EXAMPLES"
 *   [1..e]                   example names  (e = repl_example_count())
 *   [e + SCENE_OFF_DIVIDER]  "---"
 *   [e + SCENE_OFF_HDR]      "### SCENE"
 *   [e + SCENE_OFF_NEW]      "New empty scene"
 *   [e + SCENE_OFF_SAVE]     "Save to output.c"
 *   [e + SCENE_OFF_RENAME]   "Rename active scene"
 *   [e + SCENE_OFF_SCENES ..
 *      e + SCENE_OFF_SCENES + n - 1]  user scene names
 *                                     (n = repl_user_scene_count())
 */
enum {
    SCENE_OFF_DIVIDER = GLR_SCENE_OFF_DIVIDER,
    SCENE_OFF_HDR     = GLR_SCENE_OFF_HDR,
    SCENE_OFF_NEW     = GLR_SCENE_OFF_NEW,
    SCENE_OFF_SAVE    = GLR_SCENE_OFF_SAVE,
    SCENE_OFF_RENAME  = GLR_SCENE_OFF_RENAME,
    SCENE_OFF_SCENES  = GLR_SCENE_OFF_SCENES,
    SCENE_FIXED_COUNT = REPL_SCENE_FIXED_COUNT
};

static int menu_item_count(int menu_id) {
    switch (menu_id) {
    case MENU_FILE:   return FILE_ITEM_COUNT;
    case MENU_SCENE:  return repl_example_count() + SCENE_FIXED_COUNT
                             + repl_user_scene_count();
    case MENU_CONFIG: {
        int count = 0;
        repl_config_items(&count);
        return count;
    }
    }
    return 0;
}

static const char *menu_item_label(int menu_id, int i) {
    if (menu_id == MENU_FILE) {
        if (i == FILE_ITEM_EXPORT) return "Export";
        if (i == FILE_ITEM_IMPORT) return "Import";
        if (i == FILE_ITEM_SAVE_WORKSPACE) return "Save Workspace";
        if (i == FILE_ITEM_LOAD_WORKSPACE) return "Load Workspace";
        return NULL;
    }
    if (menu_id == MENU_SCENE) {
        int e = repl_example_count();
        if (i == 0)                                            return "### EXAMPLES";
        if (i >= 1 && i <= e)                                 return repl_example_name(i - 1);
        if (i == e + SCENE_OFF_DIVIDER)                       return "---";
        if (i == e + SCENE_OFF_HDR)                           return "### SCENE";
        if (i == e + SCENE_OFF_NEW)                           return "New empty scene";
        if (i == e + SCENE_OFF_SAVE)                          return "Save to output.c";
        if (i == e + SCENE_OFF_RENAME)                        return "Rename active scene";
        int scene_n = i - (e + SCENE_OFF_SCENES);
        if (scene_n >= 0 && scene_n < repl_user_scene_count()) {
            int slot = repl_scene_menu_slot_for_dense_index(scene_n);
            return (slot >= 0) ? repl_user_scene_name(slot) : NULL;
        }
        return NULL;
    }
    if (menu_id == MENU_CONFIG) {
        const ReplConfigItem *item = repl_config_item_at(i);
        if (item) return item->label;
        return NULL;
    }
    return NULL;
}

static const char *menu_item_shortcut(int menu_id, int i) {
    if (menu_id == MENU_FILE && i == FILE_ITEM_EXPORT) return "Ctrl+S";
    if (menu_id == MENU_SCENE) {
        int e = repl_example_count();
        if (i == e + SCENE_OFF_SAVE) return "Ctrl+S";
        return NULL;
    }
    if (menu_id == MENU_CONFIG) {
        const ReplConfigItem *item = repl_config_item_at(i);
        if (!item || item->section_header || item->key == REPL_CONFIG_NONE)
            return NULL;
        static char buf[16];
        if (item->key_code == 0) return NULL;
        if (item->is_special) {
            snprintf(buf, sizeof(buf), "F%d", item->key_code - GLUT_KEY_F1 + 1);
            return buf;
        } else {
            if (item->key_code > 0 && item->key_code <= 26) {
                snprintf(buf, sizeof(buf), "Ctrl+%c", item->key_code - 1 + 'a');
                return buf;
            } else if (item->key_code == KEY_CTRL_BACKSLASH) {
                return "Ctrl+\\";
            } else {
                snprintf(buf, sizeof(buf), "%c", item->key_code);
                return buf;
            }
        }
    }
    (void)i;
    return NULL;
}

static int menu_max_shortcut_px(int menu_id) {
    int n = menu_item_count(menu_id);
    int max_sc = 0;
    for (int i = 0; i < n; i++) {
        const char *sc = menu_item_shortcut(menu_id, i);
        if (sc) {
            int w = (int)strlen(sc) * FONT_SMALL_W;
            if (w > max_sc) max_sc = w;
        }
    }
    return max_sc;
}

#define CFG_STATE_MAX_CHARS 20

/* Fills `out` with the current state label, truncated to CFG_STATE_MAX_CHARS
 * (with a trailing ellipsis). Returns `out`. */
static const char *cfg_state_str(int i, char *out, int out_size) {
    const char *s = "";
    const ReplConfigItem *item = repl_config_item_at(i);
    if (item && !item->section_header && item->key != REPL_CONFIG_NONE) {
        s = repl_config_state_name(item->key, repl_config_get(item->key));
        if (!s)
            s = "";
    }
    int n = (int)strlen(s);
    int cap = out_size - 1;
    if (cap > CFG_STATE_MAX_CHARS) cap = CFG_STATE_MAX_CHARS;
    if (n <= cap) {
        memcpy(out, s, (size_t)n);
        out[n] = '\0';
    } else {
        int keep = cap - 3;
        if (keep < 0) keep = 0;
        memcpy(out, s, (size_t)keep);
        int dots = cap - keep;
        for (int k = 0; k < dots; k++) out[keep + k] = '.';
        out[cap] = '\0';
    }
    return out;
}

/* Widest possible state label across all items & all their possible values,
 * clamped to CFG_STATE_MAX_CHARS. Used to keep the menu width stable as
 * values cycle. */
static int cfg_max_state_chars(void) {
    int max_chars = 3;  /* "OFF" */
    int count = 0;
    const ReplConfigItem *items = repl_config_items(&count);
    for (int i = 0; i < count; i++) {
        const ReplConfigItem *item = &items[i];
        if (item->section_header || item->key == REPL_CONFIG_NONE)
            continue;
        const char **names = item->state_names;
        if (!names) continue;
        for (int k = 0; k < item->state_count; k++) {
            int n = (int)strlen(names[k]);
            if (n > max_chars) max_chars = n;
        }
    }
    if (max_chars > CFG_STATE_MAX_CHARS) max_chars = CFG_STATE_MAX_CHARS;
    return max_chars;
}

static void menubar_rects(int menu_x[NUM_MENUS], int menu_w[NUM_MENUS],
                          int pin_x[NUM_PIN_BTNS], int pin_w[NUM_PIN_BTNS],
                          int *row_y, int *row_h) {
    int cp_x, cp_y, cp_w, cp_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    int panel_top = cp_y + cp_h;
    int by = panel_top - CODE_MARGIN_Y - LINE_H;
    int bh = LINE_H;
    if (row_y) *row_y = by;
    if (row_h) *row_h = bh;

    int x = cp_x + CODE_MARGIN_X;
    for (int i = 0; i < NUM_MENUS; i++) {
        int label_w = (int)strlen(g_menu_labels[i]) * FONT_SMALL_W;
        menu_w[i] = label_w + 18;  /* ~9px padding each side */
        menu_x[i] = x;
        x += menu_w[i];
    }

    int right_edge = cp_x + cp_w - CODE_MARGIN_X;

    /* PIN_REPLAY - width reserves room for the widest state label plus a
     * 12px state icon (triangle / pause-bars) and padding. */
    int replay_label_w = (int)strlen("Replaying") * FONT_SMALL_W;
    pin_w[PIN_REPLAY] = replay_label_w + 12 /* icon */ + 22 /* pads */;
    pin_x[PIN_REPLAY] = right_edge - pin_w[PIN_REPLAY];

    /* PIN_SEARCH - fills the gap between the last menu and PIN_REPLAY. */
    int menus_right = menu_x[NUM_MENUS - 1] + menu_w[NUM_MENUS - 1];
    int search_w = pin_x[PIN_REPLAY] - menus_right;
    if (search_w < PIN_SEARCH_MIN_W) search_w = PIN_SEARCH_MIN_W;
    pin_w[PIN_SEARCH] = search_w;
    pin_x[PIN_SEARCH] = pin_x[PIN_REPLAY] - search_w;
}

int ui_menu_bar_menu_hit(int gx, int gy) {
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    int ry = ui_state_viewport().window_h - gy;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
    if (ry < by || ry >= by + bh) return -1;
    for (int i = 0; i < NUM_MENUS; i++)
        if (gx >= menu_x[i] && gx < menu_x[i] + menu_w[i]) return i;
    return -1;
}

int ui_menu_bar_pin_hit(int gx, int gy) {
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    int ry = ui_state_viewport().window_h - gy;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
    if (ry < by || ry >= by + bh) return -1;
    for (int i = 0; i < NUM_PIN_BTNS; i++)
        if (gx >= pin_x[i] && gx < pin_x[i] + pin_w[i]) return i;
    return -1;
}

static int menu_dropdown_rect(int *dx, int *dy, int *dw, int *dh) {
    if (g_open_menu < 0) return 0;
    if (!ui_menu_bar_panel_visible()) return 0;
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
    int n = menu_item_count(g_open_menu);

    int max_lbl = 0, max_state = 0, max_sc = 0;
    for (int i = 0; i < n; i++) {
        const char *lbl = menu_item_label(g_open_menu, i);
        const char *sc  = menu_item_shortcut(g_open_menu, i);
        int lw = (int)(lbl ? strlen(lbl) : 0) * FONT_SMALL_W;
        if (lw > max_lbl) max_lbl = lw;
        if (sc) {
            int cw = (int)strlen(sc) * FONT_SMALL_W;
            if (cw > max_sc) max_sc = cw;
        }
    }
    if (g_open_menu == MENU_CONFIG)
        max_state = cfg_max_state_chars() * FONT_SMALL_W;
    int max_w = max_lbl;
    if (max_state > 0) max_w += max_state + 20;
    if (max_sc > 0)    max_w += max_sc + 16;
    if (max_w < 80) max_w = 80;
    int width  = max_w + 28;
    int rows   = (n > 0) ? n : 1;  /* reserve one row for "(empty)" */
    int height = rows * LINE_H + 8;

    if (dx) *dx = menu_x[g_open_menu];
    if (dy) *dy = by - height;
    if (dw) *dw = width;
    if (dh) *dh = height;
    return 1;
}

int ui_menu_bar_dropdown_item_hit(int gx, int gy) {
    if (g_open_menu < 0) return -1;
    int n = menu_item_count(g_open_menu);
    if (n == 0) return -1;
    int dx, dy, dw, dh;
    if (!menu_dropdown_rect(&dx, &dy, &dw, &dh)) return -1;
    int ry = ui_state_viewport().window_h - gy;
    if (gx < dx || gx >= dx + dw || ry < dy || ry >= dy + dh) return -1;
    int row = (dy + dh - 4 - ry) / LINE_H;
    if (row < 0 || row >= n) return -1;
    const char *lbl = menu_item_label(g_open_menu, row);
    if (!lbl || strncmp(lbl, "###", 3) == 0 || strcmp(lbl, "---") == 0) return -1;
    return row;
}

UiHit ui_menu_bar_hit_test(int mx, int my) {
    UiHit h = ui_hit_none();
    int win_h = ui_state_viewport().window_h;
    if (win_h <= 0) return h;
    int gl_y = win_h - my;

    /* Open dropdown row beats every other menu region. The cmd_idx
     * carries the menu_id the row belongs to so the controller can
     * activate the action without reading ui_menu_bar state. */
    if (g_open_menu >= 0) {
        int row = ui_menu_bar_dropdown_item_hit(mx, my);
        if (row >= 0) {
            h.kind = UI_HIT_MENU_ITEM;
            h.cmd_idx = g_open_menu;
            h.item_idx = row;
            h.local_x = (float)mx;
            h.local_y = (float)gl_y;
            return h;
        }
    }

    /* Pin button (Search / Replay). Pins are rendered after the menu
     * labels and overlap the label region in narrow code panels — the
     * visible pin must beat the underlying label, so check pins
     * before the top-level menu hit. Matches the legacy press-handler
     * order (pin_hit before menu_hit). */
    int pin = ui_menu_bar_pin_hit(mx, my);
    if (pin >= 0) {
        h.kind = UI_HIT_PIN_BUTTON;
        h.item_idx = pin;
        h.local_x = (float)mx;
        h.local_y = (float)gl_y;
        return h;
    }

    /* Top-level menu button (File / Scene / Config). cmd_idx carries
     * menu_id; the controller decides whether to open / switch /
     * dismiss based on the open-menu state. */
    int menu = ui_menu_bar_menu_hit(mx, my);
    if (menu >= 0) {
        h.kind = UI_HIT_MENU_BUTTON;
        h.cmd_idx = menu;
        h.local_x = (float)mx;
        h.local_y = (float)gl_y;
        return h;
    }

    return h;
}


static int ui_menu_bar_panel_visible(void) {
    int cp_w, cp_h;
    ui_layout_code_panel_rect(NULL, NULL, &cp_w, &cp_h);
    return cp_w > 0 && cp_h > 0;
}

int ui_menu_bar_open_menu_id(void) {
    return g_open_menu;
}

void ui_menu_bar_close(void) {
    g_open_menu = -1;
    g_menu_item_hover = -1;
}

void ui_menu_bar_set_open_menu(int menu_id, float now) {
    if (menu_id < 0 || menu_id >= NUM_MENUS) {
        ui_menu_bar_close();
        return;
    }
    g_open_menu = menu_id;
    g_menu_open_time = now;
    g_menu_item_hover = -1;
}

void ui_menu_bar_open_config(float now) {
    if (g_open_menu == MENU_CONFIG) {
        ui_menu_bar_close();
        return;
    }
    ui_menu_bar_set_open_menu(MENU_CONFIG, now);
}

int ui_menu_bar_handle_config_right_press(int mx, int my) {
    if (g_open_menu != MENU_CONFIG) return 0;
    int item = ui_menu_bar_dropdown_item_hit(mx, my);
    if (item < 0) return 0;
    glr_cfg_cycle_row(item, -1);
    return 1;
}

void ui_menu_bar_note_search_opened(float now) {
    g_search_open_time = now;
}

static void code_panel_format_search_query(ReplSearchState srch,
                                           char *out, int out_sz,
                                           int max_chars,
                                           int *out_cursor_col) {
    int start = 0;
    int take = 0;

    if (out_sz <= 0)
        return;

    out[0] = '\0';
    if (out_cursor_col)
        *out_cursor_col = 0;

    if (max_chars <= 0 || srch.query_len <= 0)
        return;

    if (srch.query_len > max_chars) {
        start = srch.cursor_pos - max_chars + 1;
        if (start < 0)
            start = 0;
        if (start > srch.query_len - max_chars)
            start = srch.query_len - max_chars;
    }

    take = srch.query_len - start;
    if (take > max_chars)
        take = max_chars;
    if (take >= out_sz)
        take = out_sz - 1;
    if (take < 0)
        take = 0;

    if (take > 0)
        memcpy(out, srch.query + start, (size_t)take);
    out[take] = '\0';

    if (out_cursor_col) {
        int col = srch.cursor_pos - start;
        if (col < 0)
            col = 0;
        if (col > take)
            col = take;
        *out_cursor_col = col;
    }
}

static float ui_fade_alpha(float anim_time, float open_time) {
    if (open_time < 0.0f) return 1.0f;
    float dt = anim_time - open_time;
    if (dt >= UI_FADE_DURATION) return 1.0f;
    if (dt <= 0.0f) return 0.0f;
    return dt / UI_FADE_DURATION;
}

/* Draws a simple magnifying-glass icon (circle + handle) at (cx, cy) with
 * given radius, using the current GL color. */
static void draw_search_icon(float cx, float cy, float r) {
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 20; i++) {
        float a = (float)i * (6.2831853f / 20.0f);
        glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
    }
    glEnd();
    float hx0 = cx + cosf(-0.7853982f) * r;
    float hy0 = cy + sinf(-0.7853982f) * r;
    glBegin(GL_LINES);
    glVertex2f(hx0, hy0);
    glVertex2f(hx0 + r * 0.9f, hy0 - r * 0.9f);
    glEnd();
}

void ui_menu_bar_render_search_overlay(const UiRenderSnapshot *snap,
                                       int cp_x, int panel_w, int panel_top) {
    ReplSearchState srch = snap->search;
    char count_buf[32];
    char query_buf[128];
    int cursor_col = 0;
    (void)cp_x; (void)panel_w; (void)panel_top;

    static int prev_active = 0;
    if (srch.active && !prev_active) g_search_open_time = snap->anim_time;
    prev_active = srch.active;

    if (!srch.active)
        return;

    /* Anchor on the PIN_SEARCH slot so the search bar sits where the
     * placeholder was - matches the design's inline search affordance. */
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);

    int box_x = pin_x[PIN_SEARCH];
    int box_y = by;
    int box_w = pin_w[PIN_SEARCH];
    int box_h = bh;

    if (srch.query_len <= 0)
        snprintf(count_buf, sizeof(count_buf), "type to search");
    else if (srch.match_count <= 0)
        snprintf(count_buf, sizeof(count_buf), "0");
    else
        snprintf(count_buf, sizeof(count_buf), "%d/%d",
                 srch.hit_ordinal, srch.match_count);

    int pad_x = 8;
    int icon_r = 5;
    int icon_cx = box_x + pad_x + icon_r;
    int icon_cy = box_y + box_h / 2;
    int text_y  = box_y + (box_h - FONT_SMALL_H) / 2 + 1;
    int count_w = (int)strlen(count_buf) * FONT_SMALL_W;
    int count_x = box_x + box_w - pad_x - count_w;
    int query_x = icon_cx + icon_r + 8;
    int max_query_chars = (count_x - query_x - pad_x) / FONT_SMALL_W;
    if (max_query_chars < 1) max_query_chars = 1;
    code_panel_format_search_query(srch, query_buf, sizeof(query_buf),
                                   max_query_chars, &cursor_col);

    float alpha = ui_fade_alpha(snap->anim_time, g_search_open_time);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background: a shade lighter than the menu bar (#262626) so the active
     * search input reads as "focused". */
    glColor4f(0.149f, 0.149f, 0.149f, alpha);
    glRectf((float)box_x, (float)box_y, (float)box_x + (float)box_w, (float)box_y + (float)box_h);

    /* Inner border */
    glColor4f(0.298f, 0.329f, 0.392f, alpha); /* #4c5464 */
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)box_x + 0.5f,              (float)box_y + 0.5f);
    glVertex2f((float)(box_x + box_w) - 0.5f,    (float)box_y + 0.5f);
    glVertex2f((float)(box_x + box_w) - 0.5f,    (float)(box_y + box_h) - 0.5f);
    glVertex2f((float)box_x + 0.5f,              (float)(box_y + box_h) - 0.5f);
    glEnd();

    /* Magnifying-glass icon */
    glColor4f(0.667f, 0.706f, 0.784f, alpha); /* #aab3c8 */
    draw_search_icon((float)icon_cx, (float)icon_cy, (float)icon_r);

    /* Query text (or placeholder style when empty) */
    if (srch.query_len <= 0)
        glColor4f(0.478f, 0.478f, 0.478f, alpha); /* #7a7a7a placeholder */
    else
        glColor4f(0.941f, 0.941f, 0.902f, alpha);
    gl2d_draw_string((float)query_x, (float)text_y, query_buf, FONT_SMALL);

    /* Count/status on the right */
    if (srch.query_len > 0 && srch.match_count <= 0)
        glColor4f(0.851f, 0.424f, 0.310f, alpha); /* accent for "0" */
    else
        glColor4f(0.533f, 0.533f, 0.533f, alpha);
    gl2d_draw_string((float)count_x, (float)text_y, count_buf, FONT_SMALL);

    if (snap->code_panel.cursor_visible && srch.query_len > 0) {
        int cursor_x = query_x + cursor_col * FONT_SMALL_W;
        glColor4f(0.95f, 0.80f, 0.24f, 0.85f * alpha);
        glRectf((float)cursor_x, (float)(text_y - 2), (float)cursor_x + 2.0f,
                  (float)(text_y - 2) + (float)(FONT_SMALL_H + 2));
    }

    glDisable(GL_BLEND);
}


void ui_menu_bar_render(const UiRenderSnapshot *snap) {
    ReplReplayRuntimeState replay = snap->replay;
    int cp_x, cp_y, cp_w, cp_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    (void)cp_y;
    if (cp_w <= 0 || cp_h <= 0) return;
    /* Menu bar - design ref: Header Wireframes v2 (now at the very top of
     * the code panel; the old info bar moved into the bottom status strip). */
    {
        int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
        int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
        int by, bh;
        menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        /* Full-width strip: #1d1d1d */
        glColor4f(0.114f, 0.114f, 0.114f, 0.98f);
        glRectf((float)cp_x, (float)by, (float)cp_x + (float)cp_w, (float)by + (float)bh);

        int hover_menu = ui_menu_bar_menu_hit(snap->pointer.mouse_x, snap->pointer.mouse_y);
        int hover_pin  = ui_menu_bar_pin_hit(snap->pointer.mouse_x, snap->pointer.mouse_y);

        /* Left-side menu labels */
        for (int i = 0; i < NUM_MENUS; i++) {
            int active = (g_open_menu == i);
            int hover  = (hover_menu == i);
            if (active) {
                glColor4f(0.149f, 0.149f, 0.149f, 1.0f); /* #262626 */
                glRectf((float)menu_x[i], (float)by, (float)menu_x[i] + (float)menu_w[i], (float)by + (float)bh);
            } else if (hover) {
                glColor4f(0.165f, 0.165f, 0.165f, 1.0f); /* #2a2a2a */
                glRectf((float)menu_x[i], (float)by, (float)menu_x[i] + (float)menu_w[i], (float)by + (float)bh);
            }
            if (active || hover)
                glColor3f(1.0f, 1.0f, 1.0f);
            else
                glColor3f(0.847f, 0.847f, 0.847f);       /* #d8d8d8 */
            int tx = menu_x[i] + 9;
            gl2d_draw_string((float)tx, (float)(by + 3),
                        g_menu_labels[i], FONT_SMALL);
        }

        /* Mask the combined pin area with the menubar bg so a long menu
         * label in a narrow window can't bleed through transparent pin
         * slots (pins must stay visible and take hit-test priority). */
        int pin_block_x = pin_x[PIN_SEARCH];
        int pin_block_w = cp_x + cp_w - CODE_MARGIN_X - pin_block_x;
        glColor4f(0.114f, 0.114f, 0.114f, 1.0f); /* #1d1d1d, fully opaque */
        glRectf((float)pin_block_x, (float)by, (float)pin_block_x + (float)pin_block_w, (float)by + (float)bh);

        /* Right-side pins: Search | Replay (always rendered on top) */
        for (int i = 0; i < NUM_PIN_BTNS; i++) {
            int hover = (hover_pin == i);
            int active = (i == PIN_REPLAY && replay.active);
            if (hover) {
                glColor4f(0.165f, 0.165f, 0.165f, 1.0f);
                glRectf((float)pin_x[i], (float)by, (float)pin_x[i] + (float)pin_w[i], (float)by + (float)bh);
            } else if (active) {
                glColor4f(0.149f, 0.149f, 0.149f, 1.0f);
                glRectf((float)pin_x[i], (float)by, (float)pin_x[i] + (float)pin_w[i], (float)by + (float)bh);
            }
            /* Left separator rule (#2a2a2a) */
            glColor4f(0.165f, 0.165f, 0.165f, 1.0f);
            glBegin(GL_LINES);
            glVertex2f((float)pin_x[i], (float)by);
            glVertex2f((float)pin_x[i], (float)(by + bh));
            glEnd();

            if (i == PIN_SEARCH) {
                if (snap->search.active) {
                    /* render_code_panel_search_overlay() fills this slot */
                    continue;
                }
                /* "search..." label in muted gray */
                glColor3f(0.478f, 0.478f, 0.478f); /* #7a7a7a */
                int tx = pin_x[i] + 12;
                gl2d_draw_string((float)tx, (float)(by + 3),
                            g_pin_btn_labels[i], FONT_SMALL);
            } else if (i == PIN_REPLAY) {
                /* Green accent (#6fb36f), state icon + dynamic label */
                const char *label = "Replay";
                if (replay.state == REPLAY_PLAYING) label = "Replaying";
                else if (replay.state == REPLAY_PAUSED) label = "Paused";
                else if (replay.state == REPLAY_DONE)   label = "Done";

                int icon_x = pin_x[i] + 10;
                int icon_cy = by + bh / 2;
                int icon_sz = 8;

                glColor3f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B);

                if (replay.state == REPLAY_PLAYING) {
                    /* Two vertical bars (pause glyph) */
                    float bw = 2.5f, gap = 2.0f;
                    float by0 = (float)icon_cy - (float)icon_sz * 0.5f;
                    float bh0 = (float)icon_sz;
                    glRectf((float)icon_x,                    by0, (float)icon_x + bw, by0 + bh0);
                    glRectf((float)icon_x + bw + gap,         by0, (float)icon_x + bw + gap + bw, by0 + bh0);
                } else if (replay.state == REPLAY_DONE) {
                    /* Square - run complete */
                    float sx = (float)icon_x;
                    float sy = (float)icon_cy - (float)icon_sz * 0.5f;
                    glRectf(sx, sy, sx + (float)icon_sz, sy + (float)icon_sz);
                } else {
                    /* Play triangle - stopped (OFF) or paused, click to start */
                    float x0 = (float)icon_x;
                    float cy = (float)icon_cy;
                    glBegin(GL_TRIANGLES);
                    glVertex2f(x0,             cy - (float)icon_sz * 0.5f);
                    glVertex2f(x0,             cy + (float)icon_sz * 0.5f);
                    glVertex2f(x0 + icon_sz,   cy);
                    glEnd();
                }

                int tx = icon_x + 12 + 6;
                gl2d_draw_string((float)tx, (float)(by + 3), label, FONT_SMALL);
            } else {
                if (hover || active)
                    glColor3f(1.0f, 1.0f, 1.0f);
                else
                    glColor3f(0.847f, 0.847f, 0.847f);
                int tx = pin_x[i] + 9;
                gl2d_draw_string((float)tx, (float)(by + 3),
                            g_pin_btn_labels[i], FONT_SMALL);
            }
        }

        /* Bottom divider (#000) */
        glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)cp_x,          (float)by);
        glVertex2f((float)(cp_x + cp_w), (float)by);
        glEnd();

        glDisable(GL_BLEND);
    }

}

void ui_menu_bar_render_example_dropdown(const UiRenderSnapshot *snap) {
    if (g_open_menu < 0) return;
    int menu_id = g_open_menu;
    int n  = menu_item_count(menu_id);
    int ne = (menu_id == MENU_SCENE) ? repl_example_count() : -1;

    int dx, dy, dw, dh;
    if (!menu_dropdown_rect(&dx, &dy, &dw, &dh)) return;

    g_menu_item_hover = ui_menu_bar_dropdown_item_hit(snap->pointer.mouse_x, snap->pointer.mouse_y);

    float alpha = ui_fade_alpha(snap->anim_time, g_menu_open_time);

    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Dropdown bg (#222) + border (#3a3a3a) - design ref */
    glColor4f(0.133f, 0.133f, 0.133f, 0.98f * alpha);
    glRectf((float)dx, (float)dy, (float)dx + (float)dw, (float)dy + (float)dh);
    glColor4f(0.227f, 0.227f, 0.227f, alpha);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)dx,        (float)dy);
    glVertex2f((float)(dx + dw), (float)dy);
    glVertex2f((float)(dx + dw), (float)(dy + dh));
    glVertex2f((float)dx,        (float)(dy + dh));
    glEnd();

    if (n == 0) {
        int ey = dy + dh - LINE_H + 1;
        glColor4f(0.478f, 0.518f, 0.580f, alpha);  /* #7a8494 (header style) */
        gl2d_draw_string((float)(dx + 14), (float)ey, "(empty)", FONT_SMALL);
        glDisable(GL_BLEND);
        gl2d_end();
        return;
    }

    int max_sc_px = menu_max_shortcut_px(menu_id);
    int state_right = dx + dw - 14;
    if (max_sc_px > 0) state_right -= max_sc_px + 16;

    int ey = dy + dh - LINE_H + 1;
    for (int i = 0; i < n; i++) {
        const char *lbl = menu_item_label(menu_id, i);
        if (!lbl) continue;

        if (strncmp(lbl, "### ", 4) == 0) {
            glColor4f(0.478f, 0.518f, 0.580f, alpha);  /* #7a8494 (header style) */
            gl2d_draw_string((float)(dx + 14), (float)ey, lbl + 4, FONT_SMALL);
            ey -= LINE_H;
            continue;
        }

        if (strcmp(lbl, "---") == 0) {
            glColor4f(0.20f, 0.20f, 0.20f, alpha);  /* #333 */
            glBegin(GL_LINES);
            glVertex2f((float)(dx + 6),       (float)(ey + LINE_H / 2 - 2));
            glVertex2f((float)(dx + dw - 6),  (float)(ey + LINE_H / 2 - 2));
            glEnd();
            ey -= LINE_H;
            continue;
        }

        int scene_hit = -1;
        if (menu_id == MENU_SCENE && ne >= 0) {
            int scene_n = i - (ne + SCENE_OFF_SCENES);
            if (scene_n >= 0 && scene_n < repl_user_scene_count())
                scene_hit = repl_scene_menu_slot_for_dense_index(scene_n);
        }
        int is_active_example = (menu_id == MENU_SCENE && ne >= 0 &&
                     i >= 1 && i <= ne &&
                     (i - 1) == snap->scenes.active_example_idx);
        int is_active_scene   = (scene_hit >= 0 &&
                                 scene_hit == snap->user_scene_active_idx);

        if (i == g_menu_item_hover) {
            glColor4f(0.180f, 0.290f, 0.431f, alpha);  /* #2e4a6e */
            glRectf((float)(dx + 1), (float)(ey - 2),
                      (float)(dx + 1) + (float)(dw - 2), (float)(ey - 2) + (float)LINE_H);
            glColor4f(1.0f, 1.0f, 1.0f, alpha);
        } else if (is_active_example || is_active_scene) {
            glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, alpha);
        } else {
            glColor4f(0.847f, 0.847f, 0.847f, alpha);  /* #d8d8d8 */
        }

        gl2d_draw_string((float)(dx + 14), (float)ey, lbl, FONT_SMALL);

        const char *sc = menu_item_shortcut(menu_id, i);
        if (sc) {
            int sc_px = (int)strlen(sc) * FONT_SMALL_W;
            glColor4f(0.533f, 0.533f, 0.533f, alpha);  /* #888 */
            gl2d_draw_string((float)(dx + dw - 14 - sc_px), (float)ey, sc, FONT_SMALL);
        }

        if (menu_id == MENU_CONFIG) {
            const ReplConfigItem *item = repl_config_item_at(i);
            if (!item || item->section_header || item->key == REPL_CONFIG_NONE)
                continue;
            char st_buf[CFG_STATE_MAX_CHARS + 1];
            const char *st = cfg_state_str(i, st_buf, sizeof(st_buf));
            int st_px = (int)strlen(st) * FONT_SMALL_W;
            int val = repl_config_get(item->key);
            if (val)
                glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, alpha);
            else
                glColor4f(0.533f, 0.533f, 0.533f, alpha);
            gl2d_draw_string((float)(state_right - st_px), (float)ey, st, FONT_SMALL);
        }

        ey -= LINE_H;
    }

    glDisable(GL_BLEND);
    gl2d_end();
}
