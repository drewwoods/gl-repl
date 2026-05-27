/*
 * src/repl/scenes.c -- User scene slots, promotion, and workspace save/load.
 */
#include "repl/command_store.h"
#include "repl/core_internal.h"
#include "repl/examples.h"
#include "repl/core.h"
#include "repl/cfg_baseline.h"   /* ReplConfigBag + bridge for per-scene cfg */
#include "repl/state_owners.h"
#include "source_document.h" /* source_document_load_lines */
#include "config.h"          /* REPL_DIAG_TEXT_MAX */

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SCENE_STATE (repl_state_scenes_mut())
#define g_example_idx (SCENE_STATE->active_example_idx)
#define g_workspace_dir (SCENE_STATE->workspace_dir)
#define IMPORT_EXPORT_STATE (repl_state_import_export_mut())
#define g_export_scene_name_hint (IMPORT_EXPORT_STATE->export_scene_name_hint)
#define g_pending_scene_name (IMPORT_EXPORT_STATE->pending_scene_name)
#define g_pending_workspace_dir (IMPORT_EXPORT_STATE->pending_workspace_dir)

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
 * The per-scene cfg snapshot is embedded on UserScene (the `cfg`
 * field) so it travels with the rest of the slot via struct copy —
 * no parallel array, no lifecycle invariant to maintain. The cfg
 * bag's contents are opaque to this TU; only the controller-installed
 * bridge interprets the slugs. The "pre-example" cfg (captured before
 * the user loads an example, restored when they leave it) lives in a
 * separate `g_pre_example` wrapper struct with its own `valid` flag
 * because it isn't tied to any slot.
 * (Snapshot first split out in Step 7b of
 * feature/decouple-repl-from-gl-repl-alt.md; embedded into UserScene
 * in the 2026-05-24 audit Tier B #44 pass — see
 * plans/done/src-repl-code-smell-audit.md.) */
typedef struct {
    int      used;
    char     name[USER_SCENE_NAME_MAX];
    uint32_t last_touch;
    GLCmd    cmds[MAX_COMMANDS];
    char     lines[MAX_COMMANDS][MAX_LINE_LEN];
    int      num_cmds;
    int      edit_line;
    float    predef_vals[MAX_PREDEF_VARS];
    float    scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    char     predef_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX];
    int      num_predef_vars;
    char     func_aliases[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX];
    /* Per-slot cfg snapshot — opaque bag whose slug semantics live on
     * the controller-installed bridge. Embedded here so stash/restore
     * and slot-eviction code paths handle cfg by ordinary struct copy
     * instead of maintaining a parallel array. */
    ReplConfigBag cfg;
} UserScene;

static UserScene g_user_scenes[MAX_USER_SCENES];
static int       g_active_user_scene = -1;
static uint32_t  g_user_scene_tick = 0;

/* Captured scene-presentation cfg from before the user loaded an example,
 * used to restore the pre-example state when they leave the example. The
 * `valid` flag inside ReplConfigBag distinguishes "no capture yet" from "captured empty bag". */
static ReplConfigBag g_pre_example;

static ReplConfigBag *scene_cfg_mut(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return NULL;
    return &g_user_scenes[slot].cfg;
}

static const ReplConfigBag *scene_cfg(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return NULL;
    return &g_user_scenes[slot].cfg;
}

static void scene_cfg_clear(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return;
    repl_config_bag_clear(&g_user_scenes[slot].cfg);
}

static ReplConfigBag *pre_example_cfg_mut(void) {
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
        repl_config_bag_clear(&g_user_scenes[i].cfg);
    pre_example_cfg_clear();
}

#define WORKSPACE_DIR_MAX REPL_WORKSPACE_DIR_MAX

/* Default home-scene name -- used when slot 0 is captured on first example load. */
#define USER_SCENE_HOME_NAME "My Scene"

static uint32_t next_user_scene_tick(void) {
    return ++g_user_scene_tick;
}

