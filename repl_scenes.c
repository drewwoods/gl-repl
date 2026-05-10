/*
 * repl_scenes.c -- User scene slots, promotion, and workspace save/load.
 */
#include "repl_command_store.h"
#include "repl_core_internal.h"
#include "repl_examples.h"
#include "repl_core.h"
#include "repl_export.h"   /* ReplExportConfig + bridge for per-scene cfg */
#include "repl_state_owners.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SCENE_STATE (repl_state_scenes_mut())
#define g_example_idx (SCENE_STATE->active_example_idx)
#define g_workspace_dir (SCENE_STATE->workspace_dir)
#define IMPORT_EXPORT_STATE (repl_state_import_export_mut())
#define g_export_scene_name_hint (IMPORT_EXPORT_STATE->export_scene_name_hint)
#define g_pending_scene_name (IMPORT_EXPORT_STATE->pending_scene_name)
#define g_pending_workspace_dir (IMPORT_EXPORT_STATE->pending_workspace_dir)

/* Step 4 of feature/decouple-repl-from-gl-repl-alt.md replaced the
 * static N_SCENE_CFG_KEYS subset list with a controller-installed
 * bridge (ReplExportConfigBridge.fill_scene_subset / .apply). The
 * controller knows which slugs belong in the per-scene snapshot
 * because it owns glr_config_*; repl_scenes.c just stores the bag
 * and round-trips it through the bridge.
 *
 * camera_rotate footgun: the slug is included in the bridge's
 * scene-subset fill, so per-scene snapshots still capture/restore it
 * without repl_scenes.c having to call glr_config_*. */

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
 *   coldest slot when the 9th scene is needed. */
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
    char     predef_names[MAX_PREDEF_VARS][16];
    int      num_predef_vars;
    char     func_aliases[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX];
    /* Per-scene cfg snapshot. The bridge (controller-side) owns which
     * slugs belong here; repl_scenes.c just stores and round-trips. */
    ReplExportConfig scene_cfg;
} UserScene;

static UserScene g_user_scenes[MAX_USER_SCENES];
static int       g_active_user_scene = -1;
static uint32_t  g_user_scene_tick = 0;

/* Example sandbox: snapshot of the per-scene cfg taken at the moment
 * we enter an example from non-example state. Restored on the next
 * transition out of example state. Same shape as scene_cfg above: the
 * controller-installed bridge fills/applies the bag.
 *
 * Lifecycle:
 *   - clear at startup (repl_scenes_reset).
 *   - capture on first load_example after non-example state.
 *   - restore on the next user-scene / home load.
 *   - cleared by restore. Entering another example without an
 *     intervening user-scene load leaves the original snapshot
 *     untouched. */
static ReplExportConfig g_pre_example_cfg;
static int              g_pre_example_valid = 0;

#define WORKSPACE_DIR_MAX REPL_WORKSPACE_DIR_MAX

/* Default home-scene name -- used when slot 0 is captured on first example load. */
#define USER_SCENE_HOME_NAME "Your Scene"

static uint32_t next_user_scene_tick(void) {
    return ++g_user_scene_tick;
}

static void capture_pre_example_cfg(void) {
    repl_export_config_clear(&g_pre_example_cfg);
    const ReplExportConfigBridge *bridge = repl_export_config_bridge();
    if (bridge && bridge->fill_scene_subset)
        bridge->fill_scene_subset(&g_pre_example_cfg);
    g_pre_example_valid = 1;
}

