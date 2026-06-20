/*
 * src/repl/visible_vars.c - Loop/function-local variable lookup for parse contexts.
 */

#include "repl/visible_vars.h"

#include "repl/command.h"
#include "repl/state_views.h"
#include "repl/text_helpers.h"
#include "repl/util.h"
#include "source_document.h"

int collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out) {
    typedef struct {
        CmdType type;
        ExprVar vars[MAX_EXPR_VARS];
        int count;
    } ScopeFrame;

    ScopeFrame frames[64];
    int depth = 0;
    /* Source text reads for for-loop / func-def reparse route through
     * a SourceTextView fetched at entry. Phase D will accept the
     * view as a parameter once collect_visible_vars is folded into
     * the editor commit path. */
    SourceTextView text = source_document_view();
    const GLCmd *document_cmds = repl_state_document_cmds();

    for (int cmd_idx = 0; cmd_idx < pos && cmd_idx < repl_state_document_count(); cmd_idx++) {
        CmdType t = document_cmds[cmd_idx].type;
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
            if (count < max_vars)
                vars[count++] = frames[depth_idx].vars[var_idx];
            total++;
        }
    }
    if (total_out) *total_out = total;
    return count;
}
