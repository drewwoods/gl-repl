/*
 * repl_core.c - Core state, normalization, and display infrastructure.
 *
 * Division of labor
 * -----------------
 * This file owns everything that transforms text into renderable GL state:
 *
 *   - Control-flow state that is not pure command metadata
 *   - Global state: command arrays (repl_state_document_cmds_mut() / g_flat_cmds), camera, toggles,
 *     accumulation-buffer settings, etc.
 *   - Normalization - repl_parse_and_normalize(), repl_reformat_commands()
 *   - GLUT callback wrappers
 *   - Public API wrappers forwarded from sample.c
 *
 * repl_editor.c owns the interactive editing layer:
 *   - Editor state (g_input, cursor)
 *   - Commit handlers that decide *where* a parsed command goes in repl_state_document_cmds_mut()[]
 *   - GLUT keyboard / special / mouse / motion / timer dispatch
 *   - Panel resizing and routing to variable-drag ownership
 *   - feed_line() - the programmatic commit entry point used by file loading
 *     and test harnesses
 *
 * Other translation units:
 *   repl_eval.c    - expression evaluator, for-loop header parsers
 *   repl_export.c  - save / load  (output.c round-tripping)
 *   repl_undo.c    - undo/redo snapshots and history rings
 *   repl_camera_controls.c - viewport camera drag and momentum controls
 *   repl_actions.c - config shortcuts, menu actions, startup config defaults
 *   repl_code_panel_layout.c - pure code-panel wrapping and row lookup
 *   repl_code_panel_document.c - code-panel document rows and hit targets
 *   repl_search.c  - incremental search overlay
 *   cmd_format.c   - source-text formatting helpers
 *   repl_parser.c  - parse_command(): text → GLCmd
 *   repl_source_scope.c - source block/depth queries and indent cache
 *   repl_flatten.c - repl_flatten_program() / flatten_commands()
 *   repl_executor.c- repl_execute_program() / execute_commands()
 *   repl_autocomplete.c - completions and parameter hints
 *   repl_autonormal.c - auto-generated normals and feeding-state lookup
 *   repl_example_loader.c - built-in example loading and metadata
 *   repl_replay.c  - replay state machine and fade-batch rendering
 *   repl_replay_annotations.c - code-panel replay variable annotations
 *   scene_render.c - 3D scene setup, grid / axes / overlay drawing
 *   ui_menu_bar.c - code-panel menus, dropdowns, and search slot
 *   ui_color_picker.c - floating literal-color editor
 *   ui_autocomplete_panel.c - floating autocomplete popup renderer
 *   ui_help_overlay.c - modal F1 help overlay
 *   ui_variable_panel.c - floating variable panel renderer
 *   repl_inline_rename.c - scene-rename input buffer
 *   repl_var_drag.c - variable slider drag state/writeback
 *   ui_panels.c    - code rows, scene status, hit routing
 *   repl_examples.c- predefined example scene data
 */

#include "sample.h"
#include "repl_export.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_pipeline.h"
#include "repl_command_spec.h"
#include "repl_command_store.h"
#include "repl_parser.h"
#include "repl_source_scope.h"
#include "repl_flatten.h"
#include "cmd_format.h"
#include "prof.h"
#include "repl_state.h"

#include <sys/stat.h>
#include <sys/types.h>

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

static const char *outfile = "output.c";

/* ========================================================================= */
/* Global state                                                               */
/* ========================================================================= */

void mark_normals_dirty(void) {
    repl_state_mark_normals_dirty();
}

/* Predefined variables - defined in repl_eval.c */

/* (no display list - commands are executed directly each frame) */

/* Forward declarations (eval_expr, parse_for_header, etc. are in repl_eval.h) */
static void get_for_var_name(const GLCmd *cmd, char *var, int var_sz);

/* Forward declarations (eval_expr, parse_for_header, etc. are in repl_eval.h) */
static void get_for_var_name(const GLCmd *cmd, char *var, int var_sz);

/* ========================================================================= */
/* Utility                                                                    */
/* ========================================================================= */

void set_status(const char *msg) {
    repl_state_status_set(msg);
}

const char *mode_name(GLenum mode) {
    return repl_begin_mode_name(mode);
}

GLenum current_begin_mode(void) {
    GLenum mode = GL_TRIANGLES;
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++)
        if (repl_state_document_cmds_mut()[cmd_idx].valid && repl_state_document_cmds_mut()[cmd_idx].type == CMD_BEGIN)
            mode = repl_state_document_cmds_mut()[cmd_idx].mode;
    return mode;
}