static void restore_pre_example_cfg_if_valid(void) {
    if (!g_pre_example_valid) return;
    const ReplExportConfigBridge *bridge = repl_export_config_bridge();
    if (bridge && bridge->apply)
        bridge->apply(&g_pre_example_cfg);
    g_pre_example_valid = 0;
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

static void save_scene_to_slot(int idx, const char *name) {
    if (idx < 0 || idx >= MAX_USER_SCENES) return;
    UserScene *s = &g_user_scenes[idx];
    EditorBufferView text = editor_buffer_view();
    memcpy(s->cmds, repl_state_document_cmds_mut(), (size_t)repl_state_document_count() * sizeof(GLCmd));
    for (int i = 0; i < repl_state_document_count(); i++)
        repl_copy_string_fits(s->lines[i], MAX_LINE_LEN,
                              editor_buffer_view_line(text, i));
    s->num_cmds        = repl_state_document_count();
    s->edit_line       = repl_state_edit_line();
    s->num_predef_vars = g_num_predef_vars;
    for (int i = 0; i < g_num_predef_vars; i++) {
        s->predef_vals[i] = g_predef_vars[i].value;
        memcpy(s->predef_names[i], g_predef_vars[i].name, 16);
    }
    repl_eval_copy_scratch_arrays(s->scratch_arrays);
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        const char *alias = repl_func_alias_get(slot);
        if (alias)
            snprintf(s->func_aliases[slot], REPL_FUNC_NAME_MAX, "%s", alias);
        else
            s->func_aliases[slot][0] = '\0';
    }
    repl_export_config_clear(&s->scene_cfg);
    {
        const ReplExportConfigBridge *bridge = repl_export_config_bridge();
        if (bridge && bridge->fill_scene_subset)
            bridge->fill_scene_subset(&s->scene_cfg);
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
    ReplCommandStore store = repl_command_store_live();
    if (!repl_command_store_load(&store, cmds, num_cmds, edit_line))
        return 0;
    editor_buffer_load_lines(scene_line_ptrs(lines, num_cmds), num_cmds);
    return 1;
}

void repl_scenes_save_active_scene_if_any(void);

static void load_scene_from_slot(int idx) {
    if (idx < 0 || idx >= MAX_USER_SCENES) return;
    UserScene *s = &g_user_scenes[idx];
    if (!s->used) return;
    repl_scenes_save_active_scene_if_any();
    if (!load_commands_into_live(s->cmds, s->lines, s->num_cmds, s->edit_line))
        return;
    repl_state_flat_program_set_count(0);
    g_num_predef_vars = s->num_predef_vars;
    for (int i = 0; i < s->num_predef_vars; i++) {
        g_predef_vars[i].value = s->predef_vals[i];
        memcpy(g_predef_vars[i].name, s->predef_names[i], 16);
    }
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
        const ReplExportConfigBridge *bridge = repl_export_config_bridge();
        if (bridge && bridge->apply)
            bridge->apply(&s->scene_cfg);
    }
    editor_insert_mode_set(0);
    load_line_to_input(repl_state_edit_line());
    mark_normals_dirty();
    s->last_touch       = next_user_scene_tick();
    g_active_user_scene = idx;
    g_example_idx       = -1;
    char msg[128];
    snprintf(msg, sizeof(msg), "Loaded scene: %s", s->name);
    set_status(msg);
}

static void save_user_scene(void) {
    save_scene_to_slot(0, USER_SCENE_HOME_NAME);
}

void repl_scenes_save_active_scene_if_any(void) {
    if (g_active_user_scene >= 0 && g_active_user_scene < MAX_USER_SCENES) {
        save_scene_to_slot(g_active_user_scene,
                           g_user_scenes[g_active_user_scene].name);
    }
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
    g_num_predef_vars = s->num_predef_vars;
    for (int i = 0; i < s->num_predef_vars; i++) {
        g_predef_vars[i].value = s->predef_vals[i];
        memcpy(g_predef_vars[i].name, s->predef_names[i], 16);
    }
    repl_eval_restore_scratch_arrays(s->scratch_arrays);
    /* Apply the slot's saved per-scene cfg to live state. Without
     * this, repl_save_workspace / evict_scene_to_workspace would
     * export the scene with whichever cfg happened to be live when
     * the iteration started — see the [P1] regression test in
     * test_repl_core_io.c. The user-facing scene switch
     * (load_scene_from_slot) already does this; this matches the
     * behaviour. */
    const ReplExportConfigBridge *bridge = repl_export_config_bridge();
    if (bridge && bridge->apply)
        bridge->apply(&s->scene_cfg);
}

static void stash_live_state(UserScene *dst) {
    EditorBufferView text = editor_buffer_view();
    memset(dst, 0, sizeof(*dst));
    memcpy(dst->cmds, repl_state_document_cmds_mut(), (size_t)repl_state_document_count() * sizeof(GLCmd));
    for (int i = 0; i < repl_state_document_count(); i++)
        repl_copy_string_fits(dst->lines[i], MAX_LINE_LEN,
                              editor_buffer_view_line(text, i));
    dst->num_cmds        = repl_state_document_count();
    dst->edit_line       = repl_state_edit_line();
    dst->num_predef_vars = g_num_predef_vars;
    for (int i = 0; i < g_num_predef_vars; i++) {
        dst->predef_vals[i] = g_predef_vars[i].value;
        memcpy(dst->predef_names[i], g_predef_vars[i].name, 16);
    }
    repl_eval_copy_scratch_arrays(dst->scratch_arrays);
    /* Capture live per-scene cfg too so restore_live_from_stash can
     * roll it back. Symmetric with install_scene_into_live now
     * applying s->scene_cfg. */
    repl_export_config_clear(&dst->scene_cfg);
    const ReplExportConfigBridge *bridge = repl_export_config_bridge();
    if (bridge && bridge->fill_scene_subset)
        bridge->fill_scene_subset(&dst->scene_cfg);
}

