/*
 * src/repl/scenes.c -- User scene slots, promotion, and workspace save/load.
 */
#include "repl/scenes.h"
#include "repl/scene_snapshot.h"
#include "repl/workspace_io.h"
#include "repl/examples.h"
#include "repl/tutorials.h"       /* repl_tutorial_name for post-tutorial promotion */
#include "repl/eval.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "repl/host_effects.h"
#include "repl/load.h"
#include "repl/state_notify.h"
#include "repl/cfg_baseline.h"   /* ReplConfigBag + bridge for per-scene cfg */
#include "repl/export.h"          /* ReplExportCameraBlock + camera bridge */
#include "repl/state_owners.h"
#include "source_document.h" /* source_document_view */
#include "config.h"          /* REPL_DIAG_TEXT_MAX */

#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define g_example_idx            (repl_state_active_example_idx())
/* Post-tutorial promotion marker — the tutorial twin of g_example_idx.
 * >= 0 only while the live transient document is the retained result of a
 * COMPLETED or STOPPED tutorial; an active tutorial keeps it at -1 so its own
 * step commits can't promote. See ReplSceneRuntimeState.tutorial_origin_idx. */
#define g_tutorial_origin_idx    (repl_state_tutorial_origin_idx())
#define g_workspace_dir          (repl_state_workspace_dir())
#define g_workspace_dir_writable (repl_state_scenes_writable()->workspace_dir)

#define IMPORT_EXPORT_VIEW      (repl_state_import_export())
#define IMPORT_EXPORT_WRITABLE  (repl_state_import_export_writable())

#define g_export_scene_name_hint (IMPORT_EXPORT_VIEW.export_scene_name_hint)
#define g_pending_scene_name     (IMPORT_EXPORT_VIEW.pending_scene_name)
#define g_pending_workspace_dir  (IMPORT_EXPORT_VIEW.pending_workspace_dir)

#define g_export_scene_name_hint_writable (IMPORT_EXPORT_WRITABLE->export_scene_name_hint)
#define g_pending_scene_name_writable     (IMPORT_EXPORT_WRITABLE->pending_scene_name)
#define g_pending_workspace_dir_writable  (IMPORT_EXPORT_WRITABLE->pending_workspace_dir)
#define g_camera_comment_line_writable    (IMPORT_EXPORT_WRITABLE->camera_comment_line)

/* The per-scene cfg subset is selected by a controller-installed
 * bridge (ReplConfigBridge.fill_scene_subset / .apply) rather
 * than a static slug list. The controller knows which slugs belong
 * in the per-scene snapshot because it owns glr_config_*;
 * src/repl/scenes.c just stores the bag and round-trips it through
 * the bridge.
 *
 * camera_rotate footgun: the slug is included in the bridge's
 * scene-subset fill, so per-scene snapshots still capture/restore it
 * without src/repl/scenes.c having to call glr_config_*.
 * (Bridge introduced in Step 4 of
 * feature/decouple-repl-from-gl-repl-alt.md, which replaced the
 * former static N_SCENE_CFG_KEYS subset list.) */

/* User scene slots for the workspace / example-promotion system.
 *
 *   g_active_user_scene  >= 0  => that slot is loaded into repl_state_document_cmds_mut()[]
 *                        == -1 => an example or fresh workspace is active
 *
 *   last_touch is retained for stable snapshot compatibility and future
 *   explicit close/reopen ordering; capacity is currently a hard 8 scenes.
 *
 * The per-scene snapshot is embedded on UserScene (the `snapshot`
 * field) so cfg, camera, variables, source text, and aliases travel
 * with the rest of the slot via struct copy — no parallel arrays, no
 * lifecycle invariant to maintain. The cfg
 * bag's contents are opaque to this TU; only the controller-installed
 * bridge interprets the slugs. The "pre-example" cfg (captured before
 * the user loads an example, restored when they leave it) lives in a
 * separate `g_pre_example` wrapper struct with its own `valid` flag
 * because it isn't tied to any slot.
 * Snapshot copy/apply now lives in src/repl/scene_snapshot.c so this TU can
 * focus on slot selection, promotion policy, and workspace iteration. */
typedef struct {
    int           used;
    char          name[USER_SCENE_NAME_MAX];
    char          file_name[WORKSPACE_IO_FILE_MAX];
    uint32_t      last_touch;
    SceneSnapshot snapshot;
} UserScene;

static UserScene g_user_scenes[MAX_USER_SCENES];
static int       g_active_user_scene = -1;
static uint32_t  g_user_scene_tick = 0;

/* Captured scene-presentation cfg from before the user loaded an example,
 * used to restore the pre-example state when they leave the example. The
 * `valid` flag inside ReplConfigBag distinguishes "no capture yet" from "captured empty bag". */
static ReplConfigBag g_pre_example;

static void scene_cfg_clear(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return;
    repl_config_bag_clear(&g_user_scenes[slot].snapshot.cfg);
}

static ReplConfigBag *pre_example_cfg_writable(void) {
    return &g_pre_example;
}

static const ReplConfigBag *pre_example_cfg(void) {
    return &g_pre_example;
}

static void pre_example_cfg_clear(void) {
    repl_config_bag_clear(&g_pre_example);
}

static int pre_example_cfg_valid(void) {
    return g_pre_example.valid;
}

static void pre_example_cfg_set_valid(int valid) {
    g_pre_example.valid = valid ? 1 : 0;
}

static void scene_cfg_reset_all(void) {
    for (int i = 0; i < MAX_USER_SCENES; i++)
        repl_config_bag_clear(&g_user_scenes[i].snapshot.cfg);
    pre_example_cfg_clear();
}

static uint32_t next_user_scene_tick(void) {
    return ++g_user_scene_tick;
}

static void capture_pre_example_cfg(void) {
    ReplConfigBag *cfg = pre_example_cfg_writable();
    repl_config_bag_clear(cfg);
    const ReplConfigBridge *bridge = repl_config_bridge();
    if (bridge && bridge->fill_scene_subset)
        bridge->fill_scene_subset(cfg);
    pre_example_cfg_set_valid(1);
}

static void restore_pre_example_cfg_if_valid(void) {
    if (!pre_example_cfg_valid()) return;
    const ReplConfigBridge *bridge = repl_config_bridge();
    if (bridge && bridge->apply)
        bridge->apply(pre_example_cfg());
    pre_example_cfg_clear();
}

