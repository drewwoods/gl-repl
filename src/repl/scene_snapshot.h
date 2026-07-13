/*
 * src/repl/scene_snapshot.h - Copyable scene snapshots.
 *
 * A SceneSnapshot is the complete REPL-side payload stored in a user-scene
 * slot or transient stash: source commands/text, cursor, variables, scratch
 * arrays, function aliases, scene-local cfg, and camera text.
 */
#ifndef REPL_SCENE_SNAPSHOT_H
#define REPL_SCENE_SNAPSHOT_H

#include "config.h"
#include "repl/cfg_baseline.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/export.h"

typedef struct SceneSnapshot {
    GLCmd cmds[MAX_EDITOR_COMMANDS];
    char  lines[MAX_EDITOR_COMMANDS][MAX_LINE_LEN];
    int   num_cmds;
    int   edit_line;

    float predef_vals[MAX_PREDEF_VARS];
    float scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    char  predef_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX];
    int   num_predef_vars;

    char func_aliases[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX];

    ReplConfigBag cfg;
    char camera_comment_line[REPL_EXPORT_CAMERA_LINE_MAX];
    ReplExportCameraBlock camera_block;
} SceneSnapshot;

typedef enum {
    SCENE_SNAPSHOT_CAMERA_SNAP = 0,
    SCENE_SNAPSHOT_CAMERA_EASE = 1
} SceneSnapshotCameraMode;

void scene_snapshot_clear(SceneSnapshot *snapshot);
int  scene_snapshot_copy(SceneSnapshot *dst, const SceneSnapshot *src);

void scene_snapshot_capture_live(SceneSnapshot *dst);
int  scene_snapshot_apply_live(const SceneSnapshot *src,
                               SceneSnapshotCameraMode camera_mode);

void scene_snapshot_set_edit_line(SceneSnapshot *snapshot, int edit_line);
int  scene_snapshot_load_live_commands(
    const GLCmd *cmds,
    const char lines[MAX_EDITOR_COMMANDS][MAX_LINE_LEN],
    int num_cmds,
    int edit_line);

#endif /* REPL_SCENE_SNAPSHOT_H */
