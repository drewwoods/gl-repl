/*
 * src/repl/core.c - Residual REPL helpers awaiting redistribution.
 *
 * This translation unit is being dissolved into its natural owners.
 * For the live module map see MODULES.md. Editor input dispatch lives in
 * src/editor/input.c; cross-subsystem routing lives in glr_ctrl.c; commit
 * handlers live in src/editor/commit.c. The deleted repl_editor.{c,h} and
 * repl_commit.{c,h} are hard-guarded against return.
 */

#include "repl/core.h"
#include "source_document.h"
#include "repl/export.h"

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

static const char *outfile = "output.c";

void repl_save_default_output(const ReplExportLayout *layout) {
    (void)repl_export_save_output(outfile, source_document_view(), layout);
}

/* repl_reset_state was removed in step 2 of
 * feature/decouple-repl-from-gl-repl-alt.md. Tests and callers that
 * want full-world reset call glr_ctrl_reset_all() (declared in
 * glr_ctrl.h). REPL-only callers can use repl_state_reset_program(). */
