/*
 * menu_bar.c -- Code-panel menu bar, dropdowns, and search slot.
 */
#include "app/glr_actions.h"
#include "repl/core.h"
#include "repl/examples.h"
#include "repl/tutorials.h"
#include "app/glr_config.h"
#include <keys.h>
#include "repl/state_views.h"
#include "widgets/replay.h"   /* ReplayState (PLAYING / PAUSED / DONE) enum values */
#include "widgets/tutorial_state.h"
#include "state.h"
#include "menu_bar.h"
#include "metrics.h"
#include "theme.h"
#include "layout.h"
#include "gl_2d.h"

/* Menu bar - styled after Header Wireframes v2.
 * Left: top-level menus (File, Scene, Tutorials, Config).
 * Right: pinned buttons (Search, Replay) - retained in flat form until the
 * right-side redesign lands. */

enum {
    MENU_FILE = GLR_MENU_FILE,
    MENU_SCENE = GLR_MENU_SCENE,
    MENU_TUTORIALS = GLR_MENU_TUTORIALS,
    MENU_CONFIG = GLR_MENU_CONFIG,
    NUM_MENUS = GLR_MENU_COUNT
};

static const char *g_menu_labels[NUM_MENUS] = {
    "File", "Scene", "Tutorials", "Config"
};

/* Menubar bottom hairline: intentionally pure #000 in every theme
 * (design ref) - a theme-stable rule, not an accent. Kept as a named
 * constant per theme.h's "named constant" bucket, not a token. */
