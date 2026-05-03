/*
 * editor_services.h - REPL semantics seam used by editor commit code.
 *
 * The editor calls into REPL through this table rather than reaching
 * directly at `repl_compile_*` / `repl_apply_*`. The boundary keeps
 * the editor free of `repl_compile.h` / `repl_apply.h` includes
 * outside the orchestration site, and gives the controller a single
 * place to substitute test doubles or alternate implementations.
 *
 * The table provides REPL semantics only — compile a proposed
 * source-text change, apply the ReplState half of a successful
 * change, replay predef-var ops. The editor half of the apply
 * (`editor_buffer_apply_compiled_change`) intentionally stays in
 * the editor — it doesn't go through services. That keeps the
 * service surface narrow and prevents the table from drifting into
 * a generic app backdoor.
 *
 * Pure reads (the preflight `repl_apply_can_apply_compiled_change`
 * in particular) also stay direct; only mutating operations and
 * context construction live in the table.
 */
#ifndef EDITOR_SERVICES_H
#define EDITOR_SERVICES_H

#include "repl_compile.h"

typedef struct EditorServices_s {
    /* Build a ReplCompileContext snapshot for the current frame.
     * Reads ReplState + EditorState; pure. */
    ReplCompileContext (*context)(void *user);

    /* Pure compile entry. Walks the registered compile handlers in
     * canonical order and returns the first one that produces a
     * non-NO_CHANGE result, or REPL_COMPILE_OK + NO_CHANGE if none
     * matched. Errors fill `err` and never mutate state. */
    ReplCompileResult (*compile)(const char *text,
                                 const ReplCompileContext *ctx,
                                 ReplCompiledChange *out,
                                 char *err, int err_size,
                                 void *user);

    /* Apply the ReplState command-array half of a successful
     * change. Returns 1 on success, 0 on capacity / range error.
     * Callers preflight via `repl_apply_can_apply_compiled_change`
     * before this fires; passing the preflight is the orchestration
     * contract. */
    int  (*apply_repl_change)(const ReplCompiledChange *change, void *user);

    /* Replay the change's predef-variable side-effects against the
     * eval table. UNDECLARE first (cascading var_assign num_args
     * adjustments), then DECLARE / SET_VALUE. */
    void (*apply_predef_ops)(const ReplCompiledChange *change, void *user);

    void *user;
} EditorServices;

/* Default services bound to the live REPL implementation. The
 * `compile` hook walks the registered compile handlers
 * (`repl_compile_float_decl`, `repl_compile_var_assign`, ...) in
 * canonical order. The `apply_*` hooks call `repl_apply_*` directly.
 * The `context` hook returns `repl_compile_context_from_live`. */
EditorServices editor_services_default(void);

/* Walk the registered compile handlers in canonical order and
 * return the first one that produces a non-NO_CHANGE result. Public
 * because tests + the editor commit orchestration both call it; the
 * default `EditorServices.compile` is a thin wrapper. */
ReplCompileResult repl_compile_dispatch(const char *text,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size);

#endif /* EDITOR_SERVICES_H */