int count_vertices(void) {
    int n = 0;
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *g_flat_cmds = flat_program.cmds;
    int g_num_flat_cmds = flat_program.cmd_count;

    for (int flat_idx = 0; flat_idx < g_num_flat_cmds; flat_idx++)
        if (g_flat_cmds[flat_idx].valid && g_flat_cmds[flat_idx].type == CMD_VERTEX3F) n++;
    return n;
}

void repl_normalize_from_parsed(const char *parsed_source,
                                const char *raw_expr,
                                int ensure_semicolon,
                                char *out, int out_sz) {
    if (out_sz <= 0) return;
    char tmp[MAX_LINE_LEN];
    fmt_reindent_from_parsed(parsed_source, raw_expr, tmp, sizeof(tmp));

    int len = (int)strlen(tmp);
    while (len > 0 && isspace((unsigned char)tmp[len - 1]))
        tmp[--len] = '\0';

    if (ensure_semicolon && len > 0) {
        char last = tmp[len - 1];
        if (last != ';' && last != ':' && last != '{' && last != '}') {
            if (len < (int)sizeof(tmp) - 1) {
                tmp[len++] = ';';
                tmp[len] = '\0';
            }
        }
    }

    strncpy(out, tmp, (size_t)out_sz - 1);
    out[out_sz - 1] = '\0';
}

const char *cmd_type_name(CmdType t) {
    return repl_cmd_type_name(t);
}

/* Strip leading/trailing whitespace from `raw_expr`, normalize comma
 * spacing (remove space before comma, ensure one space after), optionally
 * append a semicolon, and prepend `indent_spaces` spaces.  Used by
 * repl_parse_and_normalize() and repl_reformat_commands() to produce
 * canonical source text for a command. */
static void normalize_with_indent(const char *raw_expr, int indent_spaces,
                                  int ensure_semicolon, char *out, int out_sz) {
    if (out_sz <= 0) return;

    const char *p = raw_expr;
    while (*p == ' ' || *p == '\t') p++;

    char body[MAX_LINE_LEN];
    size_t body_len = strlen(p);
    if (body_len >= sizeof(body))
        body_len = sizeof(body) - 1;
    memcpy(body, p, body_len);
    body[body_len] = '\0';

    int len = (int)strlen(body);
    while (len > 0 && isspace((unsigned char)body[len - 1]))
        body[--len] = '\0';
    while (ensure_semicolon && len > 0 && body[len - 1] == ';')
        body[--len] = '\0';
    while (len > 0 && isspace((unsigned char)body[len - 1]))
        body[--len] = '\0';
    if (ensure_semicolon && len > 0 && len < (int)sizeof(body) - 1) {
        body[len++] = ';';
        body[len] = '\0';
    }

    /* Keep expression tokens but normalize comma delimiters for readability. */
    {
        char spaced[MAX_LINE_LEN];
        int si = 0;
        for (int char_idx = 0; body[char_idx] && si < (int)sizeof(spaced) - 1; char_idx++) {
            char c = body[char_idx];
            if (c == ',') {
                while (si > 0 && isspace((unsigned char)spaced[si - 1]))
                    si--;
                spaced[si++] = ',';
                if (si < (int)sizeof(spaced) - 1)
                    spaced[si++] = ' ';
                while (body[char_idx + 1] && isspace((unsigned char)body[char_idx + 1]))
                    char_idx++;
                continue;
            }
            spaced[si++] = c;
        }
        spaced[si] = '\0';
        memcpy(body, spaced, (size_t)si + 1);
    }

    if (indent_spaces < 0) indent_spaces = 0;
    if (indent_spaces > out_sz - 1) indent_spaces = out_sz - 1;
    memset(out, ' ', (size_t)indent_spaces);
    size_t body_copy_len = strlen(body);
    size_t body_cap = (size_t)(out_sz - 1 - indent_spaces);
    if (body_copy_len > body_cap)
        body_copy_len = body_cap;
    memcpy(out + indent_spaces, body, body_copy_len);
    out[indent_spaces + (int)body_copy_len] = '\0';
}

static int parse_and_normalize_impl(const char *line, int pos,
                                    ExprVar *vars, int num_vars,
                                    int preserve_expr, GLCmd *out_cmd,
                                    int strict_refs) {
    ReplParseContext parse_ctx = { pos, vars, num_vars, strict_refs };
    int parsed = repl_parser_parse_command_ctx(line, out_cmd, &parse_ctx);

    if (!parsed) return 0;
    if (preserve_expr) {
        int parsed_indent = 0;
        while (out_cmd->source[parsed_indent] == ' ' ||
               out_cmd->source[parsed_indent] == '\t')
            parsed_indent++;

        normalize_with_indent(line, parsed_indent,
                              repl_cmd_type_needs_semicolon(out_cmd->type),
                              out_cmd->source, (int)sizeof(out_cmd->source));
        out_cmd->has_vars = 1;
    }
    return 1;
}

