/*
 * editor_help_session.h - Read-only editor session for the help overlay.
 *
 * Phase G commit 35 entry. The help overlay is conceptually a
 * read-only editor session: scroll, cursor, eventually search and
 * selection over a document the user cannot edit. The session state
 * (tab_idx, scroll) lives here, alongside the editable code-panel
 * session in EditorState. `UiState.help.visible` stays put — that is
 * a chrome flag (whether the overlay is on screen) rather than a
 * property of the session itself.
 *
 * Content provider: ui_help_overlay.c renders the static help text
 * by walking compiled-in tables. Phase G commit 36 generalises the
 * provider seam (for completion); for now the help session points
 * at one specific content shape.
 *
 * Forward path: cursor and search bind to the session in later
 * commits so a single editor input model serves both editable
 * source and read-only help/preview sessions.
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
