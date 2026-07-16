/*
 * src/repl/scene_snapshot.c - Copy/apply helpers for scene snapshots.
 */
#include "repl/scene_snapshot.h"

#include <stdio.h>
#include <string.h>

#include "repl/command_store.h"
#include "repl/host_effects.h"
#include "repl/state_owners.h"
#include "repl/util.h"
#include "source_document.h"

void scene_snapshot_clear(SceneSnapshot *snapshot) {
    if (!snapshot)
        return;
    memset(snapshot, 0, sizeof(*snapshot));
}


static const char *const *scene_snapshot_line_ptrs(
    const char lines[MAX_EDITOR_COMMANDS][MAX_LINE_LEN],
    int num_cmds) {
    static const char *ptrs[MAX_EDITOR_COMMANDS];

    if (!lines)
        return NULL;
    for (int i = 0; i < num_cmds && i < MAX_EDITOR_COMMANDS; i++)
        ptrs[i] = lines[i];
    return ptrs;
}

int scene_snapshot_load_live_commands(
    const GLCmd *cmds,
    const char lines[MAX_EDITOR_COMMANDS][MAX_LINE_LEN],
    int num_cmds,
    int edit_line) {
    if (num_cmds < 0)
        num_cmds = 0;
    if (num_cmds > MAX_EDITOR_COMMANDS)
        num_cmds = MAX_EDITOR_COMMANDS;

    /* Source text first so a bulk-load failure (capacity, fixture
     * refusal) leaves the cmd-store untouched and the caller can
     * surface the error to the user. */
    if (!source_document_load_lines(scene_snapshot_line_ptrs(lines, num_cmds),
                                    num_cmds))
        return 0;

    ReplCommandStore store = repl_command_store_live();
    if (!repl_command_store_load(&store, cmds, num_cmds)) {
        source_document_clear();
        return 0;
    }

    /* Scene/workspace restore policy: stamp the snapshot's saved cursor.
     * The store itself no longer writes the cursor on load, so this is the
     * explicit policy hook. */
    repl_dispatch_edit_line_set(edit_line);
    return 1;
}

void scene_snapshot_set_edit_line(SceneSnapshot *snapshot, int edit_line) {
    if (!snapshot)
        return;
    snapshot->edit_line = edit_line;
}

void scene_snapshot_capture_live(SceneSnapshot *dst) {
    if (!dst)
        return;

    scene_snapshot_clear(dst);

    SourceTextView text = source_document_view();
    int count = repl_state_document_count();
    if (count < 0)
        count = 0;
    if (count > MAX_EDITOR_COMMANDS)
        count = MAX_EDITOR_COMMANDS;

    memcpy(dst->cmds, repl_state_document_cmds(),
           (size_t)count * sizeof(GLCmd));
    for (int i = 0; i < count; i++)
        repl_copy_string_fits(dst->lines[i], MAX_LINE_LEN,
                              source_text_line(text, i));
    dst->num_cmds = count;
    dst->edit_line = repl_dispatch_edit_line_get();

    repl_eval_copy_predef_vars(dst->predef_vals,
                               dst->predef_names,
                               &dst->num_predef_vars);
    repl_eval_copy_scratch_arrays(dst->scratch_arrays);

    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        const char *alias = repl_func_alias_get(slot);
        if (alias)
            snprintf(dst->func_aliases[slot], REPL_FUNC_NAME_MAX, "%s", alias);
        else
            dst->func_aliases[slot][0] = '\0';
    }

    {
        const ReplConfigBridge *bridge = repl_config_bridge();
        if (bridge && bridge->fill_scene_subset)
            bridge->fill_scene_subset(&dst->cfg);
    }

    {
        ReplImportExportView io = repl_state_import_export();
        const ReplExportCameraBridge *cam_bridge = repl_export_camera_bridge();

        snprintf(dst->camera_comment_line, sizeof(dst->camera_comment_line),
                 "%s", io.camera_comment_line);
        if (cam_bridge && cam_bridge->fill_display_block)
            cam_bridge->fill_display_block(&dst->camera_block);
    }
}

static void scene_snapshot_apply_aliases(
    const char aliases[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX]) {
    repl_func_alias_clear_all();
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        if (aliases[slot][0])
            repl_func_alias_set(slot, aliases[slot]);
    }
}

static void scene_snapshot_apply_cfg(const ReplConfigBag *cfg) {
    const ReplConfigBridge *bridge = repl_config_bridge();
    if (bridge && bridge->apply)
        bridge->apply(cfg);
}

static void scene_snapshot_apply_camera(
    const ReplExportCameraBlock *camera_block,
    SceneSnapshotCameraMode camera_mode) {
    const ReplExportCameraBridge *cam_bridge = repl_export_camera_bridge();
    if (!cam_bridge)
        return;

    if (camera_mode == SCENE_SNAPSHOT_CAMERA_EASE) {
        if (cam_bridge->apply_example_block)
            cam_bridge->apply_example_block(camera_block);
        return;
    }

    if (cam_bridge->apply_capture_block_snap)
        cam_bridge->apply_capture_block_snap(camera_block);
}

int scene_snapshot_apply_live(const SceneSnapshot *src,
                              SceneSnapshotCameraMode camera_mode) {
    if (!src)
        return 0;
    if (!scene_snapshot_load_live_commands(src->cmds, src->lines,
                                           src->num_cmds, src->edit_line))
        return 0;

    repl_eval_restore_predef_vars(src->predef_vals,
                                  src->predef_names,
                                  src->num_predef_vars);
    repl_eval_restore_scratch_arrays(src->scratch_arrays);
    scene_snapshot_apply_aliases(src->func_aliases);
    scene_snapshot_apply_cfg(&src->cfg);
    snprintf(repl_state_import_export_writable()->camera_comment_line,
             REPL_EXPORT_CAMERA_LINE_MAX, "%s", src->camera_comment_line);
    scene_snapshot_apply_camera(&src->camera_block, camera_mode);
    return 1;
}
