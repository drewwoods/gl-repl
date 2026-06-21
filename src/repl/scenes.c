/*
 * src/repl/scenes.c -- User scene slots, promotion, and workspace save/load.
 */
#include "repl/scenes.h"
#include "repl/scene_snapshot.h"
#include "repl/workspace_io.h"
#include "repl/examples.h"
#include "repl/eval.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "repl/host_effects.h"
#include "repl/state_notify.h"
#include "repl/cfg_baseline.h"   /* ReplConfigBag + bridge for per-scene cfg */
#include "repl/export.h"          /* ReplExportCameraBlock + camera bridge */
#include "repl/state_owners.h"
#include "source_document.h" /* source_document_view */
#include "config.h"          /* REPL_DIAG_TEXT_MAX */

#include <dirent.h>
#include <limits.h>          /* NAME_MAX */
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#define g_example_idx            (repl_state_active_example_idx())
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
 * Slot 0 is the pinned "home" scene: the user's pre-example state, captured
 * once before the first example load and never LRU-evicted. Slots 1..N are
 * promoted examples / user scenes.
 *
 *   g_active_user_scene  >= 0  => that slot is loaded into repl_state_document_cmds_mut()[]
 *                        == -1 => an example or fresh workspace is active
 *
 *   last_touch is bumped from the monotonic g_user_scene_tick counter
 *   on every access (load/save/rename) so LRU eviction can pick the
 *   coldest slot when the 9th scene is needed.
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