static void capture_pre_example_cfg(void) {
    ReplConfigBag *cfg = pre_example_cfg_mut();
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
    SourceTextView text = source_document_view();
    memcpy(s->cmds, repl_state_document_cmds(), (size_t)repl_state_document_count() * sizeof(GLCmd));
    for (int i = 0; i < repl_state_document_count(); i++)
        repl_copy_string_fits(s->lines[i], MAX_LINE_LEN,
                              source_text_line(text, i));
    s->num_cmds        = repl_state_document_count();
    s->edit_line       = edit_line;
    repl_eval_copy_predef_vars(s->predef_vals,
                               s->predef_names,
                               &s->num_predef_vars);
    repl_eval_copy_scratch_arrays(s->scratch_arrays);
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        const char *alias = repl_func_alias_get(slot);
        if (alias)
            snprintf(s->func_aliases[slot], REPL_FUNC_NAME_MAX, "%s", alias);
        else
            s->func_aliases[slot][0] = '\0';
    }
    {
        ReplConfigBag *cfg = scene_cfg_mut(idx);
        repl_config_bag_clear(cfg);
        const ReplConfigBridge *bridge = repl_config_bridge();
        if (bridge && bridge->fill_scene_subset)
            bridge->fill_scene_subset(cfg);
    }
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

static const char *const *scene_line_ptrs(const char lines[MAX_COMMANDS][MAX_LINE_LEN],
                                          int num_cmds) {
    static const char *ptrs[MAX_COMMANDS];

    if (!lines)
        return NULL;
    for (int i = 0; i < num_cmds && i < MAX_COMMANDS; i++)
        ptrs[i] = lines[i];
    return ptrs;
}

static int load_commands_into_live(const GLCmd *cmds,
                                   const char lines[MAX_COMMANDS][MAX_LINE_LEN],
                                   int num_cmds, int edit_line) {
    /* Source text first so a bulk-load failure (capacity, fixture
     * refusal) leaves the cmd-store untouched and the caller can
     * surface the error to the user. */
    if (!source_document_load_lines(scene_line_ptrs(lines, num_cmds), num_cmds))
        return 0;
    ReplCommandStore store = repl_command_store_live();
    if (!repl_command_store_load(&store, cmds, num_cmds)) {
        source_document_clear();
        return 0;
    }
    /* Scene/workspace restore policy: stamp the snapshot's saved
     * cursor. The store itself no longer writes the cursor on load,
     * so this is the explicit policy hook. β-bound call (REPL →
     * REPL); once repl_state_edit_line_set is deleted alongside the
     * storage flip, this site moves to using a controller-threaded
     * value.
     * (Store stopped writing the cursor in phase 1 of the
     * edit-line-ownership plan; storage flip and
     * edit_line_set cleanup landed in phase 4.) */
    repl_dispatch_edit_line_set(edit_line);
    return 1;
}

void repl_scenes_save_active_scene_if_any(void);

void repl_scenes_enter_transient_scene(void);

static void load_scene_from_slot(int idx) {
    if (idx < 0 || idx >= MAX_USER_SCENES) return;
    UserScene *s = &g_user_scenes[idx];
    if (!s->used) return;
    repl_scenes_save_active_scene_if_any();
    if (!load_commands_into_live(s->cmds, s->lines, s->num_cmds, s->edit_line))
        return;
    repl_state_flat_program_set_count(0);
    repl_eval_restore_predef_vars(s->predef_vals,
                                  s->predef_names,
                                  s->num_predef_vars);
    repl_eval_restore_scratch_arrays(s->scratch_arrays);
    /* Restore the per-scene func-alias table. Each scene owns its own
     * mapping so renaming `drawCube` in scene A doesn't reach into B. */
    repl_func_alias_clear_all();
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        if (s->func_aliases[slot][0])
            repl_func_alias_set(slot, s->func_aliases[slot]);
    }
    /* Roll back any example sandbox before stamping in the user
     * scene's saved cfg. Observably overwritten by the apply below
     * today (scene_cfg covers all keys); becomes load-bearing if a
     * future change makes scene_cfg sparse / inherited-aware. */
    restore_pre_example_cfg_if_valid();
    {
        const ReplConfigBridge *bridge = repl_config_bridge();
        if (bridge && bridge->apply)
            bridge->apply(scene_cfg(idx));
    }
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
    g_example_idx       = -1;
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
    g_export_scene_name_hint = NULL;
    g_pending_scene_name[0] = '\0';
    g_active_user_scene = -1;
    g_example_idx = -1;
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
    if (!load_commands_into_live(s->cmds, s->lines, s->num_cmds, s->edit_line))
        return;
    repl_eval_restore_predef_vars(s->predef_vals,
                                  s->predef_names,
                                  s->num_predef_vars);
    repl_eval_restore_scratch_arrays(s->scratch_arrays);
    repl_func_alias_clear_all();
    for (int alias_slot = 0; alias_slot < REPL_FUNC_SLOT_COUNT; alias_slot++) {
        if (s->func_aliases[alias_slot][0])
            repl_func_alias_set(alias_slot, s->func_aliases[alias_slot]);
    }
    /* Apply the slot's saved per-scene cfg to live state. Without
     * this, repl_save_workspace / evict_scene_to_workspace would
     * export the scene with whichever cfg happened to be live when
     * the iteration started — see the [P1] regression test in
     * test_repl_core_io.c. The user-facing scene switch is used
     * to apply this. */
    const ReplConfigBridge *bridge = repl_config_bridge();
    if (bridge && bridge->apply)
        bridge->apply(scene_cfg(slot));
}