int repl_parse_and_normalize(const char *line, int pos,
                             ExprVar *vars, int num_vars,
                             int preserve_expr, GLCmd *out_cmd) {
    return parse_and_normalize_impl(line, pos, vars, num_vars,
                                    preserve_expr, out_cmd, 0);
}

int repl_parse_and_normalize_strict(const char *line, int pos,
                                    ExprVar *vars, int num_vars,
                                    int preserve_expr, GLCmd *out_cmd) {
    return parse_and_normalize_impl(line, pos, vars, num_vars,
                                    preserve_expr, out_cmd, 1);
}

void repl_reformat_commands(void) {
    prof_begin(PROF_REFORMAT);
    int saved_edit_line = repl_state_edit_line();
    int saved_inserting = repl_state_insert_mode();
    char saved_input[MAX_INPUT_LEN];
    int saved_input_len = repl_state_editor_input().input_len;
    int saved_cursor_pos = repl_state_cursor_pos();
    memcpy(saved_input, repl_state_editor_input().input, sizeof(saved_input));
    ReplCommandStore store = repl_command_store_live();

    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds_mut()[cmd_idx].valid) continue;

        GLCmd orig = repl_state_document_cmds_mut()[cmd_idx];
        GLCmd fmt = orig;

        char ind_s[32];
        repl_source_scope_cmd_indent(cmd_idx, ind_s, sizeof(ind_s));

        switch (orig.type) {
        case CMD_FOR_BEGIN: {
            char var[16] = "";
            char args[128] = "";
            if (!extract_for_args_text(orig.source, var, sizeof(var), args, sizeof(args)))
                get_for_var_name(&orig, var, sizeof(var));
            if (!var[0]) strncpy(var, "i", sizeof(var) - 1);

            if (orig.has_vars && args[0]) {
                snprintf(fmt.source, sizeof(fmt.source), "%sfor(%s, %s) {", ind_s, var, args);
                fmt.has_vars = 1;
            } else if (orig.args[2] != 1.0f) {
                snprintf(fmt.source, sizeof(fmt.source), "%sfor(%s, %g, %g, %g) {",
                         ind_s, var, orig.args[0], orig.args[1], orig.args[2]);
            } else {
                snprintf(fmt.source, sizeof(fmt.source), "%sfor(%s, %g, %g) {",
                         ind_s, var, orig.args[0], orig.args[1]);
            }
            repl_command_store_replace_one(&store, cmd_idx, &fmt);
            break;
        }
        case CMD_FOR_END:
        case CMD_FUNC_END:
        case CMD_IF_END: {
            int close_ind;
            char close_s[32];
            repl_source_scope_cmd_indent(cmd_idx, close_s, sizeof(close_s));
            close_ind = (int)strlen(close_s);
            if (close_ind >= 2)
                close_ind -= 2;
            else
                close_ind = 0;
            if (close_ind > (int)sizeof(close_s) - 1) close_ind = (int)sizeof(close_s) - 1;
            memset(close_s, ' ', (size_t)close_ind);
            close_s[close_ind] = '\0';
            snprintf(fmt.source, sizeof(fmt.source), "%s}", close_s);
            repl_command_store_replace_one(&store, cmd_idx, &fmt);
            break;
        }
        case CMD_FUNC_DEF: {
            int fn = (int)orig.args[0];
            int parsed_fn = fn;
            int param_count = 0;
            char param_names[MAX_EXPR_VARS][16];
            if (parse_repl_func_signature(orig.source, &parsed_fn,
                                          param_names, MAX_EXPR_VARS,
                                          &param_count))
                format_func_header(fmt.source, sizeof(fmt.source), ind_s,
                                   parsed_fn, param_names, param_count);
            else
                snprintf(fmt.source, sizeof(fmt.source), "%sfunc%d {", ind_s, fn);
            repl_command_store_replace_one(&store, cmd_idx, &fmt);
            break;
        }
        case CMD_IF_BEGIN: {
            char cond[MAX_LINE_LEN] = "";
            if (!repl_extract_paren_payload(orig.source, cond, sizeof(cond)))
                snprintf(cond, sizeof(cond), "%g", orig.args[0]);
            snprintf(fmt.source, sizeof(fmt.source), "%sif(%s) {", ind_s, cond);
            repl_command_store_replace_one(&store, cmd_idx, &fmt);
            break;
        }
        case CMD_VAR_ASSIGN: {
            const char *name = NULL;
            char rhs[MAX_LINE_LEN] = "";
            if (orig.num_args >= 0 && orig.num_args < g_num_predef_vars)
                name = g_predef_vars[orig.num_args].name;
            char fallback[16] = "";
            if (!name) {
                const char *p = orig.source;
                while (*p && isspace((unsigned char)*p)) p++;
                int n = 0;
                while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
                       n < (int)sizeof(fallback) - 1)
                    fallback[n++] = *p++;
                fallback[n] = '\0';
                if (fallback[0]) name = fallback;
            }
            repl_extract_assignment_parts(orig.source, NULL, 0, rhs, sizeof(rhs));
            {
                char comment[MAX_LINE_LEN] = "";
                const char *cp = strstr(orig.source, "//");
                if (cp) snprintf(comment, sizeof(comment), " %s", cp);
                if (name && rhs[0])
                    snprintf(fmt.source, sizeof(fmt.source), "%s%s = %s;%s", ind_s, name, rhs, comment);
                else if (name)
                    snprintf(fmt.source, sizeof(fmt.source), "%s%s = %g;%s", ind_s, name, orig.args[0], comment);
            }
            repl_command_store_replace_one(&store, cmd_idx, &fmt);
            break;
        }
        case CMD_COMMENT: {
            const char *p = orig.source;
            while (*p && isspace((unsigned char)*p)) p++;
            if (p[0] == '/' && p[1] == '/') {
                char suffix[MAX_LINE_LEN];
                p += 2;
                strncpy(suffix, p, sizeof(suffix) - 1);
                suffix[sizeof(suffix) - 1] = '\0';
                int suffix_len = (int)strlen(suffix);
                while (suffix_len > 0 &&
                       isspace((unsigned char)suffix[suffix_len - 1]))
                    suffix[--suffix_len] = '\0';
                snprintf(fmt.source, sizeof(fmt.source), "%s//%s", ind_s, suffix);
            } else {
                snprintf(fmt.source, sizeof(fmt.source), "%s//", ind_s);
            }
            repl_command_store_replace_one(&store, cmd_idx, &fmt);
            break;
        }
        case CMD_VAR_DECLARE: {
            int off = snprintf(fmt.source, sizeof(fmt.source), "%sfloat ", ind_s);
            for (int decl_idx = 0; decl_idx < orig.var_decl_count && off < (int)sizeof(fmt.source) - 4; decl_idx++) {
                if (decl_idx > 0) off += snprintf(fmt.source + off, sizeof(fmt.source) - off, ", ");
                off += snprintf(fmt.source + off, sizeof(fmt.source) - off, "%s", orig.var_names[decl_idx]);
            }
            snprintf(fmt.source + off, sizeof(fmt.source) - off, ";");
            repl_command_store_replace_one(&store, cmd_idx, &fmt);
            break;
        }
        case CMD_LABEL: {
            char label[64] = "";
            if (repl_extract_label_name(orig.source, label, sizeof(label)))
                snprintf(fmt.source, sizeof(fmt.source), "%s:", label);
            repl_command_store_replace_one(&store, cmd_idx, &fmt);
            break;
        }
        case CMD_GOTO: {
            char label[64] = "";
            if (repl_extract_goto_label(orig.source, label, sizeof(label)))
                snprintf(fmt.source, sizeof(fmt.source), "%sgoto %s;", ind_s, label);
            repl_command_store_replace_one(&store, cmd_idx, &fmt);
            break;
        }
        default: {
            ExprVar vis_vars[MAX_EXPR_VARS];
            int num_vis_vars = collect_visible_vars(cmd_idx, vis_vars, MAX_EXPR_VARS);
            int preserve_expr = (num_vis_vars > 0) || orig.has_vars;
            GLCmd parsed;
            memset(&parsed, 0, sizeof(parsed));
            if (repl_parse_and_normalize(orig.source, cmd_idx,
                                         num_vis_vars > 0 ? vis_vars : NULL,
                                         num_vis_vars > 0 ? num_vis_vars : 0,
                                         preserve_expr, &parsed) &&
                parsed.type == orig.type) {
                parsed.is_auto = orig.is_auto;
                parsed.src_cmd_idx = orig.src_cmd_idx;
                if (!preserve_expr) parsed.has_vars = orig.has_vars;
                repl_command_store_replace_one(&store, cmd_idx, &parsed);
            }
            break;
        }
        }
    }

    repl_source_scope_depth_cache_invalidate();
    mark_normals_dirty();

    repl_state_edit_line_set(saved_edit_line);
    repl_state_edit_line_clamp();
    repl_state_insert_mode_set(saved_inserting);
    if (saved_inserting) {
        ReplEditorInputState *inp = repl_state_editor_input_mut();
        memcpy(inp->input, saved_input, sizeof(saved_input));
        inp->input_len = saved_input_len;
        repl_state_cursor_pos_set(saved_cursor_pos);
    } else {
        load_line_to_input(repl_state_edit_line());
    }
    prof_end(PROF_REFORMAT);
}

