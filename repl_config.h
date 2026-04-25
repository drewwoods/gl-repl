/*
 * repl_config.h - Configuration item descriptors and keyed state access.
 *
 * Declarative system for toggles and cycles (grid theme, axes, overlays,
 * rendering features, audio, etc.). Config items are immutable descriptors
 * (ReplConfigItem) registered in a table (g_cfg_items[]). The keyed facade
 * (ReplConfigKey enum) provides stable names for items across the codebase
 * (menu, help overlay, export/import, tests).
 *
 * Item types:
 *   - Toggles (state_count = 2): simple ON/OFF (MSAA, wireframe, auto-time, etc.)
 *   - Cycles (state_count > 2): rotate through N named states (grid themes,
 *     axes themes, replay mode, backdrop, audio mode, etc.). Each state has a
 *     display name (state_names[]).
 *   - Separators (section_header = 1): visual breaks in the Config menu.
 *
 * Keyboard bindings: Each item can have an ASCII key code (Ctrl+<key>) or GLUT
 * special key (is_special = 1). Used by repl_actions.c to dispatch keyboard
 * shortcuts (F1-F11 for overlays, Ctrl+U for MSAA, etc.).
 *
 * Export format: Export/import uses ReplConfigKey values in @cfg header lines
 * (e.g., "@cfg wireframe=1"), allowing examples and user scenes to override
 * defaults. Parsing rebuilds state from keys, not indices, so reordering items
 * doesn't break saved state.
 *
 * Adding an item: Append to ReplConfigKey enum (before REPL_CONFIG_COUNT),
 * then add a ReplConfigItem entry to g_cfg_items[] in repl_actions.c with
 * label, key binding, state_count, state_names[], and section_header flag.
 * CFG_ITEM_COUNT auto-computes via sizeof.
 */
#ifndef REPL_CONFIG_H
#define REPL_CONFIG_H

/* Stable config item identifiers. Used by menu dispatch, export/import,
 * help overlay, and tests to refer to config items by name rather than table
 * index. Items are in two groups: rendering features (MSAA, wireframe, etc.)
 * and visual overlays (grid, axes, vertices, normals, etc.). */
typedef enum ReplConfigKey {
    REPL_CONFIG_NONE = 0,
    REPL_CONFIG_MSAA,
    REPL_CONFIG_LINE_SMOOTH,
    REPL_CONFIG_ACCUM_AA,
    REPL_CONFIG_WIREFRAME,
    REPL_CONFIG_POINT_ATTENUATION,
    REPL_CONFIG_AUTO_TIME,
    REPL_CONFIG_REPLAY,
    REPL_CONFIG_REPLAY_MODE,
    REPL_CONFIG_REPLAY_EXPAND,
    REPL_CONFIG_GRID_THEME,
    REPL_CONFIG_GRID_MAJOR,
    REPL_CONFIG_GRID_EXTENT,
    REPL_CONFIG_AXES_THEME,
    REPL_CONFIG_VERTEX_GUIDES,
    REPL_CONFIG_XFORM_GUIDE_MODE,
    REPL_CONFIG_LIGHT_INDICATORS,
    REPL_CONFIG_POLY_HIGHLIGHT,
    REPL_CONFIG_BACKDROP,
    REPL_CONFIG_CAMERA_ROTATE,
    REPL_CONFIG_AUTO_NORMALS,
    REPL_CONFIG_VERTEX_LABELS,
    REPL_CONFIG_NORMAL_VECTORS,
    REPL_CONFIG_VERTEX_OUTLINES,
    REPL_CONFIG_VERTEX_POINTS,
    REPL_CONFIG_VARIABLE_PANEL,
    REPL_CONFIG_CPU_PROFILE,
    REPL_CONFIG_CODE_PANEL_LAYOUT,
    REPL_CONFIG_WRAP_AT_COMMA,
    REPL_CONFIG_AUDIO_MODE,
    REPL_CONFIG_COUNT
} ReplConfigKey;

/* Descriptor for a single config item (toggle or cycle). Specifies the menu
 * label, keyboard shortcut (ASCII key code or GLUT_KEY_* with is_special flag),
 * the stable ReplConfigKey, state count (2 for toggle, >2 for cycle with named
 * states), state name labels, and whether this item is a section header/separator.
 * Registered in g_cfg_items[] in repl_actions.c; count auto-computes via sizeof. */
typedef struct {
    const char  *label;
    int          key_code;      /* ASCII ctrl key or GLUT_KEY_* */
    int          is_special;     /* 1 for GLUT special keys */
    ReplConfigKey key;
    int          state_count;    /* 2 = toggle; >2 = cycle */
    const char **state_names;
    int          section_header; /* 1 for separator/header rows */
} ReplConfigItem;

/* Exported config item table and count. g_cfg_items[] is a static array in
 * repl_actions.c; CFG_ITEM_COUNT is the count (auto-computed via sizeof). Used
 * by the menu UI and config accessor functions to iterate items. */
extern const ReplConfigItem g_cfg_items[];
extern const int CFG_ITEM_COUNT;

/* Query all config items. Outputs the item array and count. Used by the menu
 * UI to render the Config dropdown and by help overlay to list all toggles. */
const ReplConfigItem *repl_config_items(int *count);

/* Query a specific config item by index (0 to CFG_ITEM_COUNT-1). Returns the
 * item descriptor or NULL if out of bounds. Used by menu rendering. */
const ReplConfigItem *repl_config_item_at(int idx);

/* Get the current state value of a config item. Returns 0/1 for toggles, 0-N
 * for cycles (index into state_names[]). Used by rendering (to highlight the
 * active state) and by logic that checks feature flags. */
int  repl_config_get(ReplConfigKey key);

/* Query the number of states for a config item. Returns 2 for toggles, >2 for
 * cycles. Used by UI to determine whether to render a toggle or cycle menu. */
int  repl_config_state_count(ReplConfigKey key);

/* Get the display name for a specific state value. Returns the string label
 * for state (index into state_names[]). Used by menu rendering and export
 * (to convert state value back to name for @cfg headers). */
const char *repl_config_state_name(ReplConfigKey key, int value);

/* Set a config item to a specific state. Applies the change immediately (e.g.,
 * enables GL_LIGHTING, changes grid theme, etc.). Called by menu dispatch and
 * keyboard shortcuts. */
void repl_config_set(ReplConfigKey key, int value);

/* Cycle a config item by delta steps (delta +1 = next state, -1 = prev state).
 * Wraps around at boundaries. Returns the new state value. Called by keyboard
 * shortcuts and config menu cycling. */
int  repl_config_cycle(ReplConfigKey key, int delta);

#endif /* REPL_CONFIG_H */
