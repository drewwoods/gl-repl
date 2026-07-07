/*
 * glr_web_io.c - Browser import/export bridge for the Emscripten build.
 *
 * Native builds keep this translation unit inert. The web shell calls the
 * Emscripten exports below to import dropped/opened text and to write the
 * current scene into MEMFS before JavaScript turns it into a download.
 */
#if defined(__EMSCRIPTEN__)

#include "app/glr_ctrl_export.h"
#include "editor/input.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "repl/export.h"
#include "repl/host_effects.h"
#include "repl/scenes.h"
#include "source_document.h"

#include <emscripten/emscripten.h>
#include <stdio.h>

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

#else
typedef int glr_web_io_nonempty_translation_unit;
#endif
