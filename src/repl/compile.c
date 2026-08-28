/*
 * src/repl/compile.c -- Pure source-text validators that produce
 *                   ReplCompiledChange descriptors.
 *
 * Contract: every entry point in this file is pure. It
 * reads the editor buffer view + REPL state read-only handles
 * passed in via ReplCompileContext, parses + validates the user's
 * input, and writes a ReplCompiledChange describing the source
 * change plus any predef-var side effects. It never:
 *   - calls set_status() / repl_state_status_*()
 *   - writes the editor buffer
 *   - writes the command store
 *   - mutates predef-var registrations
 *   - pushes an undo entry
 *
 * The success / failure paths flow upward through return values
 * and the `err` buffer. The caller decides how to surface the
 * diagnostic.
 *
 * Apply-side mutations (predef declare / undeclare, command-store
 * shift, editor-buffer write) are performed by src/repl/apply.c and
 * orchestrated by the editor commit path.
 *
 * Reading guide - entry points and what they emit:
 *   repl_compile_float_decl         INSERT_ONE / REPLACE_ONE  +
 *                                   DECLARE / UNDECLARE / SET_VALUE
 *   repl_compile_var_assign         INSERT_ONE / REPLACE_ONE  +
 *                                   SET_VALUE (+ UNDECLARE on
 *                                   decl-row overwrite)
 *   repl_compile_set_predef_value   REPLACE_ONE on the decl
 *                                   initializer that backs the named
 *                                   variable; pure SET_VALUE if no
 *                                   declaration exists
 *   repl_compile_empty_line         INSERT_ONE
 *   repl_compile_delete_range       DELETE_RANGE + UNDECLARE for any
 *                                   decl rows in the range
 *   repl_compile_comment_range      INSERT_MANY over [first..last]
 *                                   (REPLACE_ONE for a single row);
 *                                   UNDECLAREs decl rows being
 *                                   commented out
 *   repl_compile_uncomment_line     REPLACE_ONE re-parsing one
 *                                   stripped comment row in place
 *
 * Dispatcher: repl_compile_dispatch() walks all seven per-kind
 * validators in canonical order (float_decl -> var_assign ->
 * if_branch -> close_brace -> for_loop -> func_def -> if_block) and is
 * callable from outside the editor. The uncomment path calls it as a
 * fallback. The order is load-bearing - see the chain[] table below.
 */

#define _POSIX_C_SOURCE 200809L /* for strnlen on linux */
#include "repl/compile.h"

#include "repl/command.h"
#include "repl/eval.h"
#include "repl/normalize.h"
#include "repl/text_helpers.h"
#include "repl/visible_vars.h"
#include "repl/source_scope.h"   /* ReplSourceScopeView queries */
#include "repl/state_owners.h"
#include "repl/util.h"            /* repl_format_fits / _copy_string_fits / _append_clamped */

#include "config.h"              /* REPL_INDENT_TEXT_MAX */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "source_document.h"
#include <string.h>

ReplCompileResult repl_compile_dispatch(const char *text,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!out || !ctx) return REPL_COMPILE_ERROR;

    /* Canonical order - mirrors editor_try_commit_any. Each entry
     * is a per-kind compile validator that returns NO_CHANGE when
     * the input doesn't match its grammar, OK + a populated
     * ReplCompiledChange when it does, or ERROR on syntax failure.
     * The first match wins.
     *
     * Ordering is load-bearing: float_decl must come before
     * var_assign (otherwise `float x` would misread as an
     * assignment to identifier `float`); if_branch must come before
     * close_brace so `} else ...` is not consumed as a plain block
     * close; close_brace still precedes the three block openers.
     *
     * DEFERRED (repl-clarity-review.md finding 5, in
     * docs/plans/partial/): this order is spelled twice - here and in
     * editor_try_commit_block_structs / _var_statements / _any
     * (src/editor/commit.c) - with no test, guard or shared table tying
     * the two together. The handlers are deliberately not twins (the
     * editor wrappers add header-replace / one-liner-body / paired-end
     * branches on top of the shared kernels), so what can drift is only
     * *classification*: a new structured form added to one chain and not
     * the other. Deliberately not fixed yet, because a corpus test would
     * only cover the forms someone remembered to add to it. **If you are
     * adding a structured form, this is the moment**: either export the
     * order as a shared handler-kind enum and make both dispatchers
     * exhaustive over it, or add explicit parity coverage for the new
     * form. Read the finding first - it argues against unifying the
     * handlers themselves. */
    static const struct {
        ReplCompileResult (*fn)(const char *, const ReplCompileContext *,
                                ReplCompiledChange *, char *, int);
    } chain[] = {
        /* fn */
        { repl_compile_float_decl  },
        { repl_compile_var_assign  },
        { repl_compile_if_branch   },
        { repl_compile_close_brace },
        { repl_compile_for_loop    },
        { repl_compile_func_def    },
        { repl_compile_if_block    },
    };

    for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); i++) {
        ReplCompileResult r = chain[i].fn(text, ctx, out, err, err_size);
        if (r == REPL_COMPILE_ERROR) return r;
        if (out->kind != REPL_COMPILED_NO_CHANGE) return REPL_COMPILE_OK;
    }

    out->kind = REPL_COMPILED_NO_CHANGE;
    return REPL_COMPILE_OK;
}

void repl_compiled_change_init(ReplCompiledChange *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->kind = REPL_COMPILED_NO_CHANGE;
    out->pos = 0;
    out->count = 0;
    out->delete_pos = -1;
    out->delete_count = 0;
    out->alias_op.slot = -1;
    out->alias_op.name[0] = '\0';
}

void repl_compiled_change_to_text_change(const ReplCompiledChange *in,
                                         SourceTextChange *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->delete_pos = -1;
    if (!in) return;

    switch (in->kind) {
    case REPL_COMPILED_NO_CHANGE:   out->kind = SOURCE_TEXT_NO_CHANGE;   break;
    case REPL_COMPILED_INSERT_ONE:  out->kind = SOURCE_TEXT_INSERT_ONE;  break;
    case REPL_COMPILED_REPLACE_ONE: out->kind = SOURCE_TEXT_REPLACE_ONE; break;
    case REPL_COMPILED_INSERT_MANY: out->kind = SOURCE_TEXT_INSERT_MANY; break;
    case REPL_COMPILED_DELETE_RANGE:out->kind = SOURCE_TEXT_DELETE_RANGE;break;
    }
    out->pos          = in->pos;
    out->count        = in->count;   /* DELETE_RANGE: rows to delete, may exceed MAX_COMMIT_CMDS */
    out->delete_pos   = in->delete_pos;
    out->delete_count = in->delete_count;

    /* Only the insert/replace/load kinds carry text rows. DELETE_RANGE
     * leaves text[] untouched (and its `count` is the delete range,
     * which is *not* bounded by MAX_COMMIT_CMDS); clamping out->count
     * for that kind would silently shrink the delete. */
    int copy_n;
    switch (in->kind) {
    case REPL_COMPILED_INSERT_ONE:
    case REPL_COMPILED_REPLACE_ONE:
        copy_n = 1;
        break;
    case REPL_COMPILED_INSERT_MANY:
        copy_n = in->count;
        if (copy_n < 0) copy_n = 0;
        if (copy_n > MAX_COMMIT_CMDS) copy_n = MAX_COMMIT_CMDS;
        break;
    case REPL_COMPILED_NO_CHANGE:
    case REPL_COMPILED_DELETE_RANGE:
    default:
        copy_n = 0;
        break;
    }
    for (int i = 0; i < copy_n; i++) {
        size_t len = strnlen(in->text[i], MAX_LINE_LEN - 1);
        memcpy(out->text[i], in->text[i], len);
        out->text[i][len] = '\0';
    }
}

ReplCompileContext repl_compile_context_from_live(int edit_line_idx) {
    /* insert_mode defaults to 0 (overwrite mode). Callers with insert
     * semantics (the editor commit pipeline, repl_load_apply_line)
     * overwrite ctx.insert_mode after this returns. Insert mode is
     * caller/editor state, not REPL state, so the REPL pipeline doesn't
     * reach for it.
     *
     * edit_line_idx is supplied by the caller because the REPL
     * pipeline does not reach into editor_state_* for the cursor. */
    ReplCompileContext ctx = {
        .edit_line       = edit_line_idx,
        .document_count  = repl_state_document_count(),
        .insert_mode     = 0,
        .text            = source_document_view(),
        .document_cmds   = repl_state_document_cmds(),
        .func_aliases    = repl_func_alias_view(),
        .predef          = repl_eval_predef_view(),
    };
    repl_source_scope_view_bind(&ctx.source_scope,
                                ctx.document_cmds,
                                ctx.document_count);
    return ctx;
}

static const ReplSourceScopeView *compile_source_scope(
        const ReplCompileContext *ctx) {
    return ctx ? &ctx->source_scope : NULL;
}

static void compile_scope_cmd_indent(const ReplCompileContext *ctx,
                                     int pos, char *buf, int buf_sz) {
    repl_source_scope_view_cmd_indent(compile_source_scope(ctx),
                                      pos, buf, buf_sz);
}

static int compile_scope_find_block_end(const ReplCompileContext *ctx,
                                        int begin_idx) {
    return repl_source_scope_view_find_block_end(compile_source_scope(ctx),
                                                begin_idx);
}

static CmdType compile_scope_nearest_open_block_at(
        const ReplCompileContext *ctx, int pos) {
    return repl_source_scope_view_nearest_open_block_at(compile_source_scope(ctx),
                                                       pos);
}

static int compile_nearest_open_block_head_at(
        const ReplCompileContext *ctx, int pos, CmdType *out_type) {
    int stack[REPL_MAX_BLOCK_NEST_DEPTH];
    int depth = 0;

    if (out_type)
        *out_type = CMD_TYPE_COUNT;
    if (!ctx || !ctx->document_cmds)
        return -1;
    if (pos < 0)
        pos = 0;
    if (pos > ctx->document_count)
        pos = ctx->document_count;

    for (int i = 0; i < pos; i++) {
        CmdType t = ctx->document_cmds[i].type;
        if (repl_cmd_is_block_head(t)) {
            if (depth < REPL_MAX_BLOCK_NEST_DEPTH)
                stack[depth++] = i;
        } else if (repl_cmd_is_block_end(t)) {
            if (depth > 0)
                depth--;
        }
    }

    if (depth <= 0)
        return -1;
    if (out_type)
        *out_type = ctx->document_cmds[stack[depth - 1]].type;
    return stack[depth - 1];
}

/* Insert/replace position for ctx: the edit line in insert mode;
 * otherwise the edit line clamped to document_count so a past-the-end
 * edit line appends rather than opening a gap. */
static int compile_insert_pos(const ReplCompileContext *ctx) {
    return ctx->insert_mode ? ctx->edit_line :
           (ctx->edit_line < ctx->document_count
                ? ctx->edit_line : ctx->document_count);
}

static int compile_set_err(char *err, int err_size, const char *fmt, ...) {
    va_list ap;
    if (!err || err_size <= 0)
        return 0;
    va_start(ap, fmt);
    vsnprintf(err, (size_t)err_size, fmt, ap);
    va_end(ap);
    return REPL_COMPILE_ERROR;
}

/* Explain a name the language will never accept as a variable.
 *
 * Exists because the generic "undeclared variable 'x' - use 'float x;'
 * first" prompt is actively wrong for these: following it just produces a
 * second rejection. Returns 1 (and fills err) when it took the name,
 * 0 when the name is merely undeclared and the caller's own message
 * applies. */
static int compile_name_unavailable_err(char *err, int err_size,
                                        const char *name) {
    if (repl_eval_is_c_keyword(name)) {
        compile_set_err(err, err_size,
            "'%s' is a C keyword - it cannot name a variable", name);
        return 1;
    }
    if (repl_eval_is_reserved_ident(name)) {
        compile_set_err(err, err_size,
            "'%s' is a reserved name - it cannot name a variable", name);
        return 1;
    }
    return 0;
}

/* Names that become C declarations in the exporter need one more guard than
 * ordinary expression identifiers. Function parameters and loop iterators
 * are binders rather than REPL variables, so they do not pass through the
 * float-declaration validators above, but the exporter still writes them as
 * `float <name>`. */
static ReplCompileResult compile_validate_c_binder_name(
        const char *name, const char *role, char *err, int err_size) {
    if (repl_eval_is_c_keyword(name))
        return compile_set_err(err, err_size,
            "'%s' is a C keyword - it cannot name a %s", name, role);
    return REPL_COMPILE_OK;
}

/* These names are C keywords, but they are also the heads/separators of
 * structured REPL forms. Let their handlers claim the line instead of
 * reporting them as attempted function names. Other C keywords cannot name a
 * function because export emits the alias as a C function identifier. */
static int compile_func_alias_is_structural_name(const char *name) {
    return strcmp(name, "if") == 0 ||
           strcmp(name, "else") == 0 ||
           strcmp(name, "for") == 0;
}

/* Source index of the innermost open CMD_FUNC_DEF at `pos`, or -1 at
 * document top level. Resolves *through* a nested for/if - a declaration
 * typed one level down still belongs to the owning function, which is what
 * makes hoist-from-any-depth work. */
static int compile_enclosing_func_at(const ReplCompileContext *ctx, int pos) {
    int stack[REPL_MAX_BLOCK_NEST_DEPTH];
    int depth = 0;

    if (!ctx || !ctx->document_cmds)
        return -1;
    if (pos < 0)
        pos = 0;
    if (pos > ctx->document_count)
        pos = ctx->document_count;

    for (int i = 0; i < pos; i++) {
        CmdType t = ctx->document_cmds[i].type;
        if (repl_cmd_is_block_head(t)) {
            if (depth < REPL_MAX_BLOCK_NEST_DEPTH)
                stack[depth++] = i;
        } else if (repl_cmd_is_block_end(t)) {
            if (depth > 0)
                depth--;
        }
    }
    for (int d = depth - 1; d >= 0; d--) {
        if (ctx->document_cmds[stack[d]].type == CMD_FUNC_DEF)
            return stack[d];
    }
    return -1;
}

/* Ordered lexical bindings visible at `pos`, with their kinds. One
 * wrapper so the resolvers below cannot drift on which document view or
 * ordering they collect against. */
typedef struct {
    ExprVar            vars[MAX_EXPR_VARS];
    ReplVisibleVarKind kinds[MAX_EXPR_VARS];
    int                count;
} CompileScopeBindings;

static void compile_collect_bindings(const ReplCompileContext *ctx, int pos,
                                     CompileScopeBindings *out) {
    out->count = collect_visible_vars_in(ctx->text, ctx->document_cmds,
                                         ctx->document_count, pos,
                                         out->vars, MAX_EXPR_VARS, NULL,
                                         out->kinds);
}

/* Innermost binding of `name`, or -1. The *first* match decides - a
 * matching PARAM or LOOP is never skipped in search of an outer LOCAL,
 * which is how eval_primary and flatten resolve reads. */
