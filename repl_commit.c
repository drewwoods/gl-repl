/*
 * repl_commit.c - REPL commit handlers and commit-order helpers.
 *
 * This module owns the syntactic forms that mutate source commands before
 * the general GL parser gets a chance to run: float declarations, variable
 * assignments, structured blocks, and explicit close braces. repl_editor.c
 * remains responsible for deciding when those handlers are invoked.
 */
#include "sample.h"
#include "editor_commit.h"
#include "repl_apply.h"
#include "repl_core_internal.h"
#include "repl_command_store.h"
#include "repl_compile.h"
#include "repl_parser.h"
#include "repl_source_scope.h"
#include "repl_state.h"

static int g_func_decl_resume_delta = 0;

static int function_decl_insert_pos(void) {
    int pos = 0;

    while (pos < repl_state_document_count() && repl_state_document_cmds_mut()[pos].type == CMD_VAR_DECLARE)
        pos++;

    while (pos < repl_state_document_count()) {
        if (repl_state_document_cmds_mut()[pos].type == CMD_COMMENT) {
            pos++;
            continue;
        }
        if (repl_state_document_cmds_mut()[pos].type != CMD_FUNC_DEF)
            break;

        int end = repl_source_scope_find_block_end(pos);
        if (end >= repl_state_document_count())
            return repl_state_document_count();
        pos = end + 1;
    }

    return pos;
}

static int function_leading_comment_start(int pos) {
    int start = pos;

    while (start > 0 &&
           repl_state_document_cmds_mut()[start - 1].valid &&
           repl_state_document_cmds_mut()[start - 1].type == CMD_COMMENT &&
           repl_source_scope_block_depth_at(start - 1) == 0)
        start--;

    return start;
}

/* apply_func_decl_resume removed: callers (close_brace) now route
 * through editor_commit_apply_plan which consumes the delta as part
 * of the plan's editor post-effects. The remaining static helper is
 * func_def's setter and Enter-while-inserting-at-func-end via
 * repl_commit_resolve_insert_exit_target — both still touch the
 * global directly, slated for Phase D commit 26d. */

int repl_commit_func_decl_resume_delta_peek(void) {
    return g_func_decl_resume_delta;
}

int repl_commit_func_decl_resume_delta_take(void) {
    int delta = g_func_decl_resume_delta;
    g_func_decl_resume_delta = 0;
    return delta;
}

static void fill_scope_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_cmd_indent(pos, buf, buf_sz);
}

/* fill_scope_close_indent removed: close_brace migrated to
 * editor_compile_close_brace which has its own static
 * close_brace_indent. Other callers (none today; future func/for/if
 * close-brace migrations land in 26c-26e) compute the indent
 * inside the editor compile path. */


int repl_commit_resolve_insert_exit_target(int target) {
    if (!editor_insert_mode() ||
        g_func_decl_resume_delta <= 0 ||
        repl_state_edit_line() < 0 ||
        repl_state_edit_line() >= repl_state_document_count() ||
        repl_state_document_cmds_mut()[repl_state_edit_line()].type != CMD_FUNC_END)
        return target;

    if (target == repl_state_edit_line()) {
        target += g_func_decl_resume_delta;
        if (target > repl_state_document_count())
            target = repl_state_document_count();
    }

    g_func_decl_resume_delta = 0;
    return target;
}


void repl_commit_reset_transients(void) {
    g_func_decl_resume_delta = 0;
}

/* Apply a compiled float-decl change to the live REPL+editor state.
 *
 * Drives the three apply halves in lockstep:
 *   1. predef-var cascade  (repl_apply_predef_ops)
 *   2. command store write (repl_apply_compiled_change)
 *   3. editor-buffer write (editor_buffer_apply_compiled_change)
 *
 * The float-decl-specific cursor / status / dirty-flag housekeeping
 * stays here. Phase C commit 21 lifts that into the editor commit
 * orchestration so this helper can become a thin transaction wrap.
 */
