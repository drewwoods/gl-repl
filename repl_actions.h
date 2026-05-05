/*
 * repl_actions.h - Menu bar dispatch and configuration shortcut handling.
 *
 * Bridges the menu UI with state mutations for the File, Scene, and Config
 * menus. The Scene dropdown starts with built-in examples, then the fixed
 * scene actions, then the dense user-scene list.
 *
 * Menu structure: Three top-level menus (REPL_MENU_FILE, REPL_MENU_SCENE,
 * REPL_MENU_CONFIG). The Scene menu layout is split into an Examples section,
 * then the fixed scene rows, then user scenes; offsets in REPL_SCENE_OFF_*
 * are relative to the start of the Scene section after the examples block.
 *
 * Config shortcut dispatch: Keyboard shortcuts (Ctrl+<key> and F-keys) trigger
 * config toggles/cycles via repl_cfg_handle_ascii_shortcut() and
 * repl_cfg_handle_special_shortcut(). These map key codes to config item indices
 * and call repl_cfg_cycle_row() to apply the change. The mapping is stable via
 * ReplConfigKey (repl_config.h), allowing the help overlay to display current
 * bindings.
 *
 * Default values: repl_actions_apply_defaults() applies initial config state
 * on startup, allowing examples to override via @cfg metadata.
 */
#ifndef REPL_ACTIONS_H
#define REPL_ACTIONS_H

/* Top-level menu identifiers. Corresponds to the menu bar structure (File /
 * Scene / Config). Used by the menu UI (ui_menu_bar.h) and this module's
 * dispatch to route actions. */
typedef enum {
    REPL_MENU_FILE = 0,
    REPL_MENU_SCENE,
    REPL_MENU_CONFIG,
    REPL_MENU_COUNT
} ReplMenuId;

/* File menu item indices. Export/import single files, save/load workspace
 * directories. Item counts are implicit (REPL_FILE_ITEM_COUNT). */
enum {
    REPL_FILE_ITEM_EXPORT = 0,
    REPL_FILE_ITEM_IMPORT,
    REPL_FILE_ITEM_SAVE_WORKSPACE,
    REPL_FILE_ITEM_LOAD_WORKSPACE,
    REPL_FILE_ITEM_COUNT
};

/* Scene menu item offsets and fixed rows. Offsets are relative to the start of
 * the Scene section after the built-in examples block. Fixed rows are the
 * divider, label, new scene, save output, rename, then the user-scene list.
 * Item count is implicit; lookup uses repl_scene_menu_slot_for_dense_index()
 * to map dense display index to the actual user scene slot. */
enum {
    REPL_SCENE_OFF_DIVIDER = 1,
    REPL_SCENE_OFF_HDR     = 2,
    REPL_SCENE_OFF_NEW     = 3,
    REPL_SCENE_OFF_SAVE    = 4,
    REPL_SCENE_OFF_RENAME  = 5,
    REPL_SCENE_OFF_SCENES  = 6,
    REPL_SCENE_FIXED_COUNT = 6
};

/* Default workspace directory for save/load operations. Can be overridden by
 * workspace metadata headers or explicit user selection. */
#define REPL_DEFAULT_WORKSPACE_DIR "./workspace"

/* Apply initial configuration defaults at startup. Initializes all config
 * toggles and cycles to their default values. Called during REPL init;
 * examples can override via @cfg metadata headers. */
void repl_actions_apply_defaults(void);

/* Cycle a config item by delta steps. row is the config item index; delta is
 * +1 to cycle forward, -1 to cycle backward. Wraps around at boundaries.
 * Called by keyboard shortcut handlers. */
void repl_cfg_cycle_row(int row, int delta);

/* Reset the code-panel cursor blink state after navigation moves the cursor. */
void repl_action_cursor_blink_reset(void);

/* Cycle the help overlay tabs. */
void repl_action_help_tab_next(void);
void repl_action_help_tab_prev(void);

/* Handle an ASCII key shortcut for config cycling. Maps key codes (Ctrl+<key>)
 * to config item indices and applies the cycle. Returns 1 if the key matched
 * a config shortcut, 0 otherwise. Called by the editor's key dispatcher
 * (repl_editor.c). */
int  repl_cfg_handle_ascii_shortcut(unsigned char key);

/* Handle a special key shortcut (F-keys, arrows) for config cycling. Maps GLUT
 * special key codes to config item indices and applies the cycle. Returns 1 if
 * the key matched a config shortcut, 0 otherwise. Called by the editor's key
 * dispatcher. */
int  repl_cfg_handle_special_shortcut(int key);

/* Dispatch a menu item activation. menu_id is the top-level menu (File/Scene/Config);
 * item_idx is the item index within that menu. Performs the corresponding action
 * (export, import, save scene, load scene, rename, cycle config, etc.). Returns 1
 * if the menu should close after the action, 0 if it should stay open (for toggles).
 * Called by the menu UI (ui_menu_bar.h). */
int  repl_action_menu_item_activate(int menu_id, int item_idx);

/* Map a dense user scene display index to the actual user scene slot number.
 * scene_idx is the position in the displayed menu (0 = first user scene);
 * returns the slot index into g_user_scenes[]. Used by the scene menu renderer
 * and action dispatcher to locate the correct slot when user scenes have gaps
 * (unused slots are skipped in the display). */
int  repl_scene_menu_slot_for_dense_index(int scene_idx);

#endif /* REPL_ACTIONS_H */
