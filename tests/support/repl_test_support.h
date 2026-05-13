#ifndef REPL_TEST_SUPPORT_H
#define REPL_TEST_SUPPORT_H

#include "app/glr_ctrl.h"   /* glr_app_reset_all, glr_publish_replay_annotations */
#include "repl/core.h"  /* editor_feed_line, set_status, etc. */
#include "repl/eval.h"
#include "repl/replay_annotations.h" /* ReplReplayAnnotationOutput */
#include "source_document.h"         /* source_document_view */

/* Test-support shim that mirrors what the controller's per-frame snapshot
 * build does for replay annotations: prepare + publish. Tests that drive
 * glr_ctrl_code_panel_apply_scroll_follow_for_test() or other code-panel
 * layout entry points directly (i.e. bypass glr_ctrl_build_ui_snapshot)
 * call this to bring editor_state_virtual_lines into the production
 * shape before the helper runs. Keeps the controller helper operating on
 * already-published state, per the UI/controller boundary. */
static inline void repl_test_publish_replay_annotations(void) {
    ReplReplayAnnotationOutput out;
    replay_annotations_prepare(source_document_view(), &out);
    glr_publish_replay_annotations(&out);
}

static inline int repl_test_declare_predef_vars(const char *const *names,
                                                int count,
                                                char *err,
                                                int err_sz) {
    for (int i = 0; i < count; i++) {
        if (!repl_eval_declare_predef_var(names[i], err, err_sz))
            return 0;
    }
    return 1;
}

static inline int repl_test_reset_with_xyz_vars(char *err, int err_sz) {
    static const char *const names[] = { "x", "y", "z" };

    repl_eval_init_predef_vars();
    glr_app_reset_all();
    return repl_test_declare_predef_vars(names, 3, err, err_sz);
}

#endif /* REPL_TEST_SUPPORT_H */
