/*
 * editor_services.c -- Default EditorServices bound to live REPL.
 *
 * The implementation is a thin shim over `repl_compile_*` and
 * `repl_apply_*`. The compile hook walks the registered compile
 * handlers in canonical order; later phases extend the dispatch as
 * structured-block handlers migrate.
 *
 * The handler order matters: `repl_compile_float_decl` must run
 * before `repl_compile_var_assign`, otherwise `float x;` would parse
 * as an assignment to an identifier named `float`. The legacy
 * `try_commit_*` chain has the same ordering invariant; this
 * dispatcher inherits it.
 */

#include "editor_services.h"

#include "repl_apply.h"
#include "repl_compile.h"

/* Compile dispatcher. Walks the registered compile handlers in
 * canonical order and returns the first one that produces a
 * non-NO_CHANGE result. NO_CHANGE means the input didn't match any
 * handler — the caller decides whether that is "fall through to
 * the next dispatcher" or "no commit happened". */
ReplCompileResult repl_compile_dispatch(const char *text,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!out || !ctx) return REPL_COMPILE_ERROR;

    /* Float decl first: `float x;` would otherwise parse as an
     * assignment to identifier `float`. */
    ReplCompileResult r = repl_compile_float_decl(text, ctx, out,
                                                  err, err_size);
    if (r == REPL_COMPILE_ERROR) return r;
    if (out->kind != REPL_COMPILED_NO_CHANGE) return REPL_COMPILE_OK;

    /* Var assignment: `x = expr;`. */
    r = repl_compile_var_assign(text, ctx, out, err, err_size);
    if (r == REPL_COMPILE_ERROR) return r;
    if (out->kind != REPL_COMPILED_NO_CHANGE) return REPL_COMPILE_OK;

    /* Phase D commits 26b-26e add: close_brace, if_block,
     * func_def, for_loop. Until then, structured-block syntax
     * remains routed through the legacy try_commit_* dispatchers
     * in repl_commit.c — `repl_compile_dispatch` returns NO_CHANGE
     * for those inputs and the caller falls through. */

    out->kind = REPL_COMPILED_NO_CHANGE;
    return REPL_COMPILE_OK;
}

/* ---- Default service bindings ----------------------------------- */

static ReplCompileContext default_context(void *user) {
    (void)user;
    return repl_compile_context_from_live();
}

static ReplCompileResult default_compile(const char *text,
                                         const ReplCompileContext *ctx,
                                         ReplCompiledChange *out,
                                         char *err, int err_size,
                                         void *user) {
    (void)user;
    return repl_compile_dispatch(text, ctx, out, err, err_size);
}

static int default_apply_repl_change(const ReplCompiledChange *change,
                                     void *user) {
    (void)user;
    return repl_apply_compiled_change(change);
}

static void default_apply_predef_ops(const ReplCompiledChange *change,
                                     void *user) {
    (void)user;
    repl_apply_predef_ops(change);
}

EditorServices editor_services_default(void) {
    EditorServices svc = {
        .context           = default_context,
        .compile           = default_compile,
        .apply_repl_change = default_apply_repl_change,
        .apply_predef_ops  = default_apply_predef_ops,
        .user              = NULL,
    };
    return svc;
}