static int compile_bindings_find(const CompileScopeBindings *b,
                                 const char *name) {
    for (int i = 0; i < b->count; i++) {
        if (strcmp(b->vars[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Parameters + locals of the function opened at `func_idx`; 0 at document
 * top level. Collected at the closing brace, where the frame is still
 * open, so it covers the whole body regardless of where the caller is. */
static int compile_func_binder_count(const ReplCompileContext *ctx,
                                     int func_idx) {
    ExprVar vars[MAX_EXPR_VARS];

    if (!ctx || func_idx < 0 || func_idx >= ctx->document_count)
        return 0;
    return collect_visible_vars_in(ctx->text, ctx->document_cmds,
                                   ctx->document_count,
                                   compile_scope_find_block_end(ctx, func_idx),
                                   vars, MAX_EXPR_VARS, NULL, NULL);
}

/* Open for-loop nesting at `pos`: how many CMD_FOR_BEGIN blocks enclose
 * it. Each level costs one scope-array slot, because flatten_for_loop
 * prepends its iterator to a fresh array per level. */
static int compile_open_loop_depth_at(const ReplCompileContext *ctx, int pos) {
    CmdType stack[REPL_MAX_BLOCK_NEST_DEPTH];
    int depth = 0;
    int loops = 0;

    if (!ctx || !ctx->document_cmds)
        return 0;
    if (pos > ctx->document_count)
        pos = ctx->document_count;

    for (int i = 0; i < pos; i++) {
        CmdType t = ctx->document_cmds[i].type;
        if (repl_cmd_is_block_head(t)) {
            if (depth < REPL_MAX_BLOCK_NEST_DEPTH)
                stack[depth] = t;
            depth++;
            if (t == CMD_FOR_BEGIN)
                loops++;
        } else if (repl_cmd_is_block_end(t)) {
            if (depth > 0) {
                depth--;
                if (depth < REPL_MAX_BLOCK_NEST_DEPTH &&
                    stack[depth] == CMD_FOR_BEGIN)
                    loops--;
            }
        }
    }
    return loops;
}

/* Peak scope-array occupancy anywhere in the function body opened at
 * `func_idx`: its parameters, plus its locals, plus the deepest for-loop
 * nesting in the body. flatten_for_loop prepends an iterator to a fresh
 * array per level and silently drops the last outer binding at the cap,
 * so loop depth is a real multiplier and this - not `params + locals` -
 * is the quantity that must fit MAX_EXPR_VARS. */
static int compile_func_scope_peak(const ReplCompileContext *ctx,
                                   int func_idx) {
    int body_end;
    int depth = 0, max_depth = 0;

    if (!ctx || func_idx < 0 || func_idx >= ctx->document_count)
        return 0;

    body_end = compile_scope_find_block_end(ctx, func_idx);
    for (int i = func_idx + 1; i < body_end && i < ctx->document_count; i++) {
        CmdType t = ctx->document_cmds[i].type;
        if (t == CMD_FOR_BEGIN) {
            depth++;
            if (depth > max_depth)
                max_depth = depth;
        } else if (t == CMD_FOR_END) {
            if (depth > 0)
                depth--;
        }
    }
    return compile_func_binder_count(ctx, func_idx) + max_depth;
}

/* Would binding `name` over [body_start, body_end) capture an existing
 * scalar assignment - turning a row that currently writes a global or an
 * outer local into a write to a new parameter or loop iterator, which are
 * not writable?
 *
 * Shadowing itself stays legal; this is a target-legality check, not a
 * return to the blanket name-collision ban. Nested same-name binders are
 * respected exactly as normal resolution does - a `for(name, ...)` inside
 * the body already owns the assignments it encloses - and a nested
 * function body is a different lexical scope, so it is skipped whole. */
static int compile_binder_captures_assignment(const ReplCompileContext *ctx,
                                              int body_start, int body_end,
                                              const char *name) {
    int depth = 0;          /* block nesting relative to body_start */
    int shadow_depth = -1;  /* depth at which a nested same-name binder opened */

    if (!ctx || !ctx->document_cmds || !name || !name[0])
        return 0;
    if (body_end > ctx->document_count)
        body_end = ctx->document_count;

    for (int i = body_start; i >= 0 && i < body_end; i++) {
        const GLCmd *cmd = &ctx->document_cmds[i];
        CmdType t = cmd->type;
        const char *line;

        if (t == CMD_FUNC_DEF) {
            i = compile_scope_find_block_end(ctx, i);
            continue;
        }
        if (repl_cmd_is_block_head(t)) {
            depth++;
            if (shadow_depth < 0 && t == CMD_FOR_BEGIN) {
                char vn[REPL_PREDEF_NAME_MAX];
                line = source_text_line(ctx->text, i);
                repl_extract_for_var_name(line ? line : "", vn, sizeof(vn));
                if (strcmp(vn, name) == 0)
                    shadow_depth = depth;
            }
            continue;
        }
        if (repl_cmd_is_block_end(t)) {
            if (shadow_depth == depth)
                shadow_depth = -1;
            if (depth > 0)
                depth--;
            continue;
        }
        if (shadow_depth >= 0 || t != CMD_VAR_ASSIGN)
            continue;

        {
            char lhs[REPL_PREDEF_NAME_MAX] = "";
            line = source_text_line(ctx->text, i);
            if (repl_extract_assignment_parts(line ? line : "",
                                              lhs, sizeof(lhs), NULL, 0) &&
                strcmp(lhs, name) == 0)
                return 1;
        }
    }
    return 0;
}

/* Text of a for-header after the iterator's name token - i.e. the bound
 * expressions. "" when the header is malformed. */
static const char *compile_for_header_bounds(const char *line) {
    const char *p = line ? strchr(line, '(') : NULL;

    if (!p)
        return "";
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    return p;
}

/* Select the part of a source row that can read enclosing bindings, plus
 * the position at which those bindings must be resolved.
 *
 * A function header only declares parameters. A for-header is split: its
 * iterator token declares a nested binding, but its bounds are evaluated in
 * the enclosing scope before that binding exists. Every other row resolves
 * after the block heads above it have opened. Both the global and local
 * reference guards use this helper so binder-line shadowing cannot drift. */
static const char *compile_line_reference_scan(
        const ReplCompileContext *ctx, int line_idx, int *scope_pos) {
    const char *line = source_text_line(ctx->text, line_idx);

    if (scope_pos)
        *scope_pos = line_idx + 1;
    if (!line || ctx->document_cmds[line_idx].type == CMD_FUNC_DEF)
        return "";
    if (ctx->document_cmds[line_idx].type == CMD_FOR_BEGIN) {
        if (scope_pos)
            *scope_pos = line_idx;
        return compile_for_header_bounds(line);
    }
    return line;
}

/* Does this row read the global `name` after applying lexical shadowing? */
static int compile_line_uses_global_ident(const ReplCompileContext *ctx,
                                          int line_idx,
                                          const char *name) {
    const char *scan;
    ExprVar visible_vars[MAX_EXPR_VARS];
    int scope_pos;
    int visible_nv;

    if (!ctx || !name || !name[0] ||
        line_idx < 0 || line_idx >= ctx->document_count)
        return 0;

    scan = compile_line_reference_scan(ctx, line_idx, &scope_pos);
    if (!repl_eval_source_uses_ident(scan, name))
        return 0;

    visible_nv = collect_visible_vars_in(ctx->text, ctx->document_cmds,
                                         ctx->document_count, scope_pos,
                                         visible_vars, MAX_EXPR_VARS, NULL, NULL);
    for (int var_idx = 0; var_idx < visible_nv; var_idx++) {
        if (strcmp(visible_vars[var_idx].name, name) == 0)
            return 0;
    }
    return 1;
}

static int compile_name_is_still_referenced(const ReplCompileContext *ctx,
                                            const char *name,
                                            int skip_start,
                                            int skip_end) {
    if (!ctx || !name || !name[0])
        return 0;

    for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
        if (cmd_idx >= skip_start && cmd_idx < skip_end)
            continue;
        if (compile_line_uses_global_ident(ctx, cmd_idx, name))
            return 1;
    }
    return 0;
}

/* Mirror image of compile_line_uses_global_ident: does this line read the
 * *local* `name` of the enclosing function?
 *
 * The global helper cannot be reused here - it deliberately answers 0 the
 * moment the name is bound in an enclosing scope, which since locals joined
 * collect_visible_vars_in() is every reference to a local inside its own
 * body. So the question is inverted: resolve innermost-first and count the
 * line only when the winner is a LOCAL. A nested `for(x, ...)` shadowing it
 * means the line does not reference the local at all, and a legal delete
 * must not be blocked by it.
 *
 * Binder lines are not scanned as ordinary text, because a binder's own
 * line is not uniformly inside its own scope:
 *
 *   - A function header is all declarations; it reads nothing.
 *   - A for-header's *bounds* are evaluated in the enclosing scope, before
 *     the iterator exists - `for(x, 0, x + 1)` reads the outer `x`, which
 *     compile and flatten both honor. Treating the iterator as bound
 *     across the whole header would call that outer local unreferenced and
 *     let it be deleted, and the bound would then silently resolve to a
 *     shadowed global, changing the iteration count. So the iterator's own
 *     declaring token is skipped and the rest resolves against the scope
 *     *outside* the loop. */
static int compile_line_uses_local_ident(const ReplCompileContext *ctx,
                                         int line_idx,
                                         const char *name) {
    const char *scan;
    CompileScopeBindings bind;
    int scope_pos;
    int slot;

    if (!ctx || !name || !name[0] ||
        line_idx < 0 || line_idx >= ctx->document_count)
        return 0;

    scan = compile_line_reference_scan(ctx, line_idx, &scope_pos);
    if (!repl_eval_source_uses_ident(scan, name))
        return 0;

    compile_collect_bindings(ctx, scope_pos, &bind);
    slot = compile_bindings_find(&bind, name);
    return slot >= 0 && bind.kinds[slot] == REPL_VISIBLE_VAR_LOCAL;
}

/* Is the local `name` still read anywhere in the function body
 * (func_idx, body_end), ignoring the rows in [skip_start, skip_end)? */
static int compile_local_name_is_still_referenced(const ReplCompileContext *ctx,
                                                  const char *name,
                                                  int func_idx,
                                                  int skip_start,
                                                  int skip_end) {
    int body_end;

    if (!ctx || !name || !name[0] || func_idx < 0)
        return 0;

    body_end = compile_scope_find_block_end(ctx, func_idx);
    for (int cmd_idx = func_idx + 1;
         cmd_idx < body_end && cmd_idx < ctx->document_count; cmd_idx++) {
        if (cmd_idx >= skip_start && cmd_idx < skip_end)
            continue;
        if (compile_line_uses_local_ident(ctx, cmd_idx, name))
            return 1;
    }
    return 0;
}

/* Storage-aware "is this declared name still read?" predicate - the one
 * place that decides which of the two reference walks applies.
 *
 * The declaration row's own storage picks the walk, and it has to: the
 * global scan answers "does this line read the *global* of that name" and
 * so reports 0 for every read of a local, while the local scan bounded to
 * a function body would miss a global's readers entirely. Rows in
 * [skip_start, skip_end) are the ones being replaced. */
static int compile_decl_name_is_referenced(const ReplCompileContext *ctx,
                                           int decl_idx, const char *name,
                                           int skip_start, int skip_end) {
    if (!ctx || decl_idx < 0 || decl_idx >= ctx->document_count)
        return 0;
    if (ctx->document_cmds[decl_idx].var_idx == REPL_VAR_IDX_LOCAL)
        return compile_local_name_is_still_referenced(
            ctx, name, compile_enclosing_func_at(ctx, decl_idx),
            skip_start, skip_end);
    return compile_name_is_still_referenced(ctx, name, skip_start, skip_end);
}

int repl_compile_decl_replacement_allowed(const ReplCompileContext *ctx,
                                          int pos,
                                          const char *const *kept_names,
                                          int kept_count,
                                          char *err, int err_size) {
    const GLCmd *decl;

    if (err && err_size > 0)
        err[0] = '\0';
    if (!ctx || !ctx->document_cmds || pos < 0 || pos >= ctx->document_count)
        return 1;
    decl = &ctx->document_cmds[pos];
    if (!decl->valid || decl->type != CMD_VAR_DECLARE)
        return 1;

    for (int d = 0; d < decl->payload.decl.count; d++) {
        const char *nm = decl->payload.decl.names[d];
        int kept = 0;
        for (int k = 0; k < kept_count; k++) {
            if (kept_names && kept_names[k] &&
                strcmp(kept_names[k], nm) == 0) {
                kept = 1;
                break;
            }
        }
        if (kept)
            continue;
        if (compile_decl_name_is_referenced(ctx, pos, nm, pos, pos + 1)) {
            compile_set_err(err, err_size,
                            "variable '%s' is in use, cannot overwrite", nm);
            return 0;
        }
    }
    return 1;
}

static void compile_copy_leading_ws(const char *text, char *out, int out_sz) {
    int off = 0;
    const char *p = text ? text : "";

    if (!out || out_sz <= 0)
        return;

    while (*p && isspace((unsigned char)*p) && *p != '\n' && *p != '\r' &&
           off < out_sz - 1) {
        out[off++] = *p++;
    }
    out[off] = '\0';
}

static int compile_find_var_decl(const ReplCompileContext *ctx,
                                 const char *name) {
    if (!ctx || !ctx->document_cmds || !name || !name[0])
        return -1;

    for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
        const GLCmd *cmd = &ctx->document_cmds[cmd_idx];
        if (!cmd->valid || cmd->type != CMD_VAR_DECLARE)
            continue;
        for (int decl_idx = 0; decl_idx < cmd->payload.decl.count; decl_idx++) {
            if (strcmp(cmd->payload.decl.names[decl_idx], name) == 0)
                return cmd_idx;
        }
    }
    return -1;
}

static int compile_append_text(char *out, int out_sz, int *off,
                               const char *fmt, ...) {
    va_list ap;
    int wrote;

    if (!out || !off || *off < 0 || *off >= out_sz)
        return 0;

    va_start(ap, fmt);
    wrote = vsnprintf(out + *off, (size_t)(out_sz - *off), fmt, ap);
    va_end(ap);
    if (wrote < 0 || *off + wrote >= out_sz)
        return 0;
    *off += wrote;
    return 1;
}

static int compile_append_span(char *out, int out_sz, int *off,
                               const char *start, const char *end) {
    int len;

    if (!out || !off || !start || !end || end < start)
        return 0;
    len = (int)(end - start);
    if (*off < 0 || *off + len >= out_sz)
        return 0;
    memcpy(out + *off, start, (size_t)len);
    *off += len;
    out[*off] = '\0';
    return 1;
}

/* Rewrite the initializer for one name in a multi-name decl line.
 *
 * Given `  float a = sin(t), b, c = 2; // ok` and (name="b", value=4),
 * produce `  float a = sin(t), b = 4, c = 2; // ok`.
 *
 * Approach: tokenize the body between `float ` and the first `;`
 * into name segments separated by commas at paren-depth 0 (so
 * `sin(t, u)` initializers don't split). For each segment, parse
 * the leading identifier; if it matches `name`, emit `name = value`,
 * otherwise emit the segment verbatim. Returns 1 on a clean rewrite,
 * 0 if `name` was not found or anything failed (caller falls back to
 * a pure SET_VALUE op without rewriting source).
 *
 * Indentation, the trailing `;`, and any trailing `// comment` are
 * preserved. */
static int compile_rewrite_decl_initializer_text(const char *orig_text,
                                                 const char *name,
                                                 float value,
                                                 char *out,
                                                 int out_sz) {
    char indent[REPL_INDENT_TEXT_MAX];
    const char *line;
    const char *scan;
    const char *comment;
    const char *body_end;
    const char *semi;
    const char *chunk_start;
    int off = 0;
    int found = 0;

    if (!name || !name[0] || !out || out_sz <= 0)
        return 0;

    line = orig_text ? orig_text : "";
    compile_copy_leading_ws(line, indent, sizeof(indent));
    scan = repl_scan_decl_float_prefix(line + strlen(indent), NULL);
    if (!scan)
        return 0;
    while (*scan && isspace((unsigned char)*scan))
        scan++;

    comment = strstr(scan, "//");
    body_end = comment ? comment : scan + strlen(scan);
    semi = body_end;
    for (const char *p = scan; p < body_end; p++) {
        if (*p == ';') {
            semi = p;
            break;
        }
    }

    out[0] = '\0';
    if (!compile_append_text(out, out_sz, &off, "%sstatic float ", indent))
        return 0;

    chunk_start = scan;
    int depth = 0;
    for (const char *p = scan; ; p++) {
        int at_end = (p >= semi);
        char ch = at_end ? '\0' : *p;
        if (!at_end) {
            if (ch == '(') depth++;
            else if (ch == ')' && depth > 0) depth--;
        }
        if (at_end || (ch == ',' && depth == 0)) {
            const char *seg_start = chunk_start;
            const char *seg_end = p;
            const char *name_start;
            const char *name_end;
            char decl_name[REPL_PREDEF_NAME_MAX];
            int decl_len;

            while (seg_start < seg_end && isspace((unsigned char)*seg_start))
                seg_start++;
            while (seg_end > seg_start && isspace((unsigned char)seg_end[-1]))
                seg_end--;
            if (seg_start >= seg_end)
                return 0;

            name_start = seg_start;
            if (!isalpha((unsigned char)*name_start) && *name_start != '_')
                return 0;
            name_end = name_start;
            while (name_end < seg_end &&
                   (isalnum((unsigned char)*name_end) || *name_end == '_'))
                name_end++;
            decl_len = (int)(name_end - name_start);
            if (decl_len <= 0 || decl_len >= (int)sizeof(decl_name))
                return 0;
            memcpy(decl_name, name_start, (size_t)decl_len);
            decl_name[decl_len] = '\0';

            if (seg_start != scan && !compile_append_text(out, out_sz, &off, ", "))
                return 0;

            if (strcmp(decl_name, name) == 0) {
                if (!compile_append_text(out, out_sz, &off, "%s = %g",
                                         name, (double)value))
                    return 0;
                found = 1;
            } else if (!compile_append_span(out, out_sz, &off, seg_start, seg_end)) {
                return 0;
            }

            if (at_end)
                break;
            chunk_start = p + 1;
        }
    }

    if (!found || !compile_append_text(out, out_sz, &off, ";"))
        return 0;
    if (comment && *comment) {
        while (*comment && isspace((unsigned char)*comment))
            comment++;
        if (*comment && !compile_append_text(out, out_sz, &off, " %s", comment))
            return 0;
    }

    return 1;
}

/* Parsed shape of `float a, b = expr, c;` - names + optional init
 * values (already evaluated at compile time so apply doesn't need
 * the source string). decl_comment carries any trailing `// ...`
 * verbatim so format_decl_text can re-emit it. */
typedef struct {
    char  names[MAX_NAMES_PER_DECL][16];
    float init_vals[MAX_NAMES_PER_DECL];
    int   has_init[MAX_NAMES_PER_DECL];
    int   count;
    int   is_local;   /* function-scoped: no predef slot, no initializer */
    char  decl_comment[MAX_LINE_LEN];
} FloatDeclParse;

/* Parse `float NAME [= EXPR] (, NAME [= EXPR])* ;  // comment`.
 * Returns:
 *   REPL_COMPILE_OK + parsed != NULL, *recognized = 1   on success.
 *   REPL_COMPILE_OK + *recognized = 0                   when input
 *      doesn't start with the `float` keyword (caller falls through
 *      to the next handler).
 *   REPL_COMPILE_ERROR with err filled                  on a real
 *      syntax error inside an otherwise float-shaped line.
 *
 * `local_mode` is a locality-aware preflight, not a post-hoc check. The
 * global path validates initializer identifiers against the predef table
 * and evaluates them here, so `float x = param;` inside a function body
 * would die with "unknown identifier 'param'" long before any local
 * diagnostic could run. In local mode the initializer is rejected on
 * sight instead, and so is a variable-panel tag (a local has no slot to
 * hang one on). */
static ReplCompileResult parse_float_name_list(const char *input,
                                               FloatDeclParse *parsed,
                                               int *recognized,
                                               ReplPredefView predef,
                                               int local_mode,
                                               char *err, int err_size) {
    *recognized = 0;
    memset(parsed, 0, sizeof(*parsed));
    parsed->is_local = local_mode ? 1 : 0;

    const char *p = repl_scan_decl_float_prefix(input ? input : "", NULL);
    if (!p)
        return REPL_COMPILE_OK;
    *recognized = 1;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        /* A trailing `// comment` ends the name list just like `;` or
         * end-of-string - the comment-capture below reads it. This lets
         * an interactive decl carry a comment without a typed semicolon
         * (`float n // @tune`), since the `;` key commits before the user
         * can type one; mirrors repl_compile_var_assign's `//` handling. */
        if (*p == ';' || *p == '\0' || (p[0] == '/' && p[1] == '/')) break;
        if (parsed->count > 0) {
            /* Subsequent name must be preceded by ','. A non-comma
             * here means the line was float-shaped through `float `
             * but breaks the comma-separated list - fall through to
             * the next handler instead of erroring. */
            if (*p != ',') {
                *recognized = 0;
                return REPL_COMPILE_OK;
            }
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
        }
        if (!isalpha((unsigned char)*p) && *p != '_')
            return compile_set_err(err, err_size,
                "syntax error in float declaration: expected identifier");

        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int len = (int)(p - start);
        if (len <= 0 || len >= 16)
            return compile_set_err(err, err_size, "invalid identifier (max 15 chars)");
        if (parsed->count >= MAX_NAMES_PER_DECL)
            return compile_set_err(err, err_size,
                "too many names per declaration (max %d); split across lines",
                MAX_NAMES_PER_DECL);
        memcpy(parsed->names[parsed->count], start, (size_t)len);
        parsed->names[parsed->count][len] = '\0';

        /* Optional `= expr`. Stop at unparenthesized comma so the
         * outer name loop picks up the next decl, and at a top-level
         * `//` so `float n = 1 // @tune` keeps the comment out of the
         * initializer. `==` is left for the eval validator to reject -
         * a literal `=` followed by `=` is not a decl initializer. */
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '=' && p[1] != '=') {
            if (local_mode)
                return compile_set_err(err, err_size,
                    "local declarations cannot have an initializer - assign on the next line");
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0' || *p == ';' || *p == ',')
                return compile_set_err(err, err_size, "expected expression after '='");

            const char *expr_start = p;
            int depth = 0;
            while (*p && *p != ';') {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                else if (*p == ',' && depth == 0) break;
                else if (p[0] == '/' && p[1] == '/' && depth == 0) break;
                p++;
            }

            char init_expr[MAX_LINE_LEN];
            int elen = (int)(p - expr_start);
            if (elen >= (int)sizeof(init_expr)) elen = (int)sizeof(init_expr) - 1;
            memcpy(init_expr, expr_start, (size_t)elen);
            init_expr[elen] = '\0';
            while (elen > 0 && isspace((unsigned char)init_expr[elen - 1]))
                init_expr[--elen] = '\0';
            if (elen == 0)
                return compile_set_err(err, err_size, "expected expression after '='");

            char verr[REPL_DIAG_TEXT_MAX];
            if (!repl_eval_validate_expression_idents(
                    &(ReplExprIdentValidationConfig){
                        .src = init_expr,
                        .predef = predef,
                        .err = verr,
                        .errsz = (int)sizeof(verr),
                    }))
                return compile_set_err(err, err_size, "%s", verr);
            ExprCtx eval_ctx = { init_expr, predef.vars, predef.count, NULL, 0 };
            parsed->init_vals[parsed->count] = repl_eval_expr(&eval_ctx);
            parsed->has_init[parsed->count] = 1;
        }

        parsed->count++;
    }

    if (parsed->count == 0)
        return compile_set_err(err, err_size,
            "float declaration requires at least one identifier");
    if (*p == ';') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0' && !(p[0] == '/' && p[1] == '/'))
        return compile_set_err(err, err_size,
            "syntax error: unexpected trailing text after declaration");
    if (p[0] == '/' && p[1] == '/')
        snprintf(parsed->decl_comment, sizeof(parsed->decl_comment), " %s", p);

    /* @tune / @config / @bool drive the variable panel, which is keyed on
     * predef slots. A local has none, so the tag would be silently inert. */
    if (local_mode && (strstr(parsed->decl_comment, "@tune") ||
                       strstr(parsed->decl_comment, "@config") ||
                       strstr(parsed->decl_comment, "@bool")))
        return compile_set_err(err, err_size,
            "@tune/@config/@bool require a global declaration - "
            "use 'static float'");

    return REPL_COMPILE_OK;
}

/* Validate the parsed name list against project invariants:
 *   - no duplicates within the declaration
 *   - no clash with reserved keywords (`t`, `PI`, etc.)
 *   - identifiers start with a letter or underscore
 *   - names already in the predef table are only re-declarable when
 *     they're being *kept* across a decl-row overwrite (old_decl has
 *     them); otherwise it's a duplicate decl
 *   - net new slot count fits MAX_PREDEF_VARS */
static ReplCompileResult validate_decl_names(const FloatDeclParse *parsed,
                                             const GLCmd *old_decl,
                                             ReplPredefView predef,
                                             char *err, int err_size) {
    for (int var_idx = 0; var_idx < parsed->count; var_idx++) {
        const char *nm = parsed->names[var_idx];

        for (int prev = 0; prev < var_idx; prev++) {
            if (strcmp(nm, parsed->names[prev]) == 0)
                return compile_set_err(err, err_size,
                    "duplicate name '%s' in declaration", nm);
        }
        if (repl_eval_find_predef_var_idx_in(predef.vars, predef.count, nm) >= 0) {
            int in_old_decl = 0;
            if (old_decl) {
                for (int d = 0; d < old_decl->payload.decl.count; d++) {
                    if (strcmp(old_decl->payload.decl.names[d], nm) == 0) {
                        in_old_decl = 1;
                        break;
                    }
                }
            }
            if (!in_old_decl)
                return compile_set_err(err, err_size,
                    "'%s' is already declared", nm);
        }
        if (repl_eval_is_c_keyword(nm))
            return compile_set_err(err, err_size,
                "'%s' is a C keyword - it cannot name a variable", nm);
        if (repl_eval_is_reserved_ident(nm))
            return compile_set_err(err, err_size, "'%s' is reserved", nm);
        if (!(isalpha((unsigned char)nm[0]) || nm[0] == '_'))
            return compile_set_err(err, err_size, "invalid identifier '%s'", nm);
    }

    int old_count = old_decl ? old_decl->payload.decl.count : 0;
    if (predef.count + parsed->count - old_count > MAX_PREDEF_VARS)
        return compile_set_err(err, err_size,
            "variable table full (max %d)", MAX_PREDEF_VARS);

    return REPL_COMPILE_OK;
}

/* Same-scope redefinition check for a function-scoped declaration.
 *
 * This is C's rule, not a blanket no-shadowing ban. Locals hoist to the
 * function-body top, which is the *same* scope as the parameter list, so a
 * collision with a parameter or with another local of the body is a
 * redefinition and is rejected. A loop iterator is a nested scope and
 * globals are outer, so those collisions are ordinary legal shadowing:
 * `for(i, 0, n) { float i; }` compiles, and resolution is innermost-first.
 *
 * `bind` must be collected over the whole enclosing body (so a local
 * declared *after* the row being edited still counts), with `old_decl`
 * naming the row being replaced - its own names are not collisions with
 * itself. `scope_peak` is the post-edit peak from compile_func_scope_peak
 * minus this row's contribution. */
static ReplCompileResult validate_local_decl_names(const FloatDeclParse *parsed,
                                                   const GLCmd *old_decl,
                                                   const CompileScopeBindings *bind,
                                                   int scope_peak,
                                                   char *err, int err_size) {
    for (int var_idx = 0; var_idx < parsed->count; var_idx++) {
        const char *nm = parsed->names[var_idx];

        for (int prev = 0; prev < var_idx; prev++) {
            if (strcmp(nm, parsed->names[prev]) == 0)
                return compile_set_err(err, err_size,
                    "duplicate name '%s' in declaration", nm);
        }
        if (repl_eval_is_c_keyword(nm))
            return compile_set_err(err, err_size,
                "'%s' is a C keyword - it cannot name a variable", nm);
        if (repl_eval_is_reserved_ident(nm))
            return compile_set_err(err, err_size, "'%s' is reserved", nm);
        if (!(isalpha((unsigned char)nm[0]) || nm[0] == '_'))
            return compile_set_err(err, err_size, "invalid identifier '%s'", nm);

        for (int b = 0; b < bind->count; b++) {
            if (bind->kinds[b] == REPL_VISIBLE_VAR_LOOP)
                continue;   /* nested scope - shadowing it is legal C */
            if (strcmp(bind->vars[b].name, nm) != 0)
                continue;
            if (old_decl) {
                int in_old_decl = 0;
                for (int d = 0; d < old_decl->payload.decl.count; d++) {
                    if (strcmp(old_decl->payload.decl.names[d], nm) == 0) {
                        in_old_decl = 1;
                        break;
                    }
                }
                if (in_old_decl)
                    continue;   /* this row's own binding, being rewritten */
            }
            return compile_set_err(err, err_size,
                bind->kinds[b] == REPL_VISIBLE_VAR_PARAM
                    ? "'%s' is already a parameter of this function"
                    : "'%s' is already declared in this function",
                nm);
        }
    }

    if (scope_peak + parsed->count > MAX_EXPR_VARS)
        return compile_set_err(err, err_size,
            "function scope full (max %d parameters + locals + loop depth)",
            MAX_EXPR_VARS);

    return REPL_COMPILE_OK;
}

/* Format the decl into source text. The storage keyword is canonical and
 * mirrors what the author typed: a global predef var is a file-scope
 * static that retains its value across frames (the exporter emits it as
 * `static float ...`), while a local is a plain C automatic living for
 * one call. Surfacing the keyword in the code panel makes that lifetime
 * difference - and which storage the commit actually chose - obvious.
 * `indent` is the row's gutter: depth 0 for a global, the enclosing
 * function body's depth for a local.
 *
 * Returns 0 when the *name list* did not fit, in which case the caller must
 * reject the line rather than commit a partial row: a dropped name stays
 * registered in the predef table while vanishing from the source text, which
 * is the desync the p83 regression in tests/test_repl_core_io.c guards. A
 * clipped trailing comment is not a failure - it costs no state - so it does
 * not feed the flag.
 *
 * The row can be *wider than the line the author typed*: `static ` is added,
 * `", "` replaces `","`, and each `= %g` re-render can outgrow its source
 * literal (`1e30` -> `1e+30`). Appends therefore go through
 * repl_append_clamped and never `off += snprintf` - see src/repl/util.h for
 * why the raw idiom writes out of bounds. */
static int format_decl_text(const FloatDeclParse *parsed,
                            const char *indent,
                            char *out, int out_sz) {
    int names_truncated = 0;
    int off;

    if (!out || out_sz <= 0)
        return 0;
    out[0] = '\0';

    off = repl_append_clamped(out, (size_t)out_sz, 0, &names_truncated,
                              "%s%sfloat ", indent ? indent : "  ",
                              parsed->is_local ? "" : "static ");
    for (int var_idx = 0; var_idx < parsed->count; var_idx++) {
        if (var_idx > 0)
            off = repl_append_clamped(out, (size_t)out_sz, off,
                                      &names_truncated, ", ");
        off = repl_append_clamped(out, (size_t)out_sz, off, &names_truncated,
                                  "%s", parsed->names[var_idx]);
        if (parsed->has_init[var_idx])
            off = repl_append_clamped(out, (size_t)out_sz, off,
                                      &names_truncated, " = %g",
                                      parsed->init_vals[var_idx]);
    }
    repl_append_clamped(out, (size_t)out_sz, off, NULL, ";%s",
                        parsed->decl_comment);
    return !names_truncated;
}

/* Build the predef operations for the decl change. Three cases per
 * name:
 *   - dropped (in old_decl, not in new) -> UNDECLARE
 *   - new (not currently in predef table) -> DECLARE [+ value]
 *   - kept (already declared) -> SET_VALUE if has_init else NOOP
 *
 * UNDECLAREs go first so apply's slot-shift cascade observes the
 * pre-removal indices (see repl_apply_predef_ops). */
static void build_decl_predef_ops(const FloatDeclParse *parsed,
                                  const GLCmd *old_decl,
                                  ReplPredefView predef,
                                  ReplCompiledChange *out) {
    int op_count = 0;

    if (old_decl) {
        for (int d = 0; d < old_decl->payload.decl.count; d++) {
            const char *nm = old_decl->payload.decl.names[d];
            int kept = 0;
            for (int v = 0; v < parsed->count; v++) {
                if (strcmp(parsed->names[v], nm) == 0) { kept = 1; break; }
            }
            if (kept) continue;
            if (op_count >= MAX_PREDEF_OPS_PER_COMMIT) break;
            out->predef_ops[op_count].kind = REPL_PREDEF_OP_UNDECLARE;
            repl_copy_string_fits(out->predef_ops[op_count].name,
                                  sizeof(out->predef_ops[op_count].name), nm);
            op_count++;
        }
    }
    for (int v = 0; v < parsed->count; v++) {
        if (op_count >= MAX_PREDEF_OPS_PER_COMMIT) break;
        const char *nm = parsed->names[v];
        int already_registered =
            (old_decl && repl_eval_find_predef_var_idx_in(predef.vars, predef.count, nm) >= 0);
        ReplPredefOp *op = &out->predef_ops[op_count++];

        if (already_registered) {
            if (parsed->has_init[v]) {
                op->kind = REPL_PREDEF_OP_SET_VALUE;
                op->value = parsed->init_vals[v];
                op->has_value = 1;
                repl_copy_string_fits(op->name, sizeof(op->name), nm);
            } else {
                op->kind = REPL_PREDEF_OP_NOOP;
            }
        } else {
            op->kind = REPL_PREDEF_OP_DECLARE;
            repl_copy_string_fits(op->name, sizeof(op->name), nm);
            if (parsed->has_init[v]) {
                op->value = parsed->init_vals[v];
                op->has_value = 1;
            }
        }
    }
    out->predef_op_count = op_count;
}

/* Compose "declared a, b, c" - or "declared local a, b in blade" - for
 * the status banner. Storage depends on cursor position for the plain
 * `float` case, which is otherwise invisible state; naming it here gives
 * the author a second confirmation alongside the canonical text. */
static void build_decl_commit_message(const FloatDeclParse *parsed,
                                      const char *func_name,
                                      char *msg, int msg_sz) {
    int off = snprintf(msg, (size_t)msg_sz, "declared %s",
                       parsed->is_local ? "local " : "");
    for (int v = 0; v < parsed->count && off < msg_sz - 4; v++) {
        if (v > 0)
            off += snprintf(msg + off, (size_t)(msg_sz - off), ", ");
        off += snprintf(msg + off, (size_t)(msg_sz - off), "%s",
                        parsed->names[v]);
    }
    if (parsed->is_local && func_name && func_name[0] && off < msg_sz - 4)
        snprintf(msg + off, (size_t)(msg_sz - off), " in %s", func_name);
}

/* funcN / alias name of the header at `func_idx`, for the status banner. */
static void compile_func_display_name(const ReplCompileContext *ctx,
                                      int func_idx, char *out, int out_sz) {
    const char *p;
    char ident[REPL_FUNC_NAME_MAX];
    int fn = -1;

    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';
    if (!ctx || func_idx < 0 || func_idx >= ctx->document_count)
        return;

    p = source_text_line(ctx->text, func_idx);
    if (!p)
        return;
    switch (repl_scan_func_name_token(&p, &fn, ident)) {
    case 1: snprintf(out, (size_t)out_sz, "func%d", fn); break;
    case 2: repl_copy_string_fits(out, (size_t)out_sz, ident); break;
    default: break;
    }
}

/* Return the insertion boundary for declarations in [start, limit). The
 * leading run may contain comments, blank lines and other declarations, and
 * nothing else. When declarations already exist and their trailing prologue
 * rows are only blank lines, the boundary is immediately after the last
 * declaration so those blanks stay below a newly inserted declaration. A
 * trailing comment is preserved above the next declaration because it may
 * be prose introducing that declaration. When there are no declarations
 * yet, the boundary is the end of the leading non-code run. Both storage
 * classes use it, so a global at document top and a local at function-body
 * top follow one rule, and no command type has to be enumerated (or kept up
 * to date) to describe where a declaration may land.
 *
 * The hoist moves the declaration only. Comments are never rewritten or
 * carried along - a declaration passing a comment leaves it exactly where
 * its author put it. If no declaration exists yet, the single exception is
 * a comment run that leads into a function definition: export treats such a
 * run as belonging to that definition (comment_run_attached_func_idx() in
 * export_cmd_writer.c and the backward scan in write_func_defs_as_c), so a
 * declaration landing inside the run would relocate that prose from above
 * the generated C function into draw_scene(). The boundary therefore goes
 * above that run. If a later prologue suffix contains a comment, the
 * comment-aware boundary remains after that suffix. */
static int compile_decl_prologue_end(const GLCmd *cmds, int start, int limit) {
    int pos = start;
    int comment_run_start = -1;
    int last_decl_end = -1;
    int post_decl_comment = 0;

    if (!cmds)
        return start;
    while (pos >= 0 && pos < limit) {
        if (cmds[pos].type == CMD_VAR_DECLARE) {
            comment_run_start = -1;
            last_decl_end = pos + 1;
            post_decl_comment = 0;
        } else if (cmds[pos].type == CMD_COMMENT ||
                   cmds[pos].type == CMD_EMPTY) {
            if (comment_run_start < 0)
                comment_run_start = pos;
            if (cmds[pos].type == CMD_COMMENT && last_decl_end >= start)
                post_decl_comment = 1;
        } else {
            break;
        }
        pos++;
    }
    if (last_decl_end >= start && !post_decl_comment)
        return last_decl_end;
    if (comment_run_start >= 0 && pos < limit &&
        cmds[pos].type == CMD_FUNC_DEF)
        return comment_run_start;
    return pos;
}

/* Pick the insertion point for a new declaration. Declarations typed below
 * the prologue are hoisted to its end, but a declaration typed above an
 * existing declaration must stay at the requested position rather than
 * being moved down past it. In other words, hoisting is one-way: the
 * declaration may move up, never down. */
static int compile_decl_insert_pos(const GLCmd *cmds, int start, int limit,
                                    int requested_pos) {
    int prologue_end = compile_decl_prologue_end(cmds, start, limit);

    if (requested_pos < start)
        return start;
    return requested_pos < prologue_end ? requested_pos : prologue_end;
}

/* The local arm of repl_compile_float_decl: a declaration inside a
 * function body. It emits no predef ops at all - the prologue row *is*
 * the binding, which is why deleting it is guarded like a live variable
 * and why the row carries REPL_VAR_IDX_LOCAL rather than a slot.
 *
 * `old_decl` is the local decl row being rewritten in place, or NULL. */
static ReplCompileResult compile_float_decl_local(const ReplCompileContext *ctx,
                                                  const FloatDeclParse *parsed,
                                                  int func_idx, int insert_idx,
                                                  const GLCmd *old_decl,
                                                  ReplCompiledChange *out,
                                                  char *err, int err_size) {
    CompileScopeBindings bind;
    char indent[REPL_INDENT_TEXT_MAX];
    char func_name[REPL_FUNC_NAME_MAX];
    char decl_text[MAX_LINE_LEN];
    GLCmd cmd;
    int body_end = compile_scope_find_block_end(ctx, func_idx);
    int scope_peak = compile_func_scope_peak(ctx, func_idx);
    int decl_pos;
    ReplCompileResult r;

    /* Collected at the closing brace, not at the cursor: a local declared
     * *after* the row being edited is still in the same scope, and a
     * same-scope redefinition has to be caught from either direction. */
    compile_collect_bindings(ctx, body_end, &bind);
    if (old_decl)
        scope_peak -= old_decl->payload.decl.count;

    r = validate_local_decl_names(parsed, old_decl, &bind, scope_peak,
                                  err, err_size);
    if (r != REPL_COMPILE_OK)
        return r;

    /* Overwrite-feasibility, in local terms: a dropped name must not still
     * be read in the body. Unlike a global, there is no predef slot to
     * outlive the row, so removing it would leave the reads bound to
     * nothing. */
    if (old_decl) {
        const char *kept[MAX_NAMES_PER_DECL];
        for (int v = 0; v < parsed->count && v < MAX_NAMES_PER_DECL; v++)
            kept[v] = parsed->names[v];
        if (!repl_compile_decl_replacement_allowed(ctx, insert_idx, kept,
                                                   parsed->count, err, err_size))
            return REPL_COMPILE_ERROR;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.type    = CMD_VAR_DECLARE;
    cmd.valid   = 1;
    cmd.var_idx = REPL_VAR_IDX_LOCAL;
    cmd.payload.decl.count = parsed->count;
    for (int v = 0; v < parsed->count; v++) {
        if (!repl_copy_string_fits(cmd.payload.decl.names[v],
                                   sizeof(cmd.payload.decl.names[v]),
                                   parsed->names[v]))
            return compile_set_err(err, err_size, "invalid identifier (max 15 chars)");
    }

    /* This body's declaration prologue. Hoisting here - from whatever
     * for/if the author typed in - is what keeps every reference after its
     * declaration; the body's introductory prose stays above the locals it
     * introduces because comments are not code and the prologue passes
     * them. Still valid C89: a comment is not a statement. */
    decl_pos = compile_decl_insert_pos(
        ctx->document_cmds, func_idx + 1,
        body_end < ctx->document_count ? body_end : ctx->document_count,
        insert_idx);

    if (old_decl) {
        out->kind  = REPL_COMPILED_REPLACE_ONE;
        out->pos   = insert_idx;
        out->count = 1;
        out->adjust_edit_line = 0;
    } else {
        out->kind  = REPL_COMPILED_INSERT_ONE;
        out->pos   = decl_pos;
        out->count = 1;
        out->adjust_edit_line = 1;  /* REPL_COMMAND_STORE_ADJUST_EDIT_LINE */
    }
    out->cmds[0] = cmd;

    compile_scope_cmd_indent(ctx, out->pos, indent, sizeof(indent));
    if (!format_decl_text(parsed, indent, decl_text, (int)sizeof(decl_text)))
        return compile_set_err(err, err_size,
            "declaration too long for one line (max %d chars once formatted); "
            "split across lines", MAX_LINE_LEN - 1);
    repl_copy_string_fits(out->text[0], sizeof(out->text[0]), decl_text);

    compile_func_display_name(ctx, func_idx, func_name, (int)sizeof(func_name));
    build_decl_commit_message(parsed, func_name, out->commit_message,
                              (int)sizeof(out->commit_message));
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_float_decl(const char *input,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);

    /* Storage is decided *before* parsing, because the two paths validate
     * initializers differently - the global path evaluates them against
     * the predef table, which would reject `float x = param;` with an
     * unknown-identifier error before the local diagnostic could run.
     * Two steps, in order:
     *   1. `static` typed  -> global, from any cursor position (also the
     *      escape hatch for declaring a global from inside a function);
     *   2. otherwise, an enclosing function -> local; top level -> global. */
    int has_static = 0;
    if (!repl_scan_decl_float_prefix(input ? input : "", &has_static)) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    int insert_idx = compile_insert_pos(ctx);
    int cursor_func_idx = compile_enclosing_func_at(ctx, insert_idx);
    int func_idx = has_static ? -1 : cursor_func_idx;

    FloatDeclParse parsed;
    int recognized = 0;
    ReplCompileResult r =
        parse_float_name_list(input, &parsed, &recognized, ctx->predef,
                              func_idx >= 0, err, err_size);
    if (r != REPL_COMPILE_OK)
        return r;
    if (!recognized) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    /* Decl placement and overwrite-in-place detection. New decls are
     * hoisted up to the end of the declaration prologue, unless they were
     * typed above an existing declaration; in that case they stay at the
     * requested position. Editing an existing CMD_VAR_DECLARE row is the
     * only case that replaces in-place. */
    int overwriting_decl = (!ctx->insert_mode &&
                            insert_idx < ctx->document_count &&
                            ctx->document_cmds[insert_idx].type == CMD_VAR_DECLARE);
    const GLCmd *old_decl = overwriting_decl ? &ctx->document_cmds[insert_idx] : NULL;
    int old_is_local = old_decl && old_decl->var_idx == REPL_VAR_IDX_LOCAL;

    if (func_idx >= 0) {
        if (old_decl && !old_is_local)
            return compile_set_err(err, err_size,
                "cannot convert a global declaration to a local - delete it, "
                "then retype it inside the function");
        return compile_float_decl_local(ctx, &parsed, func_idx, insert_idx,
                                        old_decl, out, err, err_size);
    }

    /* Global path. A local row being retyped as `static float` is a
     * *storage conversion*, not an ordinary overwrite: it credits no old
     * predef slot (there was none), emits no UNDECLARE, and every name
     * needs a fresh slot - so a full table rejects it before any source
     * mutation. old_global is NULL in that case precisely to get that
     * accounting. */
    const GLCmd *old_global = old_is_local ? NULL : old_decl;

    r = validate_decl_names(&parsed, old_global, ctx->predef, err, err_size);
    if (r != REPL_COMPILE_OK)
        return r;

    /* Overwrite-feasibility: removed names must not be referenced
     * elsewhere in the document. Replacement is rejected outright
     * rather than auto-deleting the references. */
    if (old_global) {
        const char *kept[MAX_NAMES_PER_DECL];
        for (int v = 0; v < parsed.count && v < MAX_NAMES_PER_DECL; v++)
            kept[v] = parsed.names[v];
        if (!repl_compile_decl_replacement_allowed(ctx, insert_idx, kept,
                                                   parsed.count, err, err_size))
            return REPL_COMPILE_ERROR;
    } else if (old_is_local) {
        /* No exemptions: an *unchanged* name is still being removed from
         * local storage, and every compiled assignment to it carries
         * REPL_VAR_IDX_LOCAL. Refusing the edit is the resolution - same
         * shape as the rule next door, and no reclassify-every-assignment
         * transaction to get wrong. */
        if (!repl_compile_decl_replacement_allowed(ctx, insert_idx, NULL, 0,
                                                   err, err_size))
            return REPL_COMPILE_ERROR;
    }

    /* Build the GLCmd that backs the source row. */
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_VAR_DECLARE;
    cmd.valid = 1;
    cmd.payload.decl.count = parsed.count;
    for (int v = 0; v < parsed.count; v++) {
        if (!repl_copy_string_fits(cmd.payload.decl.names[v],
                                   sizeof(cmd.payload.decl.names[v]),
                                   parsed.names[v]))
            return compile_set_err(err, err_size, "invalid identifier (max 15 chars)");
    }

    /* The document's declaration prologue - the same rule the local arm
     * uses, so a scene that opens with an explanatory comment keeps that
     * explanation above the declarations it introduces instead of having
     * the next declaration inserted over it. A declaration typed below the
     * first executable row is hoisted to this prologue, while one typed
     * above an existing declaration stays at its requested position. */
    int decl_pos = compile_decl_insert_pos(ctx->document_cmds, 0,
                                           ctx->document_count, insert_idx);

    if (old_global) {
        out->kind  = REPL_COMPILED_REPLACE_ONE;
        out->pos   = insert_idx;
        out->count = 1;
        out->adjust_edit_line = 0;
    } else if (old_is_local) {
        /* The row has to move - local storage lives at the function-body
         * top, global storage at the document top - so this is a
         * delete-here-and-reinsert, not a replace-in-place. `pos` is in
         * post-delete coordinates (compile.h convention). */
        out->kind         = REPL_COMPILED_INSERT_ONE;
        out->delete_pos   = insert_idx;
        out->delete_count = 1;
        out->pos          = (insert_idx < decl_pos) ? decl_pos - 1 : decl_pos;
        out->count        = 1;
        out->adjust_edit_line = 1;
    } else {
        out->kind  = REPL_COMPILED_INSERT_ONE;
        out->pos   = decl_pos;
        out->count = 1;
        out->adjust_edit_line = 1;  /* REPL_COMMAND_STORE_ADJUST_EDIT_LINE */
    }
    out->cmds[0] = cmd;

    char decl_text[MAX_LINE_LEN];
    /* A global declaration always lands at depth 0, so its indentation is
     * the display-body base alone - 0 above the boundary, 2 below it. Use
     * the INSERT comparison: a declaration committed into an all-prologue
     * document sits AT the boundary, and the existing-row rule would put
     * it at indent 2 above `void display(void) {`. Nothing reformats
     * behind this (Ctrl+\ is the user's call), so what is written here is
     * canonical. */
    char decl_indent[REPL_INDENT_TEXT_MAX];
    int decl_base = repl_source_scope_view_base_indent_for_insert(
        compile_source_scope(ctx), out->pos);
    if (decl_base > (int)sizeof(decl_indent) - 1)
        decl_base = (int)sizeof(decl_indent) - 1;
    memset(decl_indent, ' ', (size_t)decl_base);
    decl_indent[decl_base] = '\0';
    /* Before build_decl_predef_ops, so a rejected line registers nothing -
     * same atomicity as the "too many names" path above. */
    if (!format_decl_text(&parsed, decl_indent, decl_text, (int)sizeof(decl_text)))
        return compile_set_err(err, err_size,
            "declaration too long for one line (max %d chars once formatted); "
            "split across lines", MAX_LINE_LEN - 1);
    repl_copy_string_fits(out->text[0], sizeof(out->text[0]), decl_text);

    build_decl_predef_ops(&parsed, old_global, ctx->predef, out);
    build_decl_commit_message(&parsed, NULL, out->commit_message,
                              (int)sizeof(out->commit_message));
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_split_decl(const ReplCompileContext *ctx,
                                          int line_idx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);

    if (line_idx < 0 || line_idx >= ctx->document_count ||
        ctx->document_cmds[line_idx].type != CMD_VAR_DECLARE) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    /* Recover names + initializers + trailing comment from the line's
     * canonical source text. A committed decl stores evaluated literals
     * (commit already evaluated any init expression), so re-parsing and
     * re-emitting is lossless. */
    const char *line = source_text_line(ctx->text, line_idx);
    int is_local = (ctx->document_cmds[line_idx].var_idx == REPL_VAR_IDX_LOCAL);
    char indent[REPL_INDENT_TEXT_MAX];
    FloatDeclParse parsed;
    int recognized = 0;
    ReplCompileResult r =
        parse_float_name_list(line ? line : "", &parsed, &recognized,
                              ctx->predef, is_local, err, err_size);
    if (r != REPL_COMPILE_OK)
        return r;
    if (!recognized || parsed.count < 2) {
        /* Single-name (or unparseable) decl: nothing to split. */
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }
    if (parsed.count > MAX_COMMIT_CMDS)
        return compile_set_err(err, err_size,
            "too many names to split (%d > %d)", parsed.count, MAX_COMMIT_CMDS);

    /* Storage survives the split: every emitted row keeps the source
     * row's kind, so splitting a local does not silently promote it to a
     * document-top global. */
    compile_scope_cmd_indent(ctx, line_idx, indent, sizeof(indent));
    for (int i = 0; i < parsed.count; i++) {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type  = CMD_VAR_DECLARE;
        cmd.valid = 1;
        cmd.var_idx = is_local ? REPL_VAR_IDX_LOCAL : 0;
        cmd.payload.decl.count = 1;
        repl_copy_string_fits(cmd.payload.decl.names[0],
                              sizeof(cmd.payload.decl.names[0]),
                              parsed.names[i]);
        out->cmds[i] = cmd;

        /* A single-name view drives format_decl_text so each line gets
         * its own `= value`; the original line's trailing comment rides
         * the first line only, leaving the rest clean for per-variable
         * // @tune tags. */
        FloatDeclParse one;
        memset(&one, 0, sizeof(one));
        one.count = 1;
        one.is_local = is_local;
        repl_copy_string_fits(one.names[0], sizeof(one.names[0]),
                              parsed.names[i]);
        one.has_init[0]  = parsed.has_init[i];
        one.init_vals[0] = parsed.init_vals[i];
        if (i == 0)
            repl_copy_string_fits(one.decl_comment, sizeof(one.decl_comment),
                                  parsed.decl_comment);
        /* A single-name view cannot overflow the name list today, but the
         * check keeps the contract honest if the name/line limits move. */
        if (!format_decl_text(&one, indent, out->text[i],
                              (int)sizeof(out->text[i])))
            return compile_set_err(err, err_size,
                "declaration too long for one line (max %d chars once "
                "formatted)", MAX_LINE_LEN - 1);
    }

    /* Replace the one decl line in place: delete it, then insert the N
     * single-name decls at the same index. `pos` is in post-delete
     * coordinates (compile.h convention); deleting one line at line_idx
     * leaves line_idx as the insert point. No predef ops - the
     * variables stay declared with their current values. */
    out->kind             = REPL_COMPILED_INSERT_MANY;
    out->pos              = line_idx;
    out->count            = parsed.count;
    out->delete_pos       = line_idx;
    out->delete_count     = 1;
    out->adjust_edit_line = 0;

    snprintf(out->commit_message, sizeof(out->commit_message),
             "Split declaration into %d lines", parsed.count);
    return REPL_COMPILE_OK;
}

static int compile_rebase_var_assign_slot_after_undeclares(
        const char *name,
        int original_slot,
        const ReplCompiledChange *change,
        ReplPredefView predef) {
    int shifted_slot = original_slot;

    for (int op_idx = 0; op_idx < change->predef_op_count; op_idx++) {
        const ReplPredefOp *op = &change->predef_ops[op_idx];
        int dropped_slot;

        if (op->kind != REPL_PREDEF_OP_UNDECLARE)
            continue;

        dropped_slot = repl_eval_find_predef_var_idx_in(predef.vars, predef.count,
                                                        op->name);
        if (dropped_slot < 0)
            continue;
        if (strcmp(op->name, name) == 0)
            return -1;
        if (dropped_slot < original_slot)
            shifted_slot--;
    }

    return shifted_slot;
}

/* Does this assignment RHS open a `{...}` cell list? Tested on the raw
 * text rather than on a successful split so that a malformed list gets the
 * block form's diagnostic instead of falling through to "undeclared
 * variable 'A'", which describes nothing the user typed. */
static int compile_rhs_is_brace_list(const char *rhs) {
    while (rhs && *rhs && isspace((unsigned char)*rhs)) rhs++;
    return rhs && *rhs == '{';
}

/* Parse a scratch block's base index: a bare non-negative decimal
 * literal, nothing else. Returns 1 and fills `out` on success. */
static int compile_parse_scratch_base_literal(const char *index_expr, int *out) {
    const char *p = index_expr;
    int value = 0;
    int digits = 0;

    while (*p && isspace((unsigned char)*p)) p++;
    while (isdigit((unsigned char)*p)) {
        /* Saturate rather than overflow. An out-of-range base is still a
         * well-formed literal, so it must reach the caller's range check
         * and get that diagnostic, not "needs a literal base index". */
        if (value <= REPL_SCRATCH_ARRAY_LEN)
            value = value * 10 + (*p - '0');
        digits++;
        p++;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    if (!digits || *p != '\0')
        return 0;
    *out = value;
    return 1;
}

/* A[base] = {e0, ..., eN} - 2..16 cells into a scratch array.
 * Fills *cmd, out->scratch_ops / commit_message, and rewrites *rhs to
 * the canonical ", "-separated form. Shared text formatting is still
 * done by the caller. */
static ReplCompileResult compile_var_assign_block_scratch(
        const char *name, const char *index_expr, char *rhs, size_t rhs_sz,
        const char *comment, int insert_idx,
        const ReplCompileContext *ctx,
        ExprVar *vis, int vis_n,
        GLCmd *cmd, ReplCompiledChange *out,
        char *err, int err_size) {
    static const char k_block_usage[] =
        "Usage: A[base] = {e0, ..., eN} - 2 to 16 values into a scratch "
        "array (A, B, or C), with base + count <= 16";
    ReplScratchBlockCell cells[REPL_SCRATCH_ARRAY_LEN];
    float vals[REPL_SCRATCH_ARRAY_LEN];
    int scratch_array_idx = repl_eval_scratch_array_index(name);
    int cell_count = repl_split_scratch_block_rhs(rhs, cells,
                                                  REPL_SCRATCH_ARRAY_LEN);
    int base_idx = 0;
    int has_cell_vars = 0;
    int k;
    char verr[REPL_DIAG_TEXT_MAX];

    if (scratch_array_idx < 0)
        return compile_set_err(err, err_size,
                               "'%s' is not a scratch array - the {...} form writes A, B, or C",
                               name);
    /* The target cell is required. A bare `A = {...}` would be the only
     * place in the language where an array name is itself assignable -
     * `A = 5;` is an error everywhere else - and it is a spelling export
     * cannot preserve, since it writes `A[0] = ...` for either form and
     * import can only fold back to the subscripted one. One spelling per
     * row, and the subscript is the one that lines up when four of these
     * stack into a 4x4. */
    if (!index_expr[0])
        return compile_set_err(err, err_size,
                               "scratch block needs a target cell - write '%s[0] = {...}'; "
                               "the array name alone is not assignable",
                               name);
    /* One cell is the scalar form spelled oddly, and it does not survive
     * a round trip as itself: it exports to `A[4] = e;`, which import
     * reads back as CMD_SCRATCH_ASSIGN. Reject it rather than ship a
     * spelling that silently rewrites itself on save/load. */
    if (cell_count < 2)
        return compile_set_err(err, err_size, "%s", k_block_usage);

    /* The base is a literal, deliberately: it is what keeps the C this
     * row exports to a plain `A[4] = ...; A[5] = ...;` run that import
     * can fold back without a temporary. A computed target is still
     * reachable one cell at a time through `A[i] = expr;`, and this
     * restriction can be relaxed later without breaking any scene -
     * the reverse could not. */
    if (!compile_parse_scratch_base_literal(index_expr, &base_idx))
        return compile_set_err(err, err_size,
                               "the {...} form needs a literal base index - "
                               "use '%s[i] = expr;' for a computed one", name);
    if (base_idx + cell_count > REPL_SCRATCH_ARRAY_LEN)
        return compile_set_err(err, err_size,
                               "%s[%d] = {...} writes %d values past cell %d - "
                               "the array holds %d",
                               name, base_idx, cell_count,
                               REPL_SCRATCH_ARRAY_LEN - 1,
                               REPL_SCRATCH_ARRAY_LEN);

    for (k = 0; k < cell_count; k++) {
        char cell[MAX_LINE_LEN];
        repl_scratch_block_cell_text(&cells[k], cell, sizeof(cell));
        if (!repl_eval_validate_expression_idents(
                &(ReplExprIdentValidationConfig){
                    .src = cell,
                    .vars = vis_n > 0 ? vis : NULL,
                    .num_vars = vis_n,
                    .predef = ctx->predef,
                    .err = verr,
                    .errsz = (int)sizeof(verr),
                }))
            return compile_set_err(err, err_size, "%s", verr);
        {
            ExprCtx cell_ctx = { cell, vis_n > 0 ? vis : NULL, vis_n, NULL, 0,
                                 ctx->predef.vars, ctx->predef.count };
            vals[k] = repl_eval_expr(&cell_ctx);
        }
        if (input_has_any_visible_vars(cell, vis_n > 0 ? vis : NULL, vis_n))
            has_cell_vars = 1;
    }

    /* Reject a block whose exported C would not fit one physical line.
     *
     * The row itself is short - it is the *expansion* that is long.
     * Export lowers `A[0] = {TAU, ...}` to `A[0] = …; A[1] = …;`, one
     * store per cell, and each `TAU` becomes a parenthesised float
     * cast several times its source width; sixteen of them reach ~440
     * characters. Import reads physical lines into a MAX_LINE_LEN
     * buffer and rejects anything longer outright ("Import failed:
     * line too long"), so such a row exports to a file the importer
     * will not take back.
     *
     * Checked here rather than at export because export has no way to
     * refuse: the alternatives there are wrapping the run (each
     * fragment ends in `;` at depth 0, so the importer's accumulator
     * flushes them as separate rows and the fold produces N blocks
     * instead of one) or exceeding the logical-statement budget, which
     * is the same MAX_LINE_LEN. Rejecting at commit is the only point
     * where the user can still do something about it - and what they
     * do is the documented idiom anyway: split the run across rows,
     * four cells at a time. */
    {
        char block_indent[REPL_INDENT_TEXT_MAX];
        int exported;

        compile_scope_cmd_indent(ctx, insert_idx,
                                 block_indent, sizeof(block_indent));
        /* Mirrors write_scratch_block_as_c89's output exactly: per cell
         * `NAME[IDX] = EXPR;` joined by one space, then the trailing
         * comment behind two. test_scratch_block_export_line_budget
         * pins the two together so a change to either is caught. */
        exported = (int)strlen(block_indent);
        for (k = 0; k < cell_count; k++) {
            char cell[MAX_LINE_LEN];
            char c_cell[MAX_LINE_LEN];
            int idx = base_idx + k;

            repl_scratch_block_cell_text(&cells[k], cell, sizeof(cell));
            repl_eval_expr_to_c(cell, c_cell, sizeof(c_cell));
            exported += (k ? 1 : 0)                 /* joining space   */
                      + (int)strlen(name)           /* A               */
                      + 1                           /* [               */
                      + (idx >= 10 ? 2 : 1)         /* index digits    */
                      + 4                           /* ] = _           */
                      + (int)strlen(c_cell)         /* the expression  */
                      + 1;                          /* ;               */
        }
        if (comment[0])
            exported += 2 + (int)strlen(comment);
        if (exported >= MAX_LINE_LEN)
            return compile_set_err(err, err_size,
                "block is too long to export - it expands to %d chars of C "
                "and the line limit is %d; split it across rows",
                exported, MAX_LINE_LEN - 1);
    }

    cmd->type = CMD_SCRATCH_BLOCK_ASSIGN;
    cmd->valid = 1;
    cmd->args[0] = (float)scratch_array_idx;
    cmd->args[1] = (float)base_idx;
    cmd->args[2] = (float)cell_count;
    cmd->num_args = 3;
    cmd->has_vars = has_cell_vars;
    for (k = 0; k < cell_count; k++)
        cmd->payload.scratch_block.v[k] = vals[k];

    for (k = 0; k < cell_count; k++) {
        out->scratch_ops[k].array_idx = scratch_array_idx;
        out->scratch_ops[k].elem_idx = base_idx + k;
        out->scratch_ops[k].value = vals[k];
    }
    out->scratch_op_count = cell_count;

    snprintf(out->commit_message, sizeof(out->commit_message),
             "%s[%d..%d] = %d values", name, base_idx,
             base_idx + cell_count - 1, cell_count);

    /* Canonicalize the separators before the shared text formatting
     * below writes `rhs` out verbatim. Import rebuilds the list with
     * ", " between cells, so a row committed as `{1,2}` would come back
     * from a save/load as `{1, 2}` and break the text-exact round trip
     * every scene corpus checks. Only the separators are touched - each
     * cell expression keeps the text the user typed, exactly as the
     * scalar `A[i] = expr;` form does. */
    {
        char norm[MAX_LINE_LEN];
        int used = 1;
        norm[0] = '{';
        for (k = 0; k < cell_count; k++) {
            char cell[MAX_LINE_LEN];
            int wrote;
            repl_scratch_block_cell_text(&cells[k], cell, sizeof(cell));
            wrote = snprintf(norm + used, sizeof(norm) - (size_t)used,
                             "%s%s", k ? ", " : "", cell);
            if (wrote < 0 || wrote >= (int)sizeof(norm) - used)
                return compile_set_err(err, err_size, "Command too long");
            used += wrote;
        }
        if (used + 2 > (int)sizeof(norm))
            return compile_set_err(err, err_size, "Command too long");
        norm[used++] = '}';
        norm[used] = '\0';
        repl_copy_string_fits(rhs, rhs_sz, norm);
    }
    return REPL_COMPILE_OK;
}

/* A[i] = expr - single-cell scratch assignment. */
static ReplCompileResult compile_var_assign_indexed_scratch(
        const char *name, const char *index_expr, const char *rhs,
        const ReplCompileContext *ctx,
        ExprVar *vis, int vis_n,
        GLCmd *cmd, ReplCompiledChange *out,
        char *err, int err_size) {
    char verr[REPL_DIAG_TEXT_MAX];
    int scratch_array_idx = repl_eval_scratch_array_index(name);
    ExprCtx idx_ctx;
    ExprCtx rhs_ctx;
    int elem_idx;
    float val;
    int has_index_vars;
    int has_rhs_vars;

    if (scratch_array_idx < 0)
        return compile_set_err(err, err_size, "unknown array '%s'", name);

    if (!repl_eval_validate_expression_idents(
            &(ReplExprIdentValidationConfig){
                .src = index_expr,
                .vars = vis_n > 0 ? vis : NULL,
                .num_vars = vis_n,
                .predef = ctx->predef,
                .err = verr,
                .errsz = (int)sizeof(verr),
            }))
        return compile_set_err(err, err_size, "%s", verr);
    if (!repl_eval_validate_expression_idents(
            &(ReplExprIdentValidationConfig){
                .src = rhs,
                .vars = vis_n > 0 ? vis : NULL,
                .num_vars = vis_n,
                .predef = ctx->predef,
                .err = verr,
                .errsz = (int)sizeof(verr),
            }))
        return compile_set_err(err, err_size, "%s", verr);

    idx_ctx = (ExprCtx){ index_expr, vis_n > 0 ? vis : NULL, vis_n, NULL, 0,
                         ctx->predef.vars, ctx->predef.count };
    rhs_ctx = (ExprCtx){ rhs, vis_n > 0 ? vis : NULL, vis_n, NULL, 0,
                         ctx->predef.vars, ctx->predef.count };
    elem_idx = (int)repl_eval_expr(&idx_ctx);
    if (elem_idx < 0 || elem_idx >= REPL_SCRATCH_ARRAY_LEN)
        return compile_set_err(err, err_size,
                               "scratch array index out of range: %d", elem_idx);

    val = repl_eval_expr(&rhs_ctx);
    has_index_vars = input_has_any_visible_vars(index_expr,
                                                vis_n > 0 ? vis : NULL, vis_n);
    has_rhs_vars = input_has_any_visible_vars(rhs,
                                              vis_n > 0 ? vis : NULL, vis_n);

    cmd->type = CMD_SCRATCH_ASSIGN;
    cmd->valid = 1;
    cmd->args[0] = (float)scratch_array_idx;
    cmd->args[1] = (float)elem_idx;
    cmd->args[2] = val;
    cmd->num_args = 3;
    cmd->has_vars = has_index_vars || has_rhs_vars;

    out->scratch_ops[0].array_idx = scratch_array_idx;
    out->scratch_ops[0].elem_idx = elem_idx;
    out->scratch_ops[0].value = val;
    out->scratch_op_count = 1;

    snprintf(out->commit_message, sizeof(out->commit_message),
             "%s[%d] = %g", name, elem_idx, (double)val);
    return REPL_COMPILE_OK;
}

/* name = expr - predef or function-scoped local assignment. */
static ReplCompileResult compile_var_assign_scalar(
        const char *name, const char *rhs,
        const ReplCompileContext *ctx,
        const CompileScopeBindings *bind,
        ExprVar *vis, int vis_n,
        GLCmd *cmd, ReplCompiledChange *out,
        char *err, int err_size) {
    char verr[REPL_DIAG_TEXT_MAX];
    /* Lexical target resolution. The innermost binding decides, and a
     * matching PARAM/LOOP is never skipped in search of an outer
     * LOCAL - a shadowed outer binding is simply not reachable from
     * here, exactly as in C. Only when nothing scoped matches does
     * the target fall through to a predef slot. */
    int bind_slot = compile_bindings_find(bind, name);
    int var_idx;
    ExprCtx eval_ctx;
    float val;
    int has_rhs_vars;

    if (bind_slot >= 0 && bind->kinds[bind_slot] == REPL_VISIBLE_VAR_PARAM)
        return compile_set_err(err, err_size,
                               "cannot assign to function parameter '%s' - function parameters are constant",
                               name);
    if (bind_slot >= 0 && bind->kinds[bind_slot] == REPL_VISIBLE_VAR_LOOP)
        return compile_set_err(err, err_size,
                               "cannot assign to loop variable '%s' - loop variables are constant",
                               name);

    if (bind_slot >= 0) {
        var_idx = REPL_VAR_IDX_LOCAL;
    } else {
        var_idx = repl_eval_find_predef_var_idx_in(ctx->predef.vars,
                                                   ctx->predef.count, name);
        if (var_idx < 0) {
            if (compile_name_unavailable_err(err, err_size, name))
                return REPL_COMPILE_ERROR;
            return compile_set_err(err, err_size,
                "undeclared variable '%s' - use 'float %s;' first", name, name);
        }
    }

    if (!repl_eval_validate_expression_idents(
            &(ReplExprIdentValidationConfig){
                .src = rhs,
                .vars = vis_n > 0 ? vis : NULL,
                .num_vars = vis_n,
                .predef = ctx->predef,
                .err = verr,
                .errsz = (int)sizeof(verr),
            }))
        return compile_set_err(err, err_size, "%s", verr);

    eval_ctx = (ExprCtx){ rhs, vis_n > 0 ? vis : NULL, vis_n, NULL, 0,
                          ctx->predef.vars, ctx->predef.count };
    val = repl_eval_expr(&eval_ctx);
    has_rhs_vars = input_has_any_visible_vars(rhs,
                                              vis_n > 0 ? vis : NULL, vis_n);

    cmd->type     = CMD_VAR_ASSIGN;
    cmd->valid    = 1;
    cmd->args[0]  = val;
    cmd->num_args = 1;       /* args[0] holds the assigned value */
    cmd->var_idx  = var_idx; /* predef slot the executor will write, or
                              * REPL_VAR_IDX_LOCAL for a scoped target */
    cmd->has_vars = has_rhs_vars;

    /* A local has no slot to set: its value exists only inside a call
     * frame, so flatten writes it and the executor skips the row. */
    if (var_idx >= 0 && out->predef_op_count < MAX_PREDEF_OPS_PER_COMMIT) {
        out->predef_ops[out->predef_op_count].kind = REPL_PREDEF_OP_SET_VALUE;
        repl_copy_string_fits(out->predef_ops[out->predef_op_count].name,
                              sizeof(out->predef_ops[out->predef_op_count].name), name);
        out->predef_ops[out->predef_op_count].value = val;
        out->predef_ops[out->predef_op_count].has_value = 1;
        out->predef_op_count++;
    }

    snprintf(out->commit_message, sizeof(out->commit_message),
             "%s = %g", name, (double)val);
    return REPL_COMPILE_OK;
}

/* CONTRACT: context-pure for document data. The
 * ReplCompileContext snapshot is authoritative for document_cmds /
 * _count / edit_line, and visible-var collection now runs through
 * collect_visible_vars_in over that same context view (no live
 * g_repl_state read). Callers still build a fresh context per statement
 * - repl_compile_context_from_live snapshots the live document - so a
 * multi-statement commit observes each prior change via its next
 * context. Reached from both the editor commit path and the file-load
 * path via load_try_block. */
ReplCompileResult repl_compile_var_assign(const char *input,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);

    char name[REPL_PREDEF_NAME_MAX];
    char index_expr[MAX_LINE_LEN];
    char rhs[MAX_LINE_LEN];
    char comment[MAX_LINE_LEN];
    ReplCompileResult arm_rc;

    /* Bail out before extraction when the input itself can't fit in a
     * source line. Otherwise repl_extract_assignment_target_parts would
     * silently truncate rhs into the MAX_LINE_LEN-sized buffer, which
     * can land mid-token (e.g. inside `+(0)`) and trip the validator
     * with a spurious "missing ')'" before the format-fits step gets
     * to emit the real diagnostic. */
    if (input && (int)strlen(input) >= MAX_LINE_LEN)
        return compile_set_err(err, err_size, "Command too long");

    if (!repl_extract_assignment_target_parts(input ? input : "",
                                              name, sizeof(name),
                                              index_expr, sizeof(index_expr),
                                              rhs, sizeof(rhs))) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    /* The extractor keeps everything up to the trailing `;` as the rhs, so
     * `A[0] = 0.9; B[0] = 0.2;` would compile as a single assignment whose
     * expression evaluates to 0.9 and whose remaining statements are
     * silently dropped - while export writes the source line verbatim and
     * the C runs all of them. Reject the form instead, the way
     * check_trailing_garbage does for `glFoo(...)` lines. */
    if (strchr(rhs, ';'))
        return compile_set_err(err, err_size,
                               "unexpected text after ';' - one statement per line");

    comment[0] = '\0';
    {
        const char *cp = strstr(input ? input : "", "//");
        if (cp) {
            while (*cp && isspace((unsigned char)*cp)) cp++;
            if (cp[0] == '/' && cp[1] == '/')
                snprintf(comment, sizeof(comment), " %s", cp);
        }
    }

    int insert_idx = compile_insert_pos(ctx);

    CompileScopeBindings bind;
    compile_collect_bindings(ctx, insert_idx, &bind);
    ExprVar *vis = bind.vars;
    int vis_n = bind.count;
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));

    if (compile_rhs_is_brace_list(rhs)) {
        arm_rc = compile_var_assign_block_scratch(
            name, index_expr, rhs, sizeof(rhs), comment, insert_idx,
            ctx, vis, vis_n, &cmd, out, err, err_size);
    } else if (index_expr[0]) {
        arm_rc = compile_var_assign_indexed_scratch(
            name, index_expr, rhs, ctx, vis, vis_n,
            &cmd, out, err, err_size);
    } else {
        arm_rc = compile_var_assign_scalar(
            name, rhs, ctx, &bind, vis, vis_n,
            &cmd, out, err, err_size);
    }
    if (arm_rc != REPL_COMPILE_OK)
        return arm_rc;

    /* Format text using the scope indent at the insert position. */
    char indent[REPL_INDENT_TEXT_MAX];
    compile_scope_cmd_indent(ctx, insert_idx, indent, sizeof(indent));
    char assign_text[MAX_LINE_LEN];
    if (index_expr[0]) {
        if (!repl_format_fits(assign_text, sizeof(assign_text),
                              "%s%s[%s] = %s;%s",
                              indent, name, index_expr, rhs, comment))
            return compile_set_err(err, err_size, "Command too long");
    } else if (!repl_format_fits(assign_text, sizeof(assign_text),
                                 "%s%s = %s;%s", indent, name, rhs, comment)) {
        return compile_set_err(err, err_size, "Command too long");
    }

    /* Decide insert / replace. The REPLACE_ONE branch absorbs the
     * legacy var-decl overwrite cascade: when the assignment lands on
     * a CMD_VAR_DECLARE in non-insert mode, validate that the dropped
     * names are not in use elsewhere and emit UNDECLARE predef ops so
     * apply replays the slot shift through repl_apply_predef_ops. */
    int overwriting_decl = 0;
    const GLCmd *old_decl = NULL;
    if (ctx->insert_mode) {
        out->kind  = REPL_COMPILED_INSERT_ONE;
        out->pos   = insert_idx;
        out->count = 1;
        out->adjust_edit_line = 0;
    } else if (insert_idx < ctx->document_count) {
        out->kind  = REPL_COMPILED_REPLACE_ONE;
        out->pos   = insert_idx;
        out->count = 1;
        out->adjust_edit_line = 0;
        if (ctx->document_cmds[insert_idx].type == CMD_VAR_DECLARE) {
            overwriting_decl = 1;
            old_decl = &ctx->document_cmds[insert_idx];
        }
    } else {
        out->kind  = REPL_COMPILED_INSERT_ONE;
        out->pos   = ctx->document_count;
        out->count = 1;
        out->adjust_edit_line = 0;
    }
    repl_copy_string_fits(out->text[0], sizeof(out->text[0]), assign_text);

    /* Overwrite-feasibility check + UNDECLARE operations. Mirrors the
     * float-decl overwrite check; the in-use predicate skips the line
     * being replaced. SET_VALUE and UNDECLARE ordering is irrelevant:
     * repl_apply_predef_ops runs independent passes per op kind. When
     * queued undeclares remove lower predef slots, rebase the staged
     * CMD_VAR_ASSIGN slot to the post-undeclare index before publish. */
    int op_count = out->predef_op_count;
    int old_decl_is_local = overwriting_decl &&
                            old_decl->var_idx == REPL_VAR_IDX_LOCAL;
    if (overwriting_decl) {
        /* Every declared name goes away here, so nothing is exempt. */
        if (!repl_compile_decl_replacement_allowed(ctx, insert_idx, NULL, 0,
                                                   err, err_size))
            return REPL_COMPILE_ERROR;
        /* A local has no predef slot to release. */
        for (int decl_idx = 0;
             !old_decl_is_local && decl_idx < old_decl->payload.decl.count;
             decl_idx++) {
            if (op_count >= MAX_PREDEF_OPS_PER_COMMIT) break;
            out->predef_ops[op_count].kind = REPL_PREDEF_OP_UNDECLARE;
            repl_copy_string_fits(out->predef_ops[op_count].name,
                                  sizeof(out->predef_ops[op_count].name),
                                  old_decl->payload.decl.names[decl_idx]);
            op_count++;
        }
    }

    /* Rebase exists to fix up predef slot indices after undeclares, so it
     * runs only when *both* sides are global. A local-target assignment
     * carries REPL_VAR_IDX_LOCAL (-1), which the helper reads as failure -
     * ungated, it would reject the legitimate "overwrite an unused local
     * decl with an assignment to another local" edit. */
    out->predef_op_count = op_count;
    if (cmd.type == CMD_VAR_ASSIGN && overwriting_decl &&
        cmd.var_idx >= 0 && !old_decl_is_local) {
        int rebased_slot = compile_rebase_var_assign_slot_after_undeclares(
            name, cmd.var_idx, out, ctx->predef);
        if (rebased_slot < 0)
            return compile_set_err(err, err_size,
                                   "cannot overwrite declaration of '%s' with assignment",
                                   name);
        cmd.var_idx = rebased_slot;
    }
    out->cmds[0] = cmd;

    return REPL_COMPILE_OK;
}

