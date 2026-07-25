/*
 * src/repl/expr_program.c - Compiled expression programs and the line cache.
 *
 * The compiler is a recursive descent over the exact grammar of
 * repl_eval_expr (src/repl/eval.c), emitting postfix instructions instead of
 * evaluating. Because postfix from a left-to-right descent preserves
 * evaluation order, and every operator/builtin applies the same float
 * operation as the text evaluator (same guarded divide/modulo, same epsilon
 * compares, the same builtin function pointers via repl_eval_builtin_at),
 * compiled evaluation is bit-identical with text evaluation. Anything the
 * grammar walk cannot express — an unbalanced construct, an unknown call
 * target, an over-long identifier — fails compilation and the caller stays
 * on the text path for that expression, so mismatches can only manifest as
 * "no speedup", never as a different value.
 *
 * See expr_program.h for the cache/dependency-mask contract and
 * docs/plans/in-review/flatten-performance-without-vm.md (Phase 2) for the
 * design rationale.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "repl/expr_program.h"

/* Evaluation stack depth cap. The compiler tracks each program's true
 * maximum and rejects programs that exceed this, so the evaluator's fixed
 * arrays can never overflow. */
#define EXPR_STACK_MAX 64

/* Mirrors eval_primary's local name buffer: identifiers of 32+ chars make
 * the text evaluator give up on the primary, so the compiler gives up on
 * the program (fallback preserves the behavior). */
#define EXPR_NAME_MAX 32

typedef enum {
    EXPR_OP_CONST = 0,  /* push imm */
    EXPR_OP_VAR,        /* push local-by-name, else predef by slot, else 0 */
    EXPR_OP_SCRATCH,    /* pop index, push scratch[a][(int)index] (0 if OOR) */
    EXPR_OP_NEG,
    EXPR_OP_NOT,
    EXPR_OP_ADD,
    EXPR_OP_SUB,
    EXPR_OP_MUL,
    EXPR_OP_DIV,        /* guarded: |d| <= 1e-12 -> 0 */
    EXPR_OP_MOD,        /* guarded fmodf */
    EXPR_OP_LT,
    EXPR_OP_GT,
    EXPR_OP_LE,
    EXPR_OP_GE,
    EXPR_OP_EQ,         /* |a-b| < 1e-6 */
    EXPR_OP_NE,
    EXPR_OP_AND,        /* non-short-circuit, like the text evaluator */
    EXPR_OP_OR,
    EXPR_OP_CALL        /* pop a args (left-to-right order), push builtin */
} ExprOpcode;

typedef struct {
    unsigned char op;    /* ExprOpcode */
    unsigned char a;     /* CALL: argc; SCRATCH: array index */
    short         slot;  /* VAR: predef slot or -1; CALL: builtin index */
    int           sym;   /* VAR: symbol arena offset, -1 otherwise */
    float         imm;   /* CONST */
} ExprInstr;

typedef struct {
    int           first_instr;
    int           instr_count;
    int           max_stack;
    unsigned char has_scratch_read;
} ExprProgramDesc;

typedef struct {
    unsigned char role;
    short         ordinal;
    int           prog;
} ExprLineRef;

/* Internal line states: the public enum plus a transient BUILDING marker
 * held between line_begin and line_finish. */
#define EXPR_LINE_BUILDING 3

typedef struct {
    unsigned char state;
    int           first_ref;
    int           ref_count;
} ExprLineEntry;

struct ReplExprCache {
    ExprInstr       *instrs;
    int              instr_count, instr_cap;
    ExprProgramDesc *progs;
    int              prog_count, prog_cap;
    char            *syms;          /* concatenated NUL-terminated names */
    int              sym_count, sym_cap;
    int             *sym_offsets;   /* start offset of each interned name */
    int              sym_offset_count, sym_offset_cap;
    ExprLineRef     *refs;
    int              ref_count, ref_cap;
    ExprLineEntry   *lines;
    int              line_cap;
    size_t           bytes;
};

/* ---- Arena management --------------------------------------------------- */