/* ========================================================================= */
/* GLUT callbacks                                                             */
/* ========================================================================= */

/* ========================================================================= */
/* For-loop parsing and expansion                                             */
/* ========================================================================= */

/* parse_for_header, parse_c_for_header: see repl_eval.c */


/* Parse variable name from a FOR_BEGIN source string */
static void get_for_var_name(const GLCmd *cmd, char *var, int var_sz) {
    const char *p = cmd->source;
    while (*p && *p != '(') p++;
    if (*p) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    int i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && i < var_sz - 1)
        var[i++] = *p++;
    var[i] = '\0';
}

int collect_visible_vars(int pos, ExprVar *vars, int max_vars) {
    typedef struct {
        CmdType type;
        ExprVar vars[MAX_EXPR_VARS];
        int count;
    } ScopeFrame;

    ScopeFrame frames[64];
    int depth = 0;

    for (int cmd_idx = 0; cmd_idx < pos && cmd_idx < repl_state_document_count(); cmd_idx++) {
        CmdType t = repl_state_document_cmds_mut()[cmd_idx].type;
        if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) {
            if (depth >= (int)(sizeof(frames) / sizeof(frames[0])))
                break;

            frames[depth].type = t;
            frames[depth].count = 0;

            if (t == CMD_FOR_BEGIN) {
                char vn[16];
                get_for_var_name(&repl_state_document_cmds_mut()[cmd_idx], vn, sizeof(vn));
                repl_copy_string_fits(frames[depth].vars[0].name,
                                      sizeof(frames[depth].vars[0].name),
                                      vn);
                frames[depth].vars[0].value = repl_state_document_cmds_mut()[cmd_idx].args[0];
                frames[depth].count = 1;
            } else if (t == CMD_FUNC_DEF) {
                int fn = -1;
                int param_count = 0;
                char param_names[MAX_EXPR_VARS][16];
                if (parse_repl_func_signature(repl_state_document_cmds_mut()[cmd_idx].source, &fn,
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
        } else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
            if (depth > 0) depth--;
        }
    }

    int count = 0;
    for (int depth_idx = depth - 1; depth_idx >= 0 && count < max_vars; depth_idx--) {
        for (int var_idx = 0; var_idx < frames[depth_idx].count && count < max_vars; var_idx++)
            vars[count++] = frames[depth_idx].vars[var_idx];
    }

    return count;
}

/* ========================================================================= */
/* Initialization                                                             */
/* ========================================================================= */

static void scroll_to_display_function(void) {
    repl_state_refresh_workspace_header_lines();
    ReplImportExportView meta = repl_state_import_export();
    int target = meta.workspace_header_line_count;
    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++) {
        if (strcmp(g_header_pre[line_idx], "void display() {") == 0)
            break;
        target++;
    }
    repl_state_code_panel_mut()->scroll = target;
    repl_state_code_panel_mut()->scroll_follow_cursor = 0;
}

