/*
 * glr_web_io.c - Browser import/export bridge for the Emscripten build.
 *
 * Native builds keep this translation unit inert. The web shell calls the
 * Emscripten exports below to import dropped/opened text and to write the
 * current scene into MEMFS before JavaScript turns it into a download.
 */
#if defined(__EMSCRIPTEN__)

#include "app/glr_camera.h"
#include "app/glr_ctrl_export.h"
#include "editor/clipboard.h"
#include "editor/input.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "repl/export.h"
#include "repl/host_effects.h"
#include "repl/scenes.h"
#include "repl/state_notify.h"
#include "repl/state_views.h"
#include "source_document.h"

#include <emscripten/emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Browser shell "new" button: mirror File > New Scene's app-side effects
 * without exposing the native File menu in the web build. */
EMSCRIPTEN_KEEPALIVE
int glr_web_new_scene(void) {
    int slot = repl_scenes_create_empty_user_scene();
    if (slot < 0)
        return 0;

    editor_undo_note_wholesale_replacement();
    glr_camera_clear_scene_default();
    return 1;
}

static void web_load_error_status(ReplSceneLoadStatus reason) {
    switch (reason) {
    case REPL_SCENE_LOAD_ERR_EMPTY_PATH:
        repl_set_status_error("Import failed: file is empty");
        break;
    case REPL_SCENE_LOAD_ERR_PARSE:
        repl_set_status_error("Import failed: not a loadable gl-repl scene");
        break;
    case REPL_SCENE_LOAD_ERR_NO_SLOT:
        repl_set_status_error("Import failed: all scene slots are full");
        break;
    default:
        repl_set_status_error("Import failed");
        break;
    }
}

EMSCRIPTEN_KEEPALIVE
int glr_web_load_scene_text(const char *text, const char *name) {
    ReplSceneLoadStatus reason = REPL_SCENE_LOAD_OK;
    const char *fallback = (name && name[0]) ? name : "Imported Scene";
    int slot = repl_load_scene_text_as_new_slot(text, fallback, &reason);
    if (slot < 0) {
        web_load_error_status(reason);
        return 0;
    }

    editor_undo_note_wholesale_replacement();
    editor_load_line_to_input(editor_state_edit_line());

    char msg[128];
    snprintf(msg, sizeof(msg), "Imported scene: %s (slot %d)", fallback, slot);
    repl_set_status(msg);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int glr_web_export_scene(const char *path) {
    const char *target = (path && path[0]) ? path : "/tmp/gl-repl-scene.c";
    ReplExportLayout layout;
    glr_ctrl_fill_export_layout(&layout);
    if (!repl_export_save_output(target, source_document_view(), &layout))
        return 0;

    repl_set_status("Prepared scene download");
    return 1;
}

/*
 * URL-share bridge. The shell encodes shareable state into location.hash
 * (deflate + base64url, all on the JS side - see shell.html): a full-scene
 * payload reuses glr_web_export_scene / glr_web_load_scene_text unchanged,
 * while these two exports supply and apply the lighter config-only payload
 * (the same `@cfg slug = value` directive vocabulary the workspace header
 * round-trips through files).
 */

/* Newline-joined `// @cfg slug = value` lines for the current presentation
 * state - the config-only share payload. Rebuilt on every call; the returned
 * pointer stays valid until the next call (ccall copies it out synchronously). */
EMSCRIPTEN_KEEPALIVE
const char *glr_web_cfg_share_text(void) {
    static char buf[MAX_WORKSPACE_HEADER_LINES * WORKSPACE_HEADER_LINE_LEN];

    repl_state_refresh_workspace_header_lines();
    ReplImportExportView ie = repl_state_import_export();

    size_t off = 0;
    for (int i = 0; i < ie.workspace_header_line_count; i++) {
        /* Header lines are emitted as C89 block comments wrapping
         * "@cfg k = v"; keep only the @cfg ones, re-emitted in the `//`
         * form the public single-line parser accepts. */
        const char *at = strstr(ie.workspace_header_lines[i], "@cfg ");
        if (!at)
            continue;
        const char *end = strstr(at, "*/");
        size_t len = end ? (size_t)(end - at) : strlen(at);
        while (len > 0 && (at[len - 1] == ' ' || at[len - 1] == '\t'))
            len--;
        if (off + len + 4 >= sizeof(buf))
            break;
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "// %.*s\n",
                                (int)len, at);
    }
    buf[off] = '\0';
    return buf;
}

/* Apply a decoded config-only payload to the current scene. Only @cfg
 * directives are honored - @scene-name / @workspace-dir / @func would stage
 * pending import state nothing on this path consumes. Returns the number of
 * directives accepted. */
EMSCRIPTEN_KEEPALIVE
int glr_web_apply_cfg_text(const char *text) {
    if (!text || !text[0])
        return 0;

    int matched = 0;
    char line[WORKSPACE_HEADER_LINE_LEN];
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 0 && p[len - 1] == '\r')
            len--;
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        if (strstr(line, "@cfg ") &&
            repl_state_parse_workspace_header_line(line))
            matched++;
        if (!eol)
            break;
        p = eol + 1;
    }

    if (matched) {
        repl_export_apply_pending_cfg();
        repl_set_status("Applied shared settings from URL");
    }
    return matched;
}

