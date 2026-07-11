/*
 * src/repl/flatten_expr.c -- Compiled-expression boundary for flattening.
 */
#include <string.h>

#include "repl/flatten_expr.h"

static int capture_expr(void *user_data, ReplExprRole role, int ordinal,
                        const char *begin, const char *end) {
    ReplFlattenExprEngine *engine = (ReplFlattenExprEngine *)user_data;

    if (!engine || !engine->cache || engine->build_line < 0 ||
        engine->build_failed)
        return 0;

    if (role == REPL_EXPR_ROLE_CMD_ARG_LIST ||
        role == REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT) {
        char list_text[MAX_LINE_LEN];
        int len = (int)(end - begin);

        if (len < 0 || len >= (int)sizeof(list_text)) {
            engine->build_failed = 1;
            return 0;
        }
        memcpy(list_text, begin, (size_t)len);
        list_text[len] = '\0';
        return repl_flatten_expr_compile_active_list(
                   engine, REPL_EXPR_ROLE_CMD_ARG, ordinal, list_text,
                   role == REPL_EXPR_ROLE_CMD_ARG_LIST, 8) >= 0;
    }

    {
        int prog = repl_expr_cache_compile_span(engine->cache, begin, end,
                                                &engine->build_env);
        if (prog < 0 ||
            !repl_expr_cache_line_add(engine->cache, engine->build_line,
                                      role, ordinal, prog)) {
            engine->build_failed = 1;
            return 0;
        }
    }
    return 1;
}

void repl_flatten_expr_init(ReplFlattenExprEngine *engine,
                            ReplExprCache *cache, int force_text) {
    memset(engine, 0, sizeof(*engine));
    engine->cache = cache;
    engine->force_text = force_text ? 1 : 0;
    engine->build_line = -1;
}

int repl_flatten_expr_line_ready(const ReplFlattenExprEngine *engine,
                                 int line_idx) {
    return engine && engine->cache && !engine->force_text &&
           repl_expr_cache_line_state(engine->cache, line_idx) ==
               REPL_EXPR_LINE_READY;
}

int repl_flatten_expr_build_begin(ReplFlattenExprEngine *engine,
                                  int line_idx,
                                  const ExprVar *locals, int num_locals) {
    if (!engine || !engine->cache || engine->force_text)
        return 0;
    if (repl_expr_cache_line_state(engine->cache, line_idx) !=
        REPL_EXPR_LINE_EMPTY)
        return 0;
    if (!repl_expr_cache_line_begin(engine->cache, line_idx))
        return 0;
    engine->build_line = line_idx;
    engine->build_failed = 0;
    engine->build_env.predef = repl_eval_predef_view();
    engine->build_env.locals = locals;
    engine->build_env.num_locals = num_locals;
    return 1;
}

void repl_flatten_expr_build_finish(ReplFlattenExprEngine *engine,
                                    int line_idx, int parsed_ok) {
    if (!engine || engine->build_line != line_idx)
        return;
    repl_expr_cache_line_finish(engine->cache, line_idx,
                                parsed_ok && !engine->build_failed);
    engine->build_line = -1;
}

ReplExprCaptureSink repl_flatten_expr_capture_sink(
    ReplFlattenExprEngine *engine) {
    ReplExprCaptureSink sink;
    sink.fn = capture_expr;
    sink.user_data = engine;
    return sink;
}

int repl_flatten_expr_capture_span(ReplFlattenExprEngine *engine,
                                   ReplExprRole role, int ordinal,
                                   const char *begin, const char *end) {
    return capture_expr(engine, role, ordinal, begin, end);
}

int repl_flatten_expr_compile_active_list(ReplFlattenExprEngine *engine,
                                          ReplExprRole role,
                                          int base_ordinal,
                                          const char *text, int strict,
                                          int max_programs) {
    int programs[MAX_EXPR_VARS];
    int count;

    if (!engine || !engine->cache || engine->build_line < 0 ||
        max_programs < 0 || max_programs > MAX_EXPR_VARS) {
        if (engine)
            engine->build_failed = 1;
        return -1;
    }
    count = repl_expr_cache_compile_list(engine->cache, text, strict,
                                         max_programs, programs,
                                         &engine->build_env);
    if (count < 0) {
        engine->build_failed = 1;
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (!repl_expr_cache_line_add(engine->cache, engine->build_line,
                                      role, base_ordinal + i, programs[i])) {
            engine->build_failed = 1;
            return -1;
        }
    }
    return count;
}

int repl_flatten_expr_role_count(const ReplFlattenExprEngine *engine,
                                 int line_idx, ReplExprRole role) {
    if (!repl_flatten_expr_line_ready(engine, line_idx))
        return 0;
    return repl_expr_cache_line_role_count(engine->cache, line_idx, role);
}

ReplFlattenExprValue repl_flatten_expr_eval(
    const ReplFlattenExprEngine *engine, int line_idx,
    ReplExprRole role, int ordinal,
    const ExprVar *locals, int num_locals) {
    ReplFlattenExprValue result = { 0.0f, 0, 0 };
    ReplExprEvalEnv env;
    ReplPredefView predef;
    int prog;

    if (!repl_flatten_expr_line_ready(engine, line_idx))
        return result;
    prog = repl_expr_cache_line_find(engine->cache, line_idx, role, ordinal);
    if (prog < 0)
        return result;

    memset(&env, 0, sizeof(env));
    predef = repl_eval_predef_view();
    env.locals = locals;
    env.num_locals = num_locals;
    env.predef_vars = predef.vars;
    env.predef_count = predef.count;
    result.value = repl_expr_program_eval(engine->cache, prog, &env).value;
    result.found = 1;
    result.used_local = env.used_local;
    return result;
}