/* Shared kernel behind the three predef-value compile entry points.
 *
 * `emit_predef_op` stages the live REPL_PREDEF_OP_SET_VALUE side effect;
 * `emit_source_rewrite` stages the REPL_COMPILED_REPLACE_ONE that rewrites the
 * declaration's initializer. The combined entry sets both; the variable-panel
 * drag uses one during motion and the other on release. Both paths use the
 * same lookup and rewrite, so the emitted declaration text cannot drift
 * between them. */
static ReplCompileResult compile_predef_value_change(const char *name,
                                                     float value,
                                                     const ReplCompileContext *ctx,
                                                     int emit_predef_op,
                                                     int emit_source_rewrite,
                                                     ReplCompiledChange *out,
                                                     char *err, int err_size) {
    int var_idx;
    int decl_idx;

    if (!ctx || !out || !name || !name[0])
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);
    if (err && err_size > 0)
        err[0] = '\0';

    var_idx = repl_eval_find_predef_var_idx_in(ctx->predef.vars,
                                               ctx->predef.count, name);
    if (var_idx < 0) {
        if (compile_name_unavailable_err(err, err_size, name))
            return REPL_COMPILE_ERROR;
        return compile_set_err(err, err_size,
                               "undeclared variable '%s'", name);
    }

    snprintf(out->commit_message, sizeof(out->commit_message),
             "%s = %g", name, (double)value);

    if (emit_predef_op) {
        out->predef_ops[0].kind = REPL_PREDEF_OP_SET_VALUE;
        repl_copy_string_fits(out->predef_ops[0].name,
                              sizeof(out->predef_ops[0].name), name);
        out->predef_ops[0].value = value;
        out->predef_ops[0].has_value = 1;
        out->predef_op_count = 1;
    }

    if (!emit_source_rewrite)
        return REPL_COMPILE_OK;

    decl_idx = compile_find_var_decl(ctx, name);
    if (decl_idx >= 0) {
        char rewritten[MAX_LINE_LEN];
        if (compile_rewrite_decl_initializer_text(
                source_text_line(ctx->text, decl_idx),
                name, value, rewritten, sizeof(rewritten))) {
            out->kind = REPL_COMPILED_REPLACE_ONE;
            out->pos = decl_idx;
            out->count = 1;
            out->adjust_edit_line = 0;
            out->cmds[0] = ctx->document_cmds[decl_idx];
            repl_copy_string_fits(out->text[0], sizeof(out->text[0]), rewritten);
        }
    }

    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_set_predef_value(const char *name,
                                                float value,
                                                const ReplCompileContext *ctx,
                                                ReplCompiledChange *out,
                                                char *err, int err_size) {
    return compile_predef_value_change(name, value, ctx,
                                       /*emit_predef_op=*/1,
                                       /*emit_source_rewrite=*/1,
                                       out, err, err_size);
}