static int apply_float_decl_change(const ReplCompiledChange *change) {
    if (!editor_commit_apply_compiled_change(change)) {
        set_status("Command buffer full!");
        return 1;
    }

    if (change->kind == REPL_COMPILED_REPLACE_ONE) {
        repl_state_edit_line_set(repl_state_edit_line() + 1);
        load_line_to_input(repl_state_edit_line());
    } else if (change->kind == REPL_COMPILED_INSERT_ONE) {
        if (!editor_insert_mode() &&
            repl_state_edit_line() < repl_state_document_count())
            load_line_to_input(repl_state_edit_line());
    }

    if (change->commit_message[0])
        set_status(change->commit_message);
    {
        ReplEditorInputState *inp = editor_state_input_mut();
        inp->input[0] = '\0';
        inp->input_len = 0;
    }
    editor_cursor_pos_set(0);
    mark_normals_dirty();
    return 1;
}

int try_commit_float_decl(void) {
    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult result = repl_compile_float_decl(
        editor_state_input().input, &ctx, &change, err, sizeof(err));
    if (result == REPL_COMPILE_OK && change.kind == REPL_COMPILED_NO_CHANGE)
        return 0;  /* not a float decl; fall through */
    if (result != REPL_COMPILE_OK) {
        /* Phase C commit 19: the wrapper still surfaces the diagnostic
         * via set_status. Phase C commit 21 routes the diagnostic
         * through return values to the controller / status layer; the
         * compile function itself never touched status. */
        set_status(err);
        return 1;
    }
    return apply_float_decl_change(&change);
}


/* Apply a compiled var-assignment change.
 *
 * Var-assign has a special-case branch: in non-insert mode an
 * assignment whose target is currently a CMD_VAR_DECLARE silently
 * triggers the float-decl overwrite cascade (undeclare + var_assign
 * num_args fixup). The legacy code path encoded that here; the
 * compile function still classifies the change as REPLACE_ONE and
 * leaves the cascade to apply.
 *
 * Phase C commit 21 will move this var-decl-cascade logic into the
 * compile step (so the compile function emits UNDECLARE predef_ops
 * directly and apply just replays them), unifying with the
 * float-decl pattern. For now the helper preserves the legacy
 * behaviour while still routing through the shared apply primitives.
 */
static int apply_var_assign_change(const ReplCompiledChange *change) {
    /* Var-decl-overwrite cascade (legacy non-insert branch). The
     * compile classified the change as REPLACE_ONE; if the existing
     * slot at `pos` is a CMD_VAR_DECLARE the legacy path replays
     * the float-decl-style undeclare cascade here. Phase D folds
     * this into compile_var_assign so the predef-op plan covers it
     * directly. */
    if (change->kind == REPL_COMPILED_REPLACE_ONE &&
        change->pos < repl_state_document_count() &&
        repl_state_document_cmds_mut()[change->pos].type == CMD_VAR_DECLARE) {
        EditorBufferView text = editor_buffer_view();
        const GLCmd *old_decl = &repl_state_document_cmds_mut()[change->pos];
        for (int decl_idx = 0; decl_idx < old_decl->var_decl_count; decl_idx++) {
            const char *nm = old_decl->var_names[decl_idx];
            for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
                if (cmd_idx == change->pos) continue;
                const char *line = editor_buffer_view_line(text, cmd_idx);
                if (repl_eval_source_uses_ident(line ? line : "", nm)) {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "variable '%s' is in use, cannot overwrite", nm);
                    set_status(buf);
                    return 1;
                }
            }
        }
        for (int decl_idx = 0; decl_idx < old_decl->var_decl_count; decl_idx++) {
            const char *nm = old_decl->var_names[decl_idx];
            int slot = repl_eval_find_predef_var_idx(nm);
            if (slot < 0) continue;
            repl_eval_undeclare_predef_var(nm);
            for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
                if (repl_state_document_cmds_mut()[cmd_idx].type == CMD_VAR_ASSIGN &&
                    repl_state_document_cmds_mut()[cmd_idx].num_args > slot)
                    repl_state_document_cmds_mut()[cmd_idx].num_args--;
            }
        }
    }

    if (!editor_commit_apply_compiled_change(change)) {
        set_status("Command buffer full!");
        return 1;
    }

    /* Cursor housekeeping per the legacy var-assign paths. */
    if (change->kind == REPL_COMPILED_INSERT_ONE) {
        if (change->pos == repl_state_document_count() - 1) {
            /* append branch — keep cursor at end (legacy behaviour). */
            repl_state_edit_line_set(repl_state_document_count());
        } else if (editor_insert_mode()) {
            repl_state_edit_line_set(repl_state_edit_line() + 1);
        }
    } else if (change->kind == REPL_COMPILED_REPLACE_ONE) {
        repl_state_edit_line_set(repl_state_edit_line() + 1);
        load_line_to_input(repl_state_edit_line());
    }

    if (change->commit_message[0])
        set_status(change->commit_message);
    {
        ReplEditorInputState *inp = editor_state_input_mut();
        inp->input[0] = '\0';
        inp->input_len = 0;
    }
    editor_cursor_pos_set(0);
    mark_normals_dirty();
    return 1;
}