static void restore_live_from_stash(const UserScene *src) {
    if (!load_commands_into_live(src->cmds, src->lines, src->num_cmds,
                                 src->edit_line))
        return;
    g_num_predef_vars = src->num_predef_vars;
    for (int i = 0; i < src->num_predef_vars; i++) {
        g_predef_vars[i].value = src->predef_vals[i];
        memcpy(g_predef_vars[i].name, src->predef_names[i], 16);
    }
    repl_eval_restore_scratch_arrays(src->scratch_arrays);
    /* Restore live cfg so the user's pre-stash settings survive any
     * intervening install_scene_into_live calls. */
    const ReplExportConfigBridge *bridge = repl_export_config_bridge();
    if (bridge && bridge->apply)
        bridge->apply(&src->scene_cfg);
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

int repl_save_workspace(const char *dir) {
    if (!dir || !*dir) {
        set_status("Workspace save: no folder provided");
        return -1;
    }

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Workspace save: cannot create %s", dir);
        set_status(msg);
        return -1;
    }

    if (g_active_user_scene >= 0 && g_active_user_scene < MAX_USER_SCENES) {
        save_scene_to_slot(g_active_user_scene,
                           g_user_scenes[g_active_user_scene].name);
    }

    snprintf(g_workspace_dir, WORKSPACE_DIR_MAX, "%s", dir);

    UserScene stash;
    stash_live_state(&stash);

    int written = 0;
    for (int s = 0; s < MAX_USER_SCENES; s++) {
        if (!g_user_scenes[s].used) continue;
        install_scene_into_live(s);

        char slug[USER_SCENE_NAME_MAX];
        scene_filename_slug(g_user_scenes[s].name, slug, sizeof(slug));

        char path[WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
        snprintf(path, sizeof(path), "%s/%s.c", dir, slug);

        g_export_scene_name_hint = g_user_scenes[s].name;
        repl_export_save_output(path, editor_buffer_view());
        g_export_scene_name_hint = NULL;
        written++;
    }

    restore_live_from_stash(&stash);

    char msg[256];
    snprintf(msg, sizeof(msg), "Saved %d scene%s to %s",
             written, written == 1 ? "" : "s", dir);
    set_status(msg);
    return written;
}

static int load_scene_file_into_slot(const char *path) {
    load_commands_into_live(NULL, NULL, 0, 0);
    /* Start each imported scene from the built-in predef baseline (`t`).
     * Workspace headers then re-declare any user vars on top. Clearing the
     * table entirely breaks round-tripping for scenes whose expressions
     * reference `t`, because `@var t = ...` cannot re-declare a reserved
     * built-in name. */
    repl_eval_init_predef_vars();

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
    save_scene_to_slot(slot, unique);
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
        char msg[256];
        snprintf(msg, sizeof(msg), "Workspace load: cannot open %s", dir);
        set_status(msg);
        return -1;
    }

    UserScene stash;
    stash_live_state(&stash);
    int stash_example = g_example_idx;
    int stash_active  = g_active_user_scene;

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

    restore_live_from_stash(&stash);
    g_example_idx       = stash_example;
    g_active_user_scene = stash_active;

    snprintf(g_workspace_dir, WORKSPACE_DIR_MAX, "%s", dir);

    char msg[256];
    snprintf(msg, sizeof(msg), "Loaded %d scene%s from %s",
             loaded, loaded == 1 ? "" : "s", dir);
    set_status(msg);
    return loaded;
}

static int evict_scene_to_workspace(int slot) {
    if (slot <= 0 || slot >= MAX_USER_SCENES) return 0;
    if (!g_user_scenes[slot].used)            return 0;
    if (!g_workspace_dir[0])                  return 0;

    if (mkdir(g_workspace_dir, 0755) != 0 && errno != EEXIST) return 0;

    install_scene_into_live(slot);

    char slug[USER_SCENE_NAME_MAX];
    scene_filename_slug(g_user_scenes[slot].name, slug, sizeof(slug));

    char path[WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
    snprintf(path, sizeof(path), "%s/%s.c", g_workspace_dir, slug);

    g_export_scene_name_hint = g_user_scenes[slot].name;
    repl_export_save_output(path, editor_buffer_view());
    g_export_scene_name_hint = NULL;

    g_user_scenes[slot].used = 0;
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
                UserScene stash;
                stash_live_state(&stash);
                int ok = evict_scene_to_workspace(victim);
                restore_live_from_stash(&stash);
                if (ok)
                    slot = victim;
            }
        }
        if (slot < 0) {
            set_status("All user scene slots full -- save workspace to free a slot");
            return -1;
        }
    }

    const char *example_name = repl_examples_name(g_example_idx);
    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique),
                             example_name ? example_name : "Scene", -1);
    save_scene_to_slot(slot, unique);
    g_active_user_scene = slot;
    g_example_idx       = -1;

    char msg[128];
    snprintf(msg, sizeof(msg), "Promoted to scene: %s", unique);
    set_status(msg);
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
    if (g_pre_example_valid) return;
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
    save_scene_to_slot(0, unique);
    g_active_user_scene = 0;
    g_example_idx       = -1;
}

void repl_scenes_reset(void) {
    memset(g_user_scenes, 0, sizeof(g_user_scenes));
    g_active_user_scene = -1;
    g_user_scene_tick = 0;
    g_pre_example_valid = 0;
}
