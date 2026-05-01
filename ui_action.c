/*
 * ui_action.c - dispatch deferred UI actions against the live REPL.
 *
 * Phase C-1 implementation. Renderers and input handlers append to a
 * `UiActionList`; the controller drains it once per event by calling
 * `ui_action_dispatch_all()`. Centralizing the switch here keeps the
 * action-API includes off the renderer side of the boundary.
 */
#include "ui_action.h"

#include "repl_actions.h"
#include "repl_command_store.h"
#include "repl_core.h"
#include "repl_clipboard.h"
#include "repl_replay.h"
#include "repl_search.h"
#include "repl_undo.h"
#include "sample.h"
#include "ui_color_picker.h"
#include "ui_menu_bar.h"

static void ui_action_dispatch_one(const UiAction *act) {
    switch (act->kind) {
    case UI_ACTION_NONE:
        return;

    /* Editor / cursor */
    case UI_ACTION_NAVIGATE_TO_LINE:
        repl_navigate_to_line(act->args.line);
        return;
    case UI_ACTION_CURSOR_BLINK_RESET:
        repl_action_cursor_blink_reset();
        return;
    case UI_ACTION_CLEAR_AUTOCOMPLETE:
        clear_autocomplete_state();
        return;
    case UI_ACTION_CLIPBOARD_CLEAR_SELECTION:
        repl_clipboard_clear_selection();
        return;
    case UI_ACTION_SELECTION_START:
        repl_selection_start(act->args.line);
        return;
    case UI_ACTION_SELECTION_SET_END:
        repl_selection_set_end(act->args.line);
        return;

    /* Replay / search */
    case UI_ACTION_REPLAY_TOGGLE_PLAY_PAUSE:
        repl_replay_toggle_play_pause();
        return;
    case UI_ACTION_SEARCH_KEY:
        handle_search_key((unsigned char)act->args.key);
        return;
    case UI_ACTION_NOTE_SEARCH_OPENED:
        ui_menu_bar_note_search_opened();
        return;

    /* Menus */
    case UI_ACTION_MENU_OPEN:
        ui_menu_bar_set_open_menu(act->args.menu_id);
        return;
    case UI_ACTION_MENU_CLOSE:
        ui_menu_bar_close();
        return;
    case UI_ACTION_MENU_ITEM_ACTIVATE: {
        /* Mirror ui_menu_bar_activate_dropdown_item(): close the menu when
         * the activate handler reports the item is "terminal" (action
         * items, file I/O, scene switch, etc.) rather than a toggle/cycle. */
        int close = repl_action_menu_item_activate(act->args.menu.menu_id,
                                                   act->args.menu.item_idx);
        if (close)
            ui_menu_bar_close();
        return;
    }
    case UI_ACTION_CFG_CYCLE_ROW:
        repl_cfg_cycle_row(act->args.cfg.item, act->args.cfg.dir);
        return;

    /* Color picker */
    case UI_ACTION_COLOR_PICKER_OPEN:
        ui_color_picker_open(act->args.color_open.cmd_idx,
                             act->args.color_open.my);
        return;
    case UI_ACTION_COLOR_PICKER_CLOSE:
        ui_color_picker_close();
        return;
    case UI_ACTION_COLOR_SET:
        repl_command_store_set_color(act->args.color.cmd_idx,
                                     act->args.color.r,
                                     act->args.color.g,
                                     act->args.color.b,
                                     act->args.color.a,
                                     act->args.color.has_alpha);
        return;
    case UI_ACTION_CLEAR_COLOR_SET:
        repl_command_store_set_clear_color(act->args.color.cmd_idx,
                                           act->args.color.r,
                                           act->args.color.g,
                                           act->args.color.b,
                                           act->args.color.a);
        return;
    case UI_ACTION_UNDO_PUSH_SNAPSHOT:
        repl_undo_push_snapshot();
        return;
    }
}

void ui_action_dispatch_all(const UiActionList *list) {
    if (!list)
        return;
    for (int i = 0; i < list->count; i++)
        ui_action_dispatch_one(&list->items[i]);
}