static const float k_menubar_bottom_rule[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

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

/* Generic flyout-submenu open state. A submenu is identified by the
 * top-level menu it belongs to and the parent dropdown row that owns
 * it — menu-agnostic, so Scene (example tags) and Config (sections)
 * share one engine. -1 / -1 = no submenu open. */
static int   g_submenu_menu_id    = -1;
static int   g_submenu_parent_row = -1;
static float g_submenu_open_time  = -1.0f;

/* Right-hand column a dropdown reserves for the ">" flyout-affordance
 * glyph so a long parent label can't collide with it. */
#define SUBMENU_ARROW_COL 26

/* Defined with the submenu provider further down; menu_dropdown_rect
 * (above it) needs the prototype to size the affordance column. */
static int menu_row_has_submenu(int menu_id, int row);
static float g_menu_open_time = -1.0f;   /* anim_time when current menu opened */
static float g_search_open_time = -1.0f; /* anim_time when search opened */
#define UI_FADE_DURATION 0.18f

static int ui_menu_bar_panel_visible(void);

int ui_menu_bar_menu_dropdown_is_open(void) {
    return g_open_menu >= 0 &&
           ui_menu_bar_panel_visible();
}
int ui_menu_bar_example_dropdown_is_open(void) { return ui_menu_bar_menu_dropdown_is_open(); }


enum { FILE_ITEM_COUNT = GLR_FILE_ITEM_COUNT };

/* SCENE menu layout (pure selector; actions are in the File menu):
 *   [0]                      "### EXAMPLES"
 *   [1..t]                   tag names  (t = repl_example_visible_tag_count())
 *   [t + SCENE_OFF_HDR]      "### MY SCENES"
 *   [t + SCENE_OFF_SCENES ..
 *      t + SCENE_OFF_SCENES + n - 1]  user scene names
 *                                     (n = repl_user_scene_count())
 * Both headers are always present even when a section is empty.
 */
enum {
    SCENE_OFF_HDR     = GLR_SCENE_OFF_HDR,
    SCENE_OFF_SCENES  = GLR_SCENE_OFF_SCENES,
    SCENE_FIXED_COUNT = GLR_SCENE_FIXED_COUNT
};

static int scene_tag_idx_for_parent_row(int row) {
    if (row < 1 || row > repl_example_visible_tag_count())
        return -1;
    return repl_example_visible_tag_at(row - 1);
}

static int scene_parent_row_for_tag(int tag_idx) {
    int visible_count = repl_example_visible_tag_count();
    for (int dense_idx = 0; dense_idx < visible_count; dense_idx++)
        if (repl_example_visible_tag_at(dense_idx) == tag_idx)
            return dense_idx + 1;
    return -1;
}

static int menu_item_count(int menu_id) {
    switch (menu_id) {
    case MENU_FILE:   return FILE_ITEM_COUNT;
    case MENU_SCENE:  return repl_example_visible_tag_count() + SCENE_FIXED_COUNT
                             + repl_user_scene_count();
    case MENU_TUTORIALS:
        return repl_tutorial_count() + (tutorial_active() ? 3 : 0);
    case MENU_CONFIG:
        /* One parent row per "### " section, plus a synthetic "All"
         * row whose flyout is the full flat list (chrome included).
         * The +1 is owned here in the menu layer — the pure
         * glr_config_section_count() never counts All (implemented per
         * plan Finding #2). */
        return glr_config_section_count() + 1;
    }
    return 0;
}

/* Index of the synthetic Config "All" parent row (last row). */
static int config_all_parent_row(void) {
    return glr_config_section_count();
}

static const char *menu_item_label(int menu_id, int i) {
    if (menu_id == MENU_FILE) {
        if (i == GLR_FILE_ITEM_NEW_SCENE)     return "New Scene";
        if (i == GLR_FILE_ITEM_SAVE_SCENE)    return "Save Scene";
        if (i == GLR_FILE_ITEM_LOAD_SCENE)    return "Load Scene";
        if (i == GLR_FILE_ITEM_RENAME_SCENE)  return "Rename Scene";
        if (i == GLR_FILE_ITEM_SCENE_SEP)     return "---";
        if (i == GLR_FILE_ITEM_SAVE_WORKSPACE) return "Save Workspace";
        if (i == GLR_FILE_ITEM_LOAD_WORKSPACE) return "Load Workspace";
        return NULL;
    }
    if (menu_id == MENU_SCENE) {
        int tag_count = repl_example_visible_tag_count();
        if (i == 0)                                            return "### EXAMPLES F11/F12";
        if (i >= 1 && i <= tag_count) {
            int tag_idx = repl_example_visible_tag_at(i - 1);
            return repl_example_tag_label(tag_idx);
        }
        if (i == tag_count + SCENE_OFF_HDR)                   return "### MY SCENES";
        int scene_n = i - (tag_count + SCENE_OFF_SCENES);
        if (scene_n >= 0 && scene_n < repl_user_scene_count()) {
            int slot = glr_scene_menu_slot_for_dense_index(scene_n);
            return (slot >= 0) ? repl_user_scene_name(slot) : NULL;
        }
        return NULL;
    }
    if (menu_id == MENU_TUTORIALS) {
        int tutorial_count = repl_tutorial_count();
        if (i >= 0 && i < tutorial_count)
            return repl_tutorial_name(i);
        if (!tutorial_active())
            return NULL;
        if (i == tutorial_count) return "---";
        if (i == tutorial_count + 1) return "Restart Tutorial";
        if (i == tutorial_count + 2) return "Exit Tutorial";
        return NULL;
    }
    if (menu_id == MENU_CONFIG) {
        /* Config top-level rows are section parents + "All"; the
         * actual items live in each row's flyout. The section model is
         * data-faithful (UPPERCASE, as in g_cfg_items[]); the menu
         * heading is shown leading-uppercase ("RENDERING" ->
         * "Rendering", "TIME & REPLAY" -> "Time & replay"). */
        if (i == config_all_parent_row())
            return "All";
        const char *raw = glr_config_section_label(i);
        if (!raw)
            return NULL;
        static char buf[48];
        size_t k = 0;
        for (; raw[k] && k < sizeof(buf) - 1; k++) {
            char c = raw[k];
            if (k == 0) {
                if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            } else if (c >= 'A' && c <= 'Z') {
                c = (char)(c + 32);
            }
            buf[k] = c;
        }
        buf[k] = '\0';
        return buf;
    }
    return NULL;
}

static const char *menu_item_shortcut(int menu_id, int i) {
    if (menu_id == MENU_FILE && i == GLR_FILE_ITEM_SAVE_SCENE) return "Ctrl+S";
    if (menu_id == MENU_SCENE) return NULL;
    if (menu_id == MENU_TUTORIALS)
        return NULL;
    /* Config: top-level rows are section parents — no shortcut at this
     * level; the per-item shortcut renders inside the flyout (Step 8). */
    (void)i;
    return NULL;
}

/* Format the keyboard shortcut for a g_cfg_items[] entry, or NULL. */
static const char *config_item_shortcut(const GlrConfigItem *item) {
    static char buf[16];
    if (!item || item->section_header || item->key == GLR_CONFIG_NONE)
        return NULL;
    if (item->key_code == 0) return NULL;
    if (item->is_special) {
        snprintf(buf, sizeof(buf), "F%d", item->key_code - GLUT_KEY_F1 + 1);
        return buf;
    }
    /* item->modifiers (GLUT_ACTIVE_* bitmask) inserts between the
     * implied Ctrl and the letter, e.g. Shift -> "Ctrl+Shift+v". */
    if (item->key_code > 0 && item->key_code <= 26) {
        const char *mod = (item->modifiers & GLUT_ACTIVE_SHIFT) ? "Shift+" : "";
        snprintf(buf, sizeof(buf), "Ctrl+%s%c", mod, item->key_code - 1 + 'a');
        return buf;
    }
    if (item->key_code == KEY_CTRL_BACKSLASH)
        return "Ctrl+\\";
    snprintf(buf, sizeof(buf), "%c", item->key_code);
    return buf;
}

#define CFG_STATE_MAX_CHARS 20

/* Fills `out` with the current state label, truncated to CFG_STATE_MAX_CHARS
 * (with a trailing ellipsis). Returns `out`. */
static const char *cfg_state_str(int i, char *out, int out_size) {
    const char *s = "";
    const GlrConfigItem *item = glr_config_item_at(i);
    if (item && !item->section_header && item->key != GLR_CONFIG_NONE) {
        s = glr_config_state_name(item->key, glr_config_get(item->key));
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
    const GlrConfigItem *items = glr_config_items(&count);
    for (int i = 0; i < count; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE)
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
    /* Window events arrive with a top-down y; UI hit rects use the
     * bottom-up OpenGL space tracked in UiViewportState. */
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
    /* Window events arrive with a top-down y; UI hit rects use the
     * bottom-up OpenGL space tracked in UiViewportState. */
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

    int max_lbl = 0, max_sc = 0, has_submenu = 0;
    for (int i = 0; i < n; i++) {
        const char *lbl = menu_item_label(g_open_menu, i);
        const char *sc  = menu_item_shortcut(g_open_menu, i);
        int lw = (int)(lbl ? strlen(lbl) : 0) * FONT_SMALL_W;
        if (lw > max_lbl) max_lbl = lw;
        if (sc) {
            int cw = (int)strlen(sc) * FONT_SMALL_W;
            if (cw > max_sc) max_sc = cw;
        }
        if (menu_row_has_submenu(g_open_menu, i))
            has_submenu = 1;
    }
    /* Config's per-item state/shortcut columns live in the flyout
     * now, not on the top-level section rows. */
    int max_w = max_lbl;
    if (max_sc > 0)    max_w += max_sc + 16;
    /* Reserve a right-hand column for the ">" flyout affordance so a
     * long parent label (e.g. "Overlays & scene") never collides with
     * it. The glyph is drawn at dx+dw-20; SUBMENU_ARROW_COL keeps a
     * clear gap. */
    if (has_submenu) max_w += SUBMENU_ARROW_COL;
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

static int point_in_rect_gl(int mx, int my, int x, int y, int w, int h) {
    int ry = ui_state_viewport().window_h - my;
    return mx >= x && mx < x + w && ry >= y && ry < y + h;
}

/* ---- Generic flyout-submenu provider --------------------------------- *
 *
 * One engine, two menus. A submenu is keyed by (menu_id, parent_row);
 * the provider resolves that to a row list. Scene parent rows are
 * example-tag rows (the tag index is recovered from the row); Config
 * parent rows are the "### " sections plus a synthetic "All" row whose
 * flyout is the whole flat table (chrome included).
 *
 * submenu_row_kind() lets the Config "All" flyout keep its
 * "### "/"---" chrome inert; every Scene row is an ITEM. */

/* Map a Config (parent_row, ordinal) to its absolute g_cfg_items[]
 * index, or -1. Named section parents expose only their item rows;
 * the "All" parent exposes the whole table 1:1. */
static int config_submenu_abs_index(int parent_row, int ordinal) {
    if (parent_row == config_all_parent_row()) {
        if (ordinal < 0 || ordinal >= CFG_ITEM_COUNT)
            return -1;
        return ordinal;
    }
    int start = 0, count = 0;
    if (!glr_config_section_range(parent_row, &start, &count))
        return -1;
    if (ordinal < 0 || ordinal >= count)
        return -1;
    return start + ordinal;
}

static int submenu_row_count(int menu_id, int parent_row) {
    if (menu_id == MENU_SCENE) {
        int tag = scene_tag_idx_for_parent_row(parent_row);
        return (tag >= 0) ? repl_example_count_for_tag(tag) : 0;
    }
    if (menu_id == MENU_CONFIG) {
        if (parent_row == config_all_parent_row())
            return CFG_ITEM_COUNT;
        int start = 0, count = 0;
        if (!glr_config_section_range(parent_row, &start, &count))
            return 0;
        return count;
    }
    return 0;
}

static const char *submenu_row_label(int menu_id, int parent_row,
                                     int ordinal) {
    if (menu_id == MENU_SCENE) {
        int tag = scene_tag_idx_for_parent_row(parent_row);
        if (tag < 0)
            return NULL;
        return repl_example_name(repl_example_index_for_tag(tag, ordinal));
    }
    if (menu_id == MENU_CONFIG) {
        int abs = config_submenu_abs_index(parent_row, ordinal);
        const GlrConfigItem *item = glr_config_item_at(abs);
        if (!item || !item->label)
            return NULL;
        /* "### X" headers in the "All" flyout render with the marker
         * stripped, matching the old flat dropdown. */
        if (glr_config_row_kind(abs) == GLR_CFG_ROW_HEADER)
            return item->label + 4;
        return item->label;
    }
    return NULL;
}

/* Absolute target index the row activates: a global flat example index
 * for Scene, a g_cfg_items[] index for Config. */
static int submenu_row_abs_index(int menu_id, int parent_row,
                                 int ordinal) {
    if (menu_id == MENU_SCENE) {
        int tag = scene_tag_idx_for_parent_row(parent_row);
        if (tag < 0)
            return -1;
        return repl_example_index_for_tag(tag, ordinal);
    }
    if (menu_id == MENU_CONFIG)
        return config_submenu_abs_index(parent_row, ordinal);
    return -1;
}

static GlrConfigRowKind submenu_row_kind(int menu_id, int parent_row,
                                         int ordinal) {
    if (menu_id == MENU_CONFIG)
        return glr_config_row_kind(
            config_submenu_abs_index(parent_row, ordinal));
    (void)parent_row; (void)ordinal;
    return GLR_CFG_ROW_ITEM;   /* Scene rows are all items */
}

/* Extra right-column px the Config flyout reserves for the per-item
 * keyboard shortcut + state label. 0 for non-Config submenus (Scene
 * rows are a bare label). */
/* Widest keyboard-shortcut label (px) across a Config flyout's rows.
 * Constant per flyout — both the shortcut and the state-value column
 * are aligned to fixed x's derived from it, so they don't jitter
 * row-to-row (matching the original flat dropdown). */
static int config_submenu_max_sc_px(int parent_row) {
    int count = submenu_row_count(MENU_CONFIG, parent_row);
    int max_sc = 0;
    for (int o = 0; o < count; o++) {
        const GlrConfigItem *it =
            glr_config_item_at(config_submenu_abs_index(parent_row, o));
        const char *sc = config_item_shortcut(it);
        if (sc) {
            int w = (int)strlen(sc) * FONT_SMALL_W;
            if (w > max_sc) max_sc = w;
        }
    }
    return max_sc;
}

static int config_submenu_extra_w(int menu_id, int parent_row) {
    if (menu_id != MENU_CONFIG)
        return 0;
    int max_sc = config_submenu_max_sc_px(parent_row);
    int extra = cfg_max_state_chars() * FONT_SMALL_W + 20;
    if (max_sc > 0)
        extra += max_sc + 16;
    return extra;
}

/* Does dropdown row `row` of `menu_id` own a flyout submenu? */
static int menu_row_has_submenu(int menu_id, int row) {
    if (row < 0)
        return 0;
    return submenu_row_count(menu_id, row) > 0;
}

static int submenu_rect(int menu_id, int parent_row,
                        int *sx, int *sy, int *sw, int *sh) {
    int pdx, pdy, pdw, pdh;
    int count;
    int max_lbl = 0;
    int width;
    int height;
    int x;
    int y;
    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;

    if (menu_id < 0 || g_open_menu != menu_id || win_w <= 0 || win_h <= 0)
        return 0;
    if (!menu_row_has_submenu(menu_id, parent_row))
        return 0;
    if (!menu_dropdown_rect(&pdx, &pdy, &pdw, &pdh))
        return 0;

    count = submenu_row_count(menu_id, parent_row);
    for (int ordinal = 0; ordinal < count; ordinal++) {
        const char *name = submenu_row_label(menu_id, parent_row, ordinal);
        int w = (int)(name ? strlen(name) : 0) * FONT_SMALL_W;
        if (w > max_lbl)
            max_lbl = w;
    }
    if (max_lbl < 80)
        max_lbl = 80;
    width = max_lbl + 28 + config_submenu_extra_w(menu_id, parent_row);
    height = count * LINE_H + 8;

    x = pdx + pdw;
    if (x + width > win_w)
        x = pdx - width;
    if (x < 0)
        x = 0;

    {
        int parent_row_top = pdy + pdh - 4 - parent_row * LINE_H;
        y = parent_row_top - height;
    }
    if (y < 0)
        y = 0;
    if (y + height > win_h)
        y = win_h - height;
    if (y < 0)
        y = 0;

    if (sx) *sx = x;
    if (sy) *sy = y;
    if (sw) *sw = width;
    if (sh) *sh = height;
    return 1;
}

int ui_menu_bar_submenu_rect_for_test(int menu_id, int parent_row,
                                      int *sx, int *sy,
                                      int *sw, int *sh) {
    return submenu_rect(menu_id, parent_row, sx, sy, sw, sh);
}

int ui_menu_bar_scene_example_submenu_rect_for_test(int tag_idx,
                                                    int *sx, int *sy,
                                                    int *sw, int *sh) {
    return submenu_rect(MENU_SCENE, scene_parent_row_for_tag(tag_idx),
                        sx, sy, sw, sh);
}

/* Classify a raw dropdown label as header / separator / item — the
 * one place the "### "-prefix and "---" conventions are decoded, so
 * the render paths and the hit-test agree (no more 3 hand-copied
 * predicates). */
static GlrConfigRowKind menu_chrome_kind(const char *label) {
    if (!label) return GLR_CFG_ROW_ITEM;
    if (strncmp(label, "### ", 4) == 0) return GLR_CFG_ROW_HEADER;
    if (strcmp(label, "---") == 0)      return GLR_CFG_ROW_SEPARATOR;
    return GLR_CFG_ROW_ITEM;
}

/* Draw one inert chrome row (section header text or "---" divider)
 * at vertical position `ey`, spanning [x, x+w]. header_text must be
 * the already-stripped section name (callers pass past the "### ").
 * No-op for non-chrome kinds. Shared by both dropdown renderers. */
static void menu_draw_chrome_row(GlrConfigRowKind kind, int x, int w,
                                 int ey, const char *header_text,
                                 float alpha) {
    if (kind == GLR_CFG_ROW_HEADER) {
        ui_clr_a(UI_TOK_TEXT_SECTION, alpha);
        gl2d_draw_string((float)(x + MENU_TEXT_INSET_X), (float)ey, header_text,
                         FONT_SMALL);
    } else if (kind == GLR_CFG_ROW_SEPARATOR) {
        ui_clr_a(UI_TOK_DIVIDER, alpha);
        glBegin(GL_LINES);
        glVertex2f((float)(x + 6),     (float)(ey + LINE_H / 2 - 2));
        glVertex2f((float)(x + w - 6), (float)(ey + LINE_H / 2 - 2));
        glEnd();
    }
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
    if (!lbl || menu_chrome_kind(lbl) != GLR_CFG_ROW_ITEM) return -1;
    return row;
}

/* Fill `h` with the unified UI_HIT_SUBMENU_ITEM payload for an
 * open-submenu row: cmd_idx = menu_id (the controller routes Scene vs.
 * Config off this), item_idx = absolute target index (global example
 * index / g_cfg_items[] index), line_idx = ordinal. Menu-agnostic. */
static void submenu_fill_hit(UiHit *h, int menu_id, int parent_row,
                             int ordinal, int mx, int my,
                             int sx, int sy) {
    int ry = ui_state_viewport().window_h - my;
    int abs_idx = submenu_row_abs_index(menu_id, parent_row, ordinal);
    if (abs_idx < 0)
        return;
    h->kind = UI_HIT_SUBMENU_ITEM;
    h->cmd_idx = menu_id;
    h->item_idx = abs_idx;
    h->line_idx = ordinal;
    h->local_x = (float)(mx - sx);
    h->local_y = (float)(ry - sy);
}

static UiHit submenu_hit_test(int mx, int my) {
    UiHit h = ui_hit_none();
    int sx, sy, sw, sh;
    int ry;
    int ordinal;

    if (g_submenu_menu_id < 0 || g_submenu_parent_row < 0)
        return h;
    if (!submenu_rect(g_submenu_menu_id, g_submenu_parent_row,
                      &sx, &sy, &sw, &sh))
        return h;
    if (!point_in_rect_gl(mx, my, sx, sy, sw, sh))
        return h;

    ry = ui_state_viewport().window_h - my;
    ordinal = (sy + sh - 4 - ry) / LINE_H;
    if (ordinal < 0 ||
        ordinal >= submenu_row_count(g_submenu_menu_id, g_submenu_parent_row))
        return h;

    /* Inert chrome rows (Config "All" "### "/"---") never produce a hit. */
    if (submenu_row_kind(g_submenu_menu_id, g_submenu_parent_row,
                         ordinal) != GLR_CFG_ROW_ITEM)
        return h;

    submenu_fill_hit(&h, g_submenu_menu_id, g_submenu_parent_row,
                     ordinal, mx, my, sx, sy);
    return h;
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
        if (g_submenu_menu_id == g_open_menu) {
            UiHit submenu_hit = submenu_hit_test(mx, my);
            if (submenu_hit.kind != UI_HIT_NONE)
                return submenu_hit;
            /* Point is inside the open flyout but landed on inert
             * chrome (Config "All" "### "/"---") or dead space:
             * consume it like the parent dropdown's inert rows. The
             * flyout is a separate rect beside the parent dropdown, so
             * the parent-rect check below would miss it and the click
             * would fall through to the scene tabs / code panel drawn
             * beneath (overlay-precedence violation). */
            int ssx, ssy, ssw, ssh;
            if (submenu_rect(g_submenu_menu_id, g_submenu_parent_row,
                             &ssx, &ssy, &ssw, &ssh) &&
                point_in_rect_gl(mx, my, ssx, ssy, ssw, ssh)) {
                h.kind = UI_HIT_CODE_PANEL_CHROME;
                h.local_x = (float)mx;
                h.local_y = (float)gl_y;
                return h;
            }
        }

        int row = ui_menu_bar_dropdown_item_hit(mx, my);
        if (row >= 0) {
            h.kind = UI_HIT_MENU_ITEM;
            h.cmd_idx = g_open_menu;
            h.item_idx = row;
            h.local_x = (float)mx;
            h.local_y = (float)gl_y;
            return h;
        }
        /* Click inside the open dropdown rect but not on an actionable
         * row (### header / --- separator / dead space): consume it
         * inertly. ui_menu_bar_dropdown_item_hit() returns -1 for these
         * even though the point is over the dropdown, so without this
         * the click falls through to the scene tab strip / code panel
         * drawn beneath the dropdown and can switch scenes underneath
         * (overlay-precedence requirement). The controller preamble
         * closes the dropdown; CHROME is the generic inert-consume kind. */
        int dx, dy, dw, dh;
        if (menu_dropdown_rect(&dx, &dy, &dw, &dh) &&
            mx >= dx && mx < dx + dw && gl_y >= dy && gl_y < dy + dh) {
            h.kind = UI_HIT_CODE_PANEL_CHROME;
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

static void submenu_reset(void) {
    g_submenu_menu_id    = -1;
    g_submenu_parent_row = -1;
    g_submenu_open_time  = -1.0f;
}

void ui_menu_bar_close(void) {
    g_open_menu = -1;
    g_menu_item_hover = -1;
    submenu_reset();
}

void ui_menu_bar_set_open_menu(int menu_id, float now) {
    if (menu_id < 0 || menu_id >= NUM_MENUS) {
        ui_menu_bar_close();
        return;
    }
    g_open_menu = menu_id;
    g_menu_open_time = now;
    g_menu_item_hover = -1;
    submenu_reset();
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
    /* Backward-cycle the Config flyout item under the cursor.
     * submenu_hit_test only resolves actionable ITEM rows — chrome
     * ("### "/"---" in the "All" flyout) is skipped via
     * submenu_row_kind, and section/All parent rows own no submenu, so
     * a right-press over the section list (or any non-item) is a
     * no-op rather than mis-cycling a g_cfg_items[] index. The hit
     * carries the absolute index in item_idx (implemented per plan
     * Step 7, Finding #3). */
    UiHit h = submenu_hit_test(mx, my);
    if (h.kind != UI_HIT_SUBMENU_ITEM || h.cmd_idx != MENU_CONFIG ||
        h.item_idx < 0)
        return 0;
    glr_cfg_cycle_row(h.item_idx, -1);
    return 1;
}

void ui_menu_bar_note_search_opened(float now) {
    g_search_open_time = now;
}

static void code_panel_format_search_query(EditorSearchState srch,
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

/* Render-time "this row reflects the active selection" highlight —
 * tints the row LABEL with the accent colour. Only Scene uses it (the
 * active example). Config deliberately does NOT: its label stays the
 * normal primary colour like the original flat dropdown; its on/off
 * is conveyed solely by the right-hand state-value column's colour. */
static int submenu_row_is_active(int menu_id, int parent_row, int ordinal,
                                 const UiRenderSnapshot *snap) {
    if (menu_id == MENU_SCENE) {
        int example_idx = submenu_row_abs_index(menu_id, parent_row,
                                                ordinal);
        return example_idx >= 0 &&
               example_idx == snap->scenes.active_example_idx;
    }
    return 0;
}

static int submenu_hover_ordinal(const UiRenderSnapshot *snap) {
    int sx, sy, sw, sh;
    int ry;
    int ordinal;

    if (!submenu_rect(g_submenu_menu_id, g_submenu_parent_row,
                      &sx, &sy, &sw, &sh))
        return -1;
    if (!point_in_rect_gl(snap->pointer.mouse_x, snap->pointer.mouse_y,
                          sx, sy, sw, sh))
        return -1;

    ry = snap->viewport.window_h - snap->pointer.mouse_y;
    ordinal = (sy + sh - 4 - ry) / LINE_H;
    if (ordinal < 0 ||
        ordinal >= submenu_row_count(g_submenu_menu_id,
                                     g_submenu_parent_row))
        return -1;
    return ordinal;
}

/* Open the submenu owned by the hovered parent row; keep an already-open
 * one alive while the pointer is inside its flyout rect; otherwise close
 * it. Menu-agnostic — driven entirely by menu_row_has_submenu(). */
static void update_submenu_hover_at(int mx, int my, float now) {
    int sx, sy, sw, sh;

    if (g_open_menu < 0) {
        submenu_reset();
        return;
    }

    if (menu_row_has_submenu(g_open_menu, g_menu_item_hover)) {
        if (g_submenu_menu_id != g_open_menu ||
            g_submenu_parent_row != g_menu_item_hover)
            g_submenu_open_time = now;
        g_submenu_menu_id    = g_open_menu;
        g_submenu_parent_row = g_menu_item_hover;
        return;
    }

    if (g_submenu_menu_id == g_open_menu && g_submenu_parent_row >= 0 &&
        submenu_rect(g_submenu_menu_id, g_submenu_parent_row,
                     &sx, &sy, &sw, &sh) &&
        point_in_rect_gl(mx, my, sx, sy, sw, sh))
        return;

    submenu_reset();
}

int ui_menu_bar_update_pointer_hover(int mx, int my, float now) {
    int old_hover  = g_menu_item_hover;
    int old_menu   = g_submenu_menu_id;
    int old_parent = g_submenu_parent_row;

    g_menu_item_hover = ui_menu_bar_dropdown_item_hit(mx, my);
    update_submenu_hover_at(mx, my, now);

    return old_hover != g_menu_item_hover ||
           old_menu != g_submenu_menu_id ||
           old_parent != g_submenu_parent_row;
}

static void render_active_submenu(const UiRenderSnapshot *snap) {
    int sx, sy, sw, sh;
    int menu_id    = g_submenu_menu_id;
    int parent_row = g_submenu_parent_row;
    int count;
    int hover_ordinal;
    int ey;
    float alpha;

    if (menu_id != g_open_menu || parent_row < 0)
        return;
    if (!submenu_rect(menu_id, parent_row, &sx, &sy, &sw, &sh))
        return;

    count = submenu_row_count(menu_id, parent_row);
    hover_ordinal = submenu_hover_ordinal(snap);
    alpha = ui_fade_alpha(snap->anim_time, g_submenu_open_time);

    /* Fixed column x's for the Config flyout so the shortcut and the
     * state value line up across every row (no per-row jitter). The
     * shortcut is right-aligned at the flyout's right padding; the
     * state value is right-aligned in a column to its left, sized by
     * the widest shortcut in this flyout. */
    int cfg_sc_right    = sx + sw - 14;
    int cfg_max_sc      = (menu_id == MENU_CONFIG)
                              ? config_submenu_max_sc_px(parent_row) : 0;
    int cfg_state_right = cfg_sc_right
                              - (cfg_max_sc > 0 ? cfg_max_sc + 16 : 0);

    ui_clr_a(UI_TOK_RAISED, 0.98f * alpha);
    glRectf((float)sx, (float)sy, (float)(sx + sw), (float)(sy + sh));
    ui_clr_a(UI_TOK_BORDER, alpha);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)sx,        (float)sy);
    glVertex2f((float)(sx + sw), (float)sy);
    glVertex2f((float)(sx + sw), (float)(sy + sh));
    glVertex2f((float)sx,        (float)(sy + sh));
    glEnd();

    ey = sy + sh - LINE_H + 1;
    for (int ordinal = 0; ordinal < count; ordinal++) {
        const char *name = submenu_row_label(menu_id, parent_row, ordinal);
        GlrConfigRowKind kind = submenu_row_kind(menu_id, parent_row,
                                                 ordinal);
        int is_active;

        if (!name) {
            ey -= LINE_H;
            continue;
        }

        /* Config "All" chrome (header/separator) renders inert. */
        if (kind == GLR_CFG_ROW_HEADER || kind == GLR_CFG_ROW_SEPARATOR) {
            menu_draw_chrome_row(kind, sx, sw, ey, name, alpha);
            ey -= LINE_H;
            continue;
        }

        is_active = submenu_row_is_active(menu_id, parent_row, ordinal,
                                          snap);

        if (ordinal == hover_ordinal) {
            ui_clr_a(UI_TOK_DROPDOWN_ITEM_HOVER_BG, alpha);
            glRectf((float)(sx + 1), (float)(ey - 2),
                    (float)(sx + sw - 1), (float)(ey - 2 + LINE_H));
            ui_clr_a(UI_TOK_TEXT_ON_HILITE, alpha);
        } else if (is_active) {
            ui_clr_a(UI_TOK_ACCENT, alpha);
        } else {
            ui_clr_a(UI_TOK_TEXT_PRIMARY, alpha);
        }

        gl2d_draw_string((float)(sx + MENU_TEXT_INSET_X), (float)ey, name, FONT_SMALL);

        /* Config item rows carry a right-aligned shortcut + state
         * value in fixed columns (computed once above) so they stay
         * column-aligned regardless of which rows have a shortcut. */
        if (menu_id == MENU_CONFIG) {
            int abs = config_submenu_abs_index(parent_row, ordinal);
            const GlrConfigItem *item = glr_config_item_at(abs);
            if (item && !item->section_header &&
                item->key != GLR_CONFIG_NONE) {
                char st_buf[CFG_STATE_MAX_CHARS + 1];
                const char *st = cfg_state_str(abs, st_buf,
                                               sizeof(st_buf));
                int st_px = (int)strlen(st) * FONT_SMALL_W;
                const char *scut = config_item_shortcut(item);

                if (scut) {
                    int sc_px = (int)strlen(scut) * FONT_SMALL_W;
                    ui_clr_a(UI_TOK_TEXT_MUTED, alpha);
                    gl2d_draw_string((float)(cfg_sc_right - sc_px),
                                     (float)ey, scut, FONT_SMALL);
                }
                if (glr_config_get(item->key))
                    ui_clr_a(UI_TOK_ACCENT, alpha);
                else
                    ui_clr_a(UI_TOK_TEXT_MUTED, alpha);
                gl2d_draw_string((float)(cfg_state_right - st_px),
                                 (float)ey, st, FONT_SMALL);
            }
        }

        ey -= LINE_H;
    }
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
    EditorSearchState srch = snap->search;
    char count_buf[32];
    char query_buf[128];
    int cursor_col = 0;
    (void)cp_x; (void)panel_w; (void)panel_top;

    /* The search-open fade clock (g_search_open_time) is driven by the
     * controller via ui_menu_bar_note_search_opened() on both open
     * paths (Ctrl+F in glr_ctrl_keyboard, menu pin in
     * route_pin_button_hit). This renderer no longer detects the
     * rising edge itself — render stays pure. */

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

    /* Background: a shade lighter than the menu bar so the active
     * search input reads as "focused". */
    ui_clr_a(UI_TOK_MENU_LABEL_ACTIVE_BG, alpha);
    glRectf((float)box_x, (float)box_y, (float)box_x + (float)box_w, (float)box_y + (float)box_h);

    /* Inner border */
    ui_clr_a(UI_TOK_BORDER, alpha);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)box_x + 0.5f,              (float)box_y + 0.5f);
    glVertex2f((float)(box_x + box_w) - 0.5f,    (float)box_y + 0.5f);
    glVertex2f((float)(box_x + box_w) - 0.5f,    (float)(box_y + box_h) - 0.5f);
    glVertex2f((float)box_x + 0.5f,              (float)(box_y + box_h) - 0.5f);
    glEnd();

    /* Magnifying-glass icon */
    ui_clr_a(UI_TOK_TEXT_SECTION, alpha);
    draw_search_icon((float)icon_cx, (float)icon_cy, (float)icon_r);

    /* Query text (or placeholder style when empty) */
    if (srch.query_len <= 0)
        ui_clr_a(UI_TOK_TEXT_PLACEHOLDER, alpha);
    else
        ui_clr_a(UI_TOK_TEXT_PRIMARY, alpha);
    gl2d_draw_string((float)query_x, (float)text_y, query_buf, FONT_SMALL);

    /* Count/status on the right */
    if (srch.query_len > 0 && srch.match_count <= 0)
        ui_clr_a(UI_TOK_STATUS_ERR, alpha); /* "0 matches" */
    else
        ui_clr_a(UI_TOK_TEXT_MUTED, alpha);
    gl2d_draw_string((float)count_x, (float)text_y, count_buf, FONT_SMALL);

    if (snap->code_panel.cursor_visible && srch.query_len > 0) {
        int cursor_x = query_x + cursor_col * FONT_SMALL_W;
        ui_clr_a(UI_TOK_CARET, 0.85f * alpha);
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

        /* Full-width strip surface */
        ui_clr_a(UI_TOK_SURFACE, 0.98f);
        glRectf((float)cp_x, (float)by, (float)cp_x + (float)cp_w, (float)by + (float)bh);

        int hover_menu = ui_menu_bar_menu_hit(snap->pointer.mouse_x, snap->pointer.mouse_y);
        int hover_pin  = ui_menu_bar_pin_hit(snap->pointer.mouse_x, snap->pointer.mouse_y);

        /* Left-side menu labels */
        for (int i = 0; i < NUM_MENUS; i++) {
            int active = (g_open_menu == i);
            int hover  = (hover_menu == i);
            if (active) {
                ui_clr(UI_TOK_MENU_LABEL_ACTIVE_BG);
                glRectf((float)menu_x[i], (float)by, (float)menu_x[i] + (float)menu_w[i], (float)by + (float)bh);
            } else if (hover) {
                ui_clr(UI_TOK_MENU_LABEL_HOVER_BG);
                glRectf((float)menu_x[i], (float)by, (float)menu_x[i] + (float)menu_w[i], (float)by + (float)bh);
            }
            if (active || hover)
                ui_clr(UI_TOK_TEXT_ON_HILITE);
            else
                ui_clr(UI_TOK_TEXT_PRIMARY);
            int tx = menu_x[i] + 9;
            gl2d_draw_string((float)tx, (float)(by + 3),
                        g_menu_labels[i], FONT_SMALL);
        }

        /* Mask the combined pin area with the menubar bg so a long menu
         * label in a narrow window can't bleed through transparent pin
         * slots (pins must stay visible and take hit-test priority). */
        int pin_block_x = pin_x[PIN_SEARCH];
        int pin_block_w = cp_x + cp_w - CODE_MARGIN_X - pin_block_x;
        ui_clr(UI_TOK_SURFACE); /* fully opaque pin mask */
        glRectf((float)pin_block_x, (float)by, (float)pin_block_x + (float)pin_block_w, (float)by + (float)bh);

        /* Right-side pins: Search | Replay (always rendered on top) */
        for (int i = 0; i < NUM_PIN_BTNS; i++) {
            int hover = (hover_pin == i);
            int active = (i == PIN_REPLAY && replay.active);
            if (hover) {
                ui_clr(UI_TOK_MENU_LABEL_HOVER_BG);
                glRectf((float)pin_x[i], (float)by, (float)pin_x[i] + (float)pin_w[i], (float)by + (float)bh);
            } else if (active) {
                ui_clr(UI_TOK_MENU_LABEL_ACTIVE_BG);
                glRectf((float)pin_x[i], (float)by, (float)pin_x[i] + (float)pin_w[i], (float)by + (float)bh);
            }
            /* Left separator rule */
            ui_clr(UI_TOK_DIVIDER);
            glBegin(GL_LINES);
            glVertex2f((float)pin_x[i], (float)by);
            glVertex2f((float)pin_x[i], (float)(by + bh));
            glEnd();

            if (i == PIN_SEARCH) {
                if (snap->search.active) {
                    /* ui_menu_bar_render_search_overlay() fills this slot */
                    continue;
                }
                /* "search..." label in muted gray */
                ui_clr(UI_TOK_TEXT_PLACEHOLDER);
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

                ui_clr(UI_TOK_ACCENT);

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
                    ui_clr(UI_TOK_TEXT_ON_HILITE);
                else
                    ui_clr(UI_TOK_TEXT_PRIMARY);
                int tx = pin_x[i] + 9;
                gl2d_draw_string((float)tx, (float)(by + 3),
                            g_pin_btn_labels[i], FONT_SMALL);
            }
        }

        /* Bottom divider - theme-stable pure-black rule */
        glColor4fv(k_menubar_bottom_rule);
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
    int tag_count = (menu_id == MENU_SCENE) ? repl_example_visible_tag_count() : -1;

    int dx, dy, dw, dh;
    if (!menu_dropdown_rect(&dx, &dy, &dw, &dh)) return;

    ui_menu_bar_update_pointer_hover(snap->pointer.mouse_x,
                                     snap->pointer.mouse_y,
                                     snap->anim_time);

    float alpha = ui_fade_alpha(snap->anim_time, g_menu_open_time);

    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Dropdown bg (#222) + border (#3a3a3a) - design ref */
    ui_clr_a(UI_TOK_RAISED, 0.98f * alpha);
    glRectf((float)dx, (float)dy, (float)dx + (float)dw, (float)dy + (float)dh);
    ui_clr_a(UI_TOK_BORDER, alpha);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)dx,        (float)dy);
    glVertex2f((float)(dx + dw), (float)dy);
    glVertex2f((float)(dx + dw), (float)(dy + dh));
    glVertex2f((float)dx,        (float)(dy + dh));
    glEnd();

    if (n == 0) {
        int ey = dy + dh - LINE_H + 1;
        ui_clr_a(UI_TOK_TEXT_SECTION, alpha);
        gl2d_draw_string((float)(dx + MENU_TEXT_INSET_X), (float)ey, "(empty)", FONT_SMALL);
        glDisable(GL_BLEND);
        gl2d_end();
        return;
    }


    int ey = dy + dh - LINE_H + 1;
    for (int i = 0; i < n; i++) {
        const char *lbl = menu_item_label(menu_id, i);
        if (!lbl) continue;

        GlrConfigRowKind ck = menu_chrome_kind(lbl);
        if (ck == GLR_CFG_ROW_HEADER || ck == GLR_CFG_ROW_SEPARATOR) {
            menu_draw_chrome_row(ck, dx, dw, ey,
                                 lbl + (ck == GLR_CFG_ROW_HEADER ? 4 : 0),
                                 alpha);
            ey -= LINE_H;
            continue;
        }

        int scene_hit = -1;
        int has_submenu = menu_row_has_submenu(menu_id, i);
        int is_open_parent = (has_submenu &&
                              menu_id == g_submenu_menu_id &&
                              i == g_submenu_parent_row);
        if (menu_id == MENU_SCENE && tag_count >= 0) {
            int scene_n = i - (tag_count + SCENE_OFF_SCENES);
            if (scene_n >= 0 && scene_n < repl_user_scene_count())
                scene_hit = glr_scene_menu_slot_for_dense_index(scene_n);
        }
        int is_active_scene   = (scene_hit >= 0 &&
                                 scene_hit == snap->user_scene_active_idx);

        if (i == g_menu_item_hover || is_open_parent) {
            ui_clr_a(UI_TOK_DROPDOWN_ITEM_HOVER_BG, alpha);
            glRectf((float)(dx + 1), (float)(ey - 2),
                      (float)(dx + 1) + (float)(dw - 2), (float)(ey - 2) + (float)LINE_H);
            ui_clr_a(UI_TOK_TEXT_ON_HILITE, alpha);
        } else if (is_active_scene) {
            ui_clr_a(UI_TOK_ACCENT, alpha);
        } else {
            ui_clr_a(UI_TOK_TEXT_PRIMARY, alpha);
        }

        gl2d_draw_string((float)(dx + MENU_TEXT_INSET_X), (float)ey, lbl, FONT_SMALL);

        if (has_submenu) {
            ui_clr_a(UI_TOK_TEXT_MUTED, alpha);
            gl2d_draw_string((float)(dx + dw - 20), (float)ey, ">", FONT_SMALL);
        }

        const char *sc = menu_item_shortcut(menu_id, i);
        if (sc) {
            int sc_px = (int)strlen(sc) * FONT_SMALL_W;
            ui_clr_a(UI_TOK_TEXT_MUTED, alpha);
            gl2d_draw_string((float)(dx + dw - 14 - sc_px), (float)ey, sc, FONT_SMALL);
        }

        /* Config item state now renders inside each section's flyout
         * (Step 8), not on the top-level section rows. */

        ey -= LINE_H;
    }

    render_active_submenu(snap);

    glDisable(GL_BLEND);
    gl2d_end();
}
