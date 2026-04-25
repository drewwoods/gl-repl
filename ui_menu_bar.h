#ifndef UI_MENU_BAR_H
#define UI_MENU_BAR_H

enum {
    REPL_MENU_BAR_PIN_SEARCH = 0,
    REPL_MENU_BAR_PIN_REPLAY,
    REPL_MENU_BAR_PIN_COUNT
};

void ui_menu_bar_render(void);
void ui_menu_bar_render_search_overlay(int cp_x, int panel_w, int panel_top);
void ui_menu_bar_render_example_dropdown(void);

int  ui_menu_bar_open_menu_id(void);
void ui_menu_bar_close(void);
void ui_menu_bar_set_open_menu(int menu_id);
void ui_menu_bar_open_config(void);
void ui_menu_bar_note_search_opened(void);

int  ui_menu_bar_menu_hit(int mx, int my);
int  ui_menu_bar_pin_hit(int mx, int my);
int  ui_menu_bar_dropdown_item_hit(int mx, int my);
int  ui_menu_bar_activate_dropdown_item(int item_idx);
int  ui_menu_bar_handle_config_right_press(int mx, int my);

int  ui_menu_bar_menu_dropdown_is_open(void);
int  ui_menu_bar_example_dropdown_is_open(void);

#endif /* UI_MENU_BAR_H */
