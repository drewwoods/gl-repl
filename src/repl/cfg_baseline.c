/*
 * src/repl/cfg_baseline.c - Flat key/value configuration bag for presets and baselines.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include "c_compat.h"
#include "repl/cfg_baseline.h"

/* Width sanity for set_int's "%d" of a 32-bit int: "-2147483648" is
 * 11 chars + null = 12. If REPL_CFG_VALUE_MAX ever shrinks below
 * that, set_int can silently truncate values for legitimate inputs;
 * the runtime check below covers the variable-length string path. */
STATIC_ASSERT(REPL_CFG_VALUE_MAX >= 12,
              "REPL_CFG_VALUE_MAX must hold any 32-bit decimal int");

void repl_config_bag_clear(ReplConfigBag *cfg) {
    if (!cfg) return;
    cfg->count = 0;
}

/* Detect snprintf truncation. snprintf returns the number of bytes it
 * WOULD have written (excluding the terminating null); if that is
 * >= the destination size, the result was truncated. */
static int fits(int written, size_t buf_sz) {
    return written >= 0 && (size_t)written < buf_sz;
}

int repl_config_bag_set(ReplConfigBag *cfg,
                           const char *key, const char *value) {
    if (!cfg || !key || !value) return 0;

    /* Reject up front if either field would truncate. Doing the bag
     * mutation only after both fits-checks pass keeps the bag in a
     * consistent state on rejection (no partial replace). */
    if (strlen(key) >= REPL_CFG_KEY_MAX) return 0;
    if (strlen(value) >= REPL_CFG_VALUE_MAX) return 0;

    /* Replace if present. */
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->items[i].key, key) == 0) {
            int n = snprintf(cfg->items[i].value, REPL_CFG_VALUE_MAX,
                             "%s", value);
            return fits(n, REPL_CFG_VALUE_MAX);
        }
    }
    if (cfg->count >= REPL_CFG_MAX_ITEMS) return 0;
    int nk = snprintf(cfg->items[cfg->count].key,   REPL_CFG_KEY_MAX,
                      "%s", key);
    int nv = snprintf(cfg->items[cfg->count].value, REPL_CFG_VALUE_MAX,
                      "%s", value);
    if (!fits(nk, REPL_CFG_KEY_MAX) || !fits(nv, REPL_CFG_VALUE_MAX))
        return 0;
    cfg->count++;
    return 1;
}

int repl_config_bag_set_int(ReplConfigBag *cfg, const char *key, int value) {
    char buf[REPL_CFG_VALUE_MAX];
    int n = snprintf(buf, sizeof(buf), "%d", value);
    if (!fits(n, sizeof(buf))) return 0;
    return repl_config_bag_set(cfg, key, buf);
}

const char *repl_config_bag_get(const ReplConfigBag *cfg, const char *key) {
    if (!cfg || !key) return NULL;
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->items[i].key, key) == 0)
            return cfg->items[i].value;
    }
    return NULL;
}

int repl_config_bag_get_int(const ReplConfigBag *cfg,
                               const char *key, int fallback) {
    const char *s = repl_config_bag_get(cfg, key);
    if (!s) return fallback;
    return (int)strtol(s, NULL, 10);
}

int repl_config_bag_count(const ReplConfigBag *cfg) {
    return cfg ? cfg->count : 0;
}

int repl_config_bag_at(const ReplConfigBag *cfg, int idx,
                          const char **key_out, const char **value_out) {
    if (!cfg || idx < 0 || idx >= cfg->count) return 0;
    if (key_out)   *key_out   = cfg->items[idx].key;
    if (value_out) *value_out = cfg->items[idx].value;
    return 1;
}

static const ReplConfigBridge *g_cfg_bridge = NULL;

void repl_config_install_bridge(const ReplConfigBridge *bridge) {
    g_cfg_bridge = bridge;
}

const ReplConfigBridge *repl_config_bridge(void) {
    return g_cfg_bridge;
}

int repl_config_extract_slug(const char *line, char *out, size_t out_sz) {
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
    const ReplConfigBridge *b = g_cfg_bridge;
    if (!slug || !b || !b->get_int) return fallback;
    return b->get_int(slug, fallback);
}

void repl_cfg_set_int(const char *slug, int value) {
    const ReplConfigBridge *b = g_cfg_bridge;
    if (!slug || !b || !b->apply) return;
    ReplConfigBag single;
    repl_config_bag_clear(&single);
    repl_config_bag_set_int(&single, slug, value);
    b->apply(&single);
}

int repl_cfg_known(const char *slug) {
    const ReplConfigBridge *b = g_cfg_bridge;
    if (!slug || !b || !b->is_known) return 0;
    return b->is_known(slug);
}
