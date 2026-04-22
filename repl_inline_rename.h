/*
 * repl_inline_rename.h -- Inline scene-rename input overlay.
 *
 * The menu/dropdown affordance that enters rename mode lives in
 * repl_menu_bar.c; the editor's key dispatcher calls into this
 * module first so keystrokes go to the rename buffer instead of
 * the code panel while rename is active.  Commit/cancel are the
 * only mutations: the text is fed to repl_user_scene_rename() on
 * Enter, discarded on Escape.
 */
#ifndef REPL_INLINE_RENAME_H
#define REPL_INLINE_RENAME_H

int  repl_inline_rename_active(void);
int  repl_inline_rename_begin(int slot);              /* 1 on start, 0 if slot invalid */
int  repl_inline_rename_handle_key(unsigned char key); /* returns 1 if consumed */
int  repl_inline_rename_handle_special(int key);       /* returns 1 if consumed */
void repl_inline_rename_cancel(void);

#endif /* REPL_INLINE_RENAME_H */
