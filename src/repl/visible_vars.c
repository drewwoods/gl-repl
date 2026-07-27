/*
 * src/repl/visible_vars.c - Loop/function-local variable lookup for parse contexts.
 */

#include "repl/visible_vars.h"

#include "repl/command.h"
#include "repl/state_views.h"
#include "repl/text_helpers.h"
#include "repl/util.h"
#include "source_document.h"

int collect_visible_vars_in(SourceTextView text, const GLCmd *document_cmds,
                            int document_count, int pos,
                            ExprVar *vars, int max_vars, int *total_out,
                            ReplVisibleVarKind *kinds_out) {
    typedef struct {
        CmdType type;
        ExprVar vars[MAX_EXPR_VARS];
        ReplVisibleVarKind kinds[MAX_EXPR_VARS];
        int count;
    } ScopeFrame;

    ScopeFrame frames[64];
    int depth = 0;

    if (!document_cmds)
        document_count = 0;

    for (int cmd_idx = 0; cmd_idx < pos && cmd_idx < document_count; cmd_idx++) {
        CmdType t = document_cmds[cmd_idx].type;
        if (t == CMD_VAR_DECLARE &&
            document_cmds[cmd_idx].var_idx == REPL_VAR_IDX_LOCAL) {
            /* A function-scoped declaration binds into the innermost
             * enclosing function frame, not the block it was typed in:
             * locals hoist to the body top, so their scope is the whole
             * call. (The hoist means this row is normally already at the
             * body top; the search keeps the walk honest either way.) */
            const GLCmd *decl = &document_cmds[cmd_idx];
            for (int f = depth - 1; f >= 0; f--) {
                if (frames[f].type != CMD_FUNC_DEF)
                    continue;
                for (int n = 0; n < decl->payload.decl.count &&
                                frames[f].count < MAX_EXPR_VARS; n++) {
                    int slot = frames[f].count++;
                    repl_copy_string_fits(frames[f].vars[slot].name,
                                          sizeof(frames[f].vars[slot].name),
                                          decl->payload.decl.names[n]);
                    frames[f].vars[slot].value = 0.0f;
                    frames[f].kinds[slot] = REPL_VISIBLE_VAR_LOCAL;
                }
                break;
            }
            continue;
        }
        if (repl_cmd_is_block_head(t)) {
            if (depth >= (int)(sizeof(frames) / sizeof(frames[0])))
                break;

            frames[depth].type = t;
            frames[depth].count = 0;

            if (t == CMD_FOR_BEGIN) {
                char vn[16];
                const char *for_text = source_text_line(text, cmd_idx);
                repl_extract_for_var_name(for_text ? for_text : "", vn, sizeof(vn));
                repl_copy_string_fits(frames[depth].vars[0].name,
                                      sizeof(frames[depth].vars[0].name),
                                      vn);
                frames[depth].vars[0].value = document_cmds[cmd_idx].args[0];
                frames[depth].kinds[0] = REPL_VISIBLE_VAR_LOOP;
                frames[depth].count = 1;
            } else if (t == CMD_FUNC_DEF) {
                int fn = -1;
                int param_count = 0;
                char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
                const char *func_text = source_text_line(text, cmd_idx);
                if (parse_repl_func_signature(func_text ? func_text : "", &fn,
                                              param_names, MAX_EXPR_VARS,
                                              &param_count)) {
                    for (int param_idx = 0; param_idx < param_count; param_idx++) {
                        repl_copy_string_fits(frames[depth].vars[param_idx].name,
                                              sizeof(frames[depth].vars[param_idx].name),
                                              param_names[param_idx]);
                        frames[depth].vars[param_idx].value = 0.0f;
                        frames[depth].kinds[param_idx] = REPL_VISIBLE_VAR_PARAM;
                    }
                    frames[depth].count = param_count;
                }
            }
            depth++;
        } else if (repl_cmd_is_block_end(t)) {
            if (depth > 0) depth--;
        }
    }

    int count = 0, total = 0;
    for (int depth_idx = depth - 1; depth_idx >= 0; depth_idx--) {
        for (int var_idx = 0; var_idx < frames[depth_idx].count; var_idx++) {
            if (count < max_vars) {
                if (kinds_out)
                    kinds_out[count] = frames[depth_idx].kinds[var_idx];
                vars[count++] = frames[depth_idx].vars[var_idx];
            }
            total++;
        }
    }
    if (total_out) *total_out = total;
    return count;
}

/* Live-document convenience wrapper: collects against the current REPL
 * document. Editor / loader / reformat callers that intentionally parse
 * against live state use this; compile threads its own document view through
 * collect_visible_vars_in so it stays context-driven.
 *
 * Residual: the CMD_FUNC_DEF param extraction above still routes through
 * parse_repl_func_signature, which resolves a custom alias name via the live
 * func-alias table (symbol state, not document state). Bare funcN headers need
 * no such lookup; threading an alias view here is Finding-3-family follow-up. */
int collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out) {
    return collect_visible_vars_in(source_document_view(),
                                   repl_state_document_cmds(),
                                   repl_state_document_count(),
                                   pos, vars, max_vars, total_out, NULL);
}
