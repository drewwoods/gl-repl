/*
 * navigation_history.h - Browser-style source-location history.
 *
 * Symbol resolution stays with the app/REPL route that initiates a jump;
 * this editor-owned module only remembers document locations and walks them.
 */
#ifndef EDITOR_NAVIGATION_HISTORY_H
#define EDITOR_NAVIGATION_HISTORY_H

typedef struct {
    int line_idx;
    int char_idx;
} EditorNavigationLocation;

void editor_navigation_history_clear(void);
void editor_navigation_history_record_jump(EditorNavigationLocation source,
                                           EditorNavigationLocation destination);
int editor_navigation_history_step(int direction,
                                   EditorNavigationLocation *out_location);
int editor_navigation_history_can_step(int direction);

#endif /* EDITOR_NAVIGATION_HISTORY_H */