/* Default home-scene name -- used when slot 0 is captured on first example load. */
#define USER_SCENE_HOME_NAME "My Scene"

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
    } else if (s->name[0] == '\0') {
        snprintf(s->name, sizeof(s->name), "%s", USER_SCENE_HOME_NAME);
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
    char msg[REPL_DIAG_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Loaded scene: %s", s->name);
    repl_set_status(msg);
}

static void save_user_scene(void) {
    save_scene_to_slot(0, USER_SCENE_HOME_NAME, repl_dispatch_edit_line_get());
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
    repl_scenes_capture_home_if_needed();
    restore_pre_example_cfg_if_valid();
    g_export_scene_name_hint_writable = NULL;
    g_pending_scene_name_writable[0] = '\0';
    g_active_user_scene = -1;
    repl_state_scenes_set_active_example_idx(-1);
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
}

static void restore_user_scene(void) {
    if (!g_user_scenes[0].used) return;
    load_scene_from_slot(0);
}

static int find_free_user_scene_slot(void) {
    for (int s = 1; s < MAX_USER_SCENES; s++)
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
 * snapshots live while try_evict_lru adds another on a recursive
 * frame; that exceeds the default POSIX worker stack (512 KB – 2 MB)
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

static void user_scene_copy(UserScene *dst, const UserScene *src) {
    if (!dst || !src)
        return;
    /* UserScene is plain data (the embedded SceneSnapshot is itself POD), so a
     * whole-struct assignment copies every field — drop-proof if a field is
     * ever added, unlike a hand-maintained member-by-member copy. */
    *dst = *src;
}

static int scene_slug_used(const char used[][USER_SCENE_NAME_MAX],
                           int used_count,
                           const char *slug) {
    for (int i = 0; i < used_count; i++) {
        if (strcmp(used[i], slug) == 0)
            return 1;
    }
    return 0;
}

static void scene_filename_slug_for_slot(int slot, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (slot < 0 || slot >= MAX_USER_SCENES || !g_user_scenes[slot].used) {
        out[0] = '\0';
        return;
    }

    char used[MAX_USER_SCENES][USER_SCENE_NAME_MAX];
    int used_count = 0;

    for (int scene_slot = 0; scene_slot < MAX_USER_SCENES; scene_slot++) {
        if (!g_user_scenes[scene_slot].used)
            continue;

        char base_slug[USER_SCENE_NAME_MAX];
        char candidate[USER_SCENE_NAME_MAX];
        workspace_io_filename_slug(g_user_scenes[scene_slot].name,
                                   base_slug, sizeof(base_slug));
        int collision_depth = 0;
        workspace_io_slug_with_collision_depth(base_slug, collision_depth,
                                               candidate, sizeof(candidate));
        while (scene_slug_used(used, used_count, candidate)) {
            collision_depth++;
            workspace_io_slug_with_collision_depth(base_slug, collision_depth,
                                                   candidate, sizeof(candidate));
        }

        if (scene_slot == slot) {
            snprintf(out, out_sz, "%s", candidate);
            return;
        }

        snprintf(used[used_count], sizeof(used[used_count]), "%s", candidate);
        used_count++;
    }

    out[0] = '\0';
}

int repl_save_workspace(const char *dir, const ReplExportLayout *layout) {
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

    char prev_workspace_dir[REPL_WORKSPACE_DIR_MAX];
    snprintf(prev_workspace_dir, sizeof(prev_workspace_dir), "%s",
             g_workspace_dir);
    snprintf(g_workspace_dir_writable, REPL_WORKSPACE_DIR_MAX, "%s", dir);

    SceneSnapshot *stash = scene_snapshot_scratch_alloc();
    if (!stash) {
        repl_set_status_error("Workspace save: out of memory");
        snprintf(g_workspace_dir_writable, REPL_WORKSPACE_DIR_MAX, "%s", prev_workspace_dir);
        return -1;
    }
    stash_live_state(stash);

    int written = 0;
    for (int s = 0; s < MAX_USER_SCENES; s++) {
        if (!g_user_scenes[s].used) continue;
        install_scene_into_live(s);

        char slug[USER_SCENE_NAME_MAX];
        scene_filename_slug_for_slot(s, slug, sizeof(slug));

        char path[REPL_WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
        snprintf(path, sizeof(path), "%s/%s.c", dir, slug);

        g_export_scene_name_hint_writable = g_user_scenes[s].name;
        if (!repl_export_save_output(path, source_document_view(), layout)) {
            g_export_scene_name_hint_writable = NULL;
            restore_live_from_stash(stash);
            scene_snapshot_scratch_free(stash);
            snprintf(g_workspace_dir_writable, REPL_WORKSPACE_DIR_MAX, "%s",
                     prev_workspace_dir);
            return -1;
        }
        g_export_scene_name_hint_writable = NULL;
        written++;
    }

    restore_live_from_stash(stash);
    scene_snapshot_scratch_free(stash);

    char msg[REPL_STATUS_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Saved %d scene%s to %s",
             written, written == 1 ? "" : "s", dir);
    repl_set_status(msg);
    return written;
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
        snprintf(out, out_sz, "output.%s", ext);
        return;
    }
    char slug[USER_SCENE_NAME_MAX];
    scene_filename_slug_for_slot(slot, slug, sizeof(slug));
    if (workspace_dir && workspace_dir[0])
        snprintf(out, out_sz, "%s/%s.%s", workspace_dir, slug, ext);
    else
        snprintf(out, out_sz, "%s.%s", slug, ext);
}

void repl_save_active_scene(const ReplExportLayout *layout) {
    int slot = g_active_user_scene;

    /* No active named user scene (example / transient): keep the
     * historical single-file behavior (./output.c). */
    if (slot < 0 || slot >= MAX_USER_SCENES || !g_user_scenes[slot].used) {
        repl_save_default_output(layout);
        return;
    }

    const char *workspace_dir = g_workspace_dir[0] ? g_workspace_dir : NULL;

    /* Save aborts if a bound workspace dir can't be created. */
    if (workspace_dir && workspace_dir[0] && !workspace_io_ensure_dir(workspace_dir)) {
        char emsg[REPL_WORKSPACE_DIR_MAX + 48];
        snprintf(emsg, sizeof(emsg),
                 "Save scene: cannot create %s", workspace_dir);
        repl_set_status_error(emsg);
        return;
    }

    char path[REPL_WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
    format_scene_path("c", workspace_dir, path, sizeof(path));

    g_export_scene_name_hint_writable = g_user_scenes[slot].name;
    if (!repl_export_save_output(path, source_document_view(), layout)) {
        g_export_scene_name_hint_writable = NULL;
        return;
    }
    g_export_scene_name_hint_writable = NULL;

    /* repl_export_save_output hardcodes its success status to
     * "...output.c"; mask it with the real path, same as
     * repl_save_workspace does for its per-slot writes. */
    char msg[REPL_WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 48];
    snprintf(msg, sizeof(msg), "Saved %s (%d commands)",
             path, repl_state_document_count());
    repl_set_status(msg);
}

const char *repl_active_scene_export_path(const char *ext) {
    static char path[REPL_WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
    int slot = g_active_user_scene;
    const char *workspace_dir = g_workspace_dir[0] ? g_workspace_dir : NULL;

    /* Mirror Save Scene's target, best-effort: create the workspace dir if a
     * named scene will use it, and fall back to the cwd if it can't be made
     * (the exporter's own fopen reports any remaining failure). */
    int use_workspace_dir =
        (slot >= 0 && slot < MAX_USER_SCENES && g_user_scenes[slot].used &&
         workspace_dir && workspace_dir[0] && workspace_io_ensure_dir(workspace_dir));
    format_scene_path(ext, use_workspace_dir ? workspace_dir : NULL,
                      path, sizeof(path));
    return path;
}

static int load_scene_file_into_slot(const char *path) {
    scene_snapshot_load_live_commands(NULL, NULL, 0, 0);
    /* Start each imported scene from the built-in predef baseline (`t`).
     * Workspace headers then re-declare any user vars on top. Clearing the
     * table entirely breaks round-tripping for scenes whose expressions
     * reference `t`, because `@var t = ...` cannot re-declare a reserved
     * built-in name. */
    repl_eval_init_predef_vars();
    repl_func_alias_clear_all();

    ReplImportResult import_result;
    if (!repl_export_load_from_file(path, &import_result)) return -1;

    int slot = find_free_user_scene_slot();
    if (!g_user_scenes[0].used) slot = 0;
    if (slot < 0) return -1;

    char scene_name[USER_SCENE_NAME_MAX];
    if (import_result.scene_name[0]) {
        snprintf(scene_name, sizeof(scene_name), "%s", import_result.scene_name);
    } else {
        workspace_io_scene_name_from_filename(path, scene_name, sizeof(scene_name));
        if (!scene_name[0])
            snprintf(scene_name, sizeof(scene_name), "Scene %d", slot);
    }

    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique), scene_name, -1);
    save_scene_to_slot(slot, unique, repl_dispatch_edit_line_get());
    return slot;
}

int repl_load_workspace(const char *dir) {
    if (!dir || !*dir) return 0;

    DIR *d = opendir(dir);
    if (!d) {
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof(msg), "Workspace load: cannot open %s", dir);
        repl_set_status_error(msg);
        return -1;
    }

    /* Ask the host to restore tutorial-mutated cfg before this path
     * stashes live cfg as the pre-workspace snapshot. */
    repl_dispatch_tutorial_teardown();

    SceneSnapshot *stash = scene_snapshot_scratch_alloc();
    if (!stash) {
        closedir(d);
        repl_set_status_error("Workspace load: out of memory");
        return -1;
    }
    stash_live_state(stash);
    int stash_example = g_example_idx;

    /* Replace the existing workspace contents rather than merging the
     * imported files into whatever slots were already in memory. */
    repl_scenes_reset();

    int loaded = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        if (!workspace_io_has_c_ext(name)) continue;

        char path[REPL_WORKSPACE_DIR_MAX + NAME_MAX + 1];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (load_scene_file_into_slot(path) >= 0)
            loaded++;
    }
    closedir(d);

    restore_live_from_stash(stash);
    scene_snapshot_scratch_free(stash);
    repl_state_scenes_set_active_example_idx(stash_example);
    g_active_user_scene = -1;

    snprintf(g_workspace_dir_writable, REPL_WORKSPACE_DIR_MAX, "%s", dir);

    char msg[REPL_STATUS_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Loaded %d scene%s from %s",
             loaded, loaded == 1 ? "" : "s", dir);
    repl_set_status(msg);
    return loaded;
}

