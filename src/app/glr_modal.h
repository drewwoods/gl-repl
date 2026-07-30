/* glr_modal.h - App-owned inline prompt and confirmation state. */
#ifndef GLR_MODAL_H
#define GLR_MODAL_H

typedef enum {
    GLR_MODAL_NONE = 0,
    GLR_MODAL_WORKSPACE_NEW,
    GLR_MODAL_WORKSPACE_SAVE_AS,
    GLR_MODAL_WORKSPACE_OPEN_PATH,
    GLR_MODAL_SCENE_SAVE_AS,
    GLR_MODAL_CONFIRM_DELETE_SCENE
} GlrModalKind;

typedef int (*GlrModalCommitFn)(GlrModalKind kind, const char *text,
                                int context);

int glr_modal_begin(GlrModalKind kind, const char *initial_text,
                    int context, GlrModalCommitFn commit);
void glr_modal_cancel(void);
int glr_modal_active(void);
GlrModalKind glr_modal_kind(void);
const char *glr_modal_text(void);
const char *glr_modal_error(void);
void glr_modal_set_error(const char *msg);
int glr_modal_handle_key(unsigned char key);
int glr_modal_handle_special(int key);

#endif
