#ifndef REPL_TEST_SUPPORT_H
#define REPL_TEST_SUPPORT_H

#include "glr_ctrl.h"   /* glr_app_reset_all */
#include "repl_core.h"  /* repl_feed_line_public, set_status, etc. */
#include "repl_eval.h"

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
