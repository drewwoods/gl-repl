/*
 * ui_action.h - deferred-dispatch action records for UI input handlers.
 *
 * Phase C of `feature/push-architecture-ui.md`. UI input handlers stop
 * calling `repl_command_store_*` / `repl_action_*` / `repl_undo_*`
 * synchronously; they append `UiAction` records to a `UiActionList` that
 * the controller drains via `ui_action_dispatch_all()` once the input
 * event has been fully routed.
 *
 * This makes `ui_*_handle_*()` functions a pure(er) function of
 * (snapshot, event) -> action list. Tests can inspect the list without a
 * live REPL, replay can record event streams, and the renderer/input
 * boundary becomes uniform.
 *
 * Phase C is being landed in two halves:
 *   C-1 (current): the action vocabulary + dispatch + the
 *        `ui_color_picker_*` input chain as the proven pattern.
 *   C-2 (deferred): `ui_panels_handle_*` and `ui_menu_bar_*` input
 *        bridges, plus `repl_editor.c` top-level dispatch wiring.
 *
 * During C-1, handlers that have not yet been converted continue to call
 * action APIs synchronously; both styles coexist behind a stable C-2
 * conversion target.
 */
#ifndef UI_ACTION_H
#define UI_ACTION_H

#include <stddef.h>

#define UI_ACTION_LIST_CAP 16

typedef enum UiActionKind {
    UI_ACTION_NONE = 0,

    /* Editor / cursor */
    UI_ACTION_NAVIGATE_TO_LINE,           /* args.line */
    UI_ACTION_CURSOR_BLINK_RESET,
    UI_ACTION_CLEAR_AUTOCOMPLETE,
    UI_ACTION_CLIPBOARD_CLEAR_SELECTION,
    UI_ACTION_SELECTION_START,            /* args.line */
    UI_ACTION_SELECTION_SET_END,          /* args.line */

    /* Replay / search */
    UI_ACTION_REPLAY_TOGGLE_PLAY_PAUSE,
    UI_ACTION_SEARCH_KEY,                 /* args.key */
    UI_ACTION_NOTE_SEARCH_OPENED,

    /* Menus */
    UI_ACTION_MENU_OPEN,                  /* args.menu_id */
    UI_ACTION_MENU_CLOSE,
    UI_ACTION_MENU_ITEM_ACTIVATE,         /* args.menu.menu_id, item_idx */
    UI_ACTION_CFG_CYCLE_ROW,              /* args.cfg.item, dir */

    /* Color picker */
    UI_ACTION_COLOR_PICKER_OPEN,          /* args.color_open.cmd_idx, my */
    UI_ACTION_COLOR_PICKER_CLOSE,
    UI_ACTION_COLOR_SET,                  /* args.color.cmd_idx, r, g, b, a, has_alpha */
    UI_ACTION_CLEAR_COLOR_SET,            /* args.color.cmd_idx, r, g, b, a */
    UI_ACTION_UNDO_PUSH_SNAPSHOT,
} UiActionKind;

typedef struct UiAction {
    UiActionKind kind;
    union {
        int line;
        int key;
        int menu_id;
        struct { int menu_id; int item_idx; } menu;
        struct { int item; int dir; } cfg;
        struct { int cmd_idx; int my; } color_open;
        struct { int cmd_idx; float r, g, b, a; int has_alpha; } color;
    } args;
} UiAction;

typedef struct UiActionList {
    UiAction items[UI_ACTION_LIST_CAP];
    int      count;
    int      overflow;  /* set if append exceeded UI_ACTION_LIST_CAP */
} UiActionList;

static inline void ui_action_list_init(UiActionList *list) {
    list->count = 0;
    list->overflow = 0;
}

static inline UiAction *ui_action_list_append(UiActionList *list, UiActionKind kind) {
    if (list->count >= UI_ACTION_LIST_CAP) {
        list->overflow = 1;
        return NULL;
    }
    UiAction *act = &list->items[list->count++];
    act->kind = kind;
    return act;
}

/* Dispatch every action in `list` against the live REPL. Called by the
 * controller after an input event has been routed through ui_*_handle_*().
 * Implemented in ui_action.c so renderer/handler TUs stay free of direct
 * action-API includes for the actions they emit. */
void ui_action_dispatch_all(const UiActionList *list);

#endif /* UI_ACTION_H */