static int user_scene_slot_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_USER_SCENES; i++)
        if (g_user_scenes[i].used) n++;
    return n;
}

static void derive_unique_scene_name(char *out, size_t out_sz,
                                     const char *base, int ignore_slot) {
    if (!out || out_sz == 0) return;
    if (!base || !*base) base = "Scene";

    char candidate[USER_SCENE_NAME_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s", base);
    if (n < 0) { candidate[0] = '\0'; }

    int suffix = 2;
    while (1) {
        int collision = 0;
        for (int i = 0; i < MAX_USER_SCENES; i++) {
            if (!g_user_scenes[i].used || i == ignore_slot) continue;
            if (strcmp(g_user_scenes[i].name, candidate) == 0) {
                collision = 1; break;
            }
        }
        if (!collision) break;
        snprintf(candidate, sizeof(candidate), "%s (%d)", base, suffix++);
        if (suffix > 999) break;
    }

    snprintf(out, out_sz, "%s", candidate);
}

static void save_scene_to_slot(int idx, const char *name, int edit_line) {
    if (idx < 0 || idx >= MAX_USER_SCENES) return;
    UserScene *s = &g_user_scenes[idx];
    scene_snapshot_capture_live(&s->snapshot);
    scene_snapshot_set_edit_line(&s->snapshot, edit_line);
    /* Callers re-saving an existing slot pass `g_user_scenes[idx].name`,
     * which aliases s->name. snprintf with overlapping src/dst is UB
     * (glibc with `%s` produces an empty buffer), so only copy when the
     * pointers differ. The aliased path leaves the existing name in
     * place — the caller's intent in that case is "save commands; keep
     * the name". */
    if (name && *name) {
        if (name != s->name)
            snprintf(s->name, sizeof(s->name), "%s", name);
    }
    s->used       = 1;
    s->last_touch = next_user_scene_tick();
}

void repl_scenes_save_active_scene_if_any(void);

void repl_scenes_enter_transient_scene(void);

static void load_scene_from_slot(int idx) {
    if (idx < 0 || idx >= MAX_USER_SCENES) return;
    UserScene *s = &g_user_scenes[idx];
    if (!s->used) return;
    repl_scenes_save_active_scene_if_any();
    if (!scene_snapshot_apply_live(&s->snapshot, SCENE_SNAPSHOT_CAMERA_EASE))
        return;
    repl_state_flat_program_set_count(0);
    /* Exit insert mode so the next interactive line lands at the new
     * scene's tail, not inside the previous typing context. The full
     * reset (input buffer wipe, cursor home) belongs to the editor
     * wrapper called after this API returns; scene loads deliberately
     * preserve the user's typing context except for the mode flip. */
    repl_dispatch_insert_mode_off();
    /* Editor input buffer refresh is the controller's responsibility:
     * see check-no-load-line-to-input-in-pipeline. Controllers /
     * editor wrappers call editor_load_line_to_input(repl_dispatch_edit_line_get())
     * after a scene-load API returns. */
    repl_mark_source_dirty();
    s->last_touch       = next_user_scene_tick();
    g_active_user_scene = idx;
    repl_state_scenes_set_active_example_idx(-1);
    repl_state_scenes_set_tutorial_origin_idx(-1);
    char msg[REPL_DIAG_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Loaded scene: %s", s->name);
    repl_set_status(msg);
}

void repl_scenes_save_active_scene_if_any(void) {
    if (g_active_user_scene >= 0 && g_active_user_scene < MAX_USER_SCENES) {
        save_scene_to_slot(g_active_user_scene,
                           g_user_scenes[g_active_user_scene].name,
                           repl_dispatch_edit_line_get());
    }
}

void repl_scenes_enter_transient_scene(void) {
    repl_scenes_save_active_scene_if_any();
    restore_pre_example_cfg_if_valid();
    g_export_scene_name_hint_writable = NULL;
    g_pending_scene_name_writable[0] = '\0';
    g_camera_comment_line_writable[0] = '\0';
    g_active_user_scene = -1;
    repl_state_scenes_set_active_example_idx(-1);
    /* Unconditional: entering a transient buffer supersedes any retained
     * post-tutorial document. The tutorial-start path runs through here and
     * does NOT re-establish the marker — only the runner's end-of-lesson path
     * does — so a tutorial stays unpromotable for as long as it is active. */
    repl_state_scenes_set_tutorial_origin_idx(-1);
}

void repl_scenes_reset_for_transient(void) {
    repl_state_document_reset();
    /* repl_state_document_reset doesn't touch the edit-line cursor —
     * storage now lives outside ReplState (see the helper's contract
     * in state.c). The transient-scene boundary is a wholesale
     * reset, so the cursor goes back to 0 alongside the document
     * clear; routes through the host-effects sink for the same β
     * reason repl_dispatch_input_reset does.
     * (Storage moved out of ReplState in phase 4 of the
     * edit-line-ownership plan.) */
    repl_dispatch_edit_line_set(0);
    repl_state_flat_program_set_count(0);
    repl_dispatch_input_reset();
    repl_eval_init_predef_vars();
    repl_func_alias_clear_all();
    g_camera_comment_line_writable[0] = '\0';
}

static void restore_user_scene(void) {
    if (!g_user_scenes[0].used) return;
    load_scene_from_slot(0);
}

static int find_free_user_scene_slot(void) {
    for (int s = 0; s < MAX_USER_SCENES; s++)
        if (!g_user_scenes[s].used) return s;
    return -1;
}

static void install_scene_into_live(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return;
    const UserScene *s = &g_user_scenes[slot];
    if (!s->used) return;
    (void)scene_snapshot_apply_live(&s->snapshot, SCENE_SNAPSHOT_CAMERA_SNAP);
}

/* Heap-allocate a transient SceneSnapshot scratch (~1.2 MB —
 * cmds[4096] + lines[4096][256] dominate). Stack allocation is a
 * real overflow hazard because workspace/load flows can keep multiple
 * snapshots can be live together during transactional loads; putting them on
 * the stack exceeds the default POSIX worker stack (512 KB – 2 MB)
 * and pressures the 8 MB main stack alongside ASan/UBSan redzones.
 * Every successful scene_snapshot_scratch_alloc must be paired with
 * scene_snapshot_scratch_free at every exit path. Returns NULL on OOM. */
static SceneSnapshot *scene_snapshot_scratch_alloc(void) {
    return (SceneSnapshot *)malloc(sizeof(SceneSnapshot));
}

static void scene_snapshot_scratch_free(SceneSnapshot *p) {
    free(p);
}

static void stash_live_state(SceneSnapshot *dst) {
    scene_snapshot_capture_live(dst);
}

static void restore_live_from_stash(const SceneSnapshot *src) {
    (void)scene_snapshot_apply_live(src, SCENE_SNAPSHOT_CAMERA_SNAP);
}

static int scene_file_name_used(const char *file_name, int ignore_slot) {
    for (int i = 0; i < MAX_USER_SCENES; i++) {
        if (i == ignore_slot || !g_user_scenes[i].used)
            continue;
        if (g_user_scenes[i].file_name[0] &&
            strcmp(g_user_scenes[i].file_name, file_name) == 0)
            return 1;
    }
    return 0;
}

static void ensure_scene_file_name(int slot) {
    char base[USER_SCENE_NAME_MAX];
    char candidate[WORKSPACE_IO_FILE_MAX];
    int depth = 0;
    if (slot < 0 || slot >= MAX_USER_SCENES || !g_user_scenes[slot].used ||
        g_user_scenes[slot].file_name[0])
        return;
    workspace_io_filename_slug(g_user_scenes[slot].name, base, sizeof(base));
    do {
        char slug[USER_SCENE_NAME_MAX];
        workspace_io_slug_with_collision_depth(base, depth++, slug, sizeof(slug));
        snprintf(candidate, sizeof(candidate), "%s.c", slug);
    } while (scene_file_name_used(candidate, slot));
    snprintf(g_user_scenes[slot].file_name,
             sizeof(g_user_scenes[slot].file_name), "%s", candidate);
}

static void scene_export_leaf_for_slot(int slot, const char *ext,
                                       char *out, size_t out_sz) {
    char slug[USER_SCENE_NAME_MAX];
    if (slot >= 0 && slot < MAX_USER_SCENES && g_user_scenes[slot].used) {
        ensure_scene_file_name(slot);
        if (g_user_scenes[slot].file_name[0]) {
            snprintf(slug, sizeof(slug), "%s", g_user_scenes[slot].file_name);
            char *dot = strrchr(slug, '.');
            if (dot) *dot = '\0';
            snprintf(out, out_sz, "%s.%s", slug, ext);
            return;
        }
    }
    snprintf(out, out_sz, "output.%s", ext);
}

static int manifest_contains_file(const WorkspaceManifest *manifest,
                                  const char *file_name) {
    if (!manifest || !file_name)
        return 0;
    for (int i = 0; i < manifest->scene_count; i++)
        if (strcmp(manifest->scene_files[i], file_name) == 0)
            return 1;
    return 0;
}

static void cleanup_workspace_scene_temps(const char *dir,
                                          const WorkspaceManifest *manifest) {
    if (!dir || !manifest)
        return;
    for (int i = 0; i < manifest->scene_count; i++) {
        char path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 16];
        char tmp_path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 20];
        if (!workspace_io_path_join(dir, manifest->scene_files[i],
                                    path, sizeof(path)))
            continue;
        int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
        if (n >= 0 && n < (int)sizeof(tmp_path))
            unlink(tmp_path);
    }
}