int try_assign_variable(void) {
    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult result = repl_compile_var_assign(
        editor_state_input().input, &ctx, &change, err, sizeof(err));
    if (result == REPL_COMPILE_OK && change.kind == REPL_COMPILED_NO_CHANGE)
        return 0;  /* not an assignment; fall through */
    if (result != REPL_COMPILE_OK) {
        set_status(err);
        return 1;
    }
    return apply_var_assign_change(&change);
}


int try_commit_for_loop(void) {
    const char *p = editor_state_input().input;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0)
        return 0;

    {
        int pos = editor_insert_mode() ? repl_state_edit_line() :
                  (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
        ExprVar visible_vars[MAX_EXPR_VARS];
        int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS);
        char var_name[16];
        float start, end, step;
        const char *body_start;

        if (!repl_eval_parse_for_header_with_vars(p, var_name, sizeof(var_name),
                                        &start, &end, &step,
                                        visible_vars, visible_nv, &body_start)) {
            set_status("for syntax: for(var, start, end[, step]) body;");
            return 1;
        }

        while (*body_start && isspace((unsigned char)*body_start))
            body_start++;

        {
            int ind;
            char indent[32];
            char fb_text[MAX_LINE_LEN] = "";
            char fe_text[MAX_LINE_LEN] = "";
            GLCmd fb;
            GLCmd fe;

            fill_scope_indent(pos, indent, sizeof(indent));
            ind = (int)strlen(indent);

            memset(&fb, 0, sizeof(fb));
            fb.type = CMD_FOR_BEGIN;
            fb.args[0] = start;
            fb.args[1] = end;
            fb.args[2] = step;
            fb.valid = 1;

            {
                const char *raw = p;
                while (*raw && *raw != '(')
                    raw++;
                if (*raw)
                    raw++;
                while (*raw && isspace((unsigned char)*raw))
                    raw++;
                while (*raw && (isalnum((unsigned char)*raw) || *raw == '_'))
                    raw++;
                while (*raw && isspace((unsigned char)*raw))
                    raw++;
                if (*raw == ',')
                    raw++;

                {
                    const char *args_start = raw;
                    int paren = 1;
                    const char *ap = args_start;
                    char raw_args[MAX_LINE_LEN];
                    int rlen;

                    while (*ap && paren > 0) {
                        if (*ap == '(')
                            paren++;
                        else if (*ap == ')')
                            paren--;
                        if (paren > 0)
                            ap++;
                    }

                    rlen = (int)(ap - args_start);
                    if (rlen > (int)sizeof(raw_args) - 1)
                        rlen = (int)sizeof(raw_args) - 1;
                    memcpy(raw_args, args_start, (size_t)rlen);
                    raw_args[rlen] = '\0';
                    while (rlen > 0 && isspace((unsigned char)raw_args[rlen - 1]))
                        raw_args[--rlen] = '\0';

                    {
                        char *ra = raw_args;
                        while (*ra && isspace((unsigned char)*ra))
                            ra++;

                        {
                            char verr[128];
                            if (!repl_eval_validate_expression_idents(ra, visible_vars, visible_nv, verr, sizeof(verr))) {
                                set_status(verr);
                                return 1;
                            }
                        }

                        if (input_has_any_visible_vars(ra, visible_vars, visible_nv)) {
                            fb.has_vars = 1;
                            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                                  "%sfor(%s, %s) {",
                                                  indent, var_name, ra)) {
                                set_status("Command too long");
                                return 1;
                            }
                        } else if (step != 1.0f) {
                            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                                  "%sfor(%s, %g, %g, %g) {",
                                                  indent, var_name, start, end, step)) {
                                set_status("Command too long");
                                return 1;
                            }
                        } else {
                            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                                  "%sfor(%s, %g, %g) {",
                                                  indent, var_name, start, end)) {
                                set_status("Command too long");
                                return 1;
                            }
                        }
                    }
                }
            }

            memset(&fe, 0, sizeof(fe));
            fe.type = CMD_FOR_END;
            fe.valid = 1;
            snprintf(fe_text, sizeof(fe_text), "%s}", indent);

            if (*body_start == '{' || *body_start == '\0') {
                const char *loop_lines[2] = { fb_text, fe_text };

                if (!editor_insert_mode() && repl_state_edit_line() < repl_state_document_count() &&
                    repl_state_document_cmds_mut()[repl_state_edit_line()].type == CMD_FOR_BEGIN) {
                    ReplCommandStore store = repl_command_store_live();
                    int replace_idx = repl_state_edit_line();
                    if (repl_command_store_replace_one(&store, replace_idx, &fb))
                        editor_buffer_replace_line(replace_idx, fb_text);
                    repl_state_edit_line_set(repl_state_edit_line() + 1);
                    editor_insert_mode_set(1);
                    {
                        ReplEditorInputState *inp = editor_state_input_mut();
                        inp->input[0] = '\0';
                        inp->input_len = 0;
                    }
                    editor_cursor_pos_set(0);
                    clear_autocomplete_state();
                    set_status("for-loop header updated");
                    mark_normals_dirty();
                    return 1;
                }

                ReplCommandStore store = repl_command_store_live();
                GLCmd loop_cmds[2] = { fb, fe };
                if (!repl_command_store_insert_many(&store, pos,
                                                    loop_cmds, 2, 0)) {
                    set_status("Command buffer full!");
                    return 1;
                }
                editor_buffer_insert_lines(pos, loop_lines, 2);

                repl_state_edit_line_set(pos + 1);
                editor_insert_mode_set(1);
                {
                    ReplEditorInputState *inp = editor_state_input_mut();
                    inp->input[0] = '\0';
                    inp->input_len = 0;
                }
                editor_cursor_pos_set(0);
                set_status("for-loop: type body lines, press Esc when done");
                mark_normals_dirty();
                return 1;
            }

            {
                char body[MAX_LINE_LEN];
                int blen;
                ExprVar dv[MAX_EXPR_VARS];
                int dvn = 0;
                GLCmd body_cmd;

                strncpy(body, body_start, MAX_LINE_LEN - 1);
                body[MAX_LINE_LEN - 1] = '\0';
                blen = (int)strlen(body);
                while (blen > 0 &&
                       (body[blen - 1] == ';' || isspace((unsigned char)body[blen - 1])))
                    body[--blen] = '\0';
                if (blen == 0) {
                    set_status("for-loop needs a body");
                    return 1;
                }

                repl_copy_string_fits(dv[dvn].name, sizeof(dv[dvn].name),
                                      var_name);
                dv[dvn].value = start;
                dvn++;
                /* Append visible variables to the loop context. */
                for (int var_idx = 0; var_idx < visible_nv && dvn < MAX_EXPR_VARS; var_idx++)
                    dv[dvn++] = visible_vars[var_idx];

                ReplParseContext parse_ctx = { pos, dv, dvn, 1 };
                {
                    ReplParsedLine body_pl;
                    if (!repl_parser_parse_command_ctx(body, &body_pl, &parse_ctx)) {
                        set_status("Invalid for-loop body command");
                        return 1;
                    }
                    body_cmd = body_pl.cmd;
                }

                char body_text[MAX_LINE_LEN];
                {
                    char bind[32];
                    int bi = ind + 2;
                    if (bi > (int)sizeof(bind) - 1)
                        bi = (int)sizeof(bind) - 1;
                    memset(bind, ' ', (size_t)bi);
                    bind[bi] = '\0';
                    snprintf(body_text, sizeof(body_text), "%s%s;", bind, body);
                }

                ReplCommandStore store = repl_command_store_live();
                GLCmd loop_cmds[3] = { fb, body_cmd, fe };
                const char *loop_lines[3] = { fb_text, body_text, fe_text };
                if (!repl_command_store_insert_many(&store, pos,
                                                    loop_cmds, 3, 0)) {
                    set_status("Command buffer full!");
                    return 1;
                }
                editor_buffer_insert_lines(pos, loop_lines, 3);

                repl_state_edit_line_set(pos + 3);
                editor_insert_mode_set(0);
                {
                    ReplEditorInputState *inp = editor_state_input_mut();
                    inp->input[0] = '\0';
                    inp->input_len = 0;
                }
                editor_cursor_pos_set(0);
                {
                    ReplEditorInputState *inp = editor_state_input_mut();
                    inp->pending_newline[0] = '\0';
                    inp->pending_newline_len = 0;
                }

                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "for-loop: %s from %g to %g",
                             var_name, start, end);
                    set_status(msg);
                }
                mark_normals_dirty();
                return 1;
            }
        }
    }
}

