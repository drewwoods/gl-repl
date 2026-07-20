/*
 * menu_bar.c -- Code-panel menu bar, dropdowns, and search slot.
 */
#include "app/glr_actions.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "repl/scenes.h"
#include "repl/examples.h"
#include "repl/tutorials.h"
#include "app/glr_audio.h"
#include "app/glr_config.h"
#include "app/glr_tours.h"
#include "keymap.h"
#include "keys.h"
#include "repl/state_views.h"
#include "subsystems/replay/replay.h"   /* ReplayState (PLAYING / PAUSED / DONE) enum values */
#include "subsystems/tutorial/tutorial_state.h"
#include "support/cpuprof.h"
#include "ui/app/state.h"
#include "ui/app/menu_bar.h"
#include "ui/core/metrics.h"
#include "ui/core/theme.h"
#include "ui/app/layout.h"
#include "ui/app/view_mode_swatch.h"
#include "ui/app/numeric_swatch.h"
#include "ui/core/gl_2d.h"

/* Menu bar - styled after Header Wireframes v2.
 * Left: top-level menus (File, Scene, Tutorials, Tours, Config, Audio).
 * Right: pinned buttons (Search, Replay) - retained in flat form until the
 * right-side redesign lands. */

enum {
    MENU_FILE = GLR_MENU_FILE,
    MENU_SCENE = GLR_MENU_SCENE,
    MENU_TUTORIALS = GLR_MENU_TUTORIALS,
    MENU_TOURS = GLR_MENU_TOURS,
    MENU_CONFIG = GLR_MENU_CONFIG,
    MENU_AUDIO = GLR_MENU_AUDIO,
    NUM_MENUS = GLR_MENU_COUNT
};

static const char *g_menu_labels[NUM_MENUS] = {
    "File", "Scene", "Tutorials", "Tours", "Config", "Audio"
};

/* The browser shell owns file I/O in Emscripten: New/Open/Download are
 * DOM controls wired to web-safe import/export bridges, not path prompts.
 * Tours use a web-specific catalog there; its Editing Basics script targets
 * the shell's New button instead of the hidden native File menu. */
static int menu_visible(int menu_id) {
#if defined(__EMSCRIPTEN__)
    if (menu_id == MENU_FILE)
        return 0;
#endif
    return menu_id >= 0 && menu_id < NUM_MENUS;
}

/* Menubar bottom hairline: intentionally pure #000 in every theme
 * (design ref) - a theme-stable rule, not an accent. Kept as a named
 * constant per theme.h's "named constant" bucket, not a token. */
