/* glr_modal.c - Input-only app modal; actions own commit side effects. */
#include "app/glr_modal.h"

#include <stdio.h>
#include <string.h>

#include "editor/inline_file_prompt.h"
#include "editor/inline_rename.h"
#include "keys.h"

#define GLR_MODAL_TEXT_MAX 256
#define GLR_MODAL_ERROR_MAX 192

static GlrModalKind g_kind;
static char g_text[GLR_MODAL_TEXT_MAX];
static char g_error[GLR_MODAL_ERROR_MAX];
static int g_len;
static int g_context;
static GlrModalCommitFn g_commit;

int glr_modal_begin(GlrModalKind kind, const char *initial_text,
                    int context, GlrModalCommitFn commit) {
    if (kind == GLR_MODAL_NONE || !commit)
        return 0;
    editor_inline_rename_cancel();
    editor_inline_file_prompt_cancel();
    g_kind = kind;
    snprintf(g_text, sizeof(g_text), "%s", initial_text ? initial_text : "");
    g_len = (int)strlen(g_text);
    g_error[0] = '\0';
    g_context = context;
    g_commit = commit;
    return 1;
}

void glr_modal_cancel(void) {
    g_kind = GLR_MODAL_NONE;
    g_text[0] = '\0';
    g_error[0] = '\0';
    g_len = 0;
    g_context = -1;
    g_commit = NULL;
}

int glr_modal_active(void) { return g_kind != GLR_MODAL_NONE; }
GlrModalKind glr_modal_kind(void) { return g_kind; }
const char *glr_modal_text(void) { return glr_modal_active() ? g_text : ""; }
const char *glr_modal_error(void) { return glr_modal_active() ? g_error : ""; }

void glr_modal_set_error(const char *msg) {
    snprintf(g_error, sizeof(g_error), "%s", msg ? msg : "");
}

static int modal_char_ok(unsigned char c) {
    if (c < 32 || c > 126)
        return 0;
    if (g_kind == GLR_MODAL_WORKSPACE_OPEN_PATH)
        return c != '"' && c != '\'' && c != '`' && c != '|' &&
               c != '<' && c != '>' && c != '\\';
    if (g_kind == GLR_MODAL_WORKSPACE_NEW ||
        g_kind == GLR_MODAL_WORKSPACE_SAVE_AS)
        return c != '/' && c != '\\' && c != ':';
    if (g_kind == GLR_MODAL_SCENE_SAVE_AS)
        return c != '/' && c != '\\' && c != ':';
    return 0;
}

int glr_modal_handle_key(unsigned char key) {
    if (!glr_modal_active())
        return 0;
    if (key == KEY_ESC) {
        glr_modal_cancel();
        return 1;
    }
    if (g_kind == GLR_MODAL_CONFIRM_DELETE_SCENE) {
        if ((key == 'y' || key == 'Y') && g_commit &&
            g_commit(g_kind, g_text, g_context))
            glr_modal_cancel();
        return 1;
    }
    if (key == '\r' || key == '\n') {
        if (g_commit && g_commit(g_kind, g_text, g_context))
            glr_modal_cancel();
        return 1;
    }
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (g_len > 0)
            g_text[--g_len] = '\0';
        g_error[0] = '\0';
        return 1;
    }
    if (modal_char_ok(key) && g_len < (int)sizeof(g_text) - 1) {
        g_text[g_len++] = (char)key;
        g_text[g_len] = '\0';
        g_error[0] = '\0';
    }
    return 1;
}

int glr_modal_handle_special(int key) {
    if (!glr_modal_active())
        return 0;
    (void)key;
    return 1;
}