ReplCompileResult repl_compile_set_predef_value_live(const char *name,
                                                     float value,
                                                     const ReplCompileContext *ctx,
                                                     ReplCompiledChange *out,
                                                     char *err, int err_size) {
    return compile_predef_value_change(name, value, ctx,
                                       /*emit_predef_op=*/1,
                                       /*emit_source_rewrite=*/0,
                                       out, err, err_size);
}

ReplCompileResult repl_compile_persist_predef_value(const char *name,
                                                    float value,
                                                    const ReplCompileContext *ctx,
                                                    ReplCompiledChange *out,
                                                    char *err, int err_size) {
    return compile_predef_value_change(name, value, ctx,
                                       /*emit_predef_op=*/0,
                                       /*emit_source_rewrite=*/1,
                                       out, err, err_size);
}

ReplCompileResult repl_compile_empty_line(int line_idx,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);
    if (err && err_size > 0)
        err[0] = '\0';

    if (line_idx < 0)
        line_idx = 0;
    if (line_idx > ctx->document_count)
        line_idx = ctx->document_count;

    memset(&out->cmds[0], 0, sizeof(out->cmds[0]));
    out->cmds[0].type = CMD_EMPTY;
    out->cmds[0].valid = 1;
    out->text[0][0] = '\0';

    out->kind = REPL_COMPILED_INSERT_ONE;
    out->pos = line_idx;
    out->count = 1;
    out->adjust_edit_line = 0;
    snprintf(out->commit_message, sizeof(out->commit_message),
             "Inserted blank line");
    return REPL_COMPILE_OK;
}

