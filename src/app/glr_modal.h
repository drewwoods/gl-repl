/* glr_modal.h - App-owned inline prompt and confirmation state. */
#ifndef GLR_MODAL_H
#define GLR_MODAL_H

/* A modal kind is wired across three modules: this one admits characters,
 * glr_ctrl's snapshot builder formats the prompt, and glr_actions commits.
 * All three dispatches are `default:`-less switches over this enum, so
 * -Werror=switch is what stops a new kind from compiling into a modal that
 * captures the keyboard while showing no prompt and accepting no key.
 * Add new kinds before GLR_MODAL_COUNT. */
typedef enum {
    GLR_MODAL_NONE = 0,
    GLR_MODAL_WORKSPACE_NEW,
    GLR_MODAL_WORKSPACE_SAVE_AS,
    GLR_MODAL_WORKSPACE_OPEN_PATH,
    GLR_MODAL_SCENE_SAVE_AS,
    GLR_MODAL_CONFIRM_DELETE_SCENE,
    GLR_MODAL_CONFIRM_WIP_RECOVER,
    GLR_MODAL_COUNT
} GlrModalKind;

typedef int (*GlrModalCommitFn)(GlrModalKind kind, const char *text,
                                int context);

/* Open a modal. Returns 0 (nothing opened) for a kind outside
 * (GLR_MODAL_NONE, GLR_MODAL_COUNT) or a NULL commit callback - refusing to
 * open beats trapping the keyboard in a modal nothing can service. */
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