/* Land the active slot on the first occupied user scene. repl_load_workspace
 * intentionally leaves active == -1 with the pre-load document live (so the
 * caller owns the activation policy); both the CLI bootstrap and the
 * interactive Load Workspace action call this afterward so a workspace tab is
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

static int evict_scene_to_workspace(int slot);  /* defined below */
static int pick_lru_user_scene_slot(void);      /* defined below */

/* Attempt LRU eviction to free a slot. Returns the evicted slot
 * index on success (caller can restore that slot if a subsequent
 * operation fails), -1 if no eviction was needed (a free slot
 * already existed), -2 if eviction was needed but couldn't run
 * (no workspace bound, no eligible victim, or evict_scene_to_workspace
 * itself failed).
 *
 * On a successful eviction `*out_stash` receives a copy of the victim's
 * in-memory entry, including its SceneSnapshot, so the caller can restore
 * the scene tab if needed; the on-disk file written by the eviction is left
 * in place either way (the user's data is preserved both in-memory after
 * restore AND on disk via Load Workspace). */
static int try_evict_lru(UserScene *out_stash) {
    if (find_free_user_scene_slot() >= 0 || !g_user_scenes[0].used)
        return -1;
    if (!g_workspace_dir[0]) return -2;
    int victim = pick_lru_user_scene_slot();
    if (victim < 0) return -2;

    /* Snapshot the victim's in-memory entry BEFORE evict_scene_to_workspace
     * runs — that helper marks the slot unused and clears its cfg as its
     * last step, so without this capture a subsequent parse failure would
     * leave the user staring at a missing tab. */
    user_scene_copy(out_stash, &g_user_scenes[victim]);

    /* evict_scene_to_workspace clobbers live state via install_scene_into_live;
     * wrap in its own stash/restore so the caller's outer live stash stays
     * the user's actual document, not the evicted scene's content. */
    SceneSnapshot *live_temp = scene_snapshot_scratch_alloc();
    if (!live_temp) return -2;
    stash_live_state(live_temp);
    int ok = evict_scene_to_workspace(victim);
    restore_live_from_stash(live_temp);
    scene_snapshot_scratch_free(live_temp);
    if (!ok) return -2;
    return victim;
}