/* Walk [range_start, range_end) for CMD_VAR_DECLARE rows. For each
 * declared name:
 *   1. Verify no line outside the range still references it. Comments are
 *      ignored, and same-name locals (function params / for-loop vars)
 *      shadow the global on their header and body lines.
 *   2. Append a REPL_PREDEF_OP_UNDECLARE op to `out->predef_ops`.
 *
 * Returns REPL_COMPILE_OK on success (predef_op_count incremented).
 * Returns REPL_COMPILE_ERROR with `err` filled on still-referenced
 * name (`"Cannot %s '%s': still referenced"`, %s = action_verb) or
 * predef-op cap overflow. */
static ReplCompileResult compile_collect_undeclare_for_range(
        const ReplCompileContext *ctx,
        int range_start, int range_end,
        const char *action_verb,
        ReplCompiledChange *out, char *err, int err_size) {
    /* Reference scan. Storage-aware in both directions: a local's reads
     * are invisible to the global scan (it answers "does this line read
     * the *global* of that name"), and asking the local scan about a
     * global would be equally wrong. */
    for (int i = range_start; i < range_end; i++) {
        const GLCmd *cmd = &ctx->document_cmds[i];
        if (cmd->type != CMD_VAR_DECLARE) continue;
        for (int d = 0; d < cmd->payload.decl.count; d++) {
            const char *nm = cmd->payload.decl.names[d];
            /* The shared predicate, not the shared error emitter: this
             * route's diagnostic carries an action verb ("delete" /
             * "comment out") the replacement routes have no equivalent
             * for. */
            if (compile_decl_name_is_referenced(ctx, i, nm,
                                                range_start, range_end))
                return compile_set_err(err, err_size,
                                       "Cannot %s '%s': still referenced",
                                       action_verb, nm);
        }
    }

    /* Append UNDECLARE ops for every declared name in the range. Locals
     * are skipped: there is no predef slot behind them, and undeclaring
     * by name would release a same-named global instead. */
    for (int i = range_start; i < range_end; i++) {
        const GLCmd *cmd = &ctx->document_cmds[i];
        if (cmd->type != CMD_VAR_DECLARE) continue;
        if (cmd->var_idx == REPL_VAR_IDX_LOCAL) continue;
        for (int d = 0; d < cmd->payload.decl.count; d++) {
            if (out->predef_op_count >= MAX_PREDEF_OPS_PER_COMMIT)
                return compile_set_err(err, err_size,
                                       "Too many declarations in range");
            ReplPredefOp *op = &out->predef_ops[out->predef_op_count++];
            op->kind = REPL_PREDEF_OP_UNDECLARE;
            repl_copy_string_fits(op->name, sizeof(op->name),
                                  cmd->payload.decl.names[d]);
        }
    }
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_delete_range(int start, int count,
                                            const ReplCompileContext *ctx,
                                            ReplCompiledChange *out,
                                            char *err, int err_size) {
    int n;
    int end;

    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);
    if (err && err_size > 0)
        err[0] = '\0';

    n = ctx->document_count;
    if (start < 0)
        start = 0;
    if (count <= 0 || start >= n)
        return REPL_COMPILE_OK;
    if (start + count > n)
        count = n - start;
    end = start + count;

    /* Reference check + UNDECLARE op collection. Shared with the
     * comment-toggle paths; comments are not real uses, so the
     * scanner skips CMD_COMMENT lines. */
    if (compile_collect_undeclare_for_range(ctx, start, end, "remove",
                                             out, err, err_size)
            != REPL_COMPILE_OK)
        return REPL_COMPILE_ERROR;

    out->kind = REPL_COMPILED_DELETE_RANGE;
    out->pos = start;
    out->count = count;
    snprintf(out->commit_message, sizeof(out->commit_message),
             "Removed %d line%s", count, count > 1 ? "s" : "");

    return REPL_COMPILE_OK;
}

