/*
 * glr_clipboard.h -- OS clipboard access for the native builds.
 *
 * The editor owns an internal clipboard with typed payloads (source lines vs.
 * a substring of the input buffer); this module is the only place that talks
 * to the *system* clipboard, and it installs the bridge that keeps the two in
 * step (see EditorClipboardHostBridge in src/editor/clipboard.h).
 *
 * Three backends, picked at compile time:
 *
 *   - freeglut's glutSetClipboardString/glutGetClipboardString when the
 *     vendored freeglut is in play (GLUT_HAS_CLIPBOARD). Cocoa implements
 *     them today; the other backends report an empty clipboard until their
 *     implementation lands, which needs no change here.
 *   - pbcopy/pbpaste on the `make glut` fallback build, where GLUT is the
 *     Apple framework and has no clipboard entry points to call.
 *   - none on the web build, whose clipboard arrives through DOM events
 *     instead (src/app/glr_web_io.c) -- installing a bridge there would
 *     fight that path, so glr_clipboard_install() is a no-op.
 */
#ifndef GLR_CLIPBOARD_H
#define GLR_CLIPBOARD_H

/* Installs the editor's OS-clipboard bridge. Call once during startup;
 * calling it on a build with no backend is a harmless no-op. */
void glr_clipboard_install(void);

/* Reads the system clipboard as text. Returns a malloc'd NUL-terminated
 * string the caller frees, or NULL with a message in `err` (empty clipboard,
 * oversized text, no backend, read failure). `err` may be NULL. */
char *glr_clipboard_read_text(char *err, int err_sz);

/* Writes text to the system clipboard. Silently does nothing on a build
 * with no backend. */
void glr_clipboard_write_text(const char *text);

#endif /* GLR_CLIPBOARD_H */