/* Restore a slot snapshot taken by try_evict_lru. Reinstates the
 * in-memory entry (so the scene tab reappears). No-op if `slot < 0`
 * (the "no eviction happened" sentinel from try_evict_lru). */
static void restore_evicted_slot(int slot, const UserScene *stash) {
    if (slot < 0) return;
    user_scene_copy(&g_user_scenes[slot], stash);
}

static int reserve_user_scene_slot_for_new(void) {
    if (!g_user_scenes[0].used)
        return 0;

    int slot = find_free_user_scene_slot();
    if (slot >= 0)
        return slot;

    UserScene *evicted_stash = (UserScene *)malloc(sizeof(UserScene));
    if (!evicted_stash)
        return -1;
    int evicted_slot = try_evict_lru(evicted_stash);
    free(evicted_stash);
    return evicted_slot >= 0 ? evicted_slot : -1;
}

int repl_scenes_create_empty_user_scene(void) {
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

    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique), "New Scene", -1);
    save_scene_to_slot(slot, unique, repl_dispatch_edit_line_get());
    g_active_user_scene = slot;
    repl_state_scenes_set_active_example_idx(-1);

    char msg[REPL_DIAG_TEXT_MAX];
    snprintf(msg, sizeof(msg), "New scene: %s", unique);
    repl_set_status(msg);
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

    /* Persist the currently-active scene into ITS slot (not slot 0!)
     * before load_scene_file_into_slot wipes live state. Using
     * save_user_scene() here would clobber the home slot ("My Scene")
     * any time the active scene was at slot 1+, losing the user's
     * in-progress edits when they switched back via tabs / F12. */
    repl_scenes_save_active_scene_if_any();

    /* Stash so a parse failure or slots-full restore leaves the live
     * document intact. load_scene_file_into_slot clears live state as
     * its first step, so we must capture before that point.
     *
     * Both scratches are heap-allocated up front; this function holds
     * them simultaneously across a try_evict_lru call (which adds
     * another SceneSnapshot on its own frame). On the stack that totals
     * ~3.6 MB before redzones — close to overrun on default worker
     * stacks. */
    SceneSnapshot *stash = scene_snapshot_scratch_alloc();
    UserScene *evicted_stash = (UserScene *)malloc(sizeof(UserScene));
    if (!stash || !evicted_stash) {
        scene_snapshot_scratch_free(stash);
        free(evicted_stash);
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_NO_SLOT;
        return -1;
    }
    stash_live_state(stash);
    int stash_example = g_example_idx;
    int stash_active  = g_active_user_scene;

    /* Eviction is transactional with the load: snapshot the victim
     * slot's in-memory entry so a subsequent parse failure can
     * restore the tab. Without this, a bad-syntax file would silently
     * drop the LRU tab from the user's workspace. */
    int evicted_slot = try_evict_lru(evicted_stash);
    if (evicted_slot == -2) {
        restore_live_from_stash(stash);
        scene_snapshot_scratch_free(stash);
        free(evicted_stash);
        repl_state_scenes_set_active_example_idx(stash_example);
        g_active_user_scene = stash_active;
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_NO_SLOT;
        return -1;
    }
    /* evicted_slot == -1 means no eviction was needed (a free slot
     * already existed); >=0 means we evicted and snapshotted. */

    int slot = load_scene_file_into_slot(path);
    if (slot < 0) {
        restore_live_from_stash(stash);
        repl_state_scenes_set_active_example_idx(stash_example);
        g_active_user_scene = stash_active;
        /* Roll back the eviction's in-memory mutation. The on-disk
         * file written by evict_scene_to_workspace is left in place —
         * it's a faithful copy of the scene's pre-eviction state, so
         * after restore_evicted_slot the in-memory tab and the disk
         * file agree, and the user has lost nothing. */
        restore_evicted_slot(evicted_slot, evicted_stash);
        scene_snapshot_scratch_free(stash);
        free(evicted_stash);
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_PARSE;
        return -1;
    }

    scene_snapshot_scratch_free(stash);
    free(evicted_stash);

    /* load_scene_file_into_slot left live state == the loaded scene
     * and stored a copy in the slot. Activate the slot so subsequent
     * edits accumulate there and scene tabs reflect the new active. */
    g_active_user_scene = slot;
    repl_state_scenes_set_active_example_idx(-1);
    return slot;
}