static int workspace_path_exists(const char *dir, const char *leaf) {
    char path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 8];
    struct stat st;
    return workspace_io_path_join(dir, leaf, path, sizeof(path)) &&
           stat(path, &st) == 0;
}

static void ensure_scene_file_name_for_workspace(
    int slot, const char *dir, const WorkspaceManifest *old_manifest) {
    char base[USER_SCENE_NAME_MAX];
    char candidate[WORKSPACE_IO_FILE_MAX];
    int depth = 0;
    if (slot < 0 || slot >= MAX_USER_SCENES || !g_user_scenes[slot].used)
        return;

    if (g_user_scenes[slot].file_name[0] &&
        (!workspace_path_exists(dir, g_user_scenes[slot].file_name) ||
         manifest_contains_file(old_manifest,
                                g_user_scenes[slot].file_name)))
        return;

    workspace_io_filename_slug(g_user_scenes[slot].name, base, sizeof(base));
    do {
        char slug[USER_SCENE_NAME_MAX];
        workspace_io_slug_with_collision_depth(base, depth++, slug,
                                                sizeof(slug));
        snprintf(candidate, sizeof(candidate), "%s.c", slug);
    } while (scene_file_name_used(candidate, slot) ||
             (workspace_path_exists(dir, candidate) &&
              !manifest_contains_file(old_manifest, candidate)));
    snprintf(g_user_scenes[slot].file_name,
             sizeof(g_user_scenes[slot].file_name), "%s", candidate);
}

static void cleanup_unpublished_scene_files(
    const char *dir, const WorkspaceManifest *manifest,
    const WorkspaceManifest *old_manifest, int committed_count) {
    for (int i = 0; i < committed_count && i < manifest->scene_count; i++) {
        char path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 8];
        if (manifest_contains_file(old_manifest, manifest->scene_files[i]))
            continue;
        if (workspace_io_path_join(dir, manifest->scene_files[i],
                                   path, sizeof(path)))
            unlink(path);
    }
}

