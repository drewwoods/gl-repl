/*
 * src/repl/cfg_baseline.h - Flat key/value configuration bag for presets and baselines.
 *
 * Provides a neutral bag of configuration key/value pairs used by save/load,
 * scene snapshots, and the tutorial subsystem.
 */
#ifndef REPL_CFG_BASELINE_H
#define REPL_CFG_BASELINE_H

#include <stddef.h>

#define REPL_EXPORT_CFG_KEY_MAX     24
#define REPL_EXPORT_CFG_VALUE_MAX   16
#define REPL_EXPORT_CFG_MAX_ITEMS   32

typedef struct {
    char key[REPL_EXPORT_CFG_KEY_MAX];      /* opaque slug */
    char value[REPL_EXPORT_CFG_VALUE_MAX];  /* decimal-encoded */
} ReplExportConfigItem;

typedef struct {
    ReplExportConfigItem items[REPL_EXPORT_CFG_MAX_ITEMS];
    int count;
} ReplExportConfig;

void        repl_export_config_clear(ReplExportConfig *cfg);
int         repl_export_config_set(ReplExportConfig *cfg,
                                   const char *key, const char *value);
int         repl_export_config_set_int(ReplExportConfig *cfg,
                                       const char *key, int value);
const char *repl_export_config_get(const ReplExportConfig *cfg, const char *key);
int         repl_export_config_get_int(const ReplExportConfig *cfg,
                                       const char *key, int fallback);
int         repl_export_config_count(const ReplExportConfig *cfg);
int         repl_export_config_at(const ReplExportConfig *cfg, int idx,
                                  const char **key_out, const char **value_out);

/* Controller-installed adapter for cfg save/load. */
typedef struct {
    /* Fill bag with all current cfg values for save-file emission. */
    void (*fill_all)(ReplExportConfig *cfg);
    /* Fill bag with the per-scene-snapshot subset for repl_scenes. */
    void (*fill_scene_subset)(ReplExportConfig *cfg);
    /* Apply bag values to live state. The same apply works for both
     * full-set and scene-subset bags (it iterates whatever's there). */
    void (*apply)(const ReplExportConfig *cfg);
    /* Single-slug live read. Returns `fallback` when the slug is unknown or
     * the bridge isn't installed. */
    int  (*get_int)(const char *slug, int fallback);
    /* Explicit known-slug query for import/export validation. */
    int  (*is_known)(const char *slug);
    /* True when a slug is part of the scene-local cfg subset that built-in
     * examples are allowed to apply in their leading @cfg metadata. */
    int  (*slug_is_scene_subset)(const char *slug);
} ReplExportConfigBridge;

void                          repl_export_install_config_bridge(const ReplExportConfigBridge *bridge);
const ReplExportConfigBridge *repl_export_config_bridge(void);

/* Extract the slug from a `// @cfg <slug> [= ...]` line. */
int  repl_export_extract_cfg_slug(const char *line, char *out, size_t out_sz);

/* Typed live-cfg helpers backed by the controller-installed config bridge. */
int  repl_cfg_get_int(const char *slug, int fallback);
void repl_cfg_set_int(const char *slug, int value);
int  repl_cfg_known(const char *slug);

#endif /* REPL_CFG_BASELINE_H */
