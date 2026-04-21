#ifndef REPL_ACTIONS_H
#define REPL_ACTIONS_H

typedef enum {
    REPL_MENU_FILE = 0,
    REPL_MENU_SCENE,
    REPL_MENU_CONFIG,
    REPL_MENU_COUNT
} ReplMenuId;

enum {
    REPL_FILE_ITEM_EXPORT = 0,
    REPL_FILE_ITEM_IMPORT,
    REPL_FILE_ITEM_SAVE_WORKSPACE,
    REPL_FILE_ITEM_LOAD_WORKSPACE,
    REPL_FILE_ITEM_COUNT
};

enum {
    REPL_SCENE_OFF_DIVIDER = 1,
    REPL_SCENE_OFF_HDR     = 2,
    REPL_SCENE_OFF_NEW     = 3,
    REPL_SCENE_OFF_SAVE    = 4,
    REPL_SCENE_OFF_RENAME  = 5,
    REPL_SCENE_OFF_SCENES  = 6,
    REPL_SCENE_FIXED_COUNT = 6
};

#define REPL_DEFAULT_WORKSPACE_DIR "./workspace"

void repl_actions_apply_defaults(void);

void repl_cfg_cycle_row(int row, int delta);
int  repl_cfg_handle_ascii_shortcut(unsigned char key);
int  repl_cfg_handle_special_shortcut(int key);

int  repl_action_menu_item_activate(int menu_id, int item_idx);
int  repl_scene_menu_slot_for_dense_index(int scene_idx);

#endif /* REPL_ACTIONS_H */