/* Build "<leading_ws><prefix><rest_of_line>" into `dst`. Returns the
 * number of bytes written (excluding the NUL). Truncates safely if
 * the source overflows `cap`. */
static int compile_prepend_prefix(const char *orig, const char *prefix,
                                  char *dst, int cap) {
    int off = 0;
    int ws = 0;
    int prefix_len;

    if (cap <= 0) return 0;
    if (!orig) orig = "";
    if (!prefix) prefix = "";
    prefix_len = (int)strlen(prefix);

    while (orig[ws] && isspace((unsigned char)orig[ws]))
        ws++;
    for (int k = 0; k < ws && off < cap - 1; k++)
        dst[off++] = orig[k];
    for (int k = 0; k < prefix_len && off < cap - 1; k++)
        dst[off++] = prefix[k];
    for (int k = ws; orig[k] && off < cap - 1; k++)
        dst[off++] = orig[k];
    dst[off] = '\0';
    return off;
}

/* Strip the configured prefix from `orig`. Returns 1 if the line
 * begins with `prefix` after leading whitespace and the stripped
 * result was written to `dst`; 0 otherwise (line doesn't carry the
 * configured prefix). */
static int compile_strip_prefix(const char *orig, const char *prefix,
                                char *dst, int cap) {
    int ws = 0;
    int prefix_len;
    int off = 0;

    if (cap <= 0) return 0;
    if (!orig) orig = "";
    if (!prefix || !prefix[0]) return 0;
    prefix_len = (int)strlen(prefix);

    while (orig[ws] && isspace((unsigned char)orig[ws]))
        ws++;
    if (strncmp(orig + ws, prefix, (size_t)prefix_len) != 0)
        return 0;

    for (int k = 0; k < ws && off < cap - 1; k++)
        dst[off++] = orig[k];
    for (int k = ws + prefix_len; orig[k] && off < cap - 1; k++)
        dst[off++] = orig[k];
    dst[off] = '\0';
    return 1;
}

/* Scan backward from `end_idx` (a CMD_FOR_END / CMD_FUNC_END /
 * CMD_IF_END row) to find the matching block-head row. Returns the
 * head index or -1 if unmatched. */
static int compile_find_block_head(const ReplCompileContext *ctx,
                                   int end_idx) {
    int depth = 1;
    for (int j = end_idx - 1; j >= 0; j--) {
        CmdType t = ctx->document_cmds[j].type;
        if (repl_cmd_is_block_end(t)) {
            depth++;
        } else if (repl_cmd_is_block_head(t)) {
            depth--;
            if (depth == 0) return j;
        }
    }
    return -1;
}

int repl_compile_comment_prefix_strip(const char *line, const char *prefix,
                                      char *dst, int cap) {
    return compile_strip_prefix(line, prefix, dst, cap);
}

int repl_compile_block_extent_at(const ReplCompileContext *ctx, int line_idx,
                                 int *out_first, int *out_last) {
    CmdType type;
    int head;
    int end;

    if (!ctx || line_idx < 0 || line_idx >= ctx->document_count)
        return 0;

    type = ctx->document_cmds[line_idx].type;
    if (repl_cmd_is_block_head(type) ||
        repl_cmd_is_if_branch_separator(type)) {
        int count = 0;
        if (!repl_source_scope_view_block_extent(compile_source_scope(ctx),
                                                 line_idx, &head, &count))
            return 0;
        end = head + count - 1;
    } else if (repl_cmd_is_block_end(type)) {
        end = line_idx;
        head = compile_find_block_head(ctx, line_idx);
        if (head < 0)
            return 0;
    } else {
        return 0;
    }

    if (end < head)
        return 0;
    if (out_first) *out_first = head;
    if (out_last)  *out_last  = end;
    return 1;
}

/* Depth walk over the document's own command kinds. A range that opens
 * more blocks than it closes (or closes one it never opened) cannot come
 * back through the uncomment path, so it is refused going out - see the
 * header note on why the toggle is only offered for legal code. */
static ReplCompileResult compile_check_range_balanced(
        const ReplCompileContext *ctx, int first, int last,
        char *err, int err_size) {
    int depth = 0;

    for (int i = first; i <= last; i++) {
        CmdType t = ctx->document_cmds[i].type;

        if (repl_cmd_is_block_end(t)) {
            if (--depth < 0)
                return compile_set_err(err, err_size,
                                       "Line %d closes a block that starts "
                                       "outside the selection", i + 1);
        } else if (repl_cmd_is_if_branch_separator(t)) {
            /* Separators are neither head nor end, but they only make
             * sense inside their own if-chain. */
            if (depth <= 0)
                return compile_set_err(err, err_size,
                                       "Line %d continues an if that starts "
                                       "outside the selection", i + 1);
        } else if (repl_cmd_is_block_head(t)) {
            depth++;
        }
    }
    if (depth != 0)
        return compile_set_err(err, err_size,
                               "Selection opens a block it does not close");
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_comment_range(int first, int last,
                                             const char *prefix,
                                             const ReplCompileContext *ctx,
                                             ReplCompiledChange *out,
                                             char *err, int err_size) {
    int n;

    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);
    if (err && err_size > 0)
        err[0] = '\0';

    if (!prefix || !prefix[0])
        return REPL_COMPILE_OK;
    if (first < 0 || last >= ctx->document_count || last < first)
        return REPL_COMPILE_OK;

    n = last - first + 1;
    if (n > MAX_COMMIT_CMDS)
        return compile_set_err(err, err_size,
                               "Block too large to toggle (max %d lines)",
                               MAX_COMMIT_CMDS);

    if (compile_check_range_balanced(ctx, first, last, err, err_size)
            != REPL_COMPILE_OK)
        return REPL_COMPILE_ERROR;

    /* Decl-reference check + UNDECLARE op collection for any
     * CMD_VAR_DECLARE rows inside the range. Commenting a decl
     * out is symmetric to deleting it: references must be
     * removed first, and apply must undeclare the variable so
     * the runtime variable table matches the source. */
    if (compile_collect_undeclare_for_range(ctx, first, last + 1,
                                             "comment",
                                             out, err, err_size)
            != REPL_COMPILE_OK)
        return REPL_COMPILE_ERROR;

    for (int i = 0; i < n; i++) {
        const char *orig = source_text_line(ctx->text, first + i);
        compile_prepend_prefix(orig, prefix,
                               out->text[i], (int)sizeof(out->text[i]));
        memset(&out->cmds[i], 0, sizeof(out->cmds[i]));
        out->cmds[i].type = CMD_COMMENT;
        out->cmds[i].valid = 1;
    }

    out->adjust_edit_line = 0;
    if (n == 1) {
        out->kind = REPL_COMPILED_REPLACE_ONE;
        out->pos = first;
        out->count = 1;
    } else {
        out->kind = REPL_COMPILED_INSERT_MANY;
        out->pos = first;
        out->count = n;
        out->delete_pos = first;
        out->delete_count = n;
    }
    snprintf(out->commit_message, sizeof(out->commit_message),
             "Commented out %d line%s", n, n > 1 ? "s" : "");
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_uncomment_line(int line_idx,
                                              const char *prefix,
                                              const ReplCompileContext *ctx,
                                              ReplCompiledChange *out,
                                              char *err, int err_size) {
    const char *orig;
    char stripped[MAX_LINE_LEN];
    ReplCompileResult r;

    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);
    if (err && err_size > 0)
        err[0] = '\0';

    if (!prefix || !prefix[0])
        return REPL_COMPILE_OK;
    if (line_idx < 0 || line_idx >= ctx->document_count)
        return REPL_COMPILE_OK;
    if (ctx->document_cmds[line_idx].type != CMD_COMMENT)
        return compile_set_err(err, err_size, "Line is not a comment");

    orig = source_text_line(ctx->text, line_idx);
    if (!compile_strip_prefix(orig, prefix, stripped, sizeof(stripped)))
        return compile_set_err(err, err_size,
                               "Line not commented with the configured prefix");

    /* A blank line inside a commented-out block strips back to nothing.
     * No handler in the chain claims empty input, so name it here -
     * without this arm a single blank row would fail the whole
     * uncomment of the block it sits in. */
    {
        const char *scan = stripped;
        while (*scan && isspace((unsigned char)*scan)) scan++;
        if (!*scan) {
            memset(&out->cmds[0], 0, sizeof(out->cmds[0]));
            out->cmds[0].type = CMD_EMPTY;
            out->cmds[0].valid = 1;
            out->text[0][0] = '\0';
            out->kind = REPL_COMPILED_REPLACE_ONE;
            out->pos = line_idx;
            out->count = 1;
            out->adjust_edit_line = 0;
            snprintf(out->commit_message, sizeof(out->commit_message),
                     "Uncommented 1 line");
            return REPL_COMPILE_OK;
        }
    }

    /* Run the float-decl + var-assign dispatch chain. */
    r = repl_compile_dispatch(stripped, ctx, out, err, err_size);
    if (r != REPL_COMPILE_OK)
        return r;

    if (out->kind == REPL_COMPILED_NO_CHANGE) {
        /* Dispatch didn't recognize. Go through the normal commit
         * pipeline so visible-variable references in the stripped
         * text are preserved (otherwise `// glVertex3f(t, 0, 0)`
         * round-trips back as `glVertex3f(0.0000, 0, 0)` - the
         * parser's canonical text emits args from cmd->args[]
         * regardless of has_vars). The visible set is collected at
         * this row's own position: in a range uncomment each row sits
         * in a different scope. */
        ExprVar vis[MAX_EXPR_VARS];
        int vis_n = collect_visible_vars_in(ctx->text, ctx->document_cmds,
                                            ctx->document_count, line_idx,
                                            vis, MAX_EXPR_VARS, NULL, NULL);
        int preserve_expr = input_has_any_visible_vars(stripped,
                                                       vis_n > 0 ? vis : NULL,
                                                       vis_n);
        GLCmd parsed_cmd;
        char parsed_text[MAX_LINE_LEN];
        memset(&parsed_cmd, 0, sizeof(parsed_cmd));
        parsed_text[0] = '\0';
        if (!repl_parse_and_normalize_strict_with_scope(
                stripped, line_idx,
                vis_n > 0 ? vis : NULL, vis_n,
                preserve_expr, &parsed_cmd,
                parsed_text, sizeof(parsed_text),
                compile_source_scope(ctx),
                ctx->func_aliases))
            return compile_set_err(err, err_size,
                                   "Cannot uncomment: not a valid command");
        repl_compiled_change_init(out);
        out->cmds[0] = parsed_cmd;
        repl_copy_string_fits(out->text[0], sizeof(out->text[0]), parsed_text);
    }

    /* Coerce dispatch / parser result to REPLACE_ONE at line_idx. The
     * position matters as much as the count: a bare `float x;` insert
     * hoists to the top of its scope, and an uncomment has to put the
     * row back where the comment was. */
    if (out->kind == REPL_COMPILED_INSERT_MANY ||
        out->kind == REPL_COMPILED_DELETE_RANGE)
        return compile_set_err(err, err_size,
                               "Cannot uncomment into multi-line construct");
    out->kind = REPL_COMPILED_REPLACE_ONE;
    out->pos = line_idx;
    out->count = 1;
    out->adjust_edit_line = 0;
    out->delete_pos = -1;
    out->delete_count = 0;
    snprintf(out->commit_message, sizeof(out->commit_message),
             "Uncommented 1 line");
    return REPL_COMPILE_OK;
}