int repl_save_workspace(const char *dir, const ReplExportLayout *layout) {
    WorkspaceManifest old_manifest;
    WorkspaceManifest manifest;
    char previous_file_names[MAX_USER_SCENES][WORKSPACE_IO_FILE_MAX];
    char prev_workspace_dir[REPL_WORKSPACE_DIR_MAX];
    char err[REPL_STATUS_TEXT_MAX];
    SceneSnapshot *stash = NULL;
    int had_old_manifest = 0;
    int manifest_exists = 0;
    int committed_count = 0;
    int written = 0;

    memset(&old_manifest, 0, sizeof(old_manifest));
    memset(&manifest, 0, sizeof(manifest));
    err[0] = '\0';

    if (!dir || !*dir) {
        repl_set_status_error("Workspace save: no folder provided");
        return -1;
    }

    if (!workspace_io_ensure_dir(dir)) {
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof(msg), "Workspace save: cannot create %s", dir);
        repl_set_status_error(msg);
        return -1;
    }

    if (g_active_user_scene >= 0 && g_active_user_scene < MAX_USER_SCENES) {
        save_scene_to_slot(g_active_user_scene,
                           g_user_scenes[g_active_user_scene].name,
                           repl_dispatch_edit_line_get());
    }

    snprintf(prev_workspace_dir, sizeof(prev_workspace_dir), "%s",
             g_workspace_dir);
    for (int i = 0; i < MAX_USER_SCENES; i++)
        snprintf(previous_file_names[i], sizeof(previous_file_names[i]), "%s",
                 g_user_scenes[i].file_name);
    snprintf(g_workspace_dir_writable, REPL_WORKSPACE_DIR_MAX, "%s", dir);

    stash = scene_snapshot_scratch_alloc();
    if (!stash) {
        repl_set_status_error("Workspace save: out of memory");
        snprintf(g_workspace_dir_writable, REPL_WORKSPACE_DIR_MAX, "%s",
                 prev_workspace_dir);
        return -1;
    }
    stash_live_state(stash);

    manifest_exists = workspace_io_manifest_exists(dir);
    had_old_manifest = workspace_io_manifest_read(dir, &old_manifest,
                                                   err, sizeof(err));
    if (manifest_exists && !had_old_manifest) {
        goto fail;
    }
    manifest.version = 1;
    const char *base = strrchr(dir, '/');
    snprintf(manifest.name, sizeof(manifest.name), "%s",
             had_old_manifest ? old_manifest.name
                              : ((base && base[1]) ? base + 1 : dir));
    if (!workspace_io_workspace_name_valid(manifest.name))
        snprintf(manifest.name, sizeof(manifest.name), "Workspace");

    for (int s = 0; s < MAX_USER_SCENES; s++) {
        if (!g_user_scenes[s].used) continue;
        ensure_scene_file_name_for_workspace(
            s, dir, had_old_manifest ? &old_manifest : NULL);
        install_scene_into_live(s);

        char path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 8];
        char tmp_path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 16];
        if (!workspace_io_path_join(dir, g_user_scenes[s].file_name,
                                    path, sizeof(path))) {
            snprintf(err, sizeof(err),
                     "Workspace save: scene path is too long");
            goto fail;
        }
        if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >=
            (int)sizeof(tmp_path)) {
            snprintf(err, sizeof(err),
                     "Workspace save: temporary path is too long");
            goto fail;
        }

        g_export_scene_name_hint_writable = g_user_scenes[s].name;
        if (!repl_export_save_output(tmp_path, source_document_view(), layout)) {
            g_export_scene_name_hint_writable = NULL;
            unlink(tmp_path);
            snprintf(err, sizeof(err),
                     "Workspace save: could not stage scene files");
            goto fail;
        }
        g_export_scene_name_hint_writable = NULL;
        snprintf(manifest.scene_files[manifest.scene_count], WORKSPACE_IO_FILE_MAX,
                 "%s", g_user_scenes[s].file_name);
        manifest.scene_count++;
        written++;
    }

    for (int i = 0; i < manifest.scene_count; i++) {
        char path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 8];
        char tmp_path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 16];
        workspace_io_path_join(dir, manifest.scene_files[i], path, sizeof(path));
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
        if (rename(tmp_path, path) != 0) {
            snprintf(err, sizeof(err),
                     "Workspace save: could not commit scene files");
            goto fail;
        }
        committed_count++;
    }

    if (!workspace_io_manifest_write(dir, &manifest,
                                     err, sizeof(err)))
        goto fail;

    if (had_old_manifest) {
        for (int i = 0; i < old_manifest.scene_count; i++) {
            if (!manifest_contains_file(&manifest, old_manifest.scene_files[i])) {
                char stale[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 8];
                if (workspace_io_path_join(dir, old_manifest.scene_files[i],
                                           stale, sizeof(stale))) {
                    if (unlink(stale) != 0 && errno != ENOENT) {
                        int rolled_back = workspace_io_manifest_write(
                            dir, &old_manifest, err, sizeof(err));
                        snprintf(err, sizeof(err), "%s", rolled_back
                            ? "Workspace save: could not remove an obsolete scene"
                            : "Workspace save: delete and manifest rollback failed");
                        goto fail;
                    }
                }
            }
        }
    }

    restore_live_from_stash(stash);
    scene_snapshot_scratch_free(stash);

    char msg[REPL_STATUS_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Saved %d scene%s to %s",
             written, written == 1 ? "" : "s", dir);
    repl_set_status(msg);
    return written;

fail:
    g_export_scene_name_hint_writable = NULL;
    cleanup_workspace_scene_temps(dir, &manifest);
    cleanup_unpublished_scene_files(
        dir, &manifest, had_old_manifest ? &old_manifest : NULL,
        committed_count);
    for (int i = 0; i < MAX_USER_SCENES; i++)
        snprintf(g_user_scenes[i].file_name,
                 sizeof(g_user_scenes[i].file_name), "%s",
                 previous_file_names[i]);
    restore_live_from_stash(stash);
    scene_snapshot_scratch_free(stash);
    snprintf(g_workspace_dir_writable, REPL_WORKSPACE_DIR_MAX, "%s",
             prev_workspace_dir);
    repl_set_status_error(err[0] ? err : "Workspace save failed");
    return -1;
}

/* Single source of truth for active-scene export file naming, shared by Save
 * Scene (.c) and the .ply export so the two can't drift. Builds
 * "<workspace_dir>/<slug>.<ext>" when `workspace_dir` is set, else
 * "<slug>.<ext>", for the active named user scene — or "output.<ext>" when
 * there is no active named scene. `ext` has no leading dot. The mkdir / error
 * policy is the caller's (Save Scene aborts on a failed mkdir; the export
 * falls back to the cwd); the caller passes the resulting dir-vs-cwd choice. */
static void format_scene_path(const char *ext, const char *workspace_dir,
                              char *out, size_t out_sz) {
    int slot = g_active_user_scene;
    if (slot < 0 || slot >= MAX_USER_SCENES || !g_user_scenes[slot].used) {
        if (workspace_dir && workspace_dir[0])
            snprintf(out, out_sz, "%s/output.%s", workspace_dir, ext);
        else
            snprintf(out, out_sz, "output.%s", ext);
        return;
    }
    char leaf[WORKSPACE_IO_FILE_MAX];
    scene_export_leaf_for_slot(slot, ext, leaf, sizeof(leaf));
    if (workspace_dir && workspace_dir[0])
        snprintf(out, out_sz, "%s/%s", workspace_dir, leaf);
    else
        snprintf(out, out_sz, "%s", leaf);
}

