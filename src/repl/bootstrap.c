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

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define REPL_STARTUP_EXAMPLE_NAME "Rotating cube"

static int startup_example_index(void) {
    for (int idx = 0; idx < repl_example_count(); idx++) {
        const char *name = repl_example_name(idx);
        if (name && strcmp(name, REPL_STARTUP_EXAMPLE_NAME) == 0)
            return idx;
    }
    return 0;
}

static void scroll_to_display_function(void) {
    repl_state_refresh_workspace_header_lines();
    ReplImportExportView meta = repl_state_import_export();
    int target = meta.workspace_header_line_count;
    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++) {
        if (strcmp(g_header_pre[line_idx], REPL_EXPORT_DISPLAY_OPEN_LINE) == 0)
            break;
        target++;
    }
    repl_dispatch_scroll_to_line(target);
}

static int activate_empty_home_after_failed_import(void) {
    repl_scenes_reset_for_transient();
    repl_scenes_activate_home_slot(NULL);
    return repl_state_document_count();
}

int repl_load_initial_commands(const char *import_file) {
    /* Returns the post-load cursor target. Caller (controller above
     * the beta boundary) applies via editor_state_edit_line_set
     * (implemented in phase 3.6.4; see the edit-line-ownership
     * plan doc). */
    if (import_file) {
        struct stat st;
        if (stat(import_file, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (repl_load_workspace(import_file) > 0) {
                /* repl_load_workspace leaves the active slot at -1 so
                 * the live document is the pre-load stash (empty at
                 * startup). On a CLI bootstrap that strands the user on
                 * an empty buffer with all the workspace tabs visible
                 * but none of them active. Land on the first occupied
                 * slot — symmetric with the single-file branch below
                 * (which activates the home slot). */
                repl_scenes_activate_first_loaded_slot();
                scroll_to_display_function();
                return repl_state_document_count();
            }
        } else {
            ReplImportResult import_result;
            if (repl_export_load_from_file(import_file, &import_result)) {
                repl_scenes_activate_home_slot(import_result.scene_name);
                scroll_to_display_function();
                return repl_state_document_count();
            }
        }
        return activate_empty_home_after_failed_import();
    }

    /* Show the startup demo, then anchor slot 0 ("My Scene") to the current
     * live state so user edits accumulate there and persist across example
     * switches. The startup banner is the controller's to emit (see
     * glr_ctrl_bootstrap_repl); pipeline TUs do not own display-string side
     * effects. */
    int example_edit_line = repl_load_example(startup_example_index());
    repl_scenes_activate_home_slot(NULL);
    scroll_to_display_function();
    return example_edit_line;
}