/*
 * OS clipboard bridge for Ctrl/Cmd+C/X/V. The browser/OS clipboard is the
 * source of truth on web (see packaging/web/gl4es_bootstrap.c's
 * glrIsPlainClipboardCombo / GLUT.getASCIIKey override and shell.html's
 * copy/cut/paste DOM event listeners); these exports adapt that to the
 * existing internal EditorClipboardState machinery in src/editor/clipboard.c
 * so copy/cut/paste guards, undo, and the paste pipeline are reused as-is.
 */

/* Freed and replaced on the next copy/cut; the string JS reads via
 * glr_web_clipboard_text() must stay valid only until then, which holds
 * since ccall('...', 'string', ...) reads it synchronously. */
static char *g_web_clipboard_text = NULL;
static const char *g_web_clipboard_kind = "lines";

static void web_clipboard_clear_snapshot(void) {
    free(g_web_clipboard_text);
    g_web_clipboard_text = NULL;
    g_web_clipboard_kind = "lines";
}

/* Snapshots the just-populated EditorClipboardState into g_web_clipboard_text
 * (newline-joined for EDITOR_CLIPBOARD_LINES). Returns 0 (and leaves the
 * snapshot cleared) if the clipboard is empty. */
static int web_clipboard_snapshot_from_editor(void) {
    const EditorClipboardState *cb = editor_state_clipboard();
    web_clipboard_clear_snapshot();

    if (cb->kind == EDITOR_CLIPBOARD_INPUT_TEXT) {
        g_web_clipboard_kind = "input";
        g_web_clipboard_text = malloc((size_t)cb->input_text_len + 1);
        if (g_web_clipboard_text) {
            memcpy(g_web_clipboard_text, cb->input_text, (size_t)cb->input_text_len);
            g_web_clipboard_text[cb->input_text_len] = '\0';
        }
    } else if (cb->kind == EDITOR_CLIPBOARD_LINES) {
        g_web_clipboard_kind = "lines";
        size_t total = 1;
        for (int i = 0; i < cb->line_count; i++)
            total += strlen(cb->lines[i]) + 1;
        g_web_clipboard_text = malloc(total);
        if (g_web_clipboard_text) {
            char *p = g_web_clipboard_text;
            for (int i = 0; i < cb->line_count; i++) {
                size_t len = strlen(cb->lines[i]);
                memcpy(p, cb->lines[i], len);
                p += len;
                if (i + 1 < cb->line_count)
                    *p++ = '\n';
            }
            *p = '\0';
        }
    } else {
        return 0;
    }

    return g_web_clipboard_text != NULL;
}

EMSCRIPTEN_KEEPALIVE
int glr_web_clipboard_copy(void) {
    if (!editor_clipboard_copy_current_with_result()) {
        web_clipboard_clear_snapshot();
        return 0;
    }
    return web_clipboard_snapshot_from_editor();
}

EMSCRIPTEN_KEEPALIVE
int glr_web_clipboard_cut(void) {
    if (!editor_clipboard_cut_current_with_result()) {
        web_clipboard_clear_snapshot();
        return 0;
    }
    return web_clipboard_snapshot_from_editor();
}

EMSCRIPTEN_KEEPALIVE
const char *glr_web_clipboard_text(void) {
    return g_web_clipboard_text ? g_web_clipboard_text : "";
}

EMSCRIPTEN_KEEPALIVE
const char *glr_web_clipboard_kind(void) {
    return g_web_clipboard_kind;
}

/* Splits text on '\n' (tolerating a '\r' before it) into
 * EditorClipboardState.lines, dropping empty segments (in particular the
 * trailing one a terminal newline produces) so a blank-line-separated
 * external paste doesn't abort the whole multi-line paste on a blank
 * "command". Clamped to MAX_EDITOR_COMMANDS lines of MAX_LINE_LEN-1 chars each. */
static void web_clipboard_stage_lines(const char *text) {
    EditorClipboardState *cb = editor_state_clipboard_mut();
    int n = 0;
    const char *p = text;

    while (*p && n < MAX_EDITOR_COMMANDS) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 0 && p[len - 1] == '\r')
            len--;
        if (len > 0) {
            if (len >= MAX_LINE_LEN)
                len = MAX_LINE_LEN - 1;
            memcpy(cb->lines[n], p, len);
            cb->lines[n][len] = '\0';
            n++;
        }
        if (!eol)
            break;
        p = eol + 1;
    }

    editor_state_clipboard_count_set(n);
}

EMSCRIPTEN_KEEPALIVE
int glr_web_clipboard_paste_text(const char *text, const char *kind) {
    if (!text || !text[0])
        return 0;

    /* A gl-repl-originated copy round-tripping back in carries its exact
     * kind via the custom clipboardData MIME type (see shell.html), so it
     * pastes back exactly as it was copied (input text stays input text,
     * even mid-line, rather than becoming a full source-line paste).
     * Anything else (external app, or the custom type didn't survive the
     * clipboard) falls back to a plain-text heuristic. */
    if (kind && strcmp(kind, "input") == 0) {
        editor_clipboard_set_input_text(text, (int)strlen(text));
    } else if (kind && strcmp(kind, "lines") == 0) {
        web_clipboard_stage_lines(text);
    } else if (strchr(text, '\n')) {
        web_clipboard_stage_lines(text);
    } else if (editor_input_selection_active() || editor_insert_mode()) {
        editor_clipboard_set_input_text(text, (int)strlen(text));
    } else {
        web_clipboard_stage_lines(text);
    }

    editor_clipboard_paste_current();
    return 1;
}

#else
typedef int glr_web_io_nonempty_translation_unit;
#endif