/* Grow *arr (element size esz) to hold at least `need` elements, doubling
 * from the current *cap. Fails (returns 0) instead of exceeding the cache's
 * total byte budget. */
static int cache_grow(ReplExprCache *cache, void **arr, int *cap, int need,
                      size_t esz) {
    int new_cap;
    void *p;

    if (need <= *cap)
        return 1;
    new_cap = (*cap > 0) ? *cap : 32;
    while (new_cap < need) {
        if (new_cap > (1 << 28))
            return 0;
        new_cap *= 2;
    }
    if (cache->bytes + (size_t)(new_cap - *cap) * esz >
        (size_t)REPL_EXPR_CACHE_MAX_BYTES)
        return 0;
    p = realloc(*arr, (size_t)new_cap * esz);
    if (!p)
        return 0;
    cache->bytes += (size_t)(new_cap - *cap) * esz;
    *arr = p;
    *cap = new_cap;
    return 1;
}

ReplExprCache *repl_expr_cache_live(void) {
    static ReplExprCache g_live_cache;
    return &g_live_cache;
}

ReplExprCache *repl_expr_cache_create(void) {
    return (ReplExprCache *)calloc(1, sizeof(ReplExprCache));
}

void repl_expr_cache_destroy(ReplExprCache *cache) {
    if (!cache)
        return;
    free(cache->instrs);
    free(cache->progs);
    free(cache->syms);
    free(cache->sym_offsets);
    free(cache->refs);
    free(cache->lines);
    free(cache);
}

void repl_expr_cache_invalidate(ReplExprCache *cache) {
    if (!cache)
        return;
    cache->instr_count = 0;
    cache->prog_count = 0;
    cache->sym_count = 0;
    cache->sym_offset_count = 0;
    cache->ref_count = 0;
    if (cache->lines && cache->line_cap > 0)
        memset(cache->lines, 0, (size_t)cache->line_cap * sizeof(ExprLineEntry));
}


/* Intern `name` into the symbol arena, returning its offset (or -1). */
static int cache_intern_symbol(ReplExprCache *cache, const char *name) {
    int len = (int)strlen(name);

    for (int i = 0; i < cache->sym_offset_count; i++) {
        if (strcmp(cache->syms + cache->sym_offsets[i], name) == 0)
            return cache->sym_offsets[i];
    }
    if (!cache_grow(cache, (void **)&cache->syms, &cache->sym_cap,
                    cache->sym_count + len + 1, sizeof(char)))
        return -1;
    if (!cache_grow(cache, (void **)&cache->sym_offsets,
                    &cache->sym_offset_cap,
                    cache->sym_offset_count + 1, sizeof(int)))
        return -1;
    memcpy(cache->syms + cache->sym_count, name, (size_t)len + 1);
    cache->sym_offsets[cache->sym_offset_count++] = cache->sym_count;
    cache->sym_count += len + 1;
    return cache->sym_offsets[cache->sym_offset_count - 1];
}

/* ---- Compiler ----------------------------------------------------------- */

typedef struct {
    ReplExprCache            *cache;
    const char               *p;
    const ReplExprCompileEnv *env;
    int                       fail;
    int                       cur_stack;
    int                       max_stack;
    int                       has_scratch_read;
} ExprCompiler;

static void compile_expr(ExprCompiler *c);   /* forward: full grammar entry */

static void compiler_fail(ExprCompiler *c) {
    c->fail = 1;
}

static void compiler_emit(ExprCompiler *c, int op, int a, int slot, int sym,
                          float imm, int stack_delta) {
    ExprInstr *ins;

    if (c->fail)
        return;
    if (!cache_grow(c->cache, (void **)&c->cache->instrs,
                    &c->cache->instr_cap, c->cache->instr_count + 1,
                    sizeof(ExprInstr))) {
        compiler_fail(c);
        return;
    }
    ins = &c->cache->instrs[c->cache->instr_count++];
    ins->op = (unsigned char)op;
    ins->a = (unsigned char)a;
    ins->slot = (short)slot;
    ins->sym = sym;
    ins->imm = imm;

    c->cur_stack += stack_delta;
    if (c->cur_stack > c->max_stack)
        c->max_stack = c->cur_stack;
    if (c->cur_stack < 1 || c->max_stack > EXPR_STACK_MAX)
        compiler_fail(c);
}

