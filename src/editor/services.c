/*
 * editor_services.c -- Default EditorServices bound to live REPL.
 *
 * The implementation is a thin shim over `repl_compile_*` and
 * `repl_apply_*`. The compile hook delegates to `repl_compile_dispatch`
 * (defined in `src/repl/compile.c`); structured-block handlers continue
 * to live in editor commit code until step 5a of the decouple plan
 * extracts pure variants into `src/repl/compile.c`.
 */

#include "services.h"

#include "state.h"     /* editor_insert_mode */

#include "repl/apply.h"
#include "repl/compile.h"
#include "repl/parser.h"

/* ---- Default service bindings ----------------------------------- */

static ReplCompileContext default_context(void *user) {
    (void)user;
    /* Editor-side context: repl_compile_context_from_live(editor_state_edit_line()) defaults
     * insert_mode to 0 (the non-editor convention); fill in the live
     * value here so editor compile picks INSERT_ONE / REPLACE_ONE
     * correctly (feature/source-document-port.md, Phase 6). */
    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ctx.insert_mode = editor_insert_mode();
    return ctx;
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
                                     int *cursor_inout, void *user) {
    (void)user;
    return repl_apply_compiled_change(change, cursor_inout);
}

static void default_apply_predef_ops(const ReplCompiledChange *change,
                                     void *user) {
    (void)user;
    repl_apply_predef_ops(change);
}

static void default_apply_scratch_ops(const ReplCompiledChange *change,
                                      void *user) {
    (void)user;
    repl_apply_scratch_ops(change);
}

static int default_parse_command_ctx(const char *line, ReplParsedLine *out,
                                     const ReplParseContext *ctx, void *user) {
    (void)user;
    return repl_parser_parse_command_ctx(line, out, ctx);
}

EditorServices editor_services_default(void) {
    EditorServices svc = {
        .context           = default_context,
        .compile           = default_compile,
        .apply_repl_change = default_apply_repl_change,
        .apply_predef_ops  = default_apply_predef_ops,
        .apply_scratch_ops = default_apply_scratch_ops,
        .parse_command_ctx = default_parse_command_ctx,
        .user              = NULL,
    };
    return svc;
}