int try_commit_func_def(void) {
    /* Phase D commit 26d (partial migration):
     * - editor_compile_func_def handles validation + the
     *   overwrite-header branch through compile/apply.
     * - The new-def-with-comment-relocation branch (which needs
     *   a delete-before-insert plan field + g_func_decl_resume_delta
     *   publish-side post-effect) stays inline for now and runs
     *   only when the compile step returns NO_CHANGE without a
     *   commit_message (i.e. valid func decl input but not an
     *   overwrite context).
     */
    {
        ReplCompileContext compile_ctx = repl_compile_context_from_live();
        EditorCommitPlan plan;
        char err[REPL_STATUS_TEXT_MAX];
        ReplCompileResult r = editor_compile_func_def(editor_state_input().input,
                                                      &compile_ctx, &plan,
                                                      err, sizeof(err));
        if (r == REPL_COMPILE_ERROR) {
            set_status(err);
            return 1;
        }
        if (plan.commit_message_valid) {
            /* Overwrite-header path — compile produced a complete
             * plan. Apply and we're done. */
            if (!editor_commit_apply_plan(&plan)) {
                set_status("Command buffer full!");
                return 1;
            }
            mark_normals_dirty();
            return 1;
        }
        /* NO_CHANGE without commit_message → fall through to the
         * legacy path below. */
    }

    int fn = -1;
    int param_count = 0;
    char param_names[MAX_EXPR_VARS][16];
    const char *trimmed = editor_state_input().input;

    while (*trimmed && isspace((unsigned char)*trimmed))
        trimmed++;
    if (strchr(trimmed, '(') && strchr(trimmed, '{') == NULL)
        return 0;
    if (!parse_repl_func_signature(editor_state_input().input, &fn,
                                   param_names, MAX_EXPR_VARS,
                                   &param_count))
        return 0;

    {
        int edit_pos = editor_insert_mode() ? repl_state_edit_line() :
                       (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
        int overwriting_func = (!editor_insert_mode() && edit_pos < repl_state_document_count() &&
                                repl_state_document_cmds_mut()[edit_pos].type == CMD_FUNC_DEF);

        /* Reject duplicate func definitions: each func<N> may only be defined
         * once.  Overwriting the existing definition (cursor already on that
         * line) is still allowed. */
        for (int ei = 0; ei < repl_state_document_count(); ei++) {
            if (!repl_state_document_cmds_mut()[ei].valid) continue;
            if (repl_state_document_cmds_mut()[ei].type != CMD_FUNC_DEF) continue;
            if ((int)repl_state_document_cmds_mut()[ei].args[0] != fn) continue;
            if (overwriting_func && ei == edit_pos) continue;
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "func%d already defined (line %d)", fn, ei + 1);
            set_status(buf);
            return 1;
        }

        int pos = overwriting_func ? edit_pos : function_decl_insert_pos();
        char indent[32];
        GLCmd fd;
        GLCmd fe;
        ReplCommandStore store = repl_command_store_live();
        char fd_text[MAX_LINE_LEN] = "";
        char fe_text[MAX_LINE_LEN] = "";

        fill_scope_indent(pos, indent, sizeof(indent));

        if (overwriting_func) {
            GLCmd updated = repl_state_document_cmds_mut()[edit_pos];
            updated.args[0] = (float)fn;
            updated.num_args = param_count;
            format_func_header(fd_text, (int)sizeof(fd_text),
                               indent, fn, param_names, param_count);
            if (repl_command_store_replace_one(&store, edit_pos, &updated))
                editor_buffer_replace_line(edit_pos, fd_text);
            repl_state_edit_line_set(edit_pos + 1);
            editor_insert_mode_set(1);
            {
                ReplEditorInputState *inp = editor_state_input_mut();
                inp->input[0] = '\0';
                inp->input_len = 0;
            }
            editor_cursor_pos_set(0);
            clear_autocomplete_state();
            set_status("func def header updated");
            mark_normals_dirty();
            return 1;
        }

        memset(&fd, 0, sizeof(fd));
        fd.type = CMD_FUNC_DEF;
        fd.args[0] = (float)fn;
        fd.num_args = param_count;
        fd.valid = 1;
        format_func_header(fd_text, (int)sizeof(fd_text),
                           indent, fn, param_names, param_count);

        memset(&fe, 0, sizeof(fe));
        fe.type = CMD_FUNC_END;
        fe.valid = 1;
        snprintf(fe_text, sizeof(fe_text), "%s}", indent);

        int comment_start = edit_pos;
        int comment_count = 0;
        int resume_pos = edit_pos;
        char (*comment_lines)[MAX_LINE_LEN] = NULL;
        const char *insert_lines[MAX_COMMANDS];

        if (!overwriting_func) {
            comment_start = function_leading_comment_start(edit_pos);
            comment_count = edit_pos - comment_start;
            if (comment_count > 0) {
                EditorBufferView text = editor_buffer_view();
                comment_lines = malloc((size_t)comment_count * sizeof(*comment_lines));
                if (!comment_lines) {
                    set_status("Out of memory");
                    return 1;
                }
                for (int comment_idx = 0; comment_idx < comment_count; comment_idx++)
                    repl_copy_string_fits(comment_lines[comment_idx], MAX_LINE_LEN,
                                          editor_buffer_view_line(text,
                                                                  comment_start + comment_idx));
            }
        }

        int insert_count = comment_count + 2;
        GLCmd *insert_cmds = (GLCmd *)malloc((size_t)insert_count * sizeof(*insert_cmds));
        if (!insert_cmds) {
            free(comment_lines);
            set_status("Out of memory");
            return 1;
        }
        if (comment_count > 0) {
            memcpy(insert_cmds, &repl_state_document_cmds_mut()[comment_start],
                   (size_t)comment_count * sizeof(*insert_cmds));
            repl_command_store_delete_range(&store, comment_start, comment_count);
            editor_buffer_delete_range(comment_start, comment_count);
            resume_pos = edit_pos - comment_count;
        }

        pos = function_decl_insert_pos();
        insert_cmds[comment_count] = fd;
        insert_cmds[comment_count + 1] = fe;
        for (int comment_idx = 0; comment_idx < comment_count; comment_idx++)
            insert_lines[comment_idx] = comment_lines[comment_idx];
        insert_lines[comment_count] = fd_text;
        insert_lines[comment_count + 1] = fe_text;
        if (!repl_command_store_insert_many(&store, pos, insert_cmds,
                                            insert_count, 0)) {
            free(comment_lines);
            free(insert_cmds);
            set_status("Command buffer full!");
            return 1;
        }
        editor_buffer_insert_lines(pos, insert_lines, insert_count);
        g_func_decl_resume_delta = resume_pos > pos ? resume_pos - pos : 0;
        free(comment_lines);
        free(insert_cmds);

        repl_state_edit_line_set(pos + comment_count + 1);
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        set_status("func def: type body lines, press Esc when done");
        mark_normals_dirty();
        return 1;
    }
}

