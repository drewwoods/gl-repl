/*
 * src/ui/app/command_description_panel.h - GL command help popup renderer.
 */
#ifndef UI_COMMAND_DESCRIPTION_PANEL_H
#define UI_COMMAND_DESCRIPTION_PANEL_H

typedef struct {
    int visible;
    int window_w, window_h;
    int anchor_px, anchor_py;  /* y-up preferred popup top */
    const char *title;
    const char *body;
} UiCommandDescriptionPanelView;

/* Draw a word-wrapped description card anchored at the click, clamped to the
 * window (menu bar down). The card deliberately overflows the code panel it
 * came from rather than being pushed back over the row it describes. */
void ui_command_description_panel_render(
    const UiCommandDescriptionPanelView *view);

#endif /* UI_COMMAND_DESCRIPTION_PANEL_H */
