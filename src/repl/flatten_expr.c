/*
 * src/repl/flatten_expr.c -- Compiled-expression boundary for flattening.
 */
#include <string.h>

#include "repl/command.h"      /* REPL_MATRIX_CELL_COUNT */
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
        /* The cap is the widest captured list any command has, not the
         * args[] width: glMultMatrixf's compound literal captures 16 cells
         * into payload.matrix. Commands with narrower lists are unaffected
         * — they validate their own arity in the parser, and ordinals past
         * their arity are never read back. */
        return repl_flatten_expr_compile_active_list(
                   engine, REPL_EXPR_ROLE_CMD_ARG, ordinal, list_text,
                   role == REPL_EXPR_ROLE_CMD_ARG_LIST,
                   REPL_MATRIX_CELL_COUNT) >= 0;
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
    engine->rebake_ok = 1;
    for (int slot = 0; slot < MAX_PREDEF_VARS; slot++)
        engine->predef_deps[slot] = (ReplExprDepMask)1u << slot;
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
        return -1;
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
                                          ReplExprRole role, int base_ordinal,
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
    const ExprVar *locals, const ReplExprDepMask *local_deps,
    int num_locals, int structural) {
    ReplFlattenExprValue result = { 0.0f, REPL_EXPR_DEP_ALL, 0, 0 };
    ReplExprEvalEnv env;
    ReplPredefView predef;
    ReplExprValue value;
    int prog;

    if (!repl_flatten_expr_line_ready(engine, line_idx))
        return result;
    prog = repl_expr_cache_line_find(engine->cache, line_idx, role, ordinal);
    if (prog < 0)
        return result;

    memset(&env, 0, sizeof(env));
    predef = repl_eval_predef_view();
    env.locals = locals;
    env.local_deps = local_deps;
    env.num_locals = num_locals;
    env.predef_vars = predef.vars;
    env.predef_count = predef.count;
    env.predef_deps = engine->predef_deps;
    value = repl_expr_program_eval(engine->cache, prog, &env);

    result.value = value.value;
    result.deps = structural &&
                  repl_expr_program_has_scratch_read(engine->cache, prog)
                ? REPL_EXPR_DEP_ALL : value.deps;
    result.found = 1;
    result.used_local = env.used_local;
    return result;
}

ReplExprDepMask repl_flatten_expr_deps(
    const ReplFlattenExprEngine *engine, int line_idx,
    ReplExprRole role, int ordinal,
    const ExprVar *locals, const ReplExprDepMask *local_deps,
    int num_locals, int structural, int missing_ok) {
    ReplFlattenExprValue value = repl_flatten_expr_eval(
        engine, line_idx, role, ordinal, locals, local_deps, num_locals,
        structural);
    if (value.found)
        return value.deps;
    if (missing_ok && repl_flatten_expr_line_ready(engine, line_idx))
        return 0;
    return REPL_EXPR_DEP_ALL;
}

ReplExprDepMask repl_flatten_expr_header_value(
    const ReplFlattenExprEngine *engine, int line_idx,
    ReplExprRole role, const ExprVar *locals,
    const ReplExprDepMask *local_deps, int num_locals,
    float *value_inout, int missing_all_bits) {
    ReplFlattenExprValue value = repl_flatten_expr_eval(
        engine, line_idx, role, 0, locals, local_deps, num_locals,
        /*structural=*/1);
    if (!value.found)
        return missing_all_bits ? REPL_EXPR_DEP_ALL : 0;
    *value_inout = value.value;
    return value.deps;
}

void repl_flatten_expr_note_value(ReplFlattenExprEngine *engine,
                                  ReplExprDepMask deps) {
    engine->value_dep_mask |= deps;
}

void repl_flatten_expr_note_structural(ReplFlattenExprEngine *engine,
                                       ReplExprDepMask deps) {
    engine->structural_dep_mask |= deps;
}

void repl_flatten_expr_set_predef_deps(ReplFlattenExprEngine *engine,
                                       int slot, ReplExprDepMask deps) {
    if (slot >= 0 && slot < MAX_PREDEF_VARS)
        engine->predef_deps[slot] = deps;
}

void repl_flatten_expr_note_emitted(ReplFlattenExprEngine *engine,
                                    int has_vars, int line_idx) {
    if (has_vars && !repl_flatten_expr_line_ready(engine, line_idx))
        engine->rebake_ok = 0;
}

ReplExprDepMask repl_flatten_expr_value_deps(
    const ReplFlattenExprEngine *engine) {
    return engine->value_dep_mask;
}

ReplExprDepMask repl_flatten_expr_structural_deps(
    const ReplFlattenExprEngine *engine) {
    return engine->structural_dep_mask;
}

int repl_flatten_expr_rebake_ok(const ReplFlattenExprEngine *engine) {
    return engine->rebake_ok;
}

int repl_flatten_expr_rebake_line_ready(const ReplExprCache *cache,
                                        int line_idx) {
    return cache && repl_expr_cache_line_state(cache, line_idx) ==
                        REPL_EXPR_LINE_READY;
}

int repl_flatten_expr_rebake_eval(const ReplExprCache *cache, int line_idx,
                                  ReplExprRole role, int ordinal,
                                  const ExprVar *locals, int num_locals,
                                  float *value_out) {
    ReplExprEvalEnv env;
    ReplPredefView predef;
    int prog;

    if (!repl_flatten_expr_rebake_line_ready(cache, line_idx))
        return 0;
    prog = repl_expr_cache_line_find(cache, line_idx, role, ordinal);
    if (prog < 0)
        return 0;
    memset(&env, 0, sizeof(env));
    predef = repl_eval_predef_view();
    env.locals = locals;
    env.num_locals = num_locals;
    env.predef_vars = predef.vars;
    env.predef_count = predef.count;
    *value_out = repl_expr_program_eval(cache, prog, &env).value;
    return 1;
}
