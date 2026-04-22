#ifndef REPL_MENU_BAR_H
#define REPL_MENU_BAR_H

enum {
    REPL_MENU_BAR_PIN_SEARCH = 0,
    REPL_MENU_BAR_PIN_REPLAY,
    REPL_MENU_BAR_PIN_COUNT
};

void repl_menu_bar_render(void);
void repl_menu_bar_render_search_overlay(int cp_x, int panel_w, int panel_top);
void render_example_dropdown(void);

int  repl_menu_bar_open_menu_id(void);
void repl_menu_bar_close(void);
void repl_menu_bar_set_open_menu(int menu_id);
void repl_menu_bar_open_config(void);
void repl_menu_bar_note_search_opened(void);

int  repl_menu_bar_menu_hit(int mx, int my);
int  repl_menu_bar_pin_hit(int mx, int my);
int  repl_menu_bar_dropdown_item_hit(int mx, int my);
int  repl_menu_bar_activate_dropdown_item(int item_idx);
int  repl_menu_bar_handle_config_right_press(int mx, int my);

int  menu_dropdown_is_open(void);
int  example_dropdown_is_open(void);

#endif /* REPL_MENU_BAR_H */