/* Captures live state into a UserScene-shaped stash so
 * install_scene_into_live can be undone (the editor commit /
 * workspace iteration paths need this so a per-slot apply doesn't
 * permanently overwrite the document the user was editing).
 *
 * The cfg snapshot lives in `dst->cfg` (embedded by Tier B #44 —
 * see plans/done/src-repl-code-smell-audit.md) and is filled from
 * the live bridge here. `restore_live_from_stash` re-applies it. */
/* Heap-allocate a transient UserScene scratch (~1.2 MB —
 * cmds[4096] + lines[4096][256] dominate). Stack allocation is a
 * real overflow hazard because repl_load_scene_as_new_slot keeps two
 * of these live while try_evict_lru adds a third on a recursive
 * frame; that exceeds the default POSIX worker stack (512 KB – 2 MB)
 * and pressures the 8 MB main stack alongside ASan/UBSan redzones.
 * Every successful scene_scratch_alloc must be paired with
 * scene_scratch_free at every exit path. Returns NULL on OOM. */
static UserScene *scene_scratch_alloc(void) {
    return (UserScene *)malloc(sizeof(UserScene));
}

static void scene_scratch_free(UserScene *p) {
    free(p);
}

static void stash_live_state(UserScene *dst) {
    SourceTextView text = source_document_view();
    memset(dst, 0, sizeof(*dst));
    memcpy(dst->cmds, repl_state_document_cmds(), (size_t)repl_state_document_count() * sizeof(GLCmd));
    for (int i = 0; i < repl_state_document_count(); i++)
        repl_copy_string_fits(dst->lines[i], MAX_LINE_LEN,
                              source_text_line(text, i));
    dst->num_cmds        = repl_state_document_count();
    dst->edit_line       = repl_dispatch_edit_line_get();
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
}

static void restore_live_from_stash(const UserScene *src) {
    if (!load_commands_into_live(src->cmds, src->lines, src->num_cmds,
                                 src->edit_line))
        return;
    repl_eval_restore_predef_vars(src->predef_vals,
                                  src->predef_names,
                                  src->num_predef_vars);
    repl_eval_restore_scratch_arrays(src->scratch_arrays);
    repl_func_alias_clear_all();
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        if (src->func_aliases[slot][0])
            repl_func_alias_set(slot, src->func_aliases[slot]);
    }
    {
        const ReplConfigBridge *bridge = repl_config_bridge();
        if (bridge && bridge->apply)
            bridge->apply(&src->cfg);
    }
}

