#ifndef REPL_CONFIG_H
#define REPL_CONFIG_H

/*
 * repl_config.h -- Immutable config descriptors and keyed state access.
 *
 * This is the Phase 2 bridge away from raw `int *` config rows.  The menu,
 * help overlay, and export/import code now talk to config items by stable
 * ReplConfigKey values rather than by table pointers.
 */

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

typedef struct {
    const char  *label;
    int          key_code;      /* ASCII ctrl key or GLUT_KEY_* */
    int          is_special;     /* 1 for GLUT special keys */
    ReplConfigKey key;
    int          state_count;    /* 2 = toggle; >2 = cycle */
    const char **state_names;
    int          section_header; /* 1 for separator/header rows */
} ReplConfigItem;

extern const ReplConfigItem g_cfg_items[];
extern const int CFG_ITEM_COUNT;

const ReplConfigItem *repl_config_items(int *count);
const ReplConfigItem *repl_config_item_at(int idx);
int  repl_config_get(ReplConfigKey key);
int  repl_config_state_count(ReplConfigKey key);
const char *repl_config_state_name(ReplConfigKey key, int value);
void repl_config_set(ReplConfigKey key, int value);
int  repl_config_cycle(ReplConfigKey key, int delta);

#endif /* REPL_CONFIG_H */