static void compiler_skip_ws(ExprCompiler *c) {
    while (*c->p && isspace((unsigned char)*c->p))
        c->p++;
}

static int compile_env_local_index(const ExprCompiler *c, const char *name) {
    if (!c->env || !c->env->locals)
        return -1;
    return repl_eval_find_predef_var_idx_in(c->env->locals,
                                            c->env->num_locals, name);
}

/* The `primary` production. Mirrors eval_primary in src/repl/eval.c —
 * same match order (unary, paren, number, identifier: constant -> scratch
 * -> variable -> builtin call). Constructs the text evaluator resolves to
 * an error-and-0.0f at *parse structure* level (missing ']', unknown call
 * target, over-long name) fail compilation instead; value-level fallbacks
 * (out-of-range scratch index, unknown plain identifier) stay runtime
 * behavior in the emitted instruction. */
static void compile_primary(ExprCompiler *c) {
    compiler_skip_ws(c);
    if (c->fail)
        return;

    if (*c->p == '-') {
        c->p++;
        compile_primary(c);
        compiler_emit(c, EXPR_OP_NEG, 0, 0, -1, 0.0f, 0);
        return;
    }
    if (*c->p == '+') {
        c->p++;
        compile_primary(c);
        return;
    }
    if (*c->p == '!') {
        c->p++;
        compile_primary(c);
        compiler_emit(c, EXPR_OP_NOT, 0, 0, -1, 0.0f, 0);
        return;
    }

    if (*c->p == '(') {
        c->p++;
        compile_expr(c);
        compiler_skip_ws(c);
        /* The text evaluator tolerates a missing ')'. */
        if (*c->p == ')')
            c->p++;
        return;
    }

    if (isdigit((unsigned char)*c->p) || *c->p == '.') {
        char *end = NULL;
        float v = strtof(c->p, &end);
        if (end == c->p) {
            /* A bare '.' the text evaluator also fails to consume; it
             * would fall through and return 0 without progress. Give up. */
            compiler_fail(c);
            return;
        }
        c->p = end;
        if (*c->p == 'f' || *c->p == 'F')
            c->p++;
        compiler_emit(c, EXPR_OP_CONST, 0, 0, -1, v, 1);
        return;
    }

    {
        const char *name_start = NULL;
        const char *name_end = repl_eval_eat_identifier(c->p, &name_start);
        char name[EXPR_NAME_MAX];
        int name_len;
        const char *q;

        if (!name_end) {
            /* Unrecognized character: the text evaluator returns 0 for the
             * primary without consuming it, which only arises from input
             * the commit validators reject. Not worth mirroring — fail. */
            compiler_fail(c);
            return;
        }
        name_len = (int)(name_end - name_start);
        if (name_len >= (int)sizeof(name)) {
            compiler_fail(c);
            return;
        }
        memcpy(name, name_start, (size_t)name_len);
        name[name_len] = '\0';
        c->p = name_end;
        q = c->p;
        while (*q && isspace((unsigned char)*q))
            q++;

        /* Constants resolve before scratch/locals/predefs, so folding to a
         * literal is exact even if a binding shares the name. */
        {
            float constant_value = 0.0f;
            if (repl_eval_named_constant(name, &constant_value)) {
                compiler_emit(c, EXPR_OP_CONST, 0, 0, -1, constant_value, 1);
                return;
            }
        }

        {
            int scratch_idx = repl_eval_scratch_array_index(name);
            if (scratch_idx >= 0) {
                if (*q != '[') {
                    compiler_fail(c);   /* text: error + 0 */
                    return;
                }
                c->p = q + 1;
                compile_expr(c);
                compiler_skip_ws(c);
                if (*c->p != ']') {
                    compiler_fail(c);   /* text: error + 0 */
                    return;
                }
                c->p++;
                compiler_emit(c, EXPR_OP_SCRATCH, scratch_idx, 0, -1,
                              0.0f, 0);
                c->has_scratch_read = 1;
                return;
            }
            if (*q == '[') {
                compiler_fail(c);       /* text: "unknown array" error + 0 */
                return;
            }
        }

        /* A local binding shadows a would-be function call in the text
         * evaluator (locals are checked before the call branch), so a call
         * target that matches a visible binding must not compile as a
         * builtin. Binding names at this source position are fixed across
         * visits, so the compile-time check holds at evaluation time. */
        if (*q == '(' && compile_env_local_index(c, name) < 0 &&
            repl_eval_find_predef_var_idx_in(c->env ? c->env->predef.vars : NULL,
                                             c->env ? c->env->predef.count : 0,
                                             name) < 0) {
            int builtin_idx = repl_eval_builtin_index_of(name);
            ReplEvalBuiltinView builtin = repl_eval_builtin_at(builtin_idx);
            int argc = 0;

            if (!builtin.eval) {
                compiler_fail(c);       /* unknown call target: text -> 0 */
                return;
            }
            c->p = q + 1;
            compiler_skip_ws(c);
            if (*c->p != ')') {
                for (;;) {
                    compile_expr(c);
                    if (c->fail)
                        return;
                    argc++;
                    compiler_skip_ws(c);
                    if (*c->p != ',')
                        break;
                    c->p++;
                    compiler_skip_ws(c);
                }
            }
            compiler_skip_ws(c);
            if (*c->p != ')') {
                compiler_fail(c);       /* text: "missing ')'" error + 0 */
                return;
            }
            c->p++;
            if (argc < builtin.arity_min || argc > builtin.arity_max ||
                argc > REPL_EXPR_BUILTIN_ARGS_MAX) {
                compiler_fail(c);       /* text: arity error + 0 */
                return;
            }
            compiler_emit(c, EXPR_OP_CALL, argc, builtin_idx, -1, 0.0f,
                          1 - argc);
            return;
        }

        /* Variable read: locals by name at evaluation time (they shadow
         * predefs), then the compile-time-resolved predef slot, then 0.0f
         * for a name that resolves to nothing — the text evaluator's
         * unknown-identifier value. */
        {
            int sym = cache_intern_symbol(c->cache, name);
            int slot = repl_eval_find_predef_var_idx_in(
                c->env ? c->env->predef.vars : NULL,
                c->env ? c->env->predef.count : 0, name);
            if (sym < 0) {
                compiler_fail(c);
                return;
            }
            compiler_emit(c, EXPR_OP_VAR, 0, slot, sym, 0.0f, 1);
            return;
        }
    }
}

