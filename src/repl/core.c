/*
 * src/repl/core.c - Residual REPL helpers awaiting redistribution.
 *
 * This translation unit is being dissolved into its natural owners.
 * What still lives here (per the R10 plan in ARCHITECTURE.md):
 *
 *   - load_initial_commands() and a handful of startup helpers — pending move
 *     to src/repl/scenes.c.
 * For the live module map see MODULES.md. Editor input dispatch lives in
 * src/editor/input.c; cross-subsystem routing lives in glr_ctrl.c; commit
 * handlers live in src/editor/commit.c. The deleted repl_editor.{c,h} and
 * repl_commit.{c,h} are hard-guarded against return.
 */

#include "repl/core.h"
#include "source_document.h"
#include "repl/command_spec.h"
#include "repl/scenes.h"
#include "repl/export.h"
#include "repl/state_owners.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

static const char *outfile = "output.c";

/* ========================================================================= */
/* Global state                                                               */
/* ========================================================================= */

void repl_mark_source_dirty(void) {
    repl_state_mark_source_dirty();
}

/* (no display list - commands are executed directly each frame) */

const char *cmd_type_name(CmdType t) {
    return repl_cmd_type_name(t);
}

/* ========================================================================= */
/* Initialization                                                             */
/* ========================================================================= */

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

static int load_initial_commands(const char *import_file) {
    /* Returns the post-load cursor target. Caller (controller above
     * the β boundary) applies via editor_state_edit_line_set
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
    }

    /* Show example 0 as a starting demo, then anchor slot 0 ("My Scene")
     * to the current live state so user edits accumulate there and persist
     * across example switches. The startup banner is the controller's
     * to emit (see glr_ctrl_bootstrap_repl); pipeline TUs do not own
     * display-string side effects. */
    int example_edit_line = repl_load_example(0);
    repl_scenes_activate_home_slot(NULL);
    scroll_to_display_function();
    return example_edit_line;
}

void repl_save_default_output(const ReplExportLayout *layout) {
    (void)repl_export_save_output(outfile, source_document_view(), layout);
}

int repl_load_initial_commands(const char *import_file) {
    return load_initial_commands(import_file);
}

/* repl_reset_state was removed in step 2 of
 * feature/decouple-repl-from-gl-repl-alt.md. Tests and callers that
 * want full-world reset call glr_ctrl_reset_all() (declared in
 * glr_ctrl.h). REPL-only callers can use repl_state_reset_program(). */