int try_commit_if_block(void) {
    /* Phase D commit 26c migration: dispatch through the
     * compile/apply pair. editor_compile_if_block produces the
     * EditorCommitPlan; editor_commit_apply_plan replays both
     * halves atomically.
     */
    ReplCompileContext ctx = repl_compile_context_from_live();
    EditorCommitPlan plan;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = editor_compile_if_block(editor_state_input().input,
                                                  &ctx, &plan,
                                                  err, sizeof(err));
    if (r == REPL_COMPILE_OK &&
        plan.change.kind == REPL_COMPILED_NO_CHANGE &&
        !plan.commit_message_valid)
        return 0;  /* not an if-block input */
    if (r != REPL_COMPILE_OK) {
        set_status(err);
        return 1;
    }

    if (!editor_commit_apply_plan(&plan)) {
        set_status("Command buffer full!");
        return 1;
    }
    mark_normals_dirty();
    return 1;
}

int try_commit_close_brace(void) {
    /* Phase D commit 26b migration: dispatch through the
     * compile/apply pair. The compile step (editor_compile_close_brace)
     * inspects the input and current scope state and produces an
     * EditorCommitPlan; editor_commit_apply_plan replays the REPL
     * change + editor post-effects atomically.
     *
     * Returns 0 (input not recognized as a close-brace) on
     * NO_CHANGE, matching the legacy contract.
     */
    ReplCompileContext ctx = repl_compile_context_from_live();
    EditorCommitPlan plan;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = editor_compile_close_brace(editor_state_input().input,
                                                     &ctx, &plan,
                                                     err, sizeof(err));
    if (r == REPL_COMPILE_OK &&
        plan.change.kind == REPL_COMPILED_NO_CHANGE &&
        !plan.commit_message_valid)
        return 0;  /* not a close-brace context */
    if (r != REPL_COMPILE_OK) {
        set_status(err);
        return 1;
    }

    if (!editor_commit_apply_plan(&plan)) {
        set_status("Command buffer full!");
        return 1;
    }
    mark_normals_dirty();
    return 1;
}

