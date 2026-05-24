/*
 * editor_services.h - REPL semantics seam used by editor commit code.
 *
 * The editor calls into REPL through this table rather than reaching
 * directly at `repl_compile_*` / `repl_apply_*`. The boundary keeps
 * the editor free of `src/repl/compile.h` / `src/repl/apply.h` includes
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

#include "repl/compile.h"
#include "repl/parser.h"

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
     * contract.
     *
     * `cursor_inout` is an optional caller-owned cursor pointer
     * forwarded into the cmd-store; see
     * `repl_apply_compiled_change()` in `src/repl/apply.h` for the
     * full forwarding policy. NULL skips all cursor math. */
    int  (*apply_repl_change)(const ReplCompiledChange *change,
                              int *cursor_inout, void *user);

    /* Replay the change's predef-variable side-effects against the
     * eval table. UNDECLARE first (cascading var_assign var_idx
     * adjustments), then DECLARE / SET_VALUE. */
    void (*apply_predef_ops)(const ReplCompiledChange *change, void *user);

    /* Replay scratch-array side-effects against the evaluator's bound
     * scratch storage. */
    void (*apply_scratch_ops)(const ReplCompiledChange *change, void *user);

    /* Pure parser entry. Routed through services because the demo
     * shim needs a no-op alternative — there's no trivial inline
     * stub for "parse a GL command line." Returns 1 on success
     * (out->cmd + out->text populated), 0 on parse error (diagnostic
     * written to ctx->err_buf when provided). */
    int  (*parse_command_ctx)(const char *line, ReplParsedLine *out,
                              const ReplParseContext *ctx, void *user);

    void *user;
} EditorServices;

/* Default services bound to the live REPL implementation. The
 * `compile` hook walks the registered compile handlers
 * (`repl_compile_float_decl`, `repl_compile_var_assign`, ...) in
 * canonical order. The `apply_*` hooks call `repl_apply_*` directly.
 * The `context` hook returns `repl_compile_context_from_live`. */
EditorServices editor_services_default(void);

/* repl_compile_dispatch lives in src/repl/compile.h — include that
 * header directly to use it (the default `EditorServices.compile` is
 * a thin wrapper around it). */

#endif /* EDITOR_SERVICES_H */