static int evict_scene_to_workspace(int slot) {
    if (slot <= 0 || slot >= MAX_USER_SCENES) return 0;
    if (!g_user_scenes[slot].used)            return 0;
    if (!g_workspace_dir[0])                  return 0;

    if (!workspace_io_ensure_dir(g_workspace_dir)) return 0;

    install_scene_into_live(slot);

    char slug[USER_SCENE_NAME_MAX];
    scene_filename_slug_for_slot(slot, slug, sizeof(slug));

    char path[REPL_WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
    snprintf(path, sizeof(path), "%s/%s.c", g_workspace_dir, slug);

    g_export_scene_name_hint_writable = g_user_scenes[slot].name;
    /* LRU eviction runs as a side effect of repl_promote_example_if_needed
     * (called from editor_undo_push_snapshot). The user isn't actively
     * saving here, so the layout struct is unavailable — pass NULL and
     * accept the 800x600 fallback in the exported display(). */
    if (!repl_export_save_output(path, source_document_view(), NULL)) {
        g_export_scene_name_hint_writable = NULL;
        return 0;
    }
    g_export_scene_name_hint_writable = NULL;

    g_user_scenes[slot].used = 0;
    scene_cfg_clear(slot);
    return 1;
}

static int pick_lru_user_scene_slot(void) {
    int best = -1;
    uint32_t best_tick = 0;
    for (int s = 1; s < MAX_USER_SCENES; s++) {
        if (!g_user_scenes[s].used)    continue;
        if (s == g_active_user_scene)  continue;
        if (best < 0 || g_user_scenes[s].last_touch < best_tick) {
            best = s;
            best_tick = g_user_scenes[s].last_touch;
        }
    }
    return best;
}

int repl_promote_example_if_needed(void) {
    if (g_active_user_scene >= 0) return -1;
    if (g_example_idx < 0)        return -1;

    if (!g_user_scenes[0].used)
        save_user_scene();

    int slot = find_free_user_scene_slot();
    if (slot < 0) {
        if (g_workspace_dir[0]) {
            int victim = pick_lru_user_scene_slot();
            if (victim >= 0) {
                SceneSnapshot *stash = scene_snapshot_scratch_alloc();
                if (stash) {
                    stash_live_state(stash);
                    int ok = evict_scene_to_workspace(victim);
                    restore_live_from_stash(stash);
                    scene_snapshot_scratch_free(stash);
                    if (ok)
                        slot = victim;
                }
            }
        }
        if (slot < 0) {
            repl_set_status_error("All user scene slots full -- save workspace to free a slot");
            return -1;
        }
    }

    const char *example_name = repl_example_name(g_example_idx);
    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique),
                             example_name ? example_name : "Scene", -1);
    save_scene_to_slot(slot, unique, repl_dispatch_edit_line_get());
    g_active_user_scene = slot;
    repl_state_scenes_set_active_example_idx(-1);

    char msg[REPL_DIAG_TEXT_MAX];
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

void repl_scenes_capture_home_if_needed(void) {
    if (!g_user_scenes[0].used)
        save_user_scene();
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
}

void repl_scenes_activate_home_slot(const char *scene_name_hint) {
    const char *name = (scene_name_hint && scene_name_hint[0]) ? scene_name_hint
                                                              : USER_SCENE_HOME_NAME;
    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique), name, 0);
    /* Drop any pending example sandbox: workspace import / explicit
     * home activation establishes a fresh user-controlled state. */
    restore_pre_example_cfg_if_valid();
    save_scene_to_slot(0, unique, repl_dispatch_edit_line_get());
    g_active_user_scene = 0;
    repl_state_scenes_set_active_example_idx(-1);
}

void repl_scenes_reset(void) {
    memset(g_user_scenes, 0, sizeof(g_user_scenes));
    g_active_user_scene = -1;
    g_user_scene_tick = 0;
    /* Per-slot cfg snapshots + example-sandbox cfg are file-static
     * here (since 7d). Resetting the slot lifecycle drives the cfg
     * reset alongside. */
    scene_cfg_reset_all();
}
