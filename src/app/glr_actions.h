/*
 * glr_actions.h - App-shell menu dispatch and config shortcut handling.
 *
 * Owns the non-editor actions surfaced through the menu bar and related
 * keyboard shortcuts: scene/workspace load-save flows, scene rename,
 * example and user-scene switching, help-tab cycling, cursor blink reset,
 * and config item cycling.
 *
 * This header is the bridge between UI/controller event routing and the
 * app-owned action implementations. The menu bar and scene tabs report
 * selections in terms of top-level menu ids and item indices; this module
 * translates those into the corresponding scene/config operation.
 *
 * Config shortcut dispatch is part of the same surface because the key map
 * is derived from the same GlrConfigKey-backed descriptor table exposed by
 * glr_config.h. Audio-session restoration also lives here:
 * glr_actions_apply_defaults() restores the persisted audio mode before the
 * first frame.
 */
#ifndef GLR_ACTIONS_H
#define GLR_ACTIONS_H

#include "app/glr_paths.h"

/* Top-level menu identifiers. Matches the menu bar structure (File / Scene /
 * Tutorials / Tours / Config / Audio) used by ui_menu_bar and by this
 * module's dispatch. */
typedef enum {
    GLR_MENU_FILE = 0,
    GLR_MENU_SCENE,
    GLR_MENU_TUTORIALS,
    GLR_MENU_TOURS,
    GLR_MENU_CONFIG,
    GLR_MENU_AUDIO,
    GLR_MENU_COUNT
} GlrMenuId;

/* File menu item indices. Export/import single files, save/load workspace
 * directories. Item counts are implicit (GLR_FILE_ITEM_COUNT). */
enum {
    GLR_FILE_ITEM_NEW_SCENE = 0,
    GLR_FILE_ITEM_SAVE_SCENE,        /* Ctrl+S */
    GLR_FILE_ITEM_SAVE_GLR,          /* Scene as authoring-format .glr source */
    GLR_FILE_ITEM_LOAD_SCENE,        /* Prompt for a .c scene path */
    GLR_FILE_ITEM_RENAME_SCENE,
    GLR_FILE_ITEM_EXPORT_PLY,        /* Export geometry to active scene's .ply path */
    GLR_FILE_ITEM_SPLIT_DECL,        /* split multi-var decl at cursor */
    GLR_FILE_ITEM_SCENE_SEP,         /* "---" non-actionable divider row */
    GLR_FILE_ITEM_WORKSPACE_HDR,     /* "### WORKSPACE: <name>" inert header */
    GLR_FILE_ITEM_NEW_WORKSPACE,
    GLR_FILE_ITEM_SAVE_WORKSPACE,
    GLR_FILE_ITEM_SAVE_WORKSPACE_AS,
    GLR_FILE_ITEM_OPEN_WORKSPACE,
    GLR_FILE_ITEM_REVEAL_WORKSPACE,
    GLR_FILE_ITEM_DELETE_SCENE,
    GLR_FILE_ITEM_QUIT_SEP,          /* "---" non-actionable divider row */
    GLR_FILE_ITEM_QUIT,              /* Ctrl+Q */
    GLR_FILE_ITEM_COUNT
};

/* Scene menu is a selector with two navigation actions: "### EXAMPLES" +
 * tag names + Next/Previous + "### MY SCENES" + user scenes. */
enum {
    GLR_SCENE_OFF_SEP_TOP  = 1,   /* "---" at e + 1 */
    GLR_SCENE_OFF_NEXT     = 2,   /* "Next" (F12) at e + 2 */
    GLR_SCENE_OFF_PREV     = 3,   /* "Previous" (Shift+F12) at e + 3 */
    GLR_SCENE_OFF_SEP_BOT  = 4,   /* "---" at e + 4 */
    GLR_SCENE_OFF_HDR      = 5,   /* "### MY SCENES" at e + 5 */
    GLR_SCENE_OFF_SCENES   = 6,   /* first user scene row at e + 6 */
    GLR_SCENE_FIXED_COUNT  = 6    /* 2 headers + 2 dividers + Next + Previous */
};

/* Tutorials menu layout:
 *   [0..t-1]                 tag names (t = repl_tutorial_visible_tag_count())
 *   [t + TUTORIAL_OFF_SEP_TOP] "---" separator row
 *   [t + TUTORIAL_OFF_NEXT]  "Next" action (F11)
 *   [t + TUTORIAL_OFF_PREV]  "Previous" action (Shift+F11)
 *   If active:
 *     [t + TUTORIAL_OFF_SEP_MID] "---" separator row
 *     [t + TUTORIAL_OFF_RESTART] "Restart Tutorial" action
 *     [t + TUTORIAL_OFF_EXIT]  "Exit Tutorial" action */
enum {
    GLR_TUTORIAL_OFF_SEP_TOP           = 0,
    GLR_TUTORIAL_OFF_NEXT              = 1,
    GLR_TUTORIAL_OFF_PREV              = 2,
    GLR_TUTORIAL_OFF_SEP_MID           = 3,
    GLR_TUTORIAL_OFF_RESTART           = 4,
    GLR_TUTORIAL_OFF_EXIT              = 5,
    GLR_TUTORIAL_FIXED_COUNT_ACTIVE    = 6,
    GLR_TUTORIAL_FIXED_COUNT_INACTIVE  = 3
};

