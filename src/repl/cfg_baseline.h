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
/* 40 bytes: holds a 32-bit decimal int ("-2147483648" = 12) and any
 * enum value name from the cfg symbol tables. The longest current symbol
 * is "OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE" (37 chars + null = 38) - the
 * backdrop name this used to cite was never the longest of them. A
 * STATIC_ASSERT in cfg_baseline.c pins this against the int-decimal
 * lower bound; the enum-name upper bound is informational, not
 * enforced at the REPL layer (the bridge is the only TU that needs
 * to know the symbolic names). */
#define REPL_CFG_VALUE_MAX   40
/* Capacity of a config bag - must stay >= the number of emitted @cfg
 * toggles (every non-header, non-action g_cfg_items[] row). When a new
 * config toggle pushes the real count past this, glr_export_cfg_fill_all
 * silently drops the overflow and the last @cfg lines stop round-tripping
 * (caught by test_workspace_header_budget_worst_case). Bump this - and
 * MAX_WORKSPACE_HEADER_LINES in export_state.h, which is sized from it. */
#define REPL_CFG_MAX_ITEMS   48

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
int         repl_config_bag_count(const ReplConfigBag *cfg);

/* Controller-installed adapter for cfg save/load. */
typedef struct {
    /* Fill bag with all current cfg values for save-file emission. */
    void (*fill_all)(ReplConfigBag *cfg);
    /* Fill bag with the per-scene-snapshot subset for repl_scenes. */
    void (*fill_scene_subset)(ReplConfigBag *cfg);
    /* Fill bag with the *default* value of every `fill_scene_subset` slug -
     * the baseline an example load resets to before applying the scene's
     * own leading `@cfg`. Same keys, same order as fill_scene_subset, so a
     * writer can diff the two bags positionally or by slug. Used by the
     * .glr scene writer to emit only the rows that actually differ. */
    void (*fill_scene_defaults)(ReplConfigBag *cfg);
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
    /* Resolve a symbolic value name (e.g. "GRID_THEME_RADAR") to its
     * integer enum value for the given slug. Returns 1 on resolution
     * with *out_value set; 0 when the slug doesn't take symbolic
     * values or the name isn't a known constant. The bridge owns the
     * scene-enum vocabulary so REPL-layer code can stay
     * scene-agnostic - see the @cfg parser and the tutorial runner's
     * SET/REQUIRE handlers. */
    int  (*resolve_text)(const char *slug, const char *value_name,
                         int *out_value);
} ReplConfigBridge;

void                        repl_config_install_bridge(const ReplConfigBridge *bridge);
const ReplConfigBridge     *repl_config_bridge(void);

/* Extract the slug from a `// @cfg <slug> [= ...]` line. */
int  repl_config_extract_slug(const char *line, char *out, size_t out_sz, const char **end_out);

/* Typed live-cfg helpers backed by the controller-installed config bridge. */
int  repl_cfg_get_int(const char *slug, int fallback);
void repl_cfg_set_int(const char *slug, int value);
int  repl_cfg_known(const char *slug);

/* Apply a slug with a symbolic value name (e.g. "GRID_THEME_RADAR").
 * The bridge resolves the name to int and applies it the same way
 * repl_cfg_set_int would. Falls back to strtol when the bridge has no
 * resolve_text or the name isn't a known constant - that path lets
 * legacy integer-form saved files keep loading. */
void repl_cfg_set_text(const char *slug, const char *value_name);

/* Resolve a symbolic value name to int via the bridge's resolve_text.
 * Returns 1 + *out on success, 0 when no bridge / no resolver / unknown
 * name. Used by callers that need the integer for comparisons (e.g.
 * the tutorial REQUIRE matcher) without the scene-enum vocabulary
 * leaking into the REPL layer. */
int  repl_cfg_resolve_text(const char *slug, const char *value_name,
                           int *out);

#endif /* REPL_CFG_BASELINE_H */
