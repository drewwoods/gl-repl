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
 * glr_config.h. Startup defaults also live here: glr_actions_apply_defaults()
 * seeds config state before examples or workspace metadata override it.
 */
#ifndef GLR_ACTIONS_H
#define GLR_ACTIONS_H

/* Top-level menu identifiers. Matches the menu bar structure (File / Scene /
 * Tutorials / Config) used by ui_menu_bar and by this module's dispatch. */
typedef enum {
    GLR_MENU_FILE = 0,
    GLR_MENU_SCENE,
    GLR_MENU_TUTORIALS,
    GLR_MENU_CONFIG,
    GLR_MENU_COUNT
} GlrMenuId;

/* File menu item indices. Export/import single files, save/load workspace
 * directories. Item counts are implicit (GLR_FILE_ITEM_COUNT). */
enum {
    GLR_FILE_ITEM_NEW_SCENE = 0,
    GLR_FILE_ITEM_SAVE_SCENE,        /* Ctrl+S */
    GLR_FILE_ITEM_LOAD_SCENE,        /* (was Import) — not implemented yet */
    GLR_FILE_ITEM_RENAME_SCENE,
    GLR_FILE_ITEM_EXPORT_PLY,        /* F11: capture geometry to output.ply */
    GLR_FILE_ITEM_SCENE_SEP,         /* "---" non-actionable divider row */
    GLR_FILE_ITEM_SAVE_WORKSPACE,
    GLR_FILE_ITEM_LOAD_WORKSPACE,
    GLR_FILE_ITEM_COUNT
};

/* Scene menu is a pure selector: "### EXAMPLES" + example rows, then
 * "### MY SCENES" + user-scene rows. Both headers are always present
 * (fixed layout). Offsets are relative to the end of the examples block
 * (e = repl_example_count()): the second header sits at e + HDR and the
 * first user scene at e + SCENES. Lookup uses
 * glr_scene_menu_slot_for_dense_index() to map dense display index to
 * the actual user scene slot. Scene *actions* live in the File menu. */
enum {
    GLR_SCENE_OFF_HDR      = 1,   /* "### MY SCENES" at e + 1 */
    GLR_SCENE_OFF_SCENES   = 2,   /* first user scene row at e + 2 */
    GLR_SCENE_FIXED_COUNT = 2    /* the two "###" header rows */
};

/* Tutorials menu layout when tutorial is active:
 *   [0..t-1]                 tag names (t = repl_tutorial_visible_tag_count())
 *   [t + TUTORIAL_OFF_SEP]   "---" separator row
 *   [t + TUTORIAL_OFF_RESTART] "Restart Tutorial" action
 *   [t + TUTORIAL_OFF_EXIT]  "Exit Tutorial" action */
enum {
    GLR_TUTORIAL_OFF_SEP     = 0,
    GLR_TUTORIAL_OFF_RESTART = 1,
    GLR_TUTORIAL_OFF_EXIT    = 2,
    GLR_TUTORIAL_FIXED_COUNT = 3
};

/* Default workspace directory for save/load operations. Can be overridden by
 * workspace metadata headers or explicit user selection. */
#define GLR_DEFAULT_WORKSPACE_DIR "./workspace"

/* Apply initial presentation configuration defaults at startup. Initializes the
 * presentation config toggles and cycles to their default values. Called during
 * REPL init; examples can override via @cfg metadata headers. */
void glr_actions_apply_defaults(void);

/* Install the export-config bridge that lets src/repl/export.c emit/parse
 * @cfg headers and lets src/repl/scenes.c snapshot per-scene config without
 * referencing glr_config_* directly. Called once during app-service setup. */
void glr_actions_install_export_cfg_bridge(void);
void glr_actions_set_msaa_label(int samples);
void glr_actions_apply_audio_cfg_mode(int mode);

/* Cycle a config item by delta steps. row is the config item index; delta is
 * +1 to cycle forward, -1 to cycle backward. Wraps around at boundaries.
 * Called by keyboard shortcut handlers. */
void glr_cfg_cycle_row(int row, int delta);

/* Reset the code-panel cursor blink state after navigation moves the cursor. */
void glr_action_cursor_blink_reset(void);

/* Cycle the help overlay tabs. */
void glr_action_help_tab_next(void);
void glr_action_help_tab_prev(void);

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