/* Audio menu layout:
 *   [0..g-1]               source group rows (g = visible audio groups)
 *   [g + AUDIO_OFF_SEP]    "---" separator row
 *   [g + AUDIO_OFF_PLAY]   "Play" / "Pause" action
 *   [g + AUDIO_OFF_BACK10] "Jump Back 10s" action
 *   [g + AUDIO_OFF_FWD10]  "Jump Forward 10s" action
 *   [g + AUDIO_OFF_NEXT]   "Next Track" action
 *   [g + AUDIO_OFF_PREV]   "Previous Track" action
 *   [g + AUDIO_OFF_LOOP]   "Loop: <mode>" action */
enum {
    GLR_AUDIO_OFF_SEP    = 0,
    GLR_AUDIO_OFF_PLAY   = 1,
    GLR_AUDIO_OFF_BACK10 = 2,
    GLR_AUDIO_OFF_FWD10  = 3,
    GLR_AUDIO_OFF_NEXT   = 4,
    GLR_AUDIO_OFF_PREV   = 5,
    GLR_AUDIO_OFF_LOOP   = 6,
    GLR_AUDIO_FIXED_COUNT = 7
};

/* Restore the persisted audio mode into the app config at startup. This is
 * audio-only; presentation defaults are owned by glr_state and scene/example
 * reset paths. */
#define AUDIO_CFG_PAUSE 0
#define AUDIO_CFG_ALL   1

const char *glr_actions_audio_mode_status_string(int mode);
void glr_actions_apply_defaults(void);

/* Install the export-config bridge that lets src/repl/export.c emit/parse
 * @cfg headers and lets src/repl/scenes.c snapshot per-scene config without
 * referencing glr_config_* directly. Called once during app-service setup. */
void glr_actions_install_export_cfg_bridge(void);
void glr_actions_set_msaa_label(int samples);
void glr_actions_apply_audio_cfg_mode(int mode);
void glr_action_toggle_audio_play_pause(void);

/* Cycle a config item by delta steps. row is the config item index; delta is
 * +1 to cycle forward, -1 to cycle backward. Wraps around at boundaries.
 * Called by keyboard shortcut handlers. */
void glr_cfg_cycle_row(int row, int delta);

/* Toggle the 2D/3D view mode (the "View mode" config row). Reuses the
 * Config-row cycle, so it shares the keybind/menu status + transition.
 * Called by the menu-bar 2D/3D swatch click. */
void glr_action_toggle_view_mode(void);

/* Reset the code-panel cursor blink state after navigation moves the cursor. */
void glr_action_cursor_blink_reset(void);

/* Cycle the help overlay tabs. */
void glr_action_help_tab_next(void);
void glr_action_help_tab_prev(void);
void glr_action_reset_time_to_zero(void);

/* Handle an ASCII key shortcut for config cycling. Maps key codes (Ctrl+<key>)
 * to config item indices and applies the cycle. Returns 1 if the key matched
 * a config shortcut, 0 otherwise. Called by the controller's key router. */
int  glr_cfg_handle_ascii_shortcut(unsigned char key);

/* Handle a special key shortcut (F-keys, arrows) for config cycling. Maps GLUT
 * special key codes to config item indices and applies the cycle. Returns 1 if
 * the key matched a config shortcut, 0 otherwise. Called by the controller's
 * special-key router. */
int  glr_cfg_handle_special_shortcut(int key);

/* Dispatch a menu item activation. menu_id is the top-level menu (File/Scene/Config);
 * item_idx is the item index within that menu. Performs the corresponding action
 * (export, import, save scene, load scene, rename, cycle config, etc.). Returns 1
 * if the menu should close after the action, 0 if it should stay open (for toggles).
 * Called by the menu UI (ui_menu_bar.h). */
int  glr_action_menu_item_activate(int menu_id, int item_idx);
int  glr_action_save_active_scene(void);
int  glr_action_open_workspace_index(int idx);
int  glr_action_open_workspace_path(const char *path);
void glr_action_begin_open_workspace_path(void);

/* Built-in example metadata exposed through the app layer for CLI and UI
 * callers that should not include repl/examples.h directly. */
int         glr_scene_example_count(void);
const char *glr_scene_example_name(int idx);

/* Map a dense user scene display index to the actual user scene slot number.
 * scene_idx is the position in the displayed menu (0 = first user scene);
 * returns the slot index into g_user_scenes[]. Used by the scene menu renderer
 * and action dispatcher to locate the correct slot when user scenes have gaps
 * (unused slots are skipped in the display). */
int  glr_scene_menu_slot_for_dense_index(int scene_idx);

/* Shared scene-load sequences used by both the Scene menu and the scene
 * tab strip router (glr_ctrl.c). Neither self-no-ops on "already active";
 * the menu always reloads, so a no-op-wanting caller checks first.
 * glr_scene_load_example takes an explicit index (the menu loads the
 * clicked example, NOT the active one). */
void glr_scene_load_example(int example_idx);
void glr_scene_load_user_slot(int slot);

#endif /* GLR_ACTIONS_H */