static void compile_term(ExprCompiler *c) {
    compile_primary(c);
    for (;;) {
        if (c->fail)
            return;
        compiler_skip_ws(c);
        if (*c->p == '*') {
            c->p++;
            compile_primary(c);
            compiler_emit(c, EXPR_OP_MUL, 0, 0, -1, 0.0f, -1);
        } else if (*c->p == '/') {
            c->p++;
            compile_primary(c);
            compiler_emit(c, EXPR_OP_DIV, 0, 0, -1, 0.0f, -1);
        } else if (*c->p == '%') {
            c->p++;
            compile_primary(c);
            compiler_emit(c, EXPR_OP_MOD, 0, 0, -1, 0.0f, -1);
        } else {
            break;
        }
    }
}

static void compile_additive(ExprCompiler *c) {
    compile_term(c);
    for (;;) {
        if (c->fail)
            return;
        compiler_skip_ws(c);
        if (*c->p == '+') {
            c->p++;
            compile_term(c);
            compiler_emit(c, EXPR_OP_ADD, 0, 0, -1, 0.0f, -1);
        } else if (*c->p == '-') {
            c->p++;
            compile_term(c);
            compiler_emit(c, EXPR_OP_SUB, 0, 0, -1, 0.0f, -1);
        } else {
            break;
        }
    }
}

