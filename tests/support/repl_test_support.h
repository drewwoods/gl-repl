#ifndef REPL_TEST_SUPPORT_H
#define REPL_TEST_SUPPORT_H

#include "app/glr_ctrl.h"   /* glr_ctrl_reset_all */
#include "app/glr_ctrl_replay_annotations.h"
#include "editor/input.h"   /* editor_feed_line */
#include "editor/state.h"   /* editor_state_edit_line */
#include "repl/pipeline.h"
#include "repl/eval.h"
#include "subsystems/replay/replay_annotations.h" /* ReplReplayAnnotationOutput */
#include "repl/state.h"              /* repl_state_* views */
#include "source_document.h"         /* source_document_view */

#include <math.h>

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
    glr_ctrl_reset_all();
    return repl_test_declare_predef_vars(names, 3, err, err_sz);
}

static inline void reset_repl(void) {
    repl_eval_init_predef_vars();
    glr_ctrl_reset_all();
}

static inline int feed_program_lines(const char *const *lines, int count) {
    if (!lines && count > 0)
        return 0;

    for (int i = 0; i < count; i++) {
        if (!editor_feed_line(lines[i]))
            return 0;
    }

    repl_flatten_commands(editor_state_edit_line());
    return 1;
}

#define feed_program(...) \
    feed_program_lines((const char *const[]){ __VA_ARGS__ }, \
        (int)(sizeof((const char *const[]){ __VA_ARGS__ }) / \
              sizeof(const char *)))

static inline int repl_state_flat_count(void) {
    return repl_state_flat_program_count();
}

static inline float predef_val(const char *name) {
    int idx = repl_eval_find_predef_var_idx(name);
    ReplVariableView vars = repl_state_variables();

    if (idx < 0 || idx >= vars.var_count)
        return NAN;
    return vars.vars[idx].value;
}

static inline float scratch_val(const char *array_name, int elem_idx) {
    int array_idx = repl_eval_scratch_array_index(array_name);
    float value = NAN;

    if (array_idx < 0)
        return NAN;
    if (!repl_eval_scratch_get(array_idx, elem_idx, &value))
        return NAN;
    return value;
}

#endif /* REPL_TEST_SUPPORT_H */
