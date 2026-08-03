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

static void scroll_to_display_function(void) {
    repl_state_refresh_workspace_header_lines();
    ReplImportExportView meta = repl_state_import_export();
    unsigned collision_mask = repl_export_math_collision_mask();
    int target = meta.workspace_header_line_count;
    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++) {
        if (!repl_export_header_pre_line_visible(line_idx, collision_mask))
            continue;
        if (strcmp(g_header_pre[line_idx], REPL_EXPORT_DISPLAY_OPEN_LINE) == 0)
            break;
        target++;
    }
    repl_dispatch_scroll_to_line(target);
}

static int activate_new_scene_after_failed_import(void) {
    /* A positional file is an explicit request to edit a scene. If it cannot
     * be loaded, keep that workflow useful by opening the same seeded,
     * editable user scene as File -> New Scene rather than a detached empty
     * transient document. */
    (void)repl_scenes_create_empty_user_scene();
    return repl_state_document_count();
}

static int activate_initial_document(const ReplImportResult *import_result) {
    repl_scenes_activate_loaded_document_slot(import_result->scene_name);
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
                return activate_initial_document(&import_result);
            return activate_new_scene_after_failed_import();
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
                return activate_initial_document(&import_result);
        }
        return activate_new_scene_after_failed_import();
    }

    /* Show the startup demo as an example. A user scene is created only when
     * the user explicitly creates one or edits an example (promotion). The
     * startup banner is the controller's to emit (see glr_ctrl_bootstrap_repl);
     * pipeline TUs do not own display-string side effects. */
    int example_edit_line = repl_load_example(startup_example_index());
    scroll_to_display_function();
    return example_edit_line;
}