static void compile_comparison(ExprCompiler *c) {
    compile_additive(c);
    for (;;) {
        if (c->fail)
            return;
        compiler_skip_ws(c);
        if (c->p[0] == '>' && c->p[1] == '=') {
            c->p += 2;
            compile_additive(c);
            compiler_emit(c, EXPR_OP_GE, 0, 0, -1, 0.0f, -1);
        } else if (c->p[0] == '<' && c->p[1] == '=') {
            c->p += 2;
            compile_additive(c);
            compiler_emit(c, EXPR_OP_LE, 0, 0, -1, 0.0f, -1);
        } else if (c->p[0] == '=' && c->p[1] == '=') {
            c->p += 2;
            compile_additive(c);
            compiler_emit(c, EXPR_OP_EQ, 0, 0, -1, 0.0f, -1);
        } else if (c->p[0] == '!' && c->p[1] == '=') {
            c->p += 2;
            compile_additive(c);
            compiler_emit(c, EXPR_OP_NE, 0, 0, -1, 0.0f, -1);
        } else if (*c->p == '>' && c->p[1] != '=') {
            c->p++;
            compile_additive(c);
            compiler_emit(c, EXPR_OP_GT, 0, 0, -1, 0.0f, -1);
        } else if (*c->p == '<' && c->p[1] != '=') {
            c->p++;
            compile_additive(c);
            compiler_emit(c, EXPR_OP_LT, 0, 0, -1, 0.0f, -1);
        } else {
            break;
        }
    }
}

static void compile_expr(ExprCompiler *c) {
    compile_comparison(c);
    for (;;) {
        if (c->fail)
            return;
        compiler_skip_ws(c);
        if (c->p[0] == '&' && c->p[1] == '&') {
            c->p += 2;
            compile_comparison(c);
            compiler_emit(c, EXPR_OP_AND, 0, 0, -1, 0.0f, -1);
        } else if (c->p[0] == '|' && c->p[1] == '|') {
            c->p += 2;
            compile_comparison(c);
            compiler_emit(c, EXPR_OP_OR, 0, 0, -1, 0.0f, -1);
        } else {
            break;
        }
    }
}

/* Compile one expression starting at *src_inout, appending a program
 * descriptor on success and advancing *src_inout past the consumed text
 * (mirroring how repl_eval_expr advances ExprCtx.p). Rolls the instruction
 * arena back on failure. */
static int cache_compile_at(ReplExprCache *cache, const char **src_inout,
                            const ReplExprCompileEnv *env) {
    ExprCompiler c;
    int saved_instr_count;
    int prog;

    if (!cache || !src_inout || !*src_inout)
        return -1;

    saved_instr_count = cache->instr_count;
    memset(&c, 0, sizeof(c));
    c.cache = cache;
    c.p = *src_inout;
    c.env = env;

    compile_expr(&c);
    if (c.fail || c.cur_stack != 1) {
        cache->instr_count = saved_instr_count;
        return -1;
    }
    if (!cache_grow(cache, (void **)&cache->progs, &cache->prog_cap,
                    cache->prog_count + 1, sizeof(ExprProgramDesc))) {
        cache->instr_count = saved_instr_count;
        return -1;
    }
    prog = cache->prog_count++;
    cache->progs[prog].first_instr = saved_instr_count;
    cache->progs[prog].instr_count = cache->instr_count - saved_instr_count;
    cache->progs[prog].max_stack = c.max_stack;
    cache->progs[prog].has_scratch_read = (unsigned char)c.has_scratch_read;
    *src_inout = c.p;
    return prog;
}

int repl_expr_cache_compile(ReplExprCache *cache, const char *src,
                            const ReplExprCompileEnv *env) {
    const char *p = src;
    return cache_compile_at(cache, &p, env);
}