static const float k_menubar_bottom_rule[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

/* Right-to-left the pins render from highest index first, so PIN_REPLAY sits
 * at the far right matching the design. PIN_SEARCH fills the gap between the
 * last menu on the left and PIN_REPLAY. */
enum { PIN_SEARCH = 0, PIN_VIEW_MODE, PIN_REPLAY, NUM_PIN_BTNS };
static const char *g_pin_btn_labels[NUM_PIN_BTNS] = {
    "search...", "2D/3D", "Replay"
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
/* Row offset of the open flyout when it is taller than the viewport
 * (the Config "All" list is ~47 rows, taller than an 800px window).
 * 0 = top. Driven by the mouse wheel via ui_menu_bar_handle_wheel_scroll;
 * reset to 0 whenever the flyout closes or its parent row changes. */
static int   g_submenu_scroll      = 0;

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



enum { FILE_ITEM_COUNT = GLR_FILE_ITEM_COUNT };

/* SCENE menu layout (selector + the two cycle actions; other actions are
 * in the File menu):
 *   [0]                      "### EXAMPLES"
 *   [1..t]                   tag names  (t = repl_example_visible_tag_count())
 *   [t + SCENE_OFF_SEP_TOP]  "---"       (divider)
 *   [t + SCENE_OFF_NEXT]     "Next"      (F12 example/scene cycle)
 *   [t + SCENE_OFF_PREV]     "Previous"  (Shift+F12 example/scene cycle)
 *   [t + SCENE_OFF_SEP_BOT]  "---"       (divider)
 *   [t + SCENE_OFF_HDR]      "### MY SCENES"
 *   [t + SCENE_OFF_SCENES ..
 *      t + SCENE_OFF_SCENES + n - 1]  user scene names
 *                                     (n = repl_user_scene_count())
 * Both headers are always present even when a section is empty.
 */
enum {
    SCENE_OFF_SEP_TOP = GLR_SCENE_OFF_SEP_TOP,
    SCENE_OFF_NEXT    = GLR_SCENE_OFF_NEXT,
    SCENE_OFF_PREV    = GLR_SCENE_OFF_PREV,
    SCENE_OFF_SEP_BOT = GLR_SCENE_OFF_SEP_BOT,
    SCENE_OFF_HDR     = GLR_SCENE_OFF_HDR,
    SCENE_OFF_SCENES  = GLR_SCENE_OFF_SCENES,
    SCENE_FIXED_COUNT = GLR_SCENE_FIXED_COUNT
};

static int scene_tag_idx_for_parent_row(int row) {
    if (row < 1 || row > repl_example_visible_tag_count())
        return -1;
    return repl_example_visible_tag_at(row - 1);
}

int ui_menu_bar_scene_parent_row_for_tag(int tag_idx) {
    int visible_count = repl_example_visible_tag_count();
    for (int dense_idx = 0; dense_idx < visible_count; dense_idx++)
        if (repl_example_visible_tag_at(dense_idx) == tag_idx)
            return dense_idx + 1;
    return -1;
}

/* Tutorials menu mirror of the scene helpers above. Tag rows occupy
 * [0..t-1] (no leading "### TUTORIALS" header — single-section menu).
 * The trailing "---" / Restart / Exit rows when a tutorial is active
 * are owned by menu_item_label, not these helpers. */
static int tutorial_tag_idx_for_parent_row(int row) {
    if (row < 0 || row >= repl_tutorial_visible_tag_count())
        return -1;
    return repl_tutorial_visible_tag_at(row);
}

int ui_menu_bar_tutorial_parent_row_for_tag(int tag_idx) {
    int visible_count = repl_tutorial_visible_tag_count();
    for (int dense_idx = 0; dense_idx < visible_count; dense_idx++)
        if (repl_tutorial_visible_tag_at(dense_idx) == tag_idx)
            return dense_idx;
    return -1;
}

static const char *audio_group_label_for_track(int track_idx) {
    const char *group = glr_audio_track_group(track_idx);
    return (group && group[0]) ? group : "Music";
}

static int audio_group_differs(const char *a, const char *b) {
    if (a == b)
        return 0;
    if (!a || !b)
        return 1;
    return strcmp(a, b) != 0;
}

/* Number of consecutive distinct source groups in the playlist. This is
 * the count of Audio-menu parent rows; the control rows (Play/Next/Prev/
 * Loop) sit at offsets past it. INVARIANT: glr_actions.c's
 * audio_menu_group_count() must compute the identical value — it turns a
 * clicked top-level item_idx back into a control-row offset by subtracting
 * this count, so if the two ever disagree the control clicks misfire. Both
 * read the same glr_audio_track_* accessors on the main thread, so they
 * cannot diverge; keep them in lockstep if the grouping rule changes. */
static int audio_visible_group_count(void) {
    int count = glr_audio_track_count();
    int groups = 0;
    const char *prev = NULL;
    for (int i = 0; i < count; i++) {
        const char *group = audio_group_label_for_track(i);
        if (i == 0 || audio_group_differs(group, prev))
            groups++;
        prev = group;
    }
    return groups;
}

static int audio_group_start_for_parent_row(int row) {
    int count = glr_audio_track_count();
    int group_ord = -1;
    const char *prev = NULL;
    if (row < 0)
        return -1;
    for (int i = 0; i < count; i++) {
        const char *group = audio_group_label_for_track(i);
        if (i == 0 || audio_group_differs(group, prev)) {
            group_ord++;
            if (group_ord == row)
                return i;
        }
        prev = group;
    }
    return -1;
}

static int audio_group_end_for_parent_row(int row) {
    int start = audio_group_start_for_parent_row(row);
    int count = glr_audio_track_count();
    const char *group;
    if (start < 0)
        return -1;
    group = audio_group_label_for_track(start);
    for (int i = start + 1; i < count; i++)
        if (audio_group_differs(audio_group_label_for_track(i), group))
            return i;
    return count;
}

/* True if two subheadings are equivalent for grouping purposes: same
 * pointer (covers both NULL), or non-NULL strings with equal contents. */
static int subheadings_equal(const char *a, const char *b) {
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

/* Catalog flyout walker shared by the Scene (examples), Tutorials, and
 * Audio menus. The catalogs are independent data sources but the in-flyout
 * row layout is identical: walk entries in catalog order filtered to the
 * tag/group, and whenever an entry's non-NULL subheading differs from the
 * previous emitted one, emit a HEADER row (label = subheading,
 * abs_idx = -1) before its ITEM row. Authoring rule: same-subheading
 * entries must be contiguous per tag (enforced by
 * test_example_subheading_metadata and test_catalog_subheading_metadata).
 *
 * Backing the walker is a tiny ops table — four catalog query function
 * pointers — so the algorithm has one home and every menu stays in
 * lockstep when the rule changes. */
typedef struct {
    int         (*count_for_tag)(int tag);
    int         (*index_for_tag)(int tag, int ord);
    const char *(*name_of)(int idx);
    const char *(*subheading_of)(int idx);
} CatalogFlyoutOps;

static const CatalogFlyoutOps kExampleCatalogOps = {
    .count_for_tag = repl_example_count_for_tag,
    .index_for_tag = repl_example_index_for_tag,
    .name_of       = repl_example_name,
    .subheading_of = repl_example_subheading,
};

static const CatalogFlyoutOps kTutorialCatalogOps = {
    .count_for_tag = repl_tutorial_count_for_tag,
    .index_for_tag = repl_tutorial_index_for_tag,
    .name_of       = repl_tutorial_name,
    .subheading_of = repl_tutorial_subheading,
};

static int audio_count_for_group(int group) {
    int start = audio_group_start_for_parent_row(group);
    int end = audio_group_end_for_parent_row(group);
    if (start < 0 || end < start)
        return 0;
    return end - start;
}

static int audio_index_for_group(int group, int ord) {
    int start = audio_group_start_for_parent_row(group);
    int end = audio_group_end_for_parent_row(group);
    int idx = start + ord;
    if (start < 0 || ord < 0 || idx >= end)
        return -1;
    return idx;
}

static const char *audio_track_name_for_menu(int idx) {
    return glr_audio_track_display_name(idx);
}

static const char *audio_track_subheading_for_menu(int idx) {
    (void)idx;
    return NULL;
}

static const CatalogFlyoutOps kAudioCatalogOps = {
    .count_for_tag = audio_count_for_group,
    .index_for_tag = audio_index_for_group,
    .name_of       = audio_track_name_for_menu,
    .subheading_of = audio_track_subheading_for_menu,
};

/* Total visible rows in one per-tag flyout (subheading HEADER chrome +
 * entry ITEM rows). */
static int catalog_flyout_row_count(const CatalogFlyoutOps *ops, int tag) {
    if (!ops || tag < 0)
        return 0;
    int total = 0;
    const char *prev_sub = NULL;
    int n = ops->count_for_tag(tag);
    for (int o = 0; o < n; o++) {
        int idx = ops->index_for_tag(tag, o);
        const char *sub = ops->subheading_of(idx);
        if (sub && !subheadings_equal(sub, prev_sub))
            total++;
        total++;
        prev_sub = sub;
    }
    return total;
}

/* Resolve the ordinal'th row in a per-tag flyout to its kind, absolute
 * catalog index, and display label. Out-params are write-only; pass NULL
 * to skip any field. Returns 1 on hit, 0 if ordinal is out of range or
 * the tag is invalid. HEADER rows report abs_idx = -1. */
static int catalog_flyout_row_at(const CatalogFlyoutOps *ops,
                                 int tag, int ordinal,
                                 GlrConfigRowKind *kind_out,
                                 int *abs_idx_out,
                                 const char **label_out) {
    if (!ops || tag < 0 || ordinal < 0)
        return 0;
    int row = 0;
    const char *prev_sub = NULL;
    int n = ops->count_for_tag(tag);
    for (int o = 0; o < n; o++) {
        int idx = ops->index_for_tag(tag, o);
        const char *sub = ops->subheading_of(idx);
        if (sub && !subheadings_equal(sub, prev_sub)) {
            if (row == ordinal) {
                if (kind_out)    *kind_out    = GLR_CFG_ROW_HEADER;
                if (abs_idx_out) *abs_idx_out = -1;
                if (label_out)   *label_out   = sub;
                return 1;
            }
            row++;
        }
        if (row == ordinal) {
            if (kind_out)    *kind_out    = GLR_CFG_ROW_ITEM;
            if (abs_idx_out) *abs_idx_out = idx;
            if (label_out)   *label_out   = ops->name_of(idx);
            return 1;
        }
        row++;
        prev_sub = sub;
    }
    return 0;
}

static int menu_item_count(int menu_id, const UiRenderSnapshot *snap) {
    switch (menu_id) {
    case MENU_FILE:   return FILE_ITEM_COUNT;
    case MENU_SCENE:  {
        int tag_count = snap ? snap->example_visible_tag_count : repl_example_visible_tag_count();
        int user_count = snap ? snap->user_scene_count : repl_user_scene_count();
        return tag_count + SCENE_FIXED_COUNT + user_count;
    }
    case MENU_TUTORIALS: {
        int tag_count = snap ? snap->tutorial.visible_tag_count : repl_tutorial_visible_tag_count();
        int active = snap ? snap->tutorial.active : tutorial_active();
        /* Tag rows + (when a tutorial is active) "---" / Restart / Exit.
         * Tutorial selection itself happens through the per-tag flyouts
         * (route_submenu_item_hit dispatches MENU_TUTORIALS submenu
         * items to tutorial_start directly), so the top-level rows
         * here are tag rows (inert hover-only) plus the trailing
         * Restart/Exit actions — mirroring Scene's tag-row pattern. */
        return tag_count + (active ? GLR_TUTORIAL_FIXED_COUNT : 0);
    }
    case MENU_TOURS:
        return glr_tours_count();
    case MENU_CONFIG:
        /* One parent row per "### " section, plus a synthetic "All"
         * row whose flyout is the full flat list (chrome included).
         * The +1 is owned here in the menu layer — the pure
         * glr_config_section_count() never counts All (implemented per
         * plan Finding #2). */
        return glr_config_section_count() + 1;
    case MENU_AUDIO: {
        int group_count = audio_visible_group_count();
        return group_count > 0 ? group_count + GLR_AUDIO_FIXED_COUNT : 0;
    }
    }
    return 0;
}

/* Index of the synthetic Config "All" parent row (last row). */
static int config_all_parent_row(void) {
    return glr_config_section_count();
}

static const char *audio_loop_mode_label(void) {
    int mode = glr_audio_get_loop_mode();
    if (mode == GLR_AUDIO_LOOP_OFF)
        return "Off";
    if (mode == GLR_AUDIO_LOOP_SONG)
        return "Song";
    return "All";
}

/* Row label for dropdown row `i` of `menu_id` — the single label table
 * every render/measure path reads. Static menus (File) are literal
 * per-index returns; Scene / Tutorials / Config compute the label from
 * their catalog row layout (tag rows, `### ` headers, `---` separators,
 * trailing action rows), mirroring the index math in
 * glr_action_menu_item_activate. NULL = past the end. */
static const char *menu_item_label(int menu_id, int i) {
    if (menu_id == MENU_FILE) {
        if (i == GLR_FILE_ITEM_NEW_SCENE)     return "New Scene";
        if (i == GLR_FILE_ITEM_SAVE_SCENE)    return "Save Scene";
        if (i == GLR_FILE_ITEM_LOAD_SCENE)    return "Load Scene";
        if (i == GLR_FILE_ITEM_LOAD_CLIPBOARD) return "Load Scene from Clipboard";
        if (i == GLR_FILE_ITEM_RENAME_SCENE)  return "Rename Scene";
        if (i == GLR_FILE_ITEM_EXPORT_PLY)    return "Export .ply";
        if (i == GLR_FILE_ITEM_SPLIT_DECL)    return "Split Declaration";
        if (i == GLR_FILE_ITEM_SCENE_SEP)     return "---";
        if (i == GLR_FILE_ITEM_SAVE_WORKSPACE) return "Save Workspace";
        if (i == GLR_FILE_ITEM_LOAD_WORKSPACE) return "Load Workspace";
        if (i == GLR_FILE_ITEM_QUIT_SEP)      return "---";
        if (i == GLR_FILE_ITEM_QUIT)          return "Quit";
        return NULL;
    }
    if (menu_id == MENU_SCENE) {
        int tag_count = repl_example_visible_tag_count();
        if (i == 0)                                            return "### EXAMPLES";
        if (i >= 1 && i <= tag_count) {
            int tag_idx = repl_example_visible_tag_at(i - 1);
            return repl_example_tag_label(tag_idx);
        }
        if (i == tag_count + SCENE_OFF_SEP_TOP)              return "---";
        if (i == tag_count + SCENE_OFF_NEXT)                  return "Next";
        if (i == tag_count + SCENE_OFF_PREV)                  return "Previous";
        if (i == tag_count + SCENE_OFF_SEP_BOT)              return "---";
        if (i == tag_count + SCENE_OFF_HDR)                   return "### MY SCENES";
        int scene_n = i - (tag_count + SCENE_OFF_SCENES);
        if (scene_n >= 0 && scene_n < repl_user_scene_count()) {
            int slot = glr_scene_menu_slot_for_dense_index(scene_n);
            return (slot >= 0) ? repl_user_scene_name(slot) : NULL;
        }
        return NULL;
    }
    if (menu_id == MENU_TUTORIALS) {
        int tag_count = repl_tutorial_visible_tag_count();
        if (i >= 0 && i < tag_count) {
            int tag_idx = repl_tutorial_visible_tag_at(i);
            return repl_tutorial_tag_label(tag_idx);
        }
        if (!tutorial_active())
            return NULL;
        if (i == tag_count + GLR_TUTORIAL_OFF_SEP)     return "---";
        if (i == tag_count + GLR_TUTORIAL_OFF_RESTART) return "Restart Tutorial";
        if (i == tag_count + GLR_TUTORIAL_OFF_EXIT)    return "Exit Tutorial";
        return NULL;
    }
    if (menu_id == MENU_TOURS)
        return glr_tours_name(i);
    if (menu_id == MENU_CONFIG) {
        if (i == config_all_parent_row())
            return "All";
        return glr_config_section_display_label(i);
    }
    if (menu_id == MENU_AUDIO) {
        static char loop_label[32];
        int group_count = audio_visible_group_count();
        if (i >= 0 && i < group_count) {
            int start = audio_group_start_for_parent_row(i);
            return start >= 0 ? audio_group_label_for_track(start) : NULL;
        }
        if (group_count <= 0)
            return NULL;
        if (i == group_count + GLR_AUDIO_OFF_SEP)  return "---";
        if (i == group_count + GLR_AUDIO_OFF_PLAY)
            /* Read the same GLR_CONFIG_AUDIO_MODE the Play/Pause toggle
             * writes, so the label always names what a click will do
             * (on -> "Pause", off -> "Play"). Deriving it from the live
             * glr_audio_is_paused() playback state could disagree with
             * the config intent (e.g. web autoplay-gesture deferral). */
            return glr_config_get(GLR_CONFIG_AUDIO_MODE) ? "Pause" : "Play";
        if (i == group_count + GLR_AUDIO_OFF_BACK10) return "Jump Back 10s";
        if (i == group_count + GLR_AUDIO_OFF_FWD10)  return "Jump Forward 10s";
        if (i == group_count + GLR_AUDIO_OFF_NEXT)   return "Next Track";
        if (i == group_count + GLR_AUDIO_OFF_PREV)   return "Previous Track";
        if (i == group_count + GLR_AUDIO_OFF_LOOP) {
            snprintf(loop_label, sizeof(loop_label), "Loop: %s",
                     audio_loop_mode_label());
            return loop_label;
        }
        return NULL;
    }
    return NULL;
}

static const char *menu_item_shortcut(int menu_id, int i) {
    static char buf[KEYMAP_SHORTCUT_LABEL_MAX];
    if (menu_id == MENU_FILE && i == GLR_FILE_ITEM_SAVE_SCENE)
        return keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_SAVE), KM_MODS(GLR_SAVE), 0);
    if (menu_id == MENU_FILE && i == GLR_FILE_ITEM_SPLIT_DECL)
        return keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_SPLIT_DECL), KM_MODS(GLR_SPLIT_DECL), 0);
    if (menu_id == MENU_FILE && i == GLR_FILE_ITEM_QUIT)
        return keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_QUIT), KM_MODS(GLR_QUIT), 0);
    if (menu_id == MENU_SCENE) {
        int tag_count = repl_example_visible_tag_count();
        if (i == tag_count + SCENE_OFF_NEXT)
            return keymap_binding_to_string(buf, (int)sizeof(buf),
                                            KM_KEY(GLR_NEXT_EXAMPLE),
                                            KM_MODS(GLR_NEXT_EXAMPLE), 1);
        if (i == tag_count + SCENE_OFF_PREV)
            return keymap_binding_to_string(buf, (int)sizeof(buf),
                                            KM_KEY(GLR_PREV_EXAMPLE),
                                            KM_MODS(GLR_PREV_EXAMPLE), 1);
        return NULL;
    }
    if (menu_id == MENU_TUTORIALS)
        return NULL;
    if (menu_id == MENU_AUDIO) {
        int group_count = audio_visible_group_count();
        if (i == group_count + GLR_AUDIO_OFF_PLAY)
            return keymap_binding_to_string(buf, (int)sizeof(buf),
                                            KM_KEY(GLR_AUDIO),
                                            KM_MODS(GLR_AUDIO), 0);
        if (i == group_count + GLR_AUDIO_OFF_NEXT)
            return keymap_binding_to_string(buf, (int)sizeof(buf),
                                            KM_KEY(GLR_AUDIO_NEXT),
                                            KM_MODS(GLR_AUDIO_NEXT), 1);
        if (i == group_count + GLR_AUDIO_OFF_PREV)
            return keymap_binding_to_string(buf, (int)sizeof(buf),
                                            KM_KEY(GLR_AUDIO_PREV),
                                            KM_MODS(GLR_AUDIO_PREV), 1);
        return NULL;
    }
    /* Config: top-level rows are section parents — no shortcut at this
     * level; the per-item shortcut renders inside the flyout (Step 8). */
    (void)i;
    return NULL;
}