/* Block-structural commit handlers: `}`, `for(`, `funcN`, `if(`.
 * These inspect the start of g_input and are mutually exclusive with
 * var-statement handlers. */
int try_commit_block_structs(void) {
    if (try_commit_close_brace()) return 1;
    if (try_commit_for_loop())    return 1;
    if (try_commit_func_def())    return 1;
    if (try_commit_if_block())    return 1;
    return 0;
}

/* Statement-level commit handlers. float decl MUST precede assign so that
 * `float x` is not misread as an assignment to an identifier named "float". */
int try_commit_var_statements(void) {
    if (try_commit_float_decl())  return 1;
    if (try_assign_variable())    return 1;
    return 0;
}

/* Canonical order: var statements first (float/assign), then block structs.
 * Handlers are mutually exclusive on input prefix, so the relative ordering
 * of the two groups doesn't affect observed behavior. */
int try_commit_any(void) {
    if (try_commit_var_statements()) return 1;
    if (try_commit_block_structs())  return 1;
    return 0;
}

/* Overwrite-mode Enter variant: on successful var-statement commit, enter
 * insert mode and clear the input. Assign additionally publishes the
 * "Insert mode" status and marks normals dirty (float decl does not). */
int try_commit_var_statements_then_insert(void) {
    if (try_commit_float_decl()) {
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        clear_autocomplete_state();
        return 1;
    }
    if (try_assign_variable()) {
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        clear_autocomplete_state();
        set_status("Insert mode");
        mark_normals_dirty();
        return 1;
    }
    return 0;
}