/* ===== Pure structured-block validators =====
 *
 * These are the REPL-pipeline-side counterparts to the editor's
 * editor_compile_close_brace / _if_block / _func_def / _for_loop in
 * src/editor/commit.c. The editor versions handle edit-time semantics
 * (cursor target, insert mode, header-replace branch, oneliner body,
 * matched-existing-end close-brace). These pure versions cover only
 * the line-by-line load case: a single line of input produces one appended
 * CMD_* command.
 *
 * For the lean loader, ctx->edit_line == ctx->document_count and
 * ctx->insert_mode == 0; the new command lands at the end of the
 * document. The editor's edit-time branches don't arise here.
 *
 * Some parsing logic duplicates the editor versions because the editor path
 * also carries cursor and input-buffer effects.
 */

/* Compute the dedented (one-block-out) indent for a close-brace.
 * Mirror of close_brace_indent in src/editor/commit.c. */
static void compile_close_brace_indent(const ReplCompileContext *ctx,
                                       int pos, char *buf, int buf_sz) {
    compile_scope_cmd_indent(ctx, pos, buf, buf_sz);
    int len = (int)strlen(buf);
    if (len >= 2)
        len -= 2;
    else
        len = 0;
    if (len > buf_sz - 1)
        len = buf_sz - 1;
    memset(buf, ' ', (size_t)len);
    buf[len] = '\0';
}

static void compile_if_branch_indent(const ReplCompileContext *ctx,
                                     int pos, char *buf, int buf_sz) {
    compile_scope_cmd_indent(ctx, pos, buf, buf_sz);
    if (ctx && !ctx->insert_mode &&
        pos >= 0 && pos < ctx->document_count &&
        repl_cmd_is_if_branch_separator(ctx->document_cmds[pos].type))
        return;

    int len = (int)strlen(buf);
    if (len >= 2)
        len -= 2;
    else
        len = 0;
    if (len > buf_sz - 1)
        len = buf_sz - 1;
    memset(buf, ' ', (size_t)len);
    buf[len] = '\0';
}

static int compile_token_boundary(char ch) {
    return !(isalnum((unsigned char)ch) || ch == '_');
}

static ReplCompileResult compile_parse_if_branch_header(
        const char *input, CmdType *out_type,
        char *cond_text, int cond_sz,
        char *err, int err_size) {
    const char *p = input ? input : "";

    if (cond_text && cond_sz > 0)
        cond_text[0] = '\0';
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '}')
        return REPL_COMPILE_OK;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "else", 4) != 0 || !compile_token_boundary(p[4]))
        return REPL_COMPILE_OK;
    p += 4;
    while (*p && isspace((unsigned char)*p))
        p++;

    if (strncmp(p, "if", 2) == 0 && compile_token_boundary(p[2])) {
        const char *expr_start;
        int paren = 1;
        int clen;

        p += 2;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p != '(')
            return compile_set_err(err, err_size,
                                   "else-if syntax: } else if(expr) {");
        p++;
        expr_start = p;
        while (*p && paren > 0) {
            if (*p == '(')
                paren++;
            else if (*p == ')')
                paren--;
            if (paren > 0)
                p++;
        }
        if (paren != 0)
            return compile_set_err(err, err_size,
                                   "else-if syntax: } else if(expr) {");
        clen = (int)(p - expr_start);
        if (clen <= 0)
            return compile_set_err(err, err_size,
                                   "else-if needs a condition");
        if (cond_text && cond_sz > 0) {
            if (clen > cond_sz - 1)
                clen = cond_sz - 1;
            memcpy(cond_text, expr_start, (size_t)clen);
            cond_text[clen] = '\0';
            trim_in_place(cond_text);
        }

        p++;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p != '{' && *p != '\0')
            return compile_set_err(err, err_size,
                                   "else-if syntax: } else if(expr) {");
        if (out_type)
            *out_type = CMD_ELSE_IF;
        return REPL_COMPILE_OK;
    }

    if (*p == '{' || *p == '\0') {
        if (out_type)
            *out_type = CMD_ELSE;
        return REPL_COMPILE_OK;
    }

    return compile_set_err(err, err_size, "else syntax: } else {");
}

static int compile_if_branch_has_separator_in_range(
        const ReplCompileContext *ctx, int start, int end, CmdType target_type) {
    int depth = 0;

    if (!ctx || !ctx->document_cmds)
        return 0;
    if (start < 0)
        start = 0;
    if (end > ctx->document_count)
        end = ctx->document_count;
    for (int j = start; j < end; j++) {
        CmdType t = ctx->document_cmds[j].type;
        if (depth == 0 && repl_cmd_is_if_branch_separator(t) &&
            (target_type == CMD_TYPE_COUNT || t == target_type))
            return 1;
        if (repl_cmd_is_block_head(t))
            depth++;
        else if (repl_cmd_is_block_end(t)) {
            if (depth > 0)
                depth--;
            else
                break;
        }
    }
    return 0;
}

