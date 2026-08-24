/*
 * src/repl/bootstrap.c - Startup loading helpers.
 */

#include "repl/bootstrap.h"

#include "repl/example_loader.h"
#include "repl/examples.h"
#include "repl/export.h"
#include "repl/host_effects.h"
#include "repl/scenes.h"
#include "repl/state_owners.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define REPL_STARTUP_EXAMPLE_NAME "Lit cube"

static int startup_example_index(void) {
    for (int idx = 0; idx < repl_example_count(); idx++) {
        const char *name = repl_example_name(idx);
        if (name && strcmp(name, REPL_STARTUP_EXAMPLE_NAME) == 0)
            return idx;
    }
    return 0;
}

/* Park the view on the generated display() frame after a startup load.
 *
 * Asks the host rather than counting rows here. The frame is no longer
 * part of the leading chrome: declarations and function definitions
 * render above it, so its panel row depends on how many wrapped rows that
 * prologue occupies - which needs a built code-panel layout, and the REPL
 * pipeline has none. (The old walk searched g_header_pre[] for the
 * display() opener, which lives in g_display_header[] and was never in
 * that array, so it silently fell through to the end of the includes.) */
static void scroll_to_display_function(void) {
    repl_dispatch_scroll_to_display_open();
}

static int activate_new_scene_after_failed_import(const char *source_path) {
    /* A positional file is an explicit request to edit a scene. If it cannot
     * be loaded, keep that workflow useful by opening the same seeded,
     * editable user scene as File -> New Scene rather than a detached empty
     * transient document. */
    (void)repl_scenes_create_empty_user_scene();
    /* Bind it to the file the user named anyway. The scene is empty *because*
     * that file would not load - it may be one someone is still typing in an
     * external editor - and the session is still about it: Ctrl+S must write
     * there rather than to a name-derived `<slug>.c`, and `--watch` must keep
     * following it instead of losing the binding to a slot with no path. */
    repl_scenes_bind_active_source_path(source_path);
    return repl_state_document_count();
}

/* `source_path` is NULL for the stdin route: `-` is a stream, not a file, so
 * there is nothing for Ctrl+S to write back to or for --watch to follow. */
static int activate_initial_document(const ReplImportResult *import_result,
                                     const char *source_path) {
    repl_scenes_activate_loaded_document_slot(import_result->scene_name);
    /* The positional argument is the file this session is *about*: bind it so
     * Ctrl+S rewrites it instead of exporting to a name-derived `<slug>.c`
     * somewhere else, which is also what makes an external editor's save and
     * gl-repl's save name the same file (docs/plans/active/BYOE.md, D1). */
    repl_scenes_bind_active_source_path(source_path);
    scroll_to_display_function();
    return repl_state_document_count();
}

int repl_load_initial_commands(const char *import_file) {
    /* Returns the post-load cursor target. The caller above the REPL boundary
     * applies this value to its cursor state. */
    if (import_file) {
        if (strcmp(import_file, "-") == 0) {
            ReplImportResult import_result;
            if (repl_export_load_from_stream(stdin, "<stdin>", &import_result))
                return activate_initial_document(&import_result, NULL);
            return activate_new_scene_after_failed_import(NULL);
        }

        struct stat st;
        if (stat(import_file, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (repl_load_workspace(import_file) > 0) {
                /* repl_load_workspace leaves the active slot at -1 so
                 * the live document is the pre-load stash (empty at
                 * startup). On a CLI bootstrap that strands the user on
                 * an empty buffer with all the workspace tabs visible
                 * but none of them active. Land on the first occupied
                 * slot. */
                repl_scenes_activate_first_loaded_slot();
                scroll_to_display_function();
                return repl_state_document_count();
            }
        } else {
            ReplImportResult import_result;
            if (repl_export_load_from_file(import_file, &import_result))
                return activate_initial_document(&import_result, import_file);
        }
        /* NULL for a directory: a workspace that failed to load names no
         * single scene file. */
        return activate_new_scene_after_failed_import(
            (stat(import_file, &st) == 0 && S_ISDIR(st.st_mode)) ? NULL
                                                                 : import_file);
    }

    /* Show the startup demo as an example. A user scene is created only when
     * the user explicitly creates one or edits an example (promotion). The
     * startup banner is the controller's to emit (see glr_ctrl_bootstrap_repl);
     * pipeline TUs do not own display-string side effects. */
    int example_edit_line = repl_load_example(startup_example_index());
    scroll_to_display_function();
    return example_edit_line;
}
