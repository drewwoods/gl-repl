/*
 * src/repl/cfg_baseline.h - Flat key/value configuration bag for presets and baselines.
 *
 * Provides a neutral bag of configuration key/value pairs used by save/load,
 * scene snapshots, and the tutorial subsystem.
 */
#ifndef REPL_CFG_BASELINE_H
#define REPL_CFG_BASELINE_H

#include <stddef.h>

#define REPL_CFG_KEY_MAX     24
#define REPL_CFG_VALUE_MAX   16
#define REPL_CFG_MAX_ITEMS   32

typedef struct {
    char key[REPL_CFG_KEY_MAX];      /* opaque slug */
    char value[REPL_CFG_VALUE_MAX];  /* decimal-encoded */
} ReplConfigItem;

typedef struct {
    ReplConfigItem items[REPL_CFG_MAX_ITEMS];
    int count;
    int valid;
} ReplConfigBag;

void        repl_config_bag_clear(ReplConfigBag *cfg);
int         repl_config_bag_set(ReplConfigBag *cfg,
                                const char *key, const char *value);
int         repl_config_bag_set_int(ReplConfigBag *cfg,
                                    const char *key, int value);
const char *repl_config_bag_get(const ReplConfigBag *cfg, const char *key);
int         repl_config_bag_get_int(const ReplConfigBag *cfg,
                                    const char *key, int fallback);
int         repl_config_bag_count(const ReplConfigBag *cfg);
int         repl_config_bag_at(const ReplConfigBag *cfg, int idx,
                               const char **key_out, const char **value_out);

/* Controller-installed adapter for cfg save/load. */
typedef struct {
    /* Fill bag with all current cfg values for save-file emission. */
    void (*fill_all)(ReplConfigBag *cfg);
    /* Fill bag with the per-scene-snapshot subset for repl_scenes. */
    void (*fill_scene_subset)(ReplConfigBag *cfg);
    /* Apply bag values to live state. The same apply works for both
     * full-set and scene-subset bags (it iterates whatever's there). */
    void (*apply)(const ReplConfigBag *cfg);
    /* Single-slug live read. Returns `fallback` when the slug is unknown or
     * the bridge isn't installed. */
    int  (*get_int)(const char *slug, int fallback);
    /* Explicit known-slug query for import/export validation. */
    int  (*is_known)(const char *slug);
    /* True when a slug is part of the scene-local cfg subset that built-in
     * examples are allowed to apply in their leading @cfg metadata. */
    int  (*slug_is_scene_subset)(const char *slug);
} ReplConfigBridge;

void                        repl_config_install_bridge(const ReplConfigBridge *bridge);
const ReplConfigBridge     *repl_config_bridge(void);

/* Extract the slug from a `// @cfg <slug> [= ...]` line. */
int  repl_config_extract_slug(const char *line, char *out, size_t out_sz, const char **end_out);

/* Typed live-cfg helpers backed by the controller-installed config bridge. */
int  repl_cfg_get_int(const char *slug, int fallback);
void repl_cfg_set_int(const char *slug, int value);
int  repl_cfg_known(const char *slug);

#endif /* REPL_CFG_BASELINE_H */