int repl_save_active_scene(const ReplExportLayout *layout) {
    int slot = g_active_user_scene;

    /* No active named user scene (example / transient): keep the
     * historical single-file behavior (./output.c). */
    if (slot < 0 || slot >= MAX_USER_SCENES || !g_user_scenes[slot].used) {
        return repl_save_default_output(layout);
    }

    const char *workspace_dir = g_workspace_dir[0] ? g_workspace_dir : NULL;

    /* Save aborts if a bound workspace dir can't be created. */
    if (workspace_dir && workspace_dir[0] && !workspace_io_ensure_dir(workspace_dir)) {
        char emsg[REPL_WORKSPACE_DIR_MAX + 48];
        snprintf(emsg, sizeof(emsg),
                 "Save scene: cannot create %s", workspace_dir);
        repl_set_status_error(emsg);
        return 0;
    }

    char path[REPL_WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
    format_scene_path("c", workspace_dir, path, sizeof(path));

    g_export_scene_name_hint_writable = g_user_scenes[slot].name;
    if (!repl_export_save_output(path, source_document_view(), layout)) {
        g_export_scene_name_hint_writable = NULL;
        return 0;
    }
    g_export_scene_name_hint_writable = NULL;

    /* repl_export_save_output hardcodes its success status to
     * "...output.c"; mask it with the real path, same as
     * repl_save_workspace does for its per-slot writes. */
    char msg[REPL_WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 48];
    snprintf(msg, sizeof(msg), "Saved %s (%d commands)",
             path, repl_state_document_count());
    repl_set_status(msg);
    return 1;
}

const char *repl_active_scene_export_path(const char *ext) {
    static char path[REPL_WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
    const char *workspace_dir = g_workspace_dir[0] ? g_workspace_dir : NULL;

    /* Mirror Save Scene's target, best-effort: create the workspace dir if a
     * named scene will use it, and fall back to the cwd if it can't be made
     * (the exporter's own fopen reports any remaining failure). */
    int use_workspace_dir = workspace_dir && workspace_dir[0] &&
                            workspace_io_ensure_dir(workspace_dir);
    format_scene_path(ext, use_workspace_dir ? workspace_dir : NULL,
                      path, sizeof(path));
    return path;
}

static void reset_live_for_scene_import(void) {
    scene_snapshot_load_live_commands(NULL, NULL, 0, 0);
    /* Start each imported scene from the built-in predef baseline (`t`).
     * Workspace headers then re-declare any user vars on top. Clearing the
     * table entirely breaks round-tripping for scenes whose expressions
     * reference `t`, because `t` cannot be re-declared as a reserved
     * built-in name. */
    repl_eval_init_predef_vars();
    repl_func_alias_clear_all();
    g_camera_comment_line_writable[0] = '\0';

}

static int save_imported_scene_to_free_slot(const ReplImportResult *import_result,
                                            const char *fallback_name) {
    int slot = find_free_user_scene_slot();
    if (slot < 0) return -1;

    char scene_name[USER_SCENE_NAME_MAX];
    if (import_result && import_result->scene_name[0])
        snprintf(scene_name, sizeof(scene_name), "%s", import_result->scene_name);
    else if (fallback_name && fallback_name[0])
        snprintf(scene_name, sizeof(scene_name), "%s", fallback_name);
    else
        snprintf(scene_name, sizeof(scene_name), "Scene %d", slot);

    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique), scene_name, -1);
    save_scene_to_slot(slot, unique, repl_dispatch_edit_line_get());
    return slot;
}

static int load_scene_file_into_slot(const char *path) {
    reset_live_for_scene_import();

    ReplImportResult import_result;
    if (!repl_export_load_from_file(path, &import_result)) return -1;

    char fallback_name[USER_SCENE_NAME_MAX];
    workspace_io_scene_name_from_filename(path, fallback_name, sizeof(fallback_name));
    return save_imported_scene_to_free_slot(&import_result, fallback_name);
}

static int load_scene_lines_into_slot(const char *const *lines,
                                      const char *source_name,
                                      const char *fallback_name) {
    reset_live_for_scene_import();

    ReplImportResult import_result;
    if (!repl_export_load_from_lines(lines, source_name, &import_result))
        return -1;

    return save_imported_scene_to_free_slot(&import_result, fallback_name);
}

typedef struct {
    const char *path;
} SceneFileLoadCtx;

typedef struct {
    const char *const *lines;
    const char       *source_name;
    const char       *fallback_name;
} SceneTextLoadCtx;

typedef int (*SceneSlotLoader)(void *ctx);

static int scene_file_loader(void *ctx) {
    SceneFileLoadCtx *c = (SceneFileLoadCtx *)ctx;
    return load_scene_file_into_slot(c ? c->path : NULL);
}

static int scene_text_loader(void *ctx) {
    SceneTextLoadCtx *c = (SceneTextLoadCtx *)ctx;
    if (!c)
        return -1;
    return load_scene_lines_into_slot(c->lines, c->source_name, c->fallback_name);
}

