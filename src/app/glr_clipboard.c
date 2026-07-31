/*
 * glr_clipboard.c -- OS clipboard access + the editor bridge. See the header
 * for the backend selection rationale.
 */
#include "app/glr_clipboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_includes.h"
#include "editor/clipboard.h"

/* A clipboard big enough to blow past MAX_EDITOR_COMMANDS x MAX_LINE_LEN
 * many times over is not a paste anyone meant to make; refuse it rather than
 * grow a multi-megabyte buffer to truncate. */
#define GLR_CLIPBOARD_MAX_BYTES (1024 * 1024)

#if defined(__EMSCRIPTEN__)
#  define GLR_CLIPBOARD_BACKEND_NONE 1
#elif defined(GLUT_HAS_CLIPBOARD)
#  define GLR_CLIPBOARD_BACKEND_GLUT 1
#elif defined(__APPLE__)
#  define GLR_CLIPBOARD_BACKEND_PB   1
#else
#  define GLR_CLIPBOARD_BACKEND_NONE 1
#endif

/* Only the backends that hand text back (and the bridge below) need this;
 * the no-backend build would carry it as an unused static. */
#if !defined(GLR_CLIPBOARD_BACKEND_NONE)
static char *clipboard_copy_text(const char *text, size_t len) {
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}
#endif

static void clipboard_set_err(char *err, int err_sz, const char *msg) {
    if (err && err_sz > 0)
        snprintf(err, (size_t)err_sz, "%s", msg);
}

/* --- Backends ---------------------------------------------------------- */

#if defined(GLR_CLIPBOARD_BACKEND_GLUT)

char *glr_clipboard_read_text(char *err, int err_sz) {
    const char *text;
    size_t len;

    clipboard_set_err(err, err_sz, "");

    /* freeglut owns the returned string until the next call, so copy it out
     * before anything else can reach the clipboard again. */
    text = glutGetClipboardString();
    if (!text || !text[0]) {
        clipboard_set_err(err, err_sz, "Clipboard is empty");
        return NULL;
    }

    len = strlen(text);
    if (len + 1 > GLR_CLIPBOARD_MAX_BYTES) {
        clipboard_set_err(err, err_sz, "Clipboard text is too large");
        return NULL;
    }

    {
        char *copy = clipboard_copy_text(text, len);
        if (!copy)
            clipboard_set_err(err, err_sz, "Clipboard load: out of memory");
        return copy;
    }
}

void glr_clipboard_write_text(const char *text) {
    if (text)
        glutSetClipboardString(text);
}

#elif defined(GLR_CLIPBOARD_BACKEND_PB)

/* `make glut` links the Apple GLUT framework, which has no clipboard API --
 * shell out the way this file's caller used to before freeglut grew one. */

char *glr_clipboard_read_text(char *err, int err_sz) {
    FILE *pipe;
    char *buf;
    size_t cap = 4096;
    size_t len = 0;
    char chunk[1024];
    size_t nread;
    int read_failed;
    int close_status;

    clipboard_set_err(err, err_sz, "");

    pipe = popen("/usr/bin/pbpaste", "r");
    if (!pipe) {
        clipboard_set_err(err, err_sz, "Clipboard read failed");
        return NULL;
    }

    buf = (char *)malloc(cap);
    if (!buf) {
        pclose(pipe);
        clipboard_set_err(err, err_sz, "Clipboard load: out of memory");
        return NULL;
    }

    while ((nread = fread(chunk, 1, sizeof(chunk), pipe)) > 0) {
        if (len + nread + 1 > GLR_CLIPBOARD_MAX_BYTES) {
            free(buf);
            pclose(pipe);
            clipboard_set_err(err, err_sz, "Clipboard text is too large");
            return NULL;
        }
        if (len + nread + 1 > cap) {
            size_t new_cap = cap;
            char *next;
            while (len + nread + 1 > new_cap)
                new_cap *= 2;
            next = (char *)realloc(buf, new_cap);
            if (!next) {
                free(buf);
                pclose(pipe);
                clipboard_set_err(err, err_sz, "Clipboard load: out of memory");
                return NULL;
            }
            buf = next;
            cap = new_cap;
        }
        memcpy(buf + len, chunk, nread);
        len += nread;
    }

    read_failed = ferror(pipe);
    close_status = pclose(pipe);
    if (read_failed || (close_status != 0 && len == 0)) {
        free(buf);
        clipboard_set_err(err, err_sz, "Clipboard read failed");
        return NULL;
    }
    if (len == 0) {
        free(buf);
        clipboard_set_err(err, err_sz, "Clipboard is empty");
        return NULL;
    }

    buf[len] = '\0';
    return buf;
}

void glr_clipboard_write_text(const char *text) {
    FILE *pipe;

    if (!text)
        return;

    pipe = popen("/usr/bin/pbcopy", "w");
    if (!pipe)
        return;
    fwrite(text, 1, strlen(text), pipe);
    pclose(pipe);
}

#else /* GLR_CLIPBOARD_BACKEND_NONE */

char *glr_clipboard_read_text(char *err, int err_sz) {
    clipboard_set_err(err, err_sz, "No system clipboard on this build");
    return NULL;
}

void glr_clipboard_write_text(const char *text) {
    (void)text;
}

#endif

/* --- Editor bridge ----------------------------------------------------- */

#if defined(GLR_CLIPBOARD_BACKEND_NONE)

void glr_clipboard_install(void) {
}

#else

/*
 * What the system clipboard held the last time the two were in step -- set
 * both when we publish a copy and when we adopt someone else's. Comparing
 * against it is what lets a copy/paste that never left the app keep its
 * payload kind and block-aware line range: an unchanged system clipboard
 * reports "nothing new", and the editor pastes its own richer buffer.
 */
static char *g_last_synced = NULL;

/* Handed to the editor by clipboard_poll_external() and owned here, valid
 * until the next poll -- which is exactly how long the editor uses it (it
 * stages the text into its own buffer before returning). */
static char *g_external = NULL;

static void clipboard_remember_synced(const char *text) {
    char *copy = clipboard_copy_text(text, strlen(text));

    /* On allocation failure fall back to remembering nothing, which costs a
     * redundant adopt on the next paste rather than a wrong one. */
    free(g_last_synced);
    g_last_synced = copy;
}

static void clipboard_publish(const char *text) {
    if (!text)
        return;
    glr_clipboard_write_text(text);
    clipboard_remember_synced(text);
}

static const char *clipboard_poll_external(void) {
    char *text = glr_clipboard_read_text(NULL, 0);

    if (!text)
        return NULL;

    if (g_last_synced && strcmp(text, g_last_synced) == 0) {
        free(text);
        return NULL;
    }

    free(g_external);
    g_external = text;

    /* Adopting counts as syncing: a second paste of the same system
     * clipboard must not re-stage it, or a single-line paste made while an
     * input selection was active would reclassify on the way back in. */
    clipboard_remember_synced(g_external);
    return g_external;
}

static const EditorClipboardHostBridge g_bridge = {
    clipboard_publish,
    clipboard_poll_external,
};

void glr_clipboard_install(void) {
    editor_clipboard_install_host_bridge(&g_bridge);
}

#endif
