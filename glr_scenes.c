/*
 * glr_scenes.c -- Per-slot cfg snapshot storage. Step 7b of
 * feature/decouple-repl-from-gl-repl-alt.md.
 *
 * The bag contents are opaque app-state populated by the
 * controller-installed cfg bridge (see glr_actions.c
 * fill_scene_subset / apply). This module owns nothing but the
 * storage; repl_scenes.c drives the lifecycle.
 */
#include "glr_scenes.h"

#include <string.h>

#include "repl_core.h"   /* MAX_USER_SCENES */

static ReplExportConfig g_scene_cfg[MAX_USER_SCENES];
static ReplExportConfig g_pre_example_cfg;
static int              g_pre_example_valid = 0;

ReplExportConfig *glr_scenes_scene_cfg_mut(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return NULL;
    return &g_scene_cfg[slot];
}

const ReplExportConfig *glr_scenes_scene_cfg(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return NULL;
    return &g_scene_cfg[slot];
}

void glr_scenes_scene_cfg_clear(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return;
    repl_export_config_clear(&g_scene_cfg[slot]);
}

ReplExportConfig *glr_scenes_pre_example_cfg_mut(void) {
    return &g_pre_example_cfg;
}

const ReplExportConfig *glr_scenes_pre_example_cfg(void) {
    return &g_pre_example_cfg;
}

void glr_scenes_pre_example_cfg_clear(void) {
    repl_export_config_clear(&g_pre_example_cfg);
    g_pre_example_valid = 0;
}

int glr_scenes_pre_example_cfg_valid(void) {
    return g_pre_example_valid;
}

void glr_scenes_pre_example_cfg_set_valid(int valid) {
    g_pre_example_valid = valid ? 1 : 0;
}

void glr_scenes_reset_all(void) {
    for (int i = 0; i < MAX_USER_SCENES; i++)
        repl_export_config_clear(&g_scene_cfg[i]);
    glr_scenes_pre_example_cfg_clear();
}