/* Format the keyboard shortcut for a g_cfg_items[] entry, or NULL. */
static const char *config_item_shortcut(const GlrConfigItem *item) {
    static char buf[KEYMAP_SHORTCUT_LABEL_MAX];
    if (!item || item->section_header || item->key == GLR_CONFIG_NONE)
        return NULL;
    if (item->key_code == 0) return NULL;
    return keymap_binding_to_string(buf, (int)sizeof(buf),
                                    item->key_code, item->modifiers,
                                    item->is_special);
}

#define CFG_STATE_MAX_CHARS 20

/* Fills `out` with the current state label, truncated to CFG_STATE_MAX_CHARS
 * (with a trailing ellipsis). Returns `out`. */
static const char *cfg_state_str(const UiRenderSnapshot *snap, int i, char *out, int out_size) {
    const char *s = "";
    const GlrConfigItem *item = glr_config_item_at(i);
    if (item && !item->section_header && item->key != GLR_CONFIG_NONE) {
        int val = snap ? snap->config_values[item->key] : glr_config_get(item->key);
        s = glr_config_state_name(item->key, val);
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
    int cp_x, cp_w, by, bh;
    ui_layout_menu_bar_rect(&cp_x, &by, &cp_w, &bh);
    if (row_y) *row_y = by;
    if (row_h) *row_h = bh;

    int x = cp_x + CODE_MARGIN_X;
    for (int i = 0; i < NUM_MENUS; i++) {
        if (!menu_visible(i)) {
            menu_x[i] = x;
            menu_w[i] = 0;
            continue;
        }
        int label_w = (int)strlen(g_menu_labels[i]) * FONT_SMALL_W;
        menu_w[i] = label_w + MENU_LABEL_PAD_X;
        menu_x[i] = x;
        x += menu_w[i];
    }

    int right_edge = cp_x + cp_w - CODE_MARGIN_X;

    /* PIN_REPLAY - width reserves room for the widest state label plus a
     * 12px state icon (triangle / pause-bars) and padding. */
    int replay_label_w = (int)strlen("Replaying") * FONT_SMALL_W;
    pin_w[PIN_REPLAY] = replay_label_w + 12 /* icon */ + 22 /* pads */;
    pin_x[PIN_REPLAY] = right_edge - pin_w[PIN_REPLAY];

    /* PIN_VIEW_MODE - the 2D/3D toggle swatch, fixed width, left of Replay. */
    pin_w[PIN_VIEW_MODE] = ui_view_mode_swatch_label_width();
    pin_x[PIN_VIEW_MODE] = pin_x[PIN_REPLAY] - pin_w[PIN_VIEW_MODE];

    /* PIN_SEARCH - fills the gap between the last menu and PIN_VIEW_MODE. */
    int menus_right = x;
    int search_w = pin_x[PIN_VIEW_MODE] - menus_right;
    if (search_w < PIN_SEARCH_MIN_W) search_w = PIN_SEARCH_MIN_W;
    pin_w[PIN_SEARCH] = search_w;
    pin_x[PIN_SEARCH] = pin_x[PIN_VIEW_MODE] - search_w;
}

static int ui_menu_bar_menu_hit(int gx, int gy) {
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    /* Window events arrive with a top-down y; UI hit rects use the
     * bottom-up OpenGL space tracked in UiViewportState. */
    int ry = ui_state_viewport().window_h - gy;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
    if (ry < by || ry >= by + bh) return -1;
    for (int i = 0; i < NUM_MENUS; i++) {
        if (!menu_visible(i)) continue;
        if (gx >= menu_x[i] && gx < menu_x[i] + menu_w[i]) return i;
    }
    return -1;
}

static int ui_menu_bar_pin_hit(int gx, int gy) {
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

static struct {
    int valid, menu, win_w, win_h;
    int x, y, w, h;
} g_dropdown_cache;

/* Geometry of the open menu's dropdown (GL coords, below the bar):
 * width sized to the widest label + optional shortcut / submenu-arrow
 * columns, height to the row count. Render, hover, and hit-test all
 * call this every event, so the result is cached per
 * (menu, window size); opening/closing a menu resets the cache, which
 * also covers label changes (they only happen alongside a menu
 * transition). Returns 0 when no menu is open. */
static int menu_dropdown_rect(int *dx, int *dy, int *dw, int *dh) {
    int win_w, win_h;
    if (g_open_menu < 0 || !menu_visible(g_open_menu)) return 0;
    if (!ui_menu_bar_panel_visible()) return 0;

    win_w = ui_state_viewport().window_w;
    win_h = ui_state_viewport().window_h;

    if (g_dropdown_cache.valid &&
        g_dropdown_cache.menu == g_open_menu &&
        g_dropdown_cache.win_w == win_w &&
        g_dropdown_cache.win_h == win_h) {
        if (dx) *dx = g_dropdown_cache.x;
        if (dy) *dy = g_dropdown_cache.y;
        if (dw) *dw = g_dropdown_cache.w;
        if (dh) *dh = g_dropdown_cache.h;
        return 1;
    }

    {
        int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
        int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
        int by, bh;
        int n, max_lbl, max_sc, has_submenu, max_w, width, rows, height;
        menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
        n = menu_item_count(g_open_menu, NULL);

        max_lbl = 0; max_sc = 0; has_submenu = 0;
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
        max_w = max_lbl;
        if (max_sc > 0)    max_w += max_sc + DROPDOWN_SC_GAP;
        if (has_submenu) max_w += SUBMENU_ARROW_COL;
        if (max_w < 80) max_w = 80;
        width  = max_w + DROPDOWN_PAD_X;
        rows   = (n > 0) ? n : 1;
        height = rows * LINE_H + 2 * DROPDOWN_PAD_Y;

        g_dropdown_cache.valid = 1;
        g_dropdown_cache.menu  = g_open_menu;
        g_dropdown_cache.win_w = win_w;
        g_dropdown_cache.win_h = win_h;
        g_dropdown_cache.x = menu_x[g_open_menu];
        g_dropdown_cache.y = by - height;
        g_dropdown_cache.w = width;
        g_dropdown_cache.h = height;
    }

    if (dx) *dx = g_dropdown_cache.x;
    if (dy) *dy = g_dropdown_cache.y;
    if (dw) *dw = g_dropdown_cache.w;
    if (dh) *dh = g_dropdown_cache.h;
    return 1;
}

static int point_in_rect_gl(int mx, int my, int x, int y, int w, int h) {
    int ry = ui_state_viewport().window_h - my;
    return mx >= x && mx < x + w && ry >= y && ry < y + h;
}

/* Map a GL-space y coordinate (`ry`, i.e. window_h - my) inside a
 * dropdown/submenu rect (`top`, `h`) to its 0-based row ordinal.
 * The formula counts down from the top of the inner row band — the
 * caller is responsible for bounds-checking the result against the
 * actual row count. Shared by `ui_menu_bar_dropdown_item_hit`,
 * `submenu_hit_test`, and `submenu_hover_ordinal` so the three
 * sites stay in lockstep. */
static int dropdown_row_for_gl_y(int top, int h, int gl_y) {
    return (top + h - DROPDOWN_PAD_Y - gl_y) / LINE_H;
}

/* Number of rows that fit in a flyout of GL-space height `sh` — the
 * inverse of the `height = rows * LINE_H + 2 * DROPDOWN_PAD_Y` formula
 * submenu_rect uses. At least 1. */
static int submenu_visible_rows(int sh) {
    int rows = (sh - 2 * DROPDOWN_PAD_Y) / LINE_H;
    return rows < 1 ? 1 : rows;
}

/* The persistent flyout scroll offset (g_submenu_scroll) clamped into
 * [0, count - visible] for a flyout of height `sh` with `count` rows,
 * WITHOUT writing it back. Render and hit-test read through this so a
 * stale offset (e.g. after a resize shrinks the window) self-corrects
 * without those pure paths mutating chrome state; the wheel handler
 * clamps and persists separately. */
static int submenu_effective_scroll(int sh, int count) {
    int max_scroll = count - submenu_visible_rows(sh);
    int s = g_submenu_scroll;
    if (max_scroll < 0) max_scroll = 0;
    if (s > max_scroll) s = max_scroll;
    if (s < 0)          s = 0;
    return s;
}

/* ---- Polymorphic flyout provider ----------------------------------------
 *
 * A FlyoutProvider resolves (parent_row, ordinal) to a submenu row's
 * count, label, absolute index, and kind. Three static instances cover
 * Scene (examples), Tutorials, and Config; the four submenu_row_*
 * dispatchers resolve a menu_id to its provider and delegate. */

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

typedef struct {
    int              (*row_count)    (int parent_row);
    const char *     (*row_label)    (int parent_row, int ordinal);
    int              (*row_abs_index)(int parent_row, int ordinal);
    GlrConfigRowKind (*row_kind)     (int parent_row, int ordinal);
} FlyoutProvider;

/* --- Scene (examples) provider --- */

static int scene_flyout_row_count(int parent_row) {
    return catalog_flyout_row_count(&kExampleCatalogOps,
                                    scene_tag_idx_for_parent_row(parent_row));
}
static const char *scene_flyout_row_label(int parent_row, int ordinal) {
    const char *label = NULL;
    catalog_flyout_row_at(&kExampleCatalogOps,
                          scene_tag_idx_for_parent_row(parent_row),
                          ordinal, NULL, NULL, &label);
    return label;
}
static int scene_flyout_row_abs_index(int parent_row, int ordinal) {
    int abs_idx = -1;
    catalog_flyout_row_at(&kExampleCatalogOps,
                          scene_tag_idx_for_parent_row(parent_row),
                          ordinal, NULL, &abs_idx, NULL);
    return abs_idx;
}
static GlrConfigRowKind scene_flyout_row_kind(int parent_row, int ordinal) {
    GlrConfigRowKind kind = GLR_CFG_ROW_ITEM;
    catalog_flyout_row_at(&kExampleCatalogOps,
                          scene_tag_idx_for_parent_row(parent_row),
                          ordinal, &kind, NULL, NULL);
    return kind;
}

static const FlyoutProvider kSceneProvider = {
    .row_count     = scene_flyout_row_count,
    .row_label     = scene_flyout_row_label,
    .row_abs_index = scene_flyout_row_abs_index,
    .row_kind      = scene_flyout_row_kind,
};

/* --- Tutorials provider --- */

static int tutorial_flyout_row_count(int parent_row) {
    return catalog_flyout_row_count(&kTutorialCatalogOps,
                                    tutorial_tag_idx_for_parent_row(parent_row));
}
static const char *tutorial_flyout_row_label(int parent_row, int ordinal) {
    const char *label = NULL;
    catalog_flyout_row_at(&kTutorialCatalogOps,
                          tutorial_tag_idx_for_parent_row(parent_row),
                          ordinal, NULL, NULL, &label);
    return label;
}
static int tutorial_flyout_row_abs_index(int parent_row, int ordinal) {
    int abs_idx = -1;
    catalog_flyout_row_at(&kTutorialCatalogOps,
                          tutorial_tag_idx_for_parent_row(parent_row),
                          ordinal, NULL, &abs_idx, NULL);
    return abs_idx;
}
static GlrConfigRowKind tutorial_flyout_row_kind(int parent_row, int ordinal) {
    GlrConfigRowKind kind = GLR_CFG_ROW_ITEM;
    catalog_flyout_row_at(&kTutorialCatalogOps,
                          tutorial_tag_idx_for_parent_row(parent_row),
                          ordinal, &kind, NULL, NULL);
    return kind;
}

static const FlyoutProvider kTutorialProvider = {
    .row_count     = tutorial_flyout_row_count,
    .row_label     = tutorial_flyout_row_label,
    .row_abs_index = tutorial_flyout_row_abs_index,
    .row_kind      = tutorial_flyout_row_kind,
};

/* --- Audio provider --- */

static int audio_flyout_row_count(int parent_row) {
    return catalog_flyout_row_count(&kAudioCatalogOps, parent_row);
}
static const char *audio_flyout_row_label(int parent_row, int ordinal) {
    const char *label = NULL;
    catalog_flyout_row_at(&kAudioCatalogOps, parent_row,
                          ordinal, NULL, NULL, &label);
    return label;
}
static int audio_flyout_row_abs_index(int parent_row, int ordinal) {
    int abs_idx = -1;
    catalog_flyout_row_at(&kAudioCatalogOps, parent_row,
                          ordinal, NULL, &abs_idx, NULL);
    return abs_idx;
}
static GlrConfigRowKind audio_flyout_row_kind(int parent_row, int ordinal) {
    GlrConfigRowKind kind = GLR_CFG_ROW_ITEM;
    catalog_flyout_row_at(&kAudioCatalogOps, parent_row,
                          ordinal, &kind, NULL, NULL);
    return kind;
}

static const FlyoutProvider kAudioProvider = {
    .row_count     = audio_flyout_row_count,
    .row_label     = audio_flyout_row_label,
    .row_abs_index = audio_flyout_row_abs_index,
    .row_kind      = audio_flyout_row_kind,
};

/* --- Config provider --- */

static int config_flyout_row_count_fn(int parent_row) {
    if (parent_row == config_all_parent_row())
        return CFG_ITEM_COUNT;
    int start = 0, count = 0;
    if (!glr_config_section_range(parent_row, &start, &count))
        return 0;
    return count;
}
static const char *config_flyout_row_label_fn(int parent_row, int ordinal) {
    int abs = config_submenu_abs_index(parent_row, ordinal);
    const GlrConfigItem *item = glr_config_item_at(abs);
    if (!item || !item->label)
        return NULL;
    /* "### X" headers in the "All" flyout render with the marker
     * stripped, matching the old flat dropdown. */
    if (glr_config_row_kind(abs) == GLR_CFG_ROW_HEADER)
        return item->label + 4;
    return glr_config_item_display_label(item);
}
static int config_flyout_row_abs_index_fn(int parent_row, int ordinal) {
    return config_submenu_abs_index(parent_row, ordinal);
}
static GlrConfigRowKind config_flyout_row_kind_fn(int parent_row,
                                                   int ordinal) {
    return glr_config_row_kind(
        config_submenu_abs_index(parent_row, ordinal));
}

static const FlyoutProvider kConfigProvider = {
    .row_count     = config_flyout_row_count_fn,
    .row_label     = config_flyout_row_label_fn,
    .row_abs_index = config_flyout_row_abs_index_fn,
    .row_kind      = config_flyout_row_kind_fn,
};

/* Resolve menu_id to its FlyoutProvider, or NULL for menus without
 * submenus (File). */
static const FlyoutProvider *flyout_provider_for(int menu_id) {
    if (menu_id == MENU_SCENE)     return &kSceneProvider;
    if (menu_id == MENU_TUTORIALS) return &kTutorialProvider;
    if (menu_id == MENU_CONFIG)    return &kConfigProvider;
    if (menu_id == MENU_AUDIO)     return &kAudioProvider;
    return NULL;
}

/* ---- Unified submenu_row_* dispatchers -------------------------------- */

static int submenu_row_count(int menu_id, int parent_row) {
    const FlyoutProvider *p = flyout_provider_for(menu_id);
    return p ? p->row_count(parent_row) : 0;
}

static const char *submenu_row_label(int menu_id, int parent_row,
                                     int ordinal) {
    const FlyoutProvider *p = flyout_provider_for(menu_id);
    return p ? p->row_label(parent_row, ordinal) : NULL;
}

/* Absolute target index the row activates: a global flat example index
 * for Scene, a global tutorial index for Tutorials, a playlist index
 * for Audio (-1 for in-flyout subheading header rows), or a
 * g_cfg_items[] index for Config. */
static int submenu_row_abs_index(int menu_id, int parent_row,
                                 int ordinal) {
    const FlyoutProvider *p = flyout_provider_for(menu_id);
    return p ? p->row_abs_index(parent_row, ordinal) : -1;
}

static GlrConfigRowKind submenu_row_kind(int menu_id, int parent_row,
                                         int ordinal) {
    const FlyoutProvider *p = flyout_provider_for(menu_id);
    return p ? p->row_kind(parent_row, ordinal) : GLR_CFG_ROW_ITEM;
}

/* Extra right-column px a flyout reserves for per-row chrome: Config
 * uses shortcut + state label; Audio uses elapsed / duration. */
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

static int submenu_extra_w(int menu_id, int parent_row) {
    if (menu_id == MENU_CONFIG) {
        int max_sc = config_submenu_max_sc_px(parent_row);
        int extra = cfg_max_state_chars() * FONT_SMALL_W + 20;
        if (max_sc > 0)
            extra += max_sc + DROPDOWN_SC_GAP;
        return extra;
    }
    if (menu_id == MENU_AUDIO) {
        (void)parent_row;
        return 16 * FONT_SMALL_W + 20;
    }
    return 0;
}

/* Does dropdown row `row` of `menu_id` own a flyout submenu? */
static int menu_row_has_submenu(int menu_id, int row) {
    if (row < 0)
        return 0;
    return submenu_row_count(menu_id, row) > 0;
}

static struct {
    int valid, menu_id, parent_row, win_w, win_h;
    int x, y, w, h;
} g_submenu_cache;

/* Geometry of the flyout hanging off `parent_row` of the open
 * dropdown (GL coords). The branching is all placement policy: width
 * from the widest row label, height clamped to the viewport (overflow
 * rows scroll via g_submenu_scroll), x beside the dropdown but flipped
 * to its left edge when it would run off-screen, y top-aligned with
 * the parent row then clamped into the window. Cached per
 * (menu_id, parent_row, window size) like menu_dropdown_rect.
 * Returns 0 when the row has no flyout or the menu isn't open. */
static int submenu_rect(int menu_id, int parent_row,
                        int *sx, int *sy, int *sw, int *sh) {
    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;

    if (menu_id < 0 || g_open_menu != menu_id || win_w <= 0 || win_h <= 0)
        return 0;
    /* Mirror menu_dropdown_rect's panel-visible guard so a stale cache
     * doesn't hand back a rect after the code panel has been hidden.
     * The cache key (menu_id, parent_row, win_w, win_h) doesn't see
     * visibility transitions, so the check has to live before the hit. */
    if (!ui_menu_bar_panel_visible())
        return 0;
    if (!menu_row_has_submenu(menu_id, parent_row))
        return 0;

    if (g_submenu_cache.valid &&
        g_submenu_cache.menu_id == menu_id &&
        g_submenu_cache.parent_row == parent_row &&
        g_submenu_cache.win_w == win_w &&
        g_submenu_cache.win_h == win_h) {
        if (sx) *sx = g_submenu_cache.x;
        if (sy) *sy = g_submenu_cache.y;
        if (sw) *sw = g_submenu_cache.w;
        if (sh) *sh = g_submenu_cache.h;
        return 1;
    }

    {
        int pdx, pdy, pdw, pdh;
        int count, max_lbl, width, height, x, y;

        if (!menu_dropdown_rect(&pdx, &pdy, &pdw, &pdh))
            return 0;

        count = submenu_row_count(menu_id, parent_row);
        max_lbl = 0;
        for (int ordinal = 0; ordinal < count; ordinal++) {
            const char *name = submenu_row_label(menu_id, parent_row, ordinal);
            int w = (int)(name ? strlen(name) : 0) * FONT_SMALL_W;
            if (w > max_lbl)
                max_lbl = w;
        }
        if (max_lbl < 80)
            max_lbl = 80;
        width = max_lbl + DROPDOWN_PAD_X + submenu_extra_w(menu_id, parent_row);
        {
            /* Clamp tall flyouts (the Config "All" list is ~47 rows,
             * taller than an 800px window) to the viewport so every row
             * stays on-screen; the mouse wheel pages through the
             * overflow (g_submenu_scroll). */
            int max_rows = (win_h - 2 * DROPDOWN_PAD_Y) / LINE_H;
            int rows = count;
            if (max_rows < 1) max_rows = 1;
            if (rows > max_rows) rows = max_rows;
            height = rows * LINE_H + 2 * DROPDOWN_PAD_Y;
        }

        x = pdx + pdw;
        if (x + width > win_w)
            x = pdx - width;
        if (x < 0)
            x = 0;

        {
            int parent_row_top = pdy + pdh - DROPDOWN_PAD_Y - parent_row * LINE_H;
            y = parent_row_top - height;
        }
        if (y < 0)
            y = 0;
        if (y + height > win_h)
            y = win_h - height;
        if (y < 0)
            y = 0;

        g_submenu_cache.valid      = 1;
        g_submenu_cache.menu_id    = menu_id;
        g_submenu_cache.parent_row = parent_row;
        g_submenu_cache.win_w      = win_w;
        g_submenu_cache.win_h      = win_h;
        g_submenu_cache.x = x;
        g_submenu_cache.y = y;
        g_submenu_cache.w = width;
        g_submenu_cache.h = height;
    }

    if (sx) *sx = g_submenu_cache.x;
    if (sy) *sy = g_submenu_cache.y;
    if (sw) *sw = g_submenu_cache.w;
    if (sh) *sh = g_submenu_cache.h;
    return 1;
}

int ui_menu_bar_submenu_rect_for_test(int menu_id, int parent_row,
                                      int *sx, int *sy,
                                      int *sw, int *sh) {
    return submenu_rect(menu_id, parent_row, sx, sy, sw, sh);
}

const char *ui_menu_bar_menu_item_shortcut_for_test(int menu_id, int item_idx) {
    return menu_item_shortcut(menu_id, item_idx);
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

static int ui_menu_bar_dropdown_item_hit(int gx, int gy) {
    if (g_open_menu < 0) return -1;
    int n = menu_item_count(g_open_menu, NULL);
    if (n == 0) return -1;
    int dx, dy, dw, dh;
    if (!menu_dropdown_rect(&dx, &dy, &dw, &dh)) return -1;
    int ry = ui_state_viewport().window_h - gy;
    if (gx < dx || gx >= dx + dw || ry < dy || ry >= dy + dh) return -1;
    int row = dropdown_row_for_gl_y(dy, dh, ry);
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
    int ordinal;

    if (g_submenu_menu_id < 0 || g_submenu_parent_row < 0)
        return h;
    if (!submenu_rect(g_submenu_menu_id, g_submenu_parent_row,
                      &sx, &sy, &sw, &sh))
        return h;
    if (!point_in_rect_gl(mx, my, sx, sy, sw, sh))
        return h;

    {
        int count = submenu_row_count(g_submenu_menu_id, g_submenu_parent_row);
        int ry = ui_state_viewport().window_h - my;
        ordinal = dropdown_row_for_gl_y(sy, sh, ry) +
                  submenu_effective_scroll(sh, count);
        if (ordinal < 0 || ordinal >= count)
            return h;
    }

    /* Inert chrome rows (Config "All" "### "/"---") never produce a hit. */
    if (submenu_row_kind(g_submenu_menu_id, g_submenu_parent_row,
                         ordinal) != GLR_CFG_ROW_ITEM)
        return h;

    submenu_fill_hit(&h, g_submenu_menu_id, g_submenu_parent_row,
                     ordinal, mx, my, sx, sy);
    return h;
}

/* ---- Symbolic-target geometry queries (see menu_bar.h) ------------------ */

/* Normalized prefix match for pointer-script target labels: needle chars
 * compare case-insensitively against the label's start, with '_' in the
 * needle standing in for ' ' (targets are single tokens in the script
 * grammar, so spaces can't appear literally). */
static int target_label_matches(const char *label, const char *needle) {
    if (!label || !needle || !*needle)
        return 0;
    while (*needle) {
        char n = (char)tolower((unsigned char)*needle);
        char l = (char)tolower((unsigned char)*label);
        if (n == '_') n = ' ';
        if (n != l)
            return 0;
        needle++;
        label++;
    }
    return 1;
}

/* Mouse-space y for a GL-space y (the inverse of every hit path's
 * `window_h - my`). */
static int target_mouse_y(int gl_y) {
    return ui_state_viewport().window_h - gl_y;
}

int ui_menu_bar_target_menu(const char *name, int *mx, int *my) {
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
    for (int i = 0; i < NUM_MENUS; i++) {
        if (!menu_visible(i)) continue;
        if (!target_label_matches(g_menu_labels[i], name)) continue;
        if (mx) *mx = menu_x[i] + menu_w[i] / 2;
        if (my) *my = target_mouse_y(by + bh / 2);
        return 1;
    }
    return 0;
}

int ui_menu_bar_target_pin(const char *name, int *mx, int *my) {
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    int pin = -1;
    if (target_label_matches("search", name))      pin = PIN_SEARCH;
    else if (target_label_matches("view", name))   pin = PIN_VIEW_MODE;
    else if (target_label_matches("replay", name)) pin = PIN_REPLAY;
    if (pin < 0)
        return 0;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
    if (mx) *mx = pin_x[pin] + pin_w[pin] / 2;
    if (my) *my = target_mouse_y(by + bh / 2);
    return 1;
}

/* Row index of the open dropdown whose label matches `name`, or -1.
 * Chrome rows ("---", "### " headers) are skipped so a needle like
 * "examples" can't land on the inert "### EXAMPLES" header. */
static int target_open_row_by_label(const char *name) {
    if (g_open_menu < 0)
        return -1;
    int n = menu_item_count(g_open_menu, NULL);
    for (int i = 0; i < n; i++) {
        const char *lbl = menu_item_label(g_open_menu, i);
        if (!lbl || menu_chrome_kind(lbl) != GLR_CFG_ROW_ITEM)
            continue;
        if (target_label_matches(lbl, name))
            return i;
    }
    return -1;
}

/* GL-space center y of open-dropdown row `row` (inverse of
 * dropdown_row_for_gl_y). */
static int dropdown_row_center_gl_y(int top, int h, int row) {
    return top + h - DROPDOWN_PAD_Y - row * LINE_H - LINE_H / 2;
}

int ui_menu_bar_target_open_row(const char *name, int *mx, int *my) {
    int dx, dy, dw, dh;
    int row = target_open_row_by_label(name);
    if (row < 0 || !menu_dropdown_rect(&dx, &dy, &dw, &dh))
        return 0;
    if (mx) *mx = dx + dw / 2;
    if (my) *my = target_mouse_y(dropdown_row_center_gl_y(dy, dh, row));
    return 1;
}

int ui_menu_bar_target_flyout_entry(const char *parent, int *mx, int *my) {
    int dx, dy, dw, dh, sx, sy, sw, sh;
    int parent_row = target_open_row_by_label(parent);
    if (parent_row < 0 ||
        !menu_dropdown_rect(&dx, &dy, &dw, &dh) ||
        !submenu_rect(g_open_menu, parent_row, &sx, &sy, &sw, &sh))
        return 0;
    /* Enter just inside the flyout edge that faces the dropdown (the
     * flyout flips to the dropdown's left when it would run off-screen),
     * at the parent row's y clamped into the flyout's row band. */
    {
        int gl_y = dropdown_row_center_gl_y(dy, dh, parent_row);
        int lo = sy + DROPDOWN_PAD_Y + LINE_H / 2;
        int hi = sy + sh - DROPDOWN_PAD_Y - LINE_H / 2;
        if (gl_y < lo) gl_y = lo;
        if (gl_y > hi) gl_y = hi;
        if (mx) *mx = (sx > dx) ? sx + 16 : sx + sw - 16;
        if (my) *my = target_mouse_y(gl_y);
    }
    return 1;
}

int ui_menu_bar_target_flyout_row(const char *parent, const char *name,
                                  int *mx, int *my) {
    int sx, sy, sw, sh;
    int parent_row = target_open_row_by_label(parent);
    if (parent_row < 0 ||
        !submenu_rect(g_open_menu, parent_row, &sx, &sy, &sw, &sh))
        return 0;
    {
        int count = submenu_row_count(g_open_menu, parent_row);
        int scroll = submenu_effective_scroll(sh, count);
        for (int o = 0; o < count; o++) {
            const char *lbl;
            int gl_y;
            if (submenu_row_kind(g_open_menu, parent_row, o) !=
                GLR_CFG_ROW_ITEM)
                continue;
            lbl = submenu_row_label(g_open_menu, parent_row, o);
            if (!lbl || !target_label_matches(lbl, name))
                continue;
            gl_y = dropdown_row_center_gl_y(sy, sh, o - scroll);
            /* Scrolled out of the clamped flyout window -> unreachable. */
            if (gl_y < sy || gl_y >= sy + sh)
                return 0;
            if (mx) *mx = sx + sw / 2;
            if (my) *my = target_mouse_y(gl_y);
            return 1;
        }
    }
    return 0;
}

static UiHit inert_chrome_hit(int mx, int gl_y) {
    UiHit h = ui_hit_none();
    h.kind = UI_HIT_CODE_PANEL_CHROME;
    h.local_x = (float)mx;
    h.local_y = (float)gl_y;
    return h;
}

UiHit ui_menu_bar_hit_test(int mx, int my) {
    UiHit h = ui_hit_none();
    int win_h = ui_state_viewport().window_h;
    if (win_h <= 0) return h;
    int gl_y = win_h - my;

    if (g_open_menu >= 0) {
        if (g_submenu_menu_id == g_open_menu) {
            UiHit submenu_hit = submenu_hit_test(mx, my);
            if (submenu_hit.kind != UI_HIT_NONE)
                return submenu_hit;
            int ssx, ssy, ssw, ssh;
            if (submenu_rect(g_submenu_menu_id, g_submenu_parent_row,
                             &ssx, &ssy, &ssw, &ssh) &&
                point_in_rect_gl(mx, my, ssx, ssy, ssw, ssh))
                return inert_chrome_hit(mx, gl_y);
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
        int dx, dy, dw, dh;
        if (menu_dropdown_rect(&dx, &dy, &dw, &dh) &&
            mx >= dx && mx < dx + dw && gl_y >= dy && gl_y < dy + dh)
            return inert_chrome_hit(mx, gl_y);
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
    g_submenu_scroll     = 0;
    g_submenu_cache.valid = 0;
}

void ui_menu_bar_close(void) {
    g_open_menu = -1;
    g_menu_item_hover = -1;
    g_dropdown_cache.valid = 0;
    g_submenu_cache.valid  = 0;
    submenu_reset();
}

void ui_menu_bar_set_open_menu(int menu_id, float now) {
    if (menu_id < 0 || menu_id >= NUM_MENUS || !menu_visible(menu_id)) {
        ui_menu_bar_close();
        return;
    }
    g_open_menu = menu_id;
    g_menu_open_time = now;
    g_menu_item_hover = -1;
    g_dropdown_cache.valid = 0;
    submenu_reset();
}

UiMenuBarOpenState ui_menu_bar_open_state_capture(void) {
    UiMenuBarOpenState st;
    st.menu_id    = g_open_menu;
    st.open_time  = g_menu_open_time;
    st.item_hover = g_menu_item_hover;
    return st;
}

void ui_menu_bar_open_state_restore(UiMenuBarOpenState state) {
    /* Nothing was open, or the menu is no longer showable — leave closed. */
    if (state.menu_id < 0 || state.menu_id >= NUM_MENUS ||
        !menu_visible(state.menu_id)) {
        ui_menu_bar_close();
        return;
    }
    g_open_menu       = state.menu_id;
    g_menu_open_time  = state.open_time;   /* preserve fade clock (no re-flash) */
    g_menu_item_hover = state.item_hover;  /* preserve hover highlight */
    g_dropdown_cache.valid = 0;            /* geometry may have shifted */
    submenu_reset();
}

void ui_menu_bar_open_config(float now) {
    if (g_open_menu == MENU_CONFIG) {
        ui_menu_bar_close();
        return;
    }
    ui_menu_bar_set_open_menu(MENU_CONFIG, now);
}

int ui_menu_bar_handle_wheel_scroll(int mx, int my, int delta) {
    int sx, sy, sw, sh, count, max_scroll;
    if (g_open_menu < 0 ||
        g_submenu_menu_id < 0 || g_submenu_parent_row < 0)
        return 0;
    if (!submenu_rect(g_submenu_menu_id, g_submenu_parent_row,
                      &sx, &sy, &sw, &sh))
        return 0;
    if (!point_in_rect_gl(mx, my, sx, sy, sw, sh))
        return 0;
    /* Pointer is over the open flyout: consume the wheel so it never
     * leaks to the code panel / camera behind the menu, even when the
     * flyout is short enough that there is nothing to scroll. `delta` is
     * a row offset (positive reveals lower rows), matching the
     * editor_input_code_panel_scroll convention the call sites use. */
    count = submenu_row_count(g_submenu_menu_id, g_submenu_parent_row);
    max_scroll = count - submenu_visible_rows(sh);
    if (max_scroll < 0) max_scroll = 0;
    g_submenu_scroll += delta;
    if (g_submenu_scroll < 0)          g_submenu_scroll = 0;
    if (g_submenu_scroll > max_scroll) g_submenu_scroll = max_scroll;
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
    if (menu_id == MENU_TUTORIALS) {
        if (!snap || !snap->tutorial.active)
            return 0;
        /* submenu_row_abs_index returns -1 for subheading header rows
         * (and any out-of-range ordinal), so the `>= 0` check is the
         * single guard that skips them. */
        int tutorial_idx = submenu_row_abs_index(menu_id, parent_row,
                                                 ordinal);
        return tutorial_idx >= 0 &&
               tutorial_idx == snap->tutorial.tutorial_idx;
    }
    if (menu_id == MENU_AUDIO) {
        int track_idx = submenu_row_abs_index(menu_id, parent_row,
                                              ordinal);
        return track_idx >= 0 &&
               track_idx == glr_audio_current_index();
    }
    return 0;
}

static int submenu_hover_ordinal(const UiRenderSnapshot *snap) {
    int sx, sy, sw, sh;
    int ordinal;

    if (!submenu_rect(g_submenu_menu_id, g_submenu_parent_row,
                      &sx, &sy, &sw, &sh))
        return -1;
    if (!point_in_rect_gl(snap->pointer.mouse_x, snap->pointer.mouse_y,
                          sx, sy, sw, sh))
        return -1;

    {
        int count = submenu_row_count(g_submenu_menu_id, g_submenu_parent_row);
        int ry = snap->viewport.window_h - snap->pointer.mouse_y;
        ordinal = dropdown_row_for_gl_y(sy, sh, ry) +
                  submenu_effective_scroll(sh, count);
        if (ordinal < 0 || ordinal >= count)
            return -1;
    }
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
            g_submenu_parent_row != g_menu_item_hover) {
            /* Switching to a different flyout — restart it at the top. */
            g_submenu_open_time = now;
            g_submenu_scroll    = 0;
        }
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

static void paint_config_row_columns(const UiRenderSnapshot *snap,
                                     int parent_row, int ordinal,
                                     int ey, int cfg_state_right,
                                     int cfg_sc_right, float alpha) {
    int abs = config_submenu_abs_index(parent_row, ordinal);
    const GlrConfigItem *item = glr_config_item_at(abs);
    char st_buf[CFG_STATE_MAX_CHARS + 1];
    const char *st;
    int st_px, val;
    const char *scut;

    if (!item || item->section_header || item->key == GLR_CONFIG_NONE)
        return;

    st = cfg_state_str(snap, abs, st_buf, sizeof(st_buf));
    st_px = (int)strlen(st) * FONT_SMALL_W;
    scut = config_item_shortcut(item);

    if (scut) {
        int sc_px = (int)strlen(scut) * FONT_SMALL_W;
        ui_clr_a(UI_TOK_TEXT_MUTED, alpha);
        gl2d_draw_string((float)(cfg_sc_right - sc_px),
                         (float)ey, scut, FONT_SMALL);
    }
    val = snap ? snap->config_values[item->key] : glr_config_get(item->key);
    if (val)
        ui_clr_a(UI_TOK_ACCENT, alpha);
    else
        ui_clr_a(UI_TOK_TEXT_MUTED, alpha);
    gl2d_draw_string((float)(cfg_state_right - st_px),
                     (float)ey, st, FONT_SMALL);
}

static void audio_format_time(float seconds, char *out, int out_size) {
    int total, mins, secs;
    if (!out || out_size <= 0)
        return;
    if (seconds < 0.0f) {
        snprintf(out, (size_t)out_size, "--:--");
        return;
    }
    total = (int)(seconds + 0.5f);
    if (total < 0)
        total = 0;
    mins = total / 60;
    secs = total % 60;
    snprintf(out, (size_t)out_size, "%d:%02d", mins, secs);
}

static void paint_audio_row_columns(int track_idx, int ey, int right,
                                    int on_hilite, float alpha) {
    char elapsed[16];
    char duration[16];
    char label[40];
    int label_px;

    if (track_idx < 0 || track_idx != glr_audio_current_index())
        return;

    audio_format_time(glr_audio_current_cursor_seconds(),
                      elapsed, (int)sizeof(elapsed));
    audio_format_time(glr_audio_track_duration_seconds(track_idx),
                      duration, (int)sizeof(duration));
    snprintf(label, sizeof(label), "%s / %s", elapsed, duration);
    label_px = (int)strlen(label) * FONT_SMALL_W;

    if (on_hilite)
        ui_clr_a(UI_TOK_TEXT_ON_HILITE, alpha);
    else
        ui_clr_a(UI_TOK_TEXT_MUTED, alpha);
    gl2d_draw_string((float)(right - label_px), (float)ey, label, FONT_SMALL);
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
                              - (cfg_max_sc > 0 ? cfg_max_sc + DROPDOWN_SC_GAP : 0);
    int audio_time_right = sx + sw - 14;

    ui_clr_a(UI_TOK_RAISED, 0.98f * alpha);
    glRectf((float)sx, (float)sy, (float)(sx + sw), (float)(sy + sh));
    ui_clr_a(UI_TOK_BORDER, alpha);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)sx,        (float)sy);
    glVertex2f((float)(sx + sw), (float)sy);
    glVertex2f((float)(sx + sw), (float)(sy + sh));
    glVertex2f((float)sx,        (float)(sy + sh));
    glEnd();

    /* Draw only the rows in the visible window [scroll, last). hover_ordinal
     * is already absolute (submenu_hover_ordinal added the same offset), so
     * the row comparisons below stay correct. */
    int visible_rows = submenu_visible_rows(sh);
    int scroll       = submenu_effective_scroll(sh, count);
    int last         = scroll + visible_rows;
    if (last > count) last = count;

    ey = sy + sh - LINE_H + 1;
    for (int ordinal = scroll; ordinal < last; ordinal++) {
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

        if (menu_id == MENU_CONFIG)
            paint_config_row_columns(snap, parent_row, ordinal,
                                     ey, cfg_state_right,
                                     cfg_sc_right, alpha);
        if (menu_id == MENU_AUDIO)
            paint_audio_row_columns(submenu_row_abs_index(menu_id, parent_row,
                                                          ordinal),
                                    ey, audio_time_right,
                                    ordinal == hover_ordinal, alpha);

        ey -= LINE_H;
    }

    /* Overflow scrollbar hint: a thin track + proportional thumb on the
     * flyout's right inner edge, shown only when rows are hidden. Purely
     * a visual cue — the wheel does the scrolling, so there is no hit
     * region. Sits in the right padding (config columns end at sx+sw-14),
     * so it never overlaps row text. */
    if (count > visible_rows) {
        float bx1       = (float)(sx + sw - 2);
        float bx0       = bx1 - 3.0f;
        float track_top = (float)(sy + sh - DROPDOWN_PAD_Y);
        float track_bot = (float)(sy + DROPDOWN_PAD_Y);
        float track_h   = track_top - track_bot;
        float thumb_h   = track_h * (float)visible_rows / (float)count;
        float max_off, thumb_top;
        if (thumb_h < 10.0f)    thumb_h = 10.0f;
        if (thumb_h > track_h)  thumb_h = track_h;
        max_off   = track_h - thumb_h;
        thumb_top = track_top -
                    max_off * (float)scroll / (float)(count - visible_rows);
        ui_clr_a(UI_TOK_DIVIDER, alpha);
        glRectf(bx0, track_bot, bx1, track_top);
        ui_clr_a(UI_TOK_TEXT_MUTED, alpha);
        glRectf(bx0, thumb_top - thumb_h, bx1, thumb_top);
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

/* Find-bar match navigator: a small vertical stepper (up = previous
 * match, down = next, matching the Up/Down keys) shown at the right edge
 * of the search box whenever the query has matches. Reuses the generic
 * ui_stepper from numeric_swatch.h so render and hit-test share geometry. */
#define SEARCH_PAD_X       8
#define SEARCH_NAV_BTN_W   14.0f
#define SEARCH_NAV_BTN_H   8.0f
#define SEARCH_NAV_GAP     6   /* px between the count text and the stepper */

static int search_nav_geometry(EditorSearchState srch,
                               int box_x, int box_y, int box_w, int box_h,
                               float *out_x, float *out_cy) {
    if (srch.query_len <= 0 || srch.match_count <= 0)
        return 0;
    if (out_x)  *out_x  = (float)(box_x + box_w - SEARCH_PAD_X) - SEARCH_NAV_BTN_W;
    if (out_cy) *out_cy = (float)box_y + (float)box_h * 0.5f;
    return 1;
}

void ui_menu_bar_render_search_overlay(const UiRenderSnapshot *snap) {
    EditorSearchState srch = snap->search;
    char count_buf[32];
    char query_buf[128];
    int cursor_col = 0;

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

    int pad_x = SEARCH_PAD_X;
    int icon_r = 5;
    int icon_cx = box_x + pad_x + icon_r;
    int icon_cy = box_y + box_h / 2;
    int text_y  = box_y + (box_h - FONT_SMALL_H) / 2 + 1;
    float nav_x, nav_cy;
    int has_nav = search_nav_geometry(srch, box_x, box_y, box_w, box_h,
                                      &nav_x, &nav_cy);
    int nav_reserve = has_nav ? ((int)SEARCH_NAV_BTN_W + SEARCH_NAV_GAP) : 0;
    int count_w = (int)strlen(count_buf) * FONT_SMALL_W;
    int count_x = box_x + box_w - pad_x - nav_reserve - count_w;
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

    if (snap->cursor_blink.cursor_visible) {
        int cursor_x = query_x + cursor_col * FONT_SMALL_W;
        ui_clr_a(UI_TOK_CARET, 0.85f * alpha);
        glRectf((float)cursor_x, (float)(text_y - 2), (float)cursor_x + 2.0f,
                  (float)(text_y - 2) + (float)(FONT_SMALL_H + 2));
    }

    glDisable(GL_BLEND);

    /* Match stepper last: it brackets its own blend state. */
    if (has_nav)
        ui_stepper_render(nav_x, nav_cy, SEARCH_NAV_BTN_W, SEARCH_NAV_BTN_H);
}

UiHit ui_menu_bar_search_nav_hit_test(const UiRenderSnapshot *snap,
                                      int mx, int my) {
    UiHit h = ui_hit_none();
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh, win_h, dir;
    float nav_x, nav_cy;

    if (!snap || !snap->search.active)
        return h;

    win_h = snap->viewport.window_h;
    if (win_h <= 0)
        return h;

    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
    if (!search_nav_geometry(snap->search, pin_x[PIN_SEARCH], by,
                             pin_w[PIN_SEARCH], bh, &nav_x, &nav_cy))
        return h;

    dir = ui_stepper_hit(nav_x, nav_cy, SEARCH_NAV_BTN_W, SEARCH_NAV_BTN_H,
                         mx, (float)(win_h - my));
    if (dir == 0)
        return h;

    /* Up arrow (dir +1) = previous match → navigate(-1); down = next → +1. */
    h.kind = UI_HIT_SEARCH_NAV;
    h.item_idx = (dir > 0) ? -1 : +1;
    return h;
}


static void paint_menu_labels(const int *menu_x, const int *menu_w,
                              int by, int bh, int hover_menu) {
    int i;
    for (i = 0; i < NUM_MENUS; i++) {
        if (!menu_visible(i)) continue;
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
        {
            int tx = menu_x[i] + MENU_LABEL_PAD_X / 2;
            gl2d_draw_string((float)tx, (float)(by + MENUBAR_TEXT_BASE_Y),
                        g_menu_labels[i], FONT_SMALL);
        }
    }
}

static void paint_pin_buttons(const UiRenderSnapshot *snap,
                              const int *pin_x, const int *pin_w,
                              int by, int bh, int hover_pin,
                              ReplayRuntimeState replay) {
    int i;
    for (i = 0; i < NUM_PIN_BTNS; i++) {
        int hover = (hover_pin == i);
        int active = (i == PIN_REPLAY && replay.active);
        if (hover) {
            ui_clr(UI_TOK_MENU_LABEL_HOVER_BG);
            glRectf((float)pin_x[i], (float)by, (float)pin_x[i] + (float)pin_w[i], (float)by + (float)bh);
        } else if (active) {
            ui_clr(UI_TOK_MENU_LABEL_ACTIVE_BG);
            glRectf((float)pin_x[i], (float)by, (float)pin_x[i] + (float)pin_w[i], (float)by + (float)bh);
        }
        ui_clr(UI_TOK_DIVIDER);
        glBegin(GL_LINES);
        glVertex2f((float)pin_x[i], (float)by);
        glVertex2f((float)pin_x[i], (float)(by + bh));
        glEnd();

        if (i == PIN_SEARCH) {
            if (snap->search.active)
                continue;
            ui_clr(UI_TOK_TEXT_PLACEHOLDER);
            {
                int tx = pin_x[i] + 12;
                gl2d_draw_string((float)tx, (float)(by + MENUBAR_TEXT_BASE_Y),
                            g_pin_btn_labels[i], FONT_SMALL);
            }
        } else if (i == PIN_VIEW_MODE) {
            /* The loop prologue already drew the cell hover bg + left
             * divider; the swatch renders only its content (flat text /
             * cross-fade / lit cube), fully bracketing any 3D state. */
            ui_view_mode_swatch_render(pin_x[i], by, pin_w[i], bh,
                                       snap->view_ortho_mode,
                                       snap->view_projection_mix);
        } else if (i == PIN_REPLAY) {
            const char *label = "Replay";
            int icon_x = pin_x[i] + 10;
            int icon_cy = by + bh / 2;
            int icon_sz = 8;
            int tx;
            if (replay.state == REPLAY_PLAYING) label = "Replaying";
            else if (replay.state == REPLAY_PAUSED) label = "Paused";
            else if (replay.state == REPLAY_DONE)   label = "Done";

            ui_clr(UI_TOK_ACCENT);

            if (replay.state == REPLAY_PLAYING) {
                float bw = 2.5f, gap = 2.0f;
                float by0 = (float)icon_cy - (float)icon_sz * 0.5f;
                float bh0 = (float)icon_sz;
                glRectf((float)icon_x,            by0, (float)icon_x + bw, by0 + bh0);
                glRectf((float)icon_x + bw + gap, by0, (float)icon_x + bw + gap + bw, by0 + bh0);
            } else if (replay.state == REPLAY_DONE) {
                float sx = (float)icon_x;
                float sy = (float)icon_cy - (float)icon_sz * 0.5f;
                glRectf(sx, sy, sx + (float)icon_sz, sy + (float)icon_sz);
            } else {
                float x0 = (float)icon_x;
                float cy = (float)icon_cy;
                glBegin(GL_TRIANGLES);
                glVertex2f(x0,           cy - (float)icon_sz * 0.5f);
                glVertex2f(x0,           cy + (float)icon_sz * 0.5f);
                glVertex2f(x0 + icon_sz, cy);
                glEnd();
            }

            tx = icon_x + 12 + 6;
            gl2d_draw_string((float)tx, (float)(by + MENUBAR_TEXT_BASE_Y), label, FONT_SMALL);
        } else {
            if (hover || active)
                ui_clr(UI_TOK_TEXT_ON_HILITE);
            else
                ui_clr(UI_TOK_TEXT_PRIMARY);
            {
                int tx = pin_x[i] + MENU_LABEL_PAD_X / 2;
                gl2d_draw_string((float)tx, (float)(by + MENUBAR_TEXT_BASE_Y),
                            g_pin_btn_labels[i], FONT_SMALL);
            }
        }
    }
}

void ui_menu_bar_render(const UiRenderSnapshot *snap) {
    int cp_x, cp_y, cp_w, cp_h;
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    int hover_menu, hover_pin;
    int pin_block_x, pin_block_w;

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    (void)cp_y;
    if (cp_w <= 0 || cp_h <= 0) return;

    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    ui_clr_a(UI_TOK_SURFACE, 0.98f);
    glRectf((float)cp_x, (float)by, (float)cp_x + (float)cp_w, (float)by + (float)bh);

    hover_menu = ui_menu_bar_menu_hit(snap->pointer.mouse_x, snap->pointer.mouse_y);
    hover_pin  = ui_menu_bar_pin_hit(snap->pointer.mouse_x, snap->pointer.mouse_y);

    prof_begin(PROF_CODE_PANEL_OVERLAY_MENU_LABELS);
    paint_menu_labels(menu_x, menu_w, by, bh, hover_menu);
    prof_end(PROF_CODE_PANEL_OVERLAY_MENU_LABELS);

    pin_block_x = pin_x[PIN_SEARCH];
    pin_block_w = cp_x + cp_w - CODE_MARGIN_X - pin_block_x;
    ui_clr(UI_TOK_SURFACE);
    glRectf((float)pin_block_x, (float)by, (float)pin_block_x + (float)pin_block_w, (float)by + (float)bh);

    prof_begin(PROF_CODE_PANEL_OVERLAY_MENU_PINS);
    paint_pin_buttons(snap, pin_x, pin_w, by, bh, hover_pin, snap->replay);
    prof_end(PROF_CODE_PANEL_OVERLAY_MENU_PINS);

    glColor4fv(k_menubar_bottom_rule);
    glBegin(GL_LINES);
    glVertex2f((float)cp_x,          (float)by);
    glVertex2f((float)(cp_x + cp_w), (float)by);
    glEnd();

    glDisable(GL_BLEND);
}

void ui_menu_bar_render_example_dropdown(const UiRenderSnapshot *snap) {
    if (g_open_menu < 0) return;
    int menu_id = g_open_menu;
    int n  = menu_item_count(menu_id, snap);
    int tag_count = (menu_id == MENU_SCENE) ? (snap ? snap->example_visible_tag_count : repl_example_visible_tag_count()) : -1;

    int dx, dy, dw, dh;
    if (!menu_dropdown_rect(&dx, &dy, &dw, &dh)) return;



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