ReplWorkspaceLoadResult repl_load_workspace_ex(const char *dir) {
    ReplWorkspaceLoadResult result;
    WorkspaceManifest manifest;
    char err[REPL_STATUS_TEXT_MAX];
    memset(&result, 0, sizeof(result));
    if (!dir || !*dir) {
        repl_set_status_error("Workspace load: no folder provided");
        return result;
    }

    result.managed = workspace_io_manifest_exists(dir);
    if (!result.managed) {
        repl_set_status_error("Folder is not a managed gl-repl workspace");
        return result;
    }
    if (!workspace_io_manifest_read(dir, &manifest, err, sizeof(err))) {
        if (strstr(err, "more than 8 scenes"))
            result.capacity_exceeded = 1;
        repl_set_status_error(err);
        return result;
    }
    result.files_seen = manifest.scene_count;

    /* Ask the host to restore tutorial-mutated cfg before this path
     * stashes live cfg as the pre-workspace snapshot. */
    repl_dispatch_tutorial_teardown();

    SceneSnapshot *stash = scene_snapshot_scratch_alloc();
    if (!stash) {
        repl_set_status_error("Workspace load: out of memory");
        return result;
    }
    repl_scenes_save_active_scene_if_any();
    ReplScenesSnapshot *catalog_stash = repl_scenes_snapshot_capture();
    if (!catalog_stash) {
        scene_snapshot_scratch_free(stash);
        repl_set_status_error("Workspace load: out of memory");
        return result;
    }
    stash_live_state(stash);
    int stash_example = g_example_idx;
    char old_workspace[REPL_WORKSPACE_DIR_MAX];
    snprintf(old_workspace, sizeof(old_workspace), "%s", g_workspace_dir);

    /* Replace the existing workspace contents rather than merging the
     * imported files into whatever slots were already in memory. */
    repl_scenes_reset();

    for (int i = 0; i < result.files_seen; i++) {
        const char *name = manifest.scene_files[i];
        char path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 1];
        if (!workspace_io_path_join(dir, name, path, sizeof(path)) ||
            !workspace_io_regular_file(path)) {
            result.files_failed++;
            break;
        }
        int slot = load_scene_file_into_slot(path);
        if (slot < 0) {
            result.files_failed++;
            break;
        }
        snprintf(g_user_scenes[slot].file_name,
                 sizeof(g_user_scenes[slot].file_name), "%s", name);
        result.scenes_loaded++;
    }

    if (result.files_failed) {
        repl_scenes_snapshot_restore(catalog_stash);
        restore_live_from_stash(stash);
        repl_state_scenes_set_active_example_idx(stash_example);
        snprintf(g_workspace_dir_writable, REPL_WORKSPACE_DIR_MAX, "%s",
                 old_workspace);
        repl_scenes_snapshot_destroy(catalog_stash);
        scene_snapshot_scratch_free(stash);
        repl_set_status_error("Workspace load failed; previous workspace restored");
        return result;
    }

    restore_live_from_stash(stash);
    scene_snapshot_scratch_free(stash);
    repl_scenes_snapshot_destroy(catalog_stash);
    repl_state_scenes_set_active_example_idx(stash_example);
    g_active_user_scene = -1;

    snprintf(g_workspace_dir_writable, REPL_WORKSPACE_DIR_MAX, "%s", dir);

    char msg[REPL_STATUS_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Loaded %d scene%s from %s",
             result.scenes_loaded, result.scenes_loaded == 1 ? "" : "s", dir);
    repl_set_status(msg);
    result.ok = 1;
    return result;
}

int repl_load_workspace(const char *dir) {
    ReplWorkspaceLoadResult result = repl_load_workspace_ex(dir);
    return result.ok ? result.scenes_loaded : -1;
}

/* Land the active slot on the first occupied user scene. repl_load_workspace
 * intentionally leaves active == -1 with the pre-load document live (so the
 * caller owns the activation policy); both the CLI bootstrap and the
 * interactive Open Workspace action call this afterward so a workspace tab is
 * actually selected instead of stranding the user on the now-tabless pre-load
 * document. Returns the activated slot, or -1 if no slot is occupied. */
int repl_scenes_activate_first_loaded_slot(void) {
    for (int slot = 0; slot < MAX_USER_SCENES; slot++) {
        if (g_user_scenes[slot].used) {
            repl_load_user_scene_idx(slot);
            return slot;
        }
    }
    return -1;
}

static int reserve_user_scene_slot_for_new(void) {
    return find_free_user_scene_slot();
}

int repl_scenes_create_empty_user_scene(void) {
    char seed_err[REPL_STATUS_TEXT_MAX];
    int slot = reserve_user_scene_slot_for_new();
    if (slot < 0) {
        repl_set_status_error("All user scene slots full -- save workspace to free a slot");
        return -1;
    }

    repl_scenes_save_active_scene_if_any();
    restore_pre_example_cfg_if_valid();
    g_export_scene_name_hint_writable = NULL;
    g_pending_scene_name_writable[0] = '\0';

    repl_scenes_reset_for_transient();
    if (!repl_load_default_display_baseline(seed_err, sizeof(seed_err), NULL)) {
        repl_set_status_error(seed_err[0] ? seed_err
                                         : "Could not create scene defaults");
        return -1;
    }
    repl_mark_source_dirty();

    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique), "New Scene", -1);
    save_scene_to_slot(slot, unique, repl_dispatch_edit_line_get());
    g_active_user_scene = slot;
    repl_state_scenes_set_active_example_idx(-1);
    repl_state_scenes_set_tutorial_origin_idx(-1);

    char msg[REPL_DIAG_TEXT_MAX];
    snprintf(msg, sizeof(msg), "New scene: %s", unique);
    repl_set_status(msg);
    return slot;
}

static int repl_load_scene_via_loader(SceneSlotLoader loader,
                                      void *ctx,
                                      ReplSceneLoadStatus *out_reason) {
    if (!loader) {
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_PARSE;
        return -1;
    }

    /* Persist the currently-active scene into its own slot before the
     * loader wipes live state. */
    repl_scenes_save_active_scene_if_any();

    if (find_free_user_scene_slot() < 0) {
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_NO_SLOT;
        return -1;
    }

    /* Stash so a parse failure leaves the live document intact. The concrete
     * loader clears live state as its first step. */
    SceneSnapshot *stash = scene_snapshot_scratch_alloc();
    if (!stash) {
        scene_snapshot_scratch_free(stash);
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_NO_SLOT;
        return -1;
    }
    stash_live_state(stash);
    int stash_example = g_example_idx;
    int stash_active  = g_active_user_scene;

    int slot = loader(ctx);
    if (slot < 0) {
        restore_live_from_stash(stash);
        repl_state_scenes_set_active_example_idx(stash_example);
        g_active_user_scene = stash_active;
        scene_snapshot_scratch_free(stash);
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_PARSE;
        return -1;
    }

    scene_snapshot_scratch_free(stash);

    /* The loader left live state == the loaded scene and stored a copy
     * in the slot. Activate the slot so subsequent edits accumulate
     * there and scene tabs reflect the new active. */
    g_active_user_scene = slot;
    repl_state_scenes_set_active_example_idx(-1);
    repl_state_scenes_set_tutorial_origin_idx(-1);
    if (out_reason) *out_reason = REPL_SCENE_LOAD_OK;
    return slot;
}