static void load_initial_commands(const char *import_file) {
    if (import_file) {
        struct stat st;
        if (stat(import_file, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (repl_load_workspace(import_file) > 0) {
                repl_state_edit_line_set(repl_state_document_count());
                scroll_to_display_function();
                return;
            }
        } else if (repl_export_load_from_file(import_file)) {
            repl_state_edit_line_set(repl_state_document_count());
            repl_scenes_activate_home_slot();
            scroll_to_display_function();
            return;
        }
    }

    /* Show example 0 as a starting demo, then anchor slot 0 ("Your Scene")
     * to the current live state so user edits accumulate there and persist
     * across example switches. */
    repl_load_example(0);
    repl_scenes_activate_home_slot();
    set_status("Ready - type GL commands, press ; to execute. F1 for help. F12 for examples.");
    scroll_to_display_function();
}

void repl_save_default_output(void) {
    repl_export_save_output(outfile);
}

void repl_flatten_commands(void) {
    flatten_commands();
}

void repl_load_initial_commands(const char *import_file) {
    load_initial_commands(import_file);
}

void repl_advance_time(float dt) {
    repl_state_time_advance(dt);
}

void repl_reset_time_to_zero(void) {
    repl_state_time_reset_to_zero();
}

void repl_reset_state(void) {
    repl_state_reset_all();
}