static void scene_filename_slug(const char *name, char *out, size_t out_sz) {
    size_t j = 0;
    if (name) {
        for (size_t i = 0; name[i] && j + 1 < out_sz; i++) {
            unsigned char c = (unsigned char)name[i];
            if (isalnum(c))                            out[j++] = (char)tolower(c);
            else if (c == ' ' || c == '-' || c == '_') out[j++] = '_';
        }
    }
    if (j == 0 && out_sz > 0) out[j++] = 's';
    if (j >= out_sz) j = out_sz - 1;
    out[j] = '\0';
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

static void scene_slug_with_collision_depth(const char *base_slug,
                                            int collision_depth,
                                            char *out,
                                            size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (!base_slug) base_slug = "s";

    size_t max_len = out_sz - 1;
    size_t suffix_len = (size_t)collision_depth * 2u;
    if (suffix_len > max_len)
        suffix_len = max_len;

    size_t prefix_len = strlen(base_slug);
    if (prefix_len > max_len - suffix_len)
        prefix_len = max_len - suffix_len;

    memcpy(out, base_slug, prefix_len);
    size_t pos = prefix_len;
    for (int i = 0; i < collision_depth && pos + 2 <= max_len; i++) {
        out[pos++] = '_';
        out[pos++] = '0';
    }
    out[pos] = '\0';
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
        scene_filename_slug(g_user_scenes[scene_slot].name,
                            base_slug, sizeof(base_slug));
        int collision_depth = 0;
        scene_slug_with_collision_depth(base_slug, collision_depth,
                                        candidate, sizeof(candidate));
        while (scene_slug_used(used, used_count, candidate)) {
            collision_depth++;
            scene_slug_with_collision_depth(base_slug, collision_depth,
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

static void scene_name_from_filename(const char *path,
                                     char *out, size_t out_sz) {
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    int n = 0;
    while (base[n] && base[n] != '.' && n < (int)out_sz - 1) {
        out[n] = base[n];
        n++;
    }
    if (out_sz > 0) out[n] = '\0';
}

int repl_save_workspace(const char *dir, const ReplExportLayout *layout) {
    if (!dir || !*dir) {
        repl_set_status_error("Workspace save: no folder provided");
        return -1;
    }

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
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

    char prev_workspace_dir[WORKSPACE_DIR_MAX];
    snprintf(prev_workspace_dir, sizeof(prev_workspace_dir), "%s",
             g_workspace_dir);
    snprintf(g_workspace_dir, WORKSPACE_DIR_MAX, "%s", dir);

    UserScene *stash = scene_scratch_alloc();
    if (!stash) {
        repl_set_status_error("Workspace save: out of memory");
        snprintf(g_workspace_dir, WORKSPACE_DIR_MAX, "%s", prev_workspace_dir);
        return -1;
    }
    stash_live_state(stash);

    int written = 0;
    for (int s = 0; s < MAX_USER_SCENES; s++) {
        if (!g_user_scenes[s].used) continue;
        install_scene_into_live(s);

        char slug[USER_SCENE_NAME_MAX];
        scene_filename_slug_for_slot(s, slug, sizeof(slug));

        char path[WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
        snprintf(path, sizeof(path), "%s/%s.c", dir, slug);

        g_export_scene_name_hint = g_user_scenes[s].name;
        if (!repl_export_save_output(path, source_document_view(), layout)) {
            g_export_scene_name_hint = NULL;
            restore_live_from_stash(stash);
            scene_scratch_free(stash);
            snprintf(g_workspace_dir, WORKSPACE_DIR_MAX, "%s",
                     prev_workspace_dir);
            return -1;
        }
        g_export_scene_name_hint = NULL;
        written++;
    }

    restore_live_from_stash(stash);
    scene_scratch_free(stash);

    char msg[REPL_STATUS_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Saved %d scene%s to %s",
             written, written == 1 ? "" : "s", dir);
    repl_set_status(msg);
    return written;
}

void repl_save_active_scene(const ReplExportLayout *layout) {
    int slot = g_active_user_scene;

    /* No active named user scene (example / transient): keep the
     * historical single-file behavior (./output.c). */
    if (slot < 0 || slot >= MAX_USER_SCENES || !g_user_scenes[slot].used) {
        repl_save_default_output(layout);
        return;
    }

    char slug[USER_SCENE_NAME_MAX];
    scene_filename_slug_for_slot(slot, slug, sizeof(slug));

    char path[WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
    if (g_workspace_dir[0]) {
        if (mkdir(g_workspace_dir, 0755) != 0 && errno != EEXIST) {
            char emsg[WORKSPACE_DIR_MAX + 48];
            snprintf(emsg, sizeof(emsg),
                     "Save scene: cannot create %s", g_workspace_dir);
            repl_set_status_error(emsg);
            return;
        }
        snprintf(path, sizeof(path), "%s/%s.c", g_workspace_dir, slug);
    } else {
        snprintf(path, sizeof(path), "%s.c", slug);
    }

    g_export_scene_name_hint = g_user_scenes[slot].name;
    if (!repl_export_save_output(path, source_document_view(), layout)) {
        g_export_scene_name_hint = NULL;
        return;
    }
    g_export_scene_name_hint = NULL;

    /* repl_export_save_output hardcodes its success status to
     * "...output.c"; mask it with the real path, same as
     * repl_save_workspace does for its per-slot writes. */
    char msg[WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 48];
    snprintf(msg, sizeof(msg), "Saved %s (%d commands)",
             path, repl_state_document_count());
    repl_set_status(msg);
}

static int load_scene_file_into_slot(const char *path) {
    load_commands_into_live(NULL, NULL, 0, 0);
    /* Start each imported scene from the built-in predef baseline (`t`).
     * Workspace headers then re-declare any user vars on top. Clearing the
     * table entirely breaks round-tripping for scenes whose expressions
     * reference `t`, because `@var t = ...` cannot re-declare a reserved
     * built-in name. */
    repl_eval_init_predef_vars();
    repl_func_alias_clear_all();

    if (!repl_export_load_from_file(path)) return -1;

    int slot = find_free_user_scene_slot();
    if (!g_user_scenes[0].used) slot = 0;
    if (slot < 0) return -1;

    char scene_name[USER_SCENE_NAME_MAX];
    if (g_pending_scene_name[0]) {
        snprintf(scene_name, sizeof(scene_name), "%s", g_pending_scene_name);
    } else {
        scene_name_from_filename(path, scene_name, sizeof(scene_name));
        if (!scene_name[0])
            snprintf(scene_name, sizeof(scene_name), "Scene %d", slot);
    }

    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique), scene_name, -1);
    save_scene_to_slot(slot, unique, repl_dispatch_edit_line_get());
    return slot;
}

static int has_dot_c_ext(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 3) return 0;
    return name[n - 2] == '.' && (name[n - 1] == 'c' || name[n - 1] == 'C');
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

    UserScene *stash = scene_scratch_alloc();
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
        if (!has_dot_c_ext(name)) continue;

        char path[WORKSPACE_DIR_MAX + 256];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (load_scene_file_into_slot(path) >= 0)
            loaded++;
    }
    closedir(d);

    restore_live_from_stash(stash);
    scene_scratch_free(stash);
    g_example_idx       = stash_example;
    g_active_user_scene = -1;

    snprintf(g_workspace_dir, WORKSPACE_DIR_MAX, "%s", dir);

    char msg[REPL_STATUS_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Loaded %d scene%s from %s",
             loaded, loaded == 1 ? "" : "s", dir);
    repl_set_status(msg);
    return loaded;
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
 * On a successful eviction `*out_stash` receives a snapshot of the
 * victim's in-memory entry (cfg included, since Tier B #44 embedded
 * `ReplConfigBag cfg` on UserScene) so the caller can restore the
 * scene tab if needed; the on-disk file written by the eviction is
 * left in place either way (the user's data is preserved both
 * in-memory after restore AND on disk via Load Workspace). */
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
    memcpy(out_stash, &g_user_scenes[victim], sizeof(UserScene));

    /* evict_scene_to_workspace clobbers live state via install_scene_into_live;
     * wrap in its own stash/restore so the caller's outer live stash stays
     * the user's actual document, not the evicted scene's content. */
    UserScene *live_temp = scene_scratch_alloc();
    if (!live_temp) return -2;
    stash_live_state(live_temp);
    int ok = evict_scene_to_workspace(victim);
    restore_live_from_stash(live_temp);
    scene_scratch_free(live_temp);
    if (!ok) return -2;
    return victim;
}

/* Restore a slot snapshot taken by try_evict_lru. Reinstates the
 * in-memory entry (so the scene tab reappears); cfg now lives on
 * UserScene so it travels with the memcpy. No-op if `slot < 0` (the
 * "no eviction happened" sentinel from try_evict_lru). */
static void restore_evicted_slot(int slot, const UserScene *stash) {
    if (slot < 0) return;
    memcpy(&g_user_scenes[slot], stash, sizeof(UserScene));
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
     * them simultaneously across a try_evict_lru call (which adds a
     * third UserScene on its own frame). On the stack that totals
     * ~3.6 MB before redzones — close to overrun on default worker
     * stacks. */
    UserScene *stash         = scene_scratch_alloc();
    UserScene *evicted_stash = scene_scratch_alloc();
    if (!stash || !evicted_stash) {
        scene_scratch_free(stash);
        scene_scratch_free(evicted_stash);
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
        scene_scratch_free(stash);
        scene_scratch_free(evicted_stash);
        g_example_idx       = stash_example;
        g_active_user_scene = stash_active;
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_NO_SLOT;
        return -1;
    }
    /* evicted_slot == -1 means no eviction was needed (a free slot
     * already existed); >=0 means we evicted and snapshotted. */

    int slot = load_scene_file_into_slot(path);
    if (slot < 0) {
        restore_live_from_stash(stash);
        g_example_idx       = stash_example;
        g_active_user_scene = stash_active;
        /* Roll back the eviction's in-memory mutation. The on-disk
         * file written by evict_scene_to_workspace is left in place —
         * it's a faithful copy of the scene's pre-eviction state, so
         * after restore_evicted_slot the in-memory tab and the disk
         * file agree, and the user has lost nothing. */
        restore_evicted_slot(evicted_slot, evicted_stash);
        scene_scratch_free(stash);
        scene_scratch_free(evicted_stash);
        if (out_reason) *out_reason = REPL_SCENE_LOAD_ERR_PARSE;
        return -1;
    }

    scene_scratch_free(stash);
    scene_scratch_free(evicted_stash);

    /* load_scene_file_into_slot left live state == the loaded scene
     * and stored a copy in the slot. Activate the slot so subsequent
     * edits accumulate there and scene tabs reflect the new active. */
    g_active_user_scene = slot;
    g_example_idx       = -1;
    return slot;
}

static int evict_scene_to_workspace(int slot) {
    if (slot <= 0 || slot >= MAX_USER_SCENES) return 0;
    if (!g_user_scenes[slot].used)            return 0;
    if (!g_workspace_dir[0])                  return 0;

    if (mkdir(g_workspace_dir, 0755) != 0 && errno != EEXIST) return 0;

    install_scene_into_live(slot);

    char slug[USER_SCENE_NAME_MAX];
    scene_filename_slug_for_slot(slot, slug, sizeof(slug));

    char path[WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
    snprintf(path, sizeof(path), "%s/%s.c", g_workspace_dir, slug);

    g_export_scene_name_hint = g_user_scenes[slot].name;
    /* LRU eviction runs as a side effect of repl_promote_example_if_needed
     * (called from editor_undo_push_snapshot). The user isn't actively
     * saving here, so the layout struct is unavailable — pass NULL and
     * accept the 800x600 fallback in the exported display(). */
    if (!repl_export_save_output(path, source_document_view(), NULL)) {
        g_export_scene_name_hint = NULL;
        return 0;
    }
    g_export_scene_name_hint = NULL;

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
                UserScene *stash = scene_scratch_alloc();
                if (stash) {
                    stash_live_state(stash);
                    int ok = evict_scene_to_workspace(victim);
                    restore_live_from_stash(stash);
                    scene_scratch_free(stash);
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
    g_example_idx       = -1;

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
    if (!dir) { g_workspace_dir[0] = '\0'; return; }
    snprintf(g_workspace_dir, WORKSPACE_DIR_MAX, "%s", dir);
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

void repl_scenes_activate_home_slot(void) {
    const char *name = g_pending_scene_name[0] ? g_pending_scene_name
                                               : USER_SCENE_HOME_NAME;
    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique), name, 0);
    /* Drop any pending example sandbox: workspace import / explicit
     * home activation establishes a fresh user-controlled state. */
    restore_pre_example_cfg_if_valid();
    save_scene_to_slot(0, unique, repl_dispatch_edit_line_get());
    g_active_user_scene = 0;
    g_example_idx       = -1;
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
