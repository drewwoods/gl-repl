/*
 * src/repl/cfg_baseline.c - Flat key/value configuration bag for presets and baselines.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "repl/cfg_baseline.h"

void repl_export_config_clear(ReplExportConfig *cfg) {
    if (!cfg) return;
    cfg->count = 0;
}

int repl_export_config_set(ReplExportConfig *cfg,
                           const char *key, const char *value) {
    if (!cfg || !key || !value) return 0;
    /* Replace if present. */
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->items[i].key, key) == 0) {
            snprintf(cfg->items[i].value, REPL_EXPORT_CFG_VALUE_MAX, "%s", value);
            return 1;
        }
    }
    if (cfg->count >= REPL_EXPORT_CFG_MAX_ITEMS) return 0;
    snprintf(cfg->items[cfg->count].key,   REPL_EXPORT_CFG_KEY_MAX,   "%s", key);
    snprintf(cfg->items[cfg->count].value, REPL_EXPORT_CFG_VALUE_MAX, "%s", value);
    cfg->count++;
    return 1;
}

int repl_export_config_set_int(ReplExportConfig *cfg, const char *key, int value) {
    char buf[REPL_EXPORT_CFG_VALUE_MAX];
    snprintf(buf, sizeof(buf), "%d", value);
    return repl_export_config_set(cfg, key, buf);
}

const char *repl_export_config_get(const ReplExportConfig *cfg, const char *key) {
    if (!cfg || !key) return NULL;
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->items[i].key, key) == 0)
            return cfg->items[i].value;
    }
    return NULL;
}

int repl_export_config_get_int(const ReplExportConfig *cfg,
                               const char *key, int fallback) {
    const char *s = repl_export_config_get(cfg, key);
    if (!s) return fallback;
    return (int)strtol(s, NULL, 10);
}

int repl_export_config_count(const ReplExportConfig *cfg) {
    return cfg ? cfg->count : 0;
}

int repl_export_config_at(const ReplExportConfig *cfg, int idx,
                          const char **key_out, const char **value_out) {
    if (!cfg || idx < 0 || idx >= cfg->count) return 0;
    if (key_out)   *key_out   = cfg->items[idx].key;
    if (value_out) *value_out = cfg->items[idx].value;
    return 1;
}

static const ReplExportConfigBridge *g_export_cfg_bridge = NULL;

void repl_export_install_config_bridge(const ReplExportConfigBridge *bridge) {
    g_export_cfg_bridge = bridge;
}

const ReplExportConfigBridge *repl_export_config_bridge(void) {
    return g_export_cfg_bridge;
}

int repl_export_extract_cfg_slug(const char *line, char *out, size_t out_sz) {
    if (!line || !out || out_sz == 0) return 0;
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '@') return 0;
        p++;
        if (strncmp(p, "cfg", 3) != 0) return 0;
        p += 3;
        if (!isspace((unsigned char)*p)) return 0;
        while (*p && isspace((unsigned char)*p)) p++;
    } else if (*p == '@') {
        p++;
        if (strncmp(p, "cfg", 3) != 0) return 0;
        p += 3;
        if (!isspace((unsigned char)*p)) return 0;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    size_t out_i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && out_i + 1 < out_sz)
        out[out_i++] = *p++;
    out[out_i] = '\0';
    return out_i > 0;
}

int repl_cfg_get_int(const char *slug, int fallback) {
    const ReplExportConfigBridge *b = g_export_cfg_bridge;
    if (!slug || !b || !b->get_int) return fallback;
    return b->get_int(slug, fallback);
}

void repl_cfg_set_int(const char *slug, int value) {
    const ReplExportConfigBridge *b = g_export_cfg_bridge;
    if (!slug || !b || !b->apply) return;
    ReplExportConfig single;
    repl_export_config_clear(&single);
    repl_export_config_set_int(&single, slug, value);
    b->apply(&single);
}

int repl_cfg_known(const char *slug) {
    const ReplExportConfigBridge *b = g_export_cfg_bridge;
    if (!slug || !b || !b->is_known) return 0;
    return b->is_known(slug);
}
