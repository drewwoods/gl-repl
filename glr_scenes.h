/*
 * glr_scenes.h -- App-side companion to repl_scenes.c.
 *
 * Step 7b of feature/decouple-repl-from-gl-repl-alt.md splits the
 * per-scene cfg snapshot out of `UserScene` (in repl_scenes.c, a REPL
 * pipeline TU) into a parallel storage indexed by slot. The cfg
 * snapshot is opaque app-state — it carries `// @cfg key = value`
 * pairs the controller-installed bridge fills and applies — so this
 * companion module is a one-line wrapper around `MAX_USER_SCENES`
 * worth of `ReplExportConfig` bags plus a single `g_pre_example_cfg`
 * for the example sandbox.
 *
 * Slot lifecycle stays driven by `repl_scenes.c`. Every site that
 * sets `g_user_scenes[X].used = 0` (LRU evict, reset) MUST also call
 * `glr_scenes_scene_cfg_clear(X)` so the parallel array stays in
 * sync. The `check-repl-scenes-cfg-clear-paired` guard locks the
 * invariant in.
 */
#ifndef GLR_SCENES_H
#define GLR_SCENES_H

#include "repl_export.h"   /* ReplExportConfig */

ReplExportConfig       *glr_scenes_scene_cfg_mut(int slot);
const ReplExportConfig *glr_scenes_scene_cfg(int slot);
void                    glr_scenes_scene_cfg_clear(int slot);

ReplExportConfig       *glr_scenes_pre_example_cfg_mut(void);
const ReplExportConfig *glr_scenes_pre_example_cfg(void);
void                    glr_scenes_pre_example_cfg_clear(void);
int                     glr_scenes_pre_example_cfg_valid(void);
void                    glr_scenes_pre_example_cfg_set_valid(int valid);

void                    glr_scenes_reset_all(void);

#endif /* GLR_SCENES_H */