static ReplCompileResult compile_validate_if_branch_position(
        const ReplCompileContext *ctx, int pos, CmdType branch_type,
        int *out_if_head, char *err, int err_size) {
    CmdType open_type = CMD_TYPE_COUNT;
    int if_head = compile_nearest_open_block_head_at(ctx, pos, &open_type);
    int if_end;
    int scan_after_pos;

    if (if_head < 0 || open_type != CMD_IF_BEGIN)
        return compile_set_err(err, err_size,
                               "else/else-if must be inside an if-block");

    if_end = compile_scope_find_block_end(ctx, if_head);
    if (if_end < ctx->document_count && pos > if_end)
        return compile_set_err(err, err_size, "Unmatched if-block");

    if (compile_if_branch_has_separator_in_range(ctx, if_head + 1, pos,
                                                 CMD_ELSE))
        return compile_set_err(err, err_size,
                               "else-if cannot follow else");

    scan_after_pos = pos;
    if (!ctx->insert_mode && pos < ctx->document_count &&
        repl_cmd_is_if_branch_separator(ctx->document_cmds[pos].type))
        scan_after_pos = pos + 1;

    if (branch_type == CMD_ELSE &&
        compile_if_branch_has_separator_in_range(ctx, scan_after_pos, if_end,
                                                 CMD_TYPE_COUNT))
        return compile_set_err(err, err_size,
                               "else must be the final if branch");

    if (branch_type == CMD_ELSE &&
        compile_if_branch_has_separator_in_range(ctx, if_head + 1, pos,
                                                 CMD_ELSE))
        return compile_set_err(err, err_size,
                               "duplicate else branch");

    if (out_if_head)
        *out_if_head = if_head;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_if_branch_kernel(const char *input,
                                                const ReplCompileContext *ctx,
                                                ReplIfBranchKernel *out,
                                                char *err, int err_size) {
    CmdType branch_type = CMD_TYPE_COUNT;
    char cond_text[MAX_LINE_LEN];
    ReplCompileResult pr;

    if (!ctx || !out)
        return REPL_COMPILE_ERROR;
    memset(out, 0, sizeof(*out));

    pr = compile_parse_if_branch_header(input, &branch_type,
                                        cond_text, sizeof(cond_text),
                                        err, err_size);
    if (pr != REPL_COMPILE_OK)
        return pr;
    if (branch_type != CMD_ELSE_IF && branch_type != CMD_ELSE) {
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    out->pos = compile_insert_pos(ctx);

    if (compile_validate_if_branch_position(ctx, out->pos, branch_type,
                                            NULL, err, err_size)
            != REPL_COMPILE_OK)
        return REPL_COMPILE_ERROR;

    compile_if_branch_indent(ctx, out->pos, out->indent,
                             sizeof(out->indent));

    out->branch_type = branch_type;
    out->branch.type = branch_type;
    out->branch.valid = 1;

    if (branch_type == CMD_ELSE_IF) {
        ExprVar visible_vars[MAX_EXPR_VARS];
        int visible_nv = collect_visible_vars_in(ctx->text, ctx->document_cmds,
                                                 ctx->document_count, out->pos,
                                                 visible_vars, MAX_EXPR_VARS,
                                                 NULL, NULL);
        char verr[REPL_DIAG_TEXT_MAX];
        if (!repl_eval_validate_expression_idents(
                &(ReplExprIdentValidationConfig){
                    .src = cond_text,
                    .vars = visible_nv > 0 ? visible_vars : NULL,
                    .num_vars = visible_nv,
                    .predef = ctx->predef,
                    .err = verr,
                    .errsz = (int)sizeof(verr),
                })) {
            snprintf(err, (size_t)err_size, "%s", verr);
            return REPL_COMPILE_ERROR;
        }

        {
            ExprCtx cond_ctx = { cond_text, visible_nv > 0 ? visible_vars : NULL,
                                 visible_nv, NULL, 0,
                                 ctx->predef.vars, ctx->predef.count };
            out->branch.args[0] = repl_eval_expr(&cond_ctx);
        }
        out->branch.num_args = 1;
        out->branch.has_vars = input_has_any_visible_vars(cond_text,
                                                          visible_vars,
                                                          visible_nv);

        if (!repl_format_fits(out->branch_text, sizeof(out->branch_text),
                              "%s} else if(%s) {", out->indent, cond_text)) {
            snprintf(err, (size_t)err_size, "Command too long");
            return REPL_COMPILE_ERROR;
        }
    } else {
        if (!repl_format_fits(out->branch_text, sizeof(out->branch_text),
                              "%s} else {", out->indent)) {
            snprintf(err, (size_t)err_size, "Command too long");
            return REPL_COMPILE_ERROR;
        }
    }

    out->valid = 1;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_if_branch(const char *input,
                                         const ReplCompileContext *ctx,
                                         ReplCompiledChange *out,
                                         char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    ReplIfBranchKernel kernel;
    ReplCompileResult r = repl_compile_if_branch_kernel(input, ctx, &kernel,
                                                        err, err_size);
    if (r != REPL_COMPILE_OK)
        return r;
    if (!kernel.valid) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    out->kind = REPL_COMPILED_INSERT_ONE;
    out->pos = kernel.pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = kernel.branch;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", kernel.branch_text);
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_close_brace_kernel(const char *input,
                                                  const ReplCompileContext *ctx,
                                                  ReplCloseBraceKernel *out,
                                                  char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    memset(out, 0, sizeof(*out));

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '}') {
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    out->pos = compile_insert_pos(ctx);

    out->open_type = compile_scope_nearest_open_block_at(ctx, out->pos);
    if (out->open_type == CMD_FOR_BEGIN)      out->end_type = CMD_FOR_END;
    else if (out->open_type == CMD_FUNC_DEF)  out->end_type = CMD_FUNC_END;
    else if (out->open_type == CMD_IF_BEGIN)  out->end_type = CMD_IF_END;
    else {
        if (err && err_size > 0)
            snprintf(err, (size_t)err_size, "unmatched '}'");
        return REPL_COMPILE_ERROR;
    }

    /* Matched-existing-end branch: close-brace lands on a row that's
     * already the right end marker. */
    if (out->pos < ctx->document_count &&
        ctx->document_cmds[out->pos].type == out->end_type) {
        out->matched_existing = 1;
        out->valid = 1;
        return REPL_COMPILE_OK;
    }

    /* Insert-new-end-marker branch. */
    char indent[REPL_INDENT_TEXT_MAX];
    compile_close_brace_indent(ctx, out->pos, indent, sizeof(indent));

    out->fe.type  = out->end_type;
    out->fe.valid = 1;
    snprintf(out->fe_text, sizeof(out->fe_text), "%s}", indent);
    out->matched_existing = 0;
    out->valid = 1;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_close_brace(const char *input,
                                           const ReplCompileContext *ctx,
                                           ReplCompiledChange *out,
                                           char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    ReplCloseBraceKernel kernel;
    ReplCompileResult r = repl_compile_close_brace_kernel(input, ctx, &kernel,
                                                          err, err_size);
    if (r != REPL_COMPILE_OK) return r;
    if (!kernel.valid || kernel.matched_existing) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = kernel.pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = kernel.fe;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", kernel.fe_text);
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_if_block_kernel(const char *input,
                                               const ReplCompileContext *ctx,
                                               ReplIfBlockKernel *out,
                                               char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    memset(out, 0, sizeof(*out));

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "if(", 3) != 0 && strncmp(p, "if (", 4) != 0) {
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    out->pos = compile_insert_pos(ctx);

    ExprVar visible_vars[MAX_EXPR_VARS];
    int visible_nv = collect_visible_vars_in(ctx->text, ctx->document_cmds,
                                             ctx->document_count, out->pos,
                                             visible_vars, MAX_EXPR_VARS, NULL, NULL);

    /* Skip past `if` to the opening `(`. */
    while (*p && *p != '(') p++;
    if (!*p) {
        snprintf(err, (size_t)err_size, "if syntax: if(expr) {");
        return REPL_COMPILE_ERROR;
    }
    p++;

    char cond_text[MAX_LINE_LEN];
    int  paren = 1;
    const char *expr_start = p;
    while (*p && paren > 0) {
        if (*p == '(')      paren++;
        else if (*p == ')') paren--;
        if (paren > 0) p++;
    }
    if (paren != 0) {
        snprintf(err, (size_t)err_size, "if syntax: if(expr) {");
        return REPL_COMPILE_ERROR;
    }
    int clen = (int)(p - expr_start);
    if (clen > (int)sizeof(cond_text) - 1)
        clen = (int)sizeof(cond_text) - 1;
    memcpy(cond_text, expr_start, (size_t)clen);
    cond_text[clen] = '\0';

    char verr[REPL_DIAG_TEXT_MAX];
    if (!repl_eval_validate_expression_idents(
            &(ReplExprIdentValidationConfig){
                .src = cond_text,
                .vars = visible_nv > 0 ? visible_vars : NULL,
                .num_vars = visible_nv,
                .predef = ctx->predef,
                .err = verr,
                .errsz = (int)sizeof(verr),
            })) {
        snprintf(err, (size_t)err_size, "%s", verr);
        return REPL_COMPILE_ERROR;
    }

    float cond_val = 0.0f;
    {
        ExprCtx cond_ctx = { cond_text, visible_nv > 0 ? visible_vars : NULL,
                             visible_nv, NULL, 0,
                             ctx->predef.vars, ctx->predef.count };
        cond_val = repl_eval_expr(&cond_ctx);
    }

    /* Skip `)`; trailing `{` is optional for the lean loader. */
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' && *p != '\0') {
        snprintf(err, (size_t)err_size, "if syntax: if(expr) {");
        return REPL_COMPILE_ERROR;
    }

    compile_scope_cmd_indent(ctx, out->pos, out->indent, sizeof(out->indent));

    out->ib.type    = CMD_IF_BEGIN;
    out->ib.args[0] = cond_val;
    out->ib.valid   = 1;
    out->ib.has_vars = input_has_any_visible_vars(cond_text,
                                                  visible_vars, visible_nv);

    /* Trim cond_text in place for canonical formatting. */
    char *ct = cond_text;
    int ctlen;
    while (*ct && isspace((unsigned char)*ct)) ct++;
    ctlen = (int)strlen(ct);
    while (ctlen > 0 && isspace((unsigned char)ct[ctlen - 1]))
        ct[--ctlen] = '\0';

    if (!repl_format_fits(out->ib_text, sizeof(out->ib_text),
                          "%sif(%s) {", out->indent, ct)) {
        snprintf(err, (size_t)err_size, "Command too long");
        return REPL_COMPILE_ERROR;
    }

    out->valid = 1;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_if_block(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    ReplIfBlockKernel kernel;
    ReplCompileResult r = repl_compile_if_block_kernel(input, ctx, &kernel,
                                                       err, err_size);
    if (r != REPL_COMPILE_OK) return r;
    if (!kernel.valid) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = kernel.pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = kernel.ib;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", kernel.ib_text);
    return REPL_COMPILE_OK;
}

/* The name a CMD_FUNC_DEF row's canonical text must carry. Published to
 * callers through ReplFuncDefKernel.header_name; see the note there. */
static const char *compile_func_header_alias(const ReplCompileContext *ctx,
                                             const ReplFuncAliasOp *pending,
                                             int fn) {
    /* A pending op is a name this commit is introducing; it has not
     * reached the alias table yet, so it wins. Without one the slot's
     * registered alias is the row's name - resolve_alias deliberately
     * emits no op for an already-registered name, and formatting a bare
     * `funcN` there would rename the row out from under every call site
     * (that is the alias half of the comment/uncomment round trip). */
    if (pending && pending->slot >= 0 && pending->name[0])
        return pending->name;
    if (ctx && ctx->func_aliases.names &&
        fn >= 0 && fn < ctx->func_aliases.count) {
        const char *registered = ctx->func_aliases.names[fn];
        if (registered && registered[0])
            return registered;
    }
    return NULL;
}

ReplCompileResult repl_compile_func_def_resolve_alias(const ReplCompileContext *ctx,
                                                      const char *trimmed,
                                                      ReplCompiledChange *out,
                                                      int *rejected_keyword,
                                                      char *err, int err_size) {
    if (rejected_keyword)
        *rejected_keyword = 0;

    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    const char *p = trimmed;
    if (!(strncmp(p, "func", 4) == 0 && p[4] >= '0' && p[4] <= '9' &&
          !repl_eval_is_ident_continue((unsigned char)p[5])) &&
        repl_eval_is_ident_start((unsigned char)*p)) {
        char ident[REPL_FUNC_NAME_MAX];
        int len = 0;
        while (*p && repl_eval_is_ident_continue((unsigned char)*p)) {
            if (len >= REPL_FUNC_NAME_MAX - 1) { ident[0] = '\0'; break; }
            ident[len++] = *p++;
        }
        if (len > 0) {
            ident[len] = '\0';
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '{' || *p == '(') {
                int existing = repl_func_alias_lookup_slot(ident);
                if (existing < 0) {
                    /* Reject reserved / control-flow names by returning REPL_COMPILE_OK
                     * and setting out->kind = REPL_COMPILED_NO_CHANGE so the next
                     * commit handler in the chain (if-block, close-brace, etc.) can claim the input. */
                    if (!repl_func_alias_name_is_valid(ident)) {
                        if (repl_eval_is_c_keyword(ident) &&
                            !compile_func_alias_is_structural_name(ident))
                            return compile_validate_c_binder_name(
                                ident, "function", err, err_size);
                        out->kind = REPL_COMPILED_NO_CHANGE;
                        if (rejected_keyword)
                            *rejected_keyword = 1;
                        return REPL_COMPILE_OK;
                    }
                    int target_slot = -1;
                    int ep = ctx->insert_mode ? ctx->edit_line :
                             (ctx->edit_line < ctx->document_count
                                  ? ctx->edit_line : ctx->document_count);
                    if (!ctx->insert_mode &&
                        ep < ctx->document_count &&
                        ctx->document_cmds[ep].type == CMD_FUNC_DEF) {
                        target_slot = (int)ctx->document_cmds[ep].args[0];
                    }
                    if (target_slot < 0)
                        target_slot = repl_func_alias_first_free_slot();
                    if (target_slot < 0) {
                        if (err && err_size > 0) {
                            snprintf(err, (size_t)err_size,
                                     "no free function slots (max %d)",
                                     REPL_FUNC_SLOT_COUNT);
                        }
                        return REPL_COMPILE_ERROR;
                    }
                    out->alias_op.slot = target_slot;
                    snprintf(out->alias_op.name, sizeof(out->alias_op.name),
                             "%s", ident);
                }
            }
        }
    }
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_func_def_kernel(const char *input,
                                               const ReplCompileContext *ctx,
                                               int allow_overwrite_at_pos,
                                               ReplFuncDefKernel *out,
                                               char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    memset(out, 0, sizeof(*out));

    /* Quick-reject bare `else` / `else if(...)` that landed on its own
     * line without the leading `}`.  No other commit handler claims a
     * standalone `else`, so produce a descriptive compile error rather
     * than letting it fall through to the "Unknown cmd" path. */
    const char *trimmed = input ? input : "";
    while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
    if (strncmp(trimmed, "else", 4) == 0 &&
        compile_token_boundary(trimmed[4])) {
        return compile_set_err(err, err_size,
            "else must be on the same line as }: } else {");
    }

    /* Quick-reject inputs that look like function calls (have `(` and
     * no `{`). They go through the normal command parser. This test is the
     * authoritative call/def split - see repl_text_is_func_call_shaped, which
     * homes it so callers that need to PREDICT this outcome (the Ctrl+click
     * go-to-definition preflight) apply the same rule instead of re-deriving
     * a looser one from parse_repl_func_signature. */
    if (repl_text_is_func_call_shaped(trimmed)) {
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    /* Resolve new user names (e.g. `drawCube { ... }`) into a pending
     * alias op. The parser accepts that pending name for this call only;
     * the alias table is updated later by apply, after the command-store
     * mutation succeeds. */
    ReplCompiledChange alias_state;
    repl_compiled_change_init(&alias_state);
    int rejected_keyword = 0;
    ReplCompileResult alias_res = repl_compile_func_def_resolve_alias(
        ctx, trimmed, &alias_state, &rejected_keyword, err, err_size);
    if (alias_res != REPL_COMPILE_OK)
        return alias_res;
    if (rejected_keyword) {
        out->rejected_keyword = 1;
        return REPL_COMPILE_OK;
    }
    out->alias_op = alias_state.alias_op;

    if (!parse_repl_func_signature_with_pending_alias(
            input ? input : "",
            out->alias_op.name, out->alias_op.slot,
            &out->fn, out->param_names, MAX_EXPR_VARS,
            &out->param_count)) {
        out->alias_op.slot = -1;
        out->alias_op.name[0] = '\0';
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    for (int param_idx = 0; param_idx < out->param_count; param_idx++) {
        ReplCompileResult name_result = compile_validate_c_binder_name(
            out->param_names[param_idx], "function parameter", err, err_size);
        if (name_result != REPL_COMPILE_OK)
            return name_result;
    }

    /* Reject duplicate funcN definitions. The loader passes
     * allow_overwrite_at_pos = -1 to reject any duplicate; the editor
     * passes its edit_pos when it has detected an in-place rewrite so
     * the dup-at-cursor case is exempt. */
    for (int ei = 0; ei < ctx->document_count; ei++) {
        const GLCmd *c = &ctx->document_cmds[ei];
        if (!c->valid) continue;
        if (c->type != CMD_FUNC_DEF) continue;
        if ((int)c->args[0] != out->fn) continue;
        if (ei == allow_overwrite_at_pos) continue;
        snprintf(err, (size_t)err_size,
                 "func%d already defined (line %d)", out->fn, ei + 1);
        out->alias_op.slot = -1;
        out->alias_op.name[0] = '\0';
        return REPL_COMPILE_ERROR;
    }

    /* Reverse binder guards, for a header rewrite over an existing body.
     * Validating only where the local is declared would leave a later
     * header edit free to create a same-scope redefinition, to overflow
     * the frame, or to capture a write - so the parameter list is checked
     * against the body that is already there. This lives in the kernel and
     * not the wrapper because the editor calls the kernel directly
     * (src/editor/commit.c), so a check on the wrapper alone is bypassed
     * on the interactive path. */
    if (allow_overwrite_at_pos >= 0 &&
        allow_overwrite_at_pos < ctx->document_count) {
        int body_end = compile_scope_find_block_end(ctx, allow_overwrite_at_pos);
        int old_params = ctx->document_cmds[allow_overwrite_at_pos].num_args;
        int peak = compile_func_scope_peak(ctx, allow_overwrite_at_pos);
        CompileScopeBindings bind;

        compile_collect_bindings(ctx, body_end, &bind);
        for (int p = 0; p < out->param_count; p++) {
            const char *pn = out->param_names[p];
            int bad = 0;

            /* A local hoists to the body top, the same scope as the
             * parameter list - C calls that a redefinition, not
             * shadowing. Only LOCAL matches count: the PARAM entries in
             * `bind` are the old parameters this edit is replacing. */
            for (int b = 0; b < bind.count && !bad; b++) {
                if (bind.kinds[b] == REPL_VISIBLE_VAR_LOCAL &&
                    strcmp(bind.vars[b].name, pn) == 0) {
                    compile_set_err(err, err_size,
                        "'%s' is already declared in this function", pn);
                    bad = 1;
                }
            }
            /* Shadowing does not make a parameter writable, so an
             * assignment the new parameter would capture has to block the
             * edit - otherwise the row silently becomes a write to a
             * constant binding. */
            if (!bad &&
                compile_binder_captures_assignment(ctx,
                                                   allow_overwrite_at_pos + 1,
                                                   body_end, pn)) {
                compile_set_err(err, err_size,
                    "parameter '%s' would capture an assignment in the body - "
                    "function parameters are constant", pn);
                bad = 1;
            }
            if (bad) {
                out->alias_op.slot = -1;
                out->alias_op.name[0] = '\0';
                return REPL_COMPILE_ERROR;
            }
        }
        if (peak - old_params + out->param_count > MAX_EXPR_VARS) {
            out->alias_op.slot = -1;
            out->alias_op.name[0] = '\0';
            return compile_set_err(err, err_size,
                "function scope full (max %d parameters + locals + loop depth)",
                MAX_EXPR_VARS);
        }
    }

    out->pos = compile_insert_pos(ctx);

    compile_scope_cmd_indent(ctx, out->pos, out->indent, sizeof(out->indent));

    out->fd.type     = CMD_FUNC_DEF;
    out->fd.args[0]  = (float)out->fn;
    out->fd.num_args = out->param_count;
    out->fd.valid    = 1;

    {
        const char *header_name = compile_func_header_alias(ctx, &out->alias_op,
                                                            out->fn);
        repl_copy_string_fits(out->header_name, sizeof(out->header_name),
                              header_name ? header_name : "");
        format_func_header_with_alias(out->fd_text, (int)sizeof(out->fd_text),
                                      out->indent, out->fn, out->param_names,
                                      out->param_count, header_name);
    }

    out->valid = 1;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_func_def(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    ReplFuncDefKernel kernel;
    /* Loader path: reject every duplicate. */
    ReplCompileResult r = repl_compile_func_def_kernel(input, ctx, -1, &kernel,
                                                       err, err_size);
    if (r != REPL_COMPILE_OK) return r;
    if (kernel.rejected_keyword || !kernel.valid) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = kernel.pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = kernel.fd;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", kernel.fd_text);
    out->alias_op = kernel.alias_op;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_for_loop_kernel(const char *input,
                                               const ReplCompileContext *ctx,
                                               ReplForLoopKernel *out,
                                               char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    memset(out, 0, sizeof(*out));

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0) {
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    out->pos = compile_insert_pos(ctx);

    out->visible_nv = collect_visible_vars_in(ctx->text, ctx->document_cmds,
                                              ctx->document_count, out->pos,
                                              out->visible_vars, MAX_EXPR_VARS, NULL, NULL);

    if (!repl_eval_parse_for_header(
            &(ReplForHeaderParseConfig){
                .input = p,
                .var_name = out->var_name,
                .var_sz = (int)sizeof(out->var_name),
                .start = &out->start,
                .end = &out->end,
                .step = &out->step,
                .body_start = &out->body_start,
                .vars = out->visible_vars,
                .num_vars = out->visible_nv,
                .predef_vars = ctx->predef.vars,
                .predef_count = ctx->predef.count,
            })) {
        snprintf(err, (size_t)err_size,
                 "for syntax: for(var, start, end[, step]) body;");
        return REPL_COMPILE_ERROR;
    }

    {
        ReplCompileResult name_result = compile_validate_c_binder_name(
            out->var_name, "loop variable", err, err_size);
        if (name_result != REPL_COMPILE_OK)
            return name_result;
    }

    /* Reverse binder guards, the loop-header twin of the func-def
     * kernel's. Capacity first: flatten_for_loop prepends this iterator to
     * a *fresh* scope array and copies the outer bindings under
     * `lnv < MAX_EXPR_VARS`, silently dropping the last one at the cap -
     * with locals in that array, a live local would vanish mid-body and
     * read as 0. Reject at compile time instead. */
    {
        int enclosing_func = compile_enclosing_func_at(ctx, out->pos);
        int scope_after = compile_func_binder_count(ctx, enclosing_func) +
                          compile_open_loop_depth_at(ctx, out->pos) + 1;
        int rewriting_header = (!ctx->insert_mode &&
                                out->pos < ctx->document_count &&
                                ctx->document_cmds[out->pos].type == CMD_FOR_BEGIN);

        if (scope_after > MAX_EXPR_VARS)
            return compile_set_err(err, err_size,
                "function scope full (max %d parameters + locals + loop depth)",
                MAX_EXPR_VARS);
        /* Renaming `for(i, ...)` to `for(x, ...)` over a body that
         * assigns an outer `x` must not turn that row into a write to the
         * iterator. Only a header rewrite can do this: a newly inserted
         * loop has no body yet. */
        if (rewriting_header &&
            compile_binder_captures_assignment(
                ctx, out->pos + 1, compile_scope_find_block_end(ctx, out->pos),
                out->var_name))
            return compile_set_err(err, err_size,
                "loop variable '%s' would capture an assignment in the loop body - "
                "loop variables are constant", out->var_name);
    }

    compile_scope_cmd_indent(ctx, out->pos, out->indent, sizeof(out->indent));

    out->fb.type    = CMD_FOR_BEGIN;
    out->fb.args[0] = out->start;
    out->fb.args[1] = out->end;
    out->fb.args[2] = out->step;
    out->fb.valid   = 1;

    /* Re-walk the input to extract the args expression text so the
     * formatted line preserves the user's symbolic form when args
     * reference visible vars. */
    const char *raw = p;
    while (*raw && *raw != '(') raw++;
    if (*raw) raw++;
    while (*raw && isspace((unsigned char)*raw)) raw++;
    while (*raw && (isalnum((unsigned char)*raw) || *raw == '_')) raw++;
    while (*raw && isspace((unsigned char)*raw)) raw++;
    if (*raw == ',') raw++;

    const char *args_start = raw;
    int paren = 1;
    const char *ap = args_start;
    while (*ap && paren > 0) {
        if (*ap == '(')      paren++;
        else if (*ap == ')') paren--;
        if (paren > 0) ap++;
    }
    int rlen = (int)(ap - args_start);
    char raw_args[MAX_LINE_LEN];
    if (rlen > (int)sizeof(raw_args) - 1)
        rlen = (int)sizeof(raw_args) - 1;
    memcpy(raw_args, args_start, (size_t)rlen);
    raw_args[rlen] = '\0';
    while (rlen > 0 && isspace((unsigned char)raw_args[rlen - 1]))
        raw_args[--rlen] = '\0';

    char *ra = raw_args;
    while (*ra && isspace((unsigned char)*ra)) ra++;

    char verr[REPL_DIAG_TEXT_MAX];
    if (!repl_eval_validate_expression_idents(
            &(ReplExprIdentValidationConfig){
                .src = ra,
                .vars = out->visible_vars,
                .num_vars = out->visible_nv,
                .predef = ctx->predef,
                .err = verr,
                .errsz = (int)sizeof(verr),
            })) {
        snprintf(err, (size_t)err_size, "%s", verr);
        return REPL_COMPILE_ERROR;
    }

    if (input_has_any_visible_vars(ra, out->visible_vars, out->visible_nv)) {
        out->fb.has_vars = 1;
        if (!repl_format_fits(out->fb_text, sizeof(out->fb_text),
                              "%sfor(%s, %s) {",
                              out->indent, out->var_name, ra)) {
            snprintf(err, (size_t)err_size, "Command too long");
            return REPL_COMPILE_ERROR;
        }
    } else if (out->step != 1.0f) {
        char start_buf[32];
        char end_buf[32];
        char step_buf[32];
        repl_format_source_float(start_buf, sizeof(start_buf), out->start);
        repl_format_source_float(end_buf,   sizeof(end_buf),   out->end);
        repl_format_source_float(step_buf,  sizeof(step_buf),  out->step);
        if (!repl_format_fits(out->fb_text, sizeof(out->fb_text),
                              "%sfor(%s, %s, %s, %s) {",
                              out->indent, out->var_name,
                              start_buf, end_buf, step_buf)) {
            snprintf(err, (size_t)err_size, "Command too long");
            return REPL_COMPILE_ERROR;
        }
    } else {
        char start_buf[32];
        char end_buf[32];
        repl_format_source_float(start_buf, sizeof(start_buf), out->start);
        repl_format_source_float(end_buf,   sizeof(end_buf),   out->end);
        if (!repl_format_fits(out->fb_text, sizeof(out->fb_text),
                              "%sfor(%s, %s, %s) {",
                              out->indent, out->var_name, start_buf, end_buf)) {
            snprintf(err, (size_t)err_size, "Command too long");
            return REPL_COMPILE_ERROR;
        }
    }

    out->valid = 1;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_for_loop(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    ReplForLoopKernel kernel;
    ReplCompileResult r = repl_compile_for_loop_kernel(input, ctx, &kernel,
                                                       err, err_size);
    if (r != REPL_COMPILE_OK) return r;
    if (!kernel.valid) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = kernel.pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = kernel.fb;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", kernel.fb_text);
    return REPL_COMPILE_OK;
}

/* repl_load_apply_line moved to src/repl/load.c after a [P2] review
 * finding: this file is the pure-validator module per the contract
 * at the top, and apply orchestration (writing the command store /
 * editor buffer / predef-var registrations) belongs elsewhere. */