int repl_expr_cache_compile_span(ReplExprCache *cache,
                                 const char *begin, const char *end,
                                 const ReplExprCompileEnv *env) {
    char buf[MAX_LINE_LEN];
    int len;

    if (!begin || !end || end < begin)
        return -1;
    len = (int)(end - begin);
    if (len >= (int)sizeof(buf))
        return -1;
    memcpy(buf, begin, (size_t)len);
    buf[len] = '\0';
    return repl_expr_cache_compile(cache, buf, env);
}

int repl_expr_cache_compile_list(ReplExprCache *cache, const char *src,
                                 int strict, int max, int *prog_out,
                                 const ReplExprCompileEnv *env) {
    const char *p = src;
    int count = 0;
    int saved_instr_count, saved_prog_count;

    if (!cache || !src || max < 0)
        return -1;

    saved_instr_count = cache->instr_count;
    saved_prog_count = cache->prog_count;

    if (strict) {
        /* Mirrors parse_expr_list_exact (src/repl/text_helpers.c): the
         * whole input must be a well-formed comma-separated list. */
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            return 0;
        for (;;) {
            const char *before = p;
            int prog = cache_compile_at(cache, &p, env);
            if (prog < 0 || p == before)
                goto fail;
            if (count >= max)
                goto fail;
            if (prog_out)
                prog_out[count] = prog;
            count++;
            while (*p && isspace((unsigned char)*p))
                p++;
            if (!*p)
                break;
            if (*p != ',')
                goto fail;
            p++;
            while (*p && isspace((unsigned char)*p))
                p++;
            if (!*p)
                goto fail;
        }
        return count;
    }

    /* Mirrors repl_eval_parse_exprs (src/repl/eval.c): lenient — skips
     * leading whitespace/commas, stops at ')' / end / no progress. */
    while (*p && count < max) {
        const char *before;
        int prog;
        while (*p && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (!*p || *p == ')')
            break;
        before = p;
        prog = cache_compile_at(cache, &p, env);
        if (prog < 0)
            goto fail;
        if (p == before)
            break;
        if (prog_out)
            prog_out[count] = prog;
        count++;
    }
    return count;

fail:
    cache->instr_count = saved_instr_count;
    cache->prog_count = saved_prog_count;
    return -1;
}

int repl_expr_program_has_scratch_read(const ReplExprCache *cache, int prog) {
    if (!cache || prog < 0 || prog >= cache->prog_count)
        return 1;   /* conservative */
    return cache->progs[prog].has_scratch_read;
}

/* ---- Evaluator ---------------------------------------------------------- */

ReplExprValue repl_expr_program_eval(const ReplExprCache *cache, int prog,
                                     ReplExprEvalEnv *env) {
    ReplExprValue bad = { 0.0f, REPL_EXPR_DEP_ALL };
    float stack[EXPR_STACK_MAX];
    ReplExprDepMask deps[EXPR_STACK_MAX];
    int sp = 0;
    const ExprInstr *ins;
    const ExprInstr *end;

    if (!cache || !env || prog < 0 || prog >= cache->prog_count)
        return bad;

    ins = cache->instrs + cache->progs[prog].first_instr;
    end = ins + cache->progs[prog].instr_count;

    for (; ins < end; ins++) {
        switch ((ExprOpcode)ins->op) {
        case EXPR_OP_CONST:
            stack[sp] = ins->imm;
            deps[sp] = 0;
            sp++;
            break;
        case EXPR_OP_VAR: {
            const char *name = cache->syms + ins->sym;
            int li = repl_eval_find_predef_var_idx_in(env->locals,
                                                      env->num_locals, name);
            if (li >= 0) {
                stack[sp] = env->locals[li].value;
                deps[sp] = env->local_deps ? env->local_deps[li] : 0;
                env->used_local = 1;
            } else if (ins->slot >= 0 && ins->slot < env->predef_count) {
                stack[sp] = env->predef_vars[ins->slot].value;
                deps[sp] = env->predef_deps
                         ? env->predef_deps[ins->slot]
                         : ((ReplExprDepMask)1u << ins->slot);
            } else {
                stack[sp] = 0.0f;   /* unknown identifier, like the text path */
                deps[sp] = 0;
            }
            sp++;
            break;
        }
        case EXPR_OP_SCRATCH: {
            float v = 0.0f;
            /* Out-of-range reads yield 0, matching eval_primary's
             * error-and-0.0f. Dependency stays the index's — see the
             * value-position rationale in expr_program.h / the plan. */
            (void)repl_eval_scratch_get(ins->a, (int)stack[sp - 1], &v);
            stack[sp - 1] = v;
            break;
        }
        case EXPR_OP_NEG:
            stack[sp - 1] = -stack[sp - 1];
            break;
        case EXPR_OP_NOT:
            stack[sp - 1] = (stack[sp - 1] != 0.0f) ? 0.0f : 1.0f;
            break;
        case EXPR_OP_ADD:
            stack[sp - 2] = stack[sp - 2] + stack[sp - 1];
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_SUB:
            stack[sp - 2] = stack[sp - 2] - stack[sp - 1];
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_MUL:
            stack[sp - 2] = stack[sp - 2] * stack[sp - 1];
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_DIV: {
            float d = stack[sp - 1];
            stack[sp - 2] = (fabsf(d) > 1e-12f) ? stack[sp - 2] / d : 0.0f;
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        }
        case EXPR_OP_MOD: {
            float d = stack[sp - 1];
            stack[sp - 2] = (fabsf(d) > 1e-12f) ? fmodf(stack[sp - 2], d)
                                                : 0.0f;
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        }
        case EXPR_OP_LT:
            stack[sp - 2] = (stack[sp - 2] < stack[sp - 1]) ? 1.0f : 0.0f;
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_GT:
            stack[sp - 2] = (stack[sp - 2] > stack[sp - 1]) ? 1.0f : 0.0f;
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_LE:
            stack[sp - 2] = (stack[sp - 2] <= stack[sp - 1]) ? 1.0f : 0.0f;
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_GE:
            stack[sp - 2] = (stack[sp - 2] >= stack[sp - 1]) ? 1.0f : 0.0f;
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_EQ:
            stack[sp - 2] = (fabsf(stack[sp - 2] - stack[sp - 1]) < 1e-6f)
                          ? 1.0f : 0.0f;
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_NE:
            stack[sp - 2] = (fabsf(stack[sp - 2] - stack[sp - 1]) >= 1e-6f)
                          ? 1.0f : 0.0f;
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_AND:
            stack[sp - 2] = (stack[sp - 2] != 0.0f && stack[sp - 1] != 0.0f)
                          ? 1.0f : 0.0f;
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_OR:
            stack[sp - 2] = (stack[sp - 2] != 0.0f || stack[sp - 1] != 0.0f)
                          ? 1.0f : 0.0f;
            deps[sp - 2] |= deps[sp - 1];
            sp--;
            break;
        case EXPR_OP_CALL: {
            ReplEvalBuiltinView builtin = repl_eval_builtin_at(ins->slot);
            /* Same shape as eval_primary's call site: a zero-initialized
             * buffer so a call passing fewer than arity_max args
             * (rand(seed)) sees the same implicit trailing zeros. */
            float args[REPL_EXPR_BUILTIN_ARGS_MAX] = { 0.0f };
            int argc = ins->a;
            ReplExprDepMask d = 0;
            for (int k = 0; k < argc; k++) {
                args[argc - 1 - k] = stack[sp - 1 - k];
                d |= deps[sp - 1 - k];
            }
            sp -= argc;
            stack[sp] = builtin.eval ? builtin.eval(args) : 0.0f;
            deps[sp] = d;
            sp++;
            break;
        }
        }
    }

    {
        ReplExprValue out;
        out.value = stack[sp - 1];
        out.deps = deps[sp - 1];
        return out;
    }
}

/* ---- Per-line entries ---------------------------------------------------- */

static ExprLineEntry *cache_line_entry(ReplExprCache *cache, int line_idx,
                                       int create) {
    if (!cache || line_idx < 0)
        return NULL;
    if (line_idx >= cache->line_cap) {
        int old_cap;
        if (!create)
            return NULL;
        old_cap = cache->line_cap;
        if (!cache_grow(cache, (void **)&cache->lines, &cache->line_cap,
                        line_idx + 1, sizeof(ExprLineEntry)))
            return NULL;
        memset(cache->lines + old_cap, 0,
               (size_t)(cache->line_cap - old_cap) * sizeof(ExprLineEntry));
    }
    return &cache->lines[line_idx];
}

ReplExprLineState repl_expr_cache_line_state(const ReplExprCache *cache,
                                             int line_idx) {
    ExprLineEntry *entry =
        cache_line_entry((ReplExprCache *)cache, line_idx, 0);
    if (!entry)
        return REPL_EXPR_LINE_EMPTY;
    if (entry->state == EXPR_LINE_BUILDING)
        return REPL_EXPR_LINE_FAILED;   /* mid-build queries never trust it */
    return (ReplExprLineState)entry->state;
}

int repl_expr_cache_line_begin(ReplExprCache *cache, int line_idx) {
    ExprLineEntry *entry = cache_line_entry(cache, line_idx, 1);
    if (!entry)
        return 0;
    entry->state = EXPR_LINE_BUILDING;
    entry->first_ref = cache->ref_count;
    entry->ref_count = 0;
    return 1;
}

int repl_expr_cache_line_add(ReplExprCache *cache, int line_idx,
                             ReplExprRole role, int ordinal, int prog) {
    ExprLineEntry *entry = cache_line_entry(cache, line_idx, 0);
    ExprLineRef *ref;

    if (!entry || entry->state != EXPR_LINE_BUILDING || prog < 0)
        return 0;
    /* A line's refs are contiguous: builds never interleave (one build
     * brackets a single parse/helper call). */
    if (entry->first_ref + entry->ref_count != cache->ref_count)
        return 0;
    if (!cache_grow(cache, (void **)&cache->refs, &cache->ref_cap,
                    cache->ref_count + 1, sizeof(ExprLineRef)))
        return 0;
    ref = &cache->refs[cache->ref_count++];
    ref->role = (unsigned char)role;
    ref->ordinal = (short)ordinal;
    ref->prog = prog;
    entry->ref_count++;
    return 1;
}

void repl_expr_cache_line_finish(ReplExprCache *cache, int line_idx, int ok) {
    ExprLineEntry *entry = cache_line_entry(cache, line_idx, 0);
    if (!entry)
        return;
    if (!ok) {
        /* Drop the refs appended for this build (they sit at the tail). */
        if (entry->first_ref + entry->ref_count == cache->ref_count)
            cache->ref_count = entry->first_ref;
        entry->ref_count = 0;
        entry->state = REPL_EXPR_LINE_FAILED;
        return;
    }
    entry->state = REPL_EXPR_LINE_READY;
}

int repl_expr_cache_line_find(const ReplExprCache *cache, int line_idx,
                              ReplExprRole role, int ordinal) {
    ExprLineEntry *entry =
        cache_line_entry((ReplExprCache *)cache, line_idx, 0);
    if (!entry || entry->state != REPL_EXPR_LINE_READY)
        return -1;
    for (int i = 0; i < entry->ref_count; i++) {
        const ExprLineRef *ref = &cache->refs[entry->first_ref + i];
        if (ref->role == (unsigned char)role && ref->ordinal == ordinal)
            return ref->prog;
    }
    return -1;
}

int repl_expr_cache_line_role_count(const ReplExprCache *cache, int line_idx,
                                    ReplExprRole role) {
    ExprLineEntry *entry =
        cache_line_entry((ReplExprCache *)cache, line_idx, 0);
    int n = 0;
    if (!entry || entry->state != REPL_EXPR_LINE_READY)
        return 0;
    for (int i = 0; i < entry->ref_count; i++) {
        if (cache->refs[entry->first_ref + i].role == (unsigned char)role)
            n++;
    }
    return n;
}
