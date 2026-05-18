/*
 * editor_help_session.h - Read-only editor session for the help overlay.
 *
 * Stores the help overlay's session-local navigation state: active tab and
 * scroll offset. This sits alongside the editable code-panel session, but it
 * remains intentionally small because the help overlay is read-only.
 *
 * Visibility is not part of this session state; that chrome flag still lives
 * on UiState. The controller builds the overlay content separately and the UI
 * renderer consumes this session state when drawing or scrolling the help
 * view.
 */
#ifndef EDITOR_HELP_SESSION_H
#define EDITOR_HELP_SESSION_H

typedef struct {
    int tab_idx;
    int scroll;
} EditorHelpSession;

void              editor_help_session_capture(EditorHelpSession *snapshot);
void              editor_help_session_restore(const EditorHelpSession *snapshot);
void              editor_help_session_reset(void);

EditorHelpSession  editor_help_session_view(void);
EditorHelpSession *editor_help_session_mut(void);

/* Narrow read accessors. */
int  editor_help_session_tab_idx(void);
int  editor_help_session_scroll(void);

/* Narrow mutators. */
void editor_help_session_set_tab(int tab_idx);
void editor_help_session_set_scroll(int scroll);
void editor_help_session_scroll_by(int delta);

#endif /* EDITOR_HELP_SESSION_H */