int repl_load_scene_as_new_slot(const char *path,
                                ReplSceneLoadStatus *out_reason) {
    if (out_reason) *out_reason = REPL_SCENE_LOAD_OK;

    if (!path || !*path) {
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_EMPTY_PATH;
        return -1;
    }

    /* Filesystem probing happens here (not in editor/) so the editor
     * stays a pure modal text controller. Distinguish missing-file
     * vs directory so the caller can render the right error message
     * without re-stat()ing. */
    struct stat st;
    if (stat(path, &st) != 0) {
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_NOT_FOUND;
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_IS_DIR;
        return -1;
    }

    SceneFileLoadCtx ctx = { path };
    return repl_load_scene_via_loader(scene_file_loader, &ctx, out_reason);
}

static int split_scene_text_lines(const char *text,
                                  char ***out_lines,
                                  char **out_storage) {
    if (out_lines) *out_lines = NULL;
    if (out_storage) *out_storage = NULL;
    if (!text || !*text || !out_lines || !out_storage)
        return 0;

    size_t len = strlen(text);
    int cap = 2;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\r') {
            cap++;
            if (i + 1 < len && text[i + 1] == '\n')
                i++;
        } else if (text[i] == '\n') {
            cap++;
        }
    }

    char *storage = (char *)malloc(len + 1);
    char **lines = (char **)malloc((size_t)cap * sizeof(char *));
    if (!storage || !lines) {
        free(storage);
        free(lines);
        return 0;
    }
    memcpy(storage, text, len + 1);

    int count = 0;
    char *start = storage;
    char *p = storage;
    while (1) {
        if (*p == '\r' || *p == '\n' || *p == '\0') {
            char delim = *p;
            *p = '\0';
            if (count < cap - 1)
                lines[count++] = start;
            if (delim == '\0')
                break;
            if (delim == '\r' && p[1] == '\n') {
                p++;
                *p = '\0';
            }
            p++;
            start = p;
            if (*start == '\0')
                break;
        } else {
            p++;
        }
    }
    lines[count] = NULL;

    *out_lines = lines;
    *out_storage = storage;
    return count;
}

int repl_load_scene_text_as_new_slot(const char *text,
                                     const char *fallback_name,
                                     ReplSceneLoadStatus *out_reason) {
    if (out_reason) *out_reason = REPL_SCENE_LOAD_OK;
    if (!text || !*text) {
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_EMPTY_PATH;
        return -1;
    }

    char **lines = NULL;
    char *storage = NULL;
    int line_count = split_scene_text_lines(text, &lines, &storage);
    if (line_count <= 0) {
        free(lines);
        free(storage);
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_EMPTY_PATH;
        return -1;
    }

    SceneTextLoadCtx ctx = {
        (const char *const *)lines,
        "<clipboard>",
        fallback_name,
    };
    int slot = repl_load_scene_via_loader(scene_text_loader, &ctx, out_reason);
    free(lines);
    free(storage);
    return slot;
}

static int reserve_slot_for_promotion(void) {
    return find_free_user_scene_slot();
}

/* Push just the per-scene cfg subset of `slot` back onto the live view.
 * Used after a post-tutorial promotion, where tutorial teardown has already
 * restored the user's pre-tutorial values wholesale: the promoted slot owns
 * the tutorial-mutated per-scene settings, so the live view must follow the
 * new scene, while any slug the tutorial touched from OUTSIDE the bridge's
 * scene subset (a global / tutorial-only setting) stays restored — it isn't
 * in the bag, so this pass cannot re-assert it.
 *
 * Deliberately NOT a full scene_snapshot_apply_live: promotion runs inside an
 * editor commit transaction, and a full apply would rewrite the document,
 * predefs, camera, and cursor out from under it. Only cfg needs replaying. */
static void apply_scene_cfg_from_slot(int slot) {
    const ReplConfigBridge *bridge = repl_config_bridge();
    if (slot < 0 || slot >= MAX_USER_SCENES || !g_user_scenes[slot].used ||
        !bridge || !bridge->apply)
        return;
    bridge->apply(&g_user_scenes[slot].snapshot.cfg);
}

typedef enum {
    PROMOTE_ORIGIN_EXAMPLE = 0,
    PROMOTE_ORIGIN_TUTORIAL
} PromoteOriginKind;

int repl_promote_transient_if_needed(void) {
    PromoteOriginKind origin_kind;
    const char *origin_name;
    char unique[USER_SCENE_NAME_MAX];
    char msg[REPL_DIAG_TEXT_MAX];
    int slot;

    if (g_active_user_scene >= 0) return -1;

    /* Two promotable transient origins. Example wins when both are somehow
     * set: an example load tears the tutorial down (clearing the marker), so
     * the overlap can't legitimately occur. */
    if (g_example_idx >= 0) {
        origin_kind = PROMOTE_ORIGIN_EXAMPLE;
        origin_name = repl_example_name(g_example_idx);
    } else if (g_tutorial_origin_idx >= 0) {
        origin_kind = PROMOTE_ORIGIN_TUTORIAL;
        origin_name = repl_tutorial_name(g_tutorial_origin_idx);
    } else {
        return -1;
    }

    slot = reserve_slot_for_promotion();
    if (slot < 0) {
        /* Origin and (for a tutorial) the pending cfg baseline are left
         * untouched, so the next edit retries and captures everything typed
         * into the transient document in the meantime. */
        repl_set_status_error("All user scene slots full -- save workspace to free a slot");
        return -1;
    }

    derive_unique_scene_name(unique, sizeof(unique),
                             origin_name ? origin_name : "Scene", -1);
    /* Capture the live document AND the tutorial-mutated cfg into the slot
     * BEFORE any teardown: teardown is what restores the pre-tutorial
     * baseline, and the promoted scene is supposed to own the lesson's view. */
    save_scene_to_slot(slot, unique, repl_dispatch_edit_line_get());

    if (origin_kind == PROMOTE_ORIGIN_TUTORIAL) {
        /* Flush the pending baseline now that the slot holds the view:
         * global / tutorial-only slugs go back to their pre-tutorial values,
         * and the marker is cleared. */
        repl_dispatch_tutorial_teardown();
        /* Then re-assert only the per-scene subset the new scene owns, so the
         * live view keeps matching the slot the user just landed in. */
        apply_scene_cfg_from_slot(slot);
    }

    g_active_user_scene = slot;
    repl_state_scenes_set_active_example_idx(-1);
    repl_state_scenes_set_tutorial_origin_idx(-1);

    /* Published last: the teardown above can emit status of its own. */
    snprintf(msg, sizeof(msg), "Promoted to scene: %s", unique);
    repl_set_status(msg);
    return slot;
}

