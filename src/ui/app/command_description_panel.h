/*
 * src/ui/app/command_description_panel.h - GL command help popup renderer.
 */
#ifndef UI_COMMAND_DESCRIPTION_PANEL_H
#define UI_COMMAND_DESCRIPTION_PANEL_H

typedef struct {
    int visible;
    int window_w, window_h;
    int panel_x, panel_y, panel_w, panel_h; /* y-up code-panel bounds */
    int anchor_px, anchor_py;              /* y-up preferred popup top */
    const char *title;
    const char *body;
} UiCommandDescriptionPanelView;

/* Draw a word-wrapped description card inside the code-panel bounds. */
void ui_command_description_panel_render(
    const UiCommandDescriptionPanelView *view);

#endif /* UI_COMMAND_DESCRIPTION_PANEL_H */