int repl_user_scene_valid(void) {
    return user_scene_slot_count() > 0;
}

void repl_load_user_scene(void) {
    restore_user_scene();
}

int repl_user_scene_count(void) {
    return user_scene_slot_count();
}

int repl_user_scene_slot_used(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return 0;
    return g_user_scenes[slot].used;
}

const char *repl_user_scene_name(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return NULL;
    if (!g_user_scenes[slot].used) return NULL;
    return g_user_scenes[slot].name;
}

int repl_load_user_scene_idx(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return 0;
    if (!g_user_scenes[slot].used) return 0;

    /* Ask the host to restore tutorial-mutated cfg before the user-scene
     * cfg restore observes live state. */
    repl_dispatch_tutorial_teardown();

    load_scene_from_slot(slot);
    return 1;
}

int repl_active_user_scene(void) {
    return g_active_user_scene;
}

const char *repl_workspace_dir(void) {
    return g_workspace_dir;
}

void repl_set_workspace_dir(const char *dir) {
    if (!dir) { g_workspace_dir_writable[0] = '\0'; return; }
    snprintf(g_workspace_dir_writable, REPL_WORKSPACE_DIR_MAX, "%s", dir);
}

int repl_user_scene_rename(int slot, const char *new_name) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return 0;
    if (!g_user_scenes[slot].used) return 0;
    if (!new_name) return 0;

    while (*new_name == ' ' || *new_name == '\t') new_name++;
    size_t n = strlen(new_name);
    while (n > 0 && (new_name[n - 1] == ' ' || new_name[n - 1] == '\t')) n--;
    if (n == 0) return 0;

    char trimmed[USER_SCENE_NAME_MAX];
    if (n >= sizeof(trimmed)) n = sizeof(trimmed) - 1;
    memcpy(trimmed, new_name, n);
    trimmed[n] = '\0';

    derive_unique_scene_name(g_user_scenes[slot].name,
                             sizeof(g_user_scenes[slot].name),
                             trimmed, slot);
    g_user_scenes[slot].last_touch = next_user_scene_tick();
    return 1;
}

const char *repl_user_scene_file_name(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES || !g_user_scenes[slot].used)
        return "";
    ensure_scene_file_name(slot);
    return g_user_scenes[slot].file_name;
}

int repl_user_scene_delete(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES || !g_user_scenes[slot].used)
        return 0;
    g_user_scenes[slot].used = 0;
    g_user_scenes[slot].file_name[0] = '\0';
    scene_cfg_clear(slot);
    if (g_active_user_scene == slot)
        g_active_user_scene = -1;
    return 1;
}

int repl_workspace_is_managed(void) {
    return g_workspace_dir[0] && workspace_io_manifest_exists(g_workspace_dir);
}

void repl_scenes_capture_pre_example_cfg_if_entering(void) {
    /* Capture only on the transition from non-example -> example.
     * Subsequent example->example F12 cycles leave the snapshot
     * untouched so the pre-example baseline survives across them. */
    if (g_example_idx >= 0) return;
    if (pre_example_cfg_valid()) return;
    capture_pre_example_cfg();
}

void repl_scenes_mark_example_active(void) {
    g_active_user_scene = -1;
    repl_state_scenes_set_tutorial_origin_idx(-1);
}

void repl_scenes_activate_loaded_document_slot(const char *scene_name_hint) {
    const char *name = (scene_name_hint && scene_name_hint[0]) ? scene_name_hint
                                                              : "Scene";
    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique), name, 0);
    /* Drop any pending example sandbox: workspace import / explicit
     * document-slot activation establishes a fresh user-controlled state. */
    restore_pre_example_cfg_if_valid();
    save_scene_to_slot(0, unique, repl_dispatch_edit_line_get());
    g_active_user_scene = 0;
    repl_state_scenes_set_active_example_idx(-1);
    repl_state_scenes_set_tutorial_origin_idx(-1);
}

void repl_scenes_reset(void) {
    memset(g_user_scenes, 0, sizeof(g_user_scenes));
    g_active_user_scene = -1;
    repl_state_scenes_set_tutorial_origin_idx(-1);
    g_user_scene_tick = 0;
    /* Per-slot cfg snapshots + example-sandbox cfg are file-static
     * here (since 7d). Resetting the slot lifecycle drives the cfg
     * reset alongside. */
    scene_cfg_reset_all();
}

/* Whole-catalog snapshot. The slot array dominates (~10 MB: 8 * SceneSnapshot),
 * so this is heap-allocated. Copies every slot verbatim (occupied or not — a
 * whole-array copy is drop-proof if UserScene grows a field) plus the active
 * index, the monotonic activity tick, and the pre-example cfg bag. */
struct ReplScenesSnapshot {
    UserScene     slots[MAX_USER_SCENES];
    int           active_user_scene;
    uint32_t      user_scene_tick;
    ReplConfigBag pre_example;
};

ReplScenesSnapshot *repl_scenes_snapshot_capture(void) {
    ReplScenesSnapshot *s =
        (ReplScenesSnapshot *)malloc(sizeof(ReplScenesSnapshot));
    if (!s)
        return NULL;
    /* Deliberately NOT repl_scenes_save_active_scene_if_any() — recording the
     * catalog as-is, not flushing the live document into its slot. */
    memcpy(s->slots, g_user_scenes, sizeof(g_user_scenes));
    s->active_user_scene = g_active_user_scene;
    s->user_scene_tick   = g_user_scene_tick;
    s->pre_example       = g_pre_example;
    return s;
}

int repl_scenes_snapshot_restore(const ReplScenesSnapshot *snapshot) {
    if (!snapshot)
        return 0;
    memcpy(g_user_scenes, snapshot->slots, sizeof(g_user_scenes));
    g_active_user_scene = snapshot->active_user_scene;
    g_user_scene_tick   = snapshot->user_scene_tick;
    g_pre_example       = snapshot->pre_example;
    return 1;
}

void repl_scenes_snapshot_destroy(ReplScenesSnapshot *snapshot) {
    free(snapshot);
}
