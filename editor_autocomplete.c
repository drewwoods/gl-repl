/*
 * editor_autocomplete.c -- Input completion and parameter hints.
 *
 * Runtime storage lives in repl_state.c and is accessed through the typed
 * autocomplete facade.
 */
#include "repl_state.h"
#include "repl_core_internal.h"
#include "repl_command_spec.h"
#include "editor_completion.h"
static const ReplFuncCompletion *g_ac_func_matches[MAX_AC_MATCHES];

typedef enum {
    AC_MODE_NONE = 0,
    AC_MODE_POINT_PARAM,
    AC_MODE_ENUM_ARG1,
    AC_MODE_ENUM_ARG2,
    AC_MODE_FUNC_PREFIX
} AutocompleteMode;

static AutocompleteMode g_ac_mode = AC_MODE_NONE;
static int g_ac_token_len = 0;
static char g_ac_suffix[8] = "";

static void hint_append(char *out, int out_sz, const char *text) {
    int len = (int)strlen(out);
    if (len >= out_sz - 1)
        return;
    snprintf(out + len, (size_t)(out_sz - len), "%s", text);
}

static void build_param_hint_text(const char *const *params, int param_count,
                                  const char *after, char *out, int out_sz) {
    int arg_index = 0;
    int arg_has_text = 0;
    int depth = 0;

    out[0] = '\0';
    if (!after || !params || param_count <= 0)
        return;

    for (const char *p = after; *p; p++) {
        unsigned char ch = (unsigned char)*p;

        if (depth == 0 && ch == ')')
            return;
        if (depth == 0 && ch == ',') {
            arg_index++;
            arg_has_text = 0;
            continue;
        }

        if (ch == '(') depth++;
        else if (ch == ')' && depth > 0) depth--;

        if (!isspace(ch))
            arg_has_text = 1;
    }

    if (arg_index < 0 || arg_index > param_count)
        return;

    int next_param = arg_has_text ? arg_index + 1 : arg_index;
    if (next_param < 0 || next_param > param_count)
        return;

    if (next_param == param_count) {
        if (arg_has_text)
            snprintf(out, (size_t)out_sz, ")");
        return;
    }

    if (arg_has_text)
        hint_append(out, out_sz, ", ");

    for (int i = next_param; i < param_count; i++) {
        if (i > next_param)
            hint_append(out, out_sz, ", ");
        hint_append(out, out_sz, params[i]);
    }
    hint_append(out, out_sz, ")");
}

static const ReplFuncCompletion *find_builtin_completion_for_input(const char *input,
                                                               const char **after_out) {
    const ReplFuncCompletion *completions = repl_func_completions();
    for (int i = 0; completions[i].insert_text; i++) {
        int plen = (int)strlen(completions[i].insert_text);
        if (completions[i].param_count <= 0)
            continue;
        if (strncmp(input, completions[i].insert_text, (size_t)plen) == 0) {
            if (after_out)
                *after_out = input + plen;
            return &completions[i];
        }
    }
    return NULL;
}

static int find_defined_func_call_params(const char *input, const char **after_out,
                                         const char *params_out[MAX_EXPR_VARS],
                                         int *count_out,
                                         char param_storage[MAX_EXPR_VARS][16]) {
    const char *p = input;
    int fn = 0;

    if (strncmp(p, "func", 4) != 0)
        return 0;
    p += 4;
    if (!isdigit((unsigned char)*p))
        return 0;

    while (isdigit((unsigned char)*p)) {
        fn = fn * 10 + (*p - '0');
        p++;
    }
    if (*p != '(')
        return 0;

    if (after_out)
        *after_out = p + 1;

    EditorBufferView text = editor_buffer_view();
    for (int i = 0; i < repl_state_document_count(); i++) {
        int parsed_fn = -1;
        int param_count = 0;
        if (!repl_state_document_cmds_mut()[i].valid || repl_state_document_cmds_mut()[i].type != CMD_FUNC_DEF)
            continue;
        if ((int)repl_state_document_cmds_mut()[i].args[0] != fn)
            continue;
        {
            const char *func_text = editor_buffer_view_line(text, i);
            if (!parse_repl_func_signature(func_text ? func_text : "", &parsed_fn,
                                           param_storage, MAX_EXPR_VARS,
                                           &param_count))
                continue;
        }
        if (parsed_fn != fn || param_count <= 0)
            continue;
        for (int j = 0; j < param_count; j++)
            params_out[j] = param_storage[j];
        if (count_out)
            *count_out = param_count;
        return 1;
    }

    return 0;
}

static void update_input_param_hint(void) {
    const char *input = editor_state_input().input;
    const char *after = NULL;
    const ReplFuncCompletion *builtin = find_builtin_completion_for_input(input, &after);
    ReplAutocompleteState *ac = editor_state_autocomplete_mut();
    if (builtin) {
        build_param_hint_text(builtin->params, builtin->param_count,
                              after, ac->hint, (int)sizeof(ac->hint));
        return;
    }

    {
        const char *params[MAX_EXPR_VARS];
        char param_storage[MAX_EXPR_VARS][16];
        int param_count = 0;

        if (find_defined_func_call_params(input, &after, params,
                                          &param_count, param_storage)) {
            build_param_hint_text(params, param_count, after,
                                  ac->hint, (int)sizeof(ac->hint));
        }
    }
}

void update_selected_autocomplete_preview(void) {
    ReplEditorInputView inp = editor_state_input();
    ReplAutocompleteState *ac = editor_state_autocomplete_mut();

    ac->ghost[0] = '\0';
    ac->hint[0] = '\0';

    if (ac->match_count <= 0 || !ac->insert_matches[ac->selected_idx])
        return;

    if (g_ac_mode == AC_MODE_FUNC_PREFIX) {
        const char *after = NULL;
        const char *params[MAX_EXPR_VARS];
        char param_storage[MAX_EXPR_VARS][16];
        int param_count = 0;

        snprintf(ac->ghost, sizeof(ac->ghost), "%s",
                 ac->insert_matches[ac->selected_idx] + inp.input_len);
        if (g_ac_func_matches[ac->selected_idx] && g_ac_func_matches[ac->selected_idx]->param_count > 0) {
            build_param_hint_text(g_ac_func_matches[ac->selected_idx]->params,
                                  g_ac_func_matches[ac->selected_idx]->param_count,
                                  "", ac->hint, (int)sizeof(ac->hint));
        } else if (find_defined_func_call_params(inp.input, &after, params,
                                                 &param_count, param_storage)) {
            ac->ghost[0] = '\0';
            build_param_hint_text(params, param_count, after,
                                  ac->hint, (int)sizeof(ac->hint));
        }
        return;
    }

    if (g_ac_mode == AC_MODE_POINT_PARAM ||
        g_ac_mode == AC_MODE_ENUM_ARG1 ||
        g_ac_mode == AC_MODE_ENUM_ARG2) {
        snprintf(ac->ghost, sizeof(ac->ghost), "%s%s",
                 ac->insert_matches[ac->selected_idx] + g_ac_token_len, g_ac_suffix);
    }
}

void update_autocomplete(void) {
    ReplEditorInputView inp = editor_state_input();
    ReplAutocompleteState *ac = editor_state_autocomplete_mut();
    const char *input = inp.input;
    int input_len = inp.input_len;

    clear_autocomplete_state();
    g_ac_mode = AC_MODE_NONE;
    g_ac_token_len = 0;
    g_ac_suffix[0] = '\0';

    if (input_len == 0) return;

    /* Only offer completions when cursor is at the end of input. */
    if (editor_cursor_pos() != input_len) return;

    /* glPointParameterfv enum completion (custom: 1 enum + 3 floats). */
    {
        static const char prefix[] = "glPointParameterfv(";
        const ReplEnumEntry *point_param_pnames = repl_point_param_pname_entries();
        int plen = (int)sizeof(prefix) - 1;
        if (strncmp(input, prefix, plen) == 0 && input_len > plen &&
            strchr(input + plen, ',') == NULL) {
            const char *after = input + plen;
            int alen = input_len - plen;
            for (int j = 0; point_param_pnames[j].name && ac->match_count < MAX_AC_MATCHES; j++) {
                if (strncmp(point_param_pnames[j].name, after, alen) == 0 &&
                    (int)strlen(point_param_pnames[j].name) > alen) {
                    ac->matches[ac->match_count] = point_param_pnames[j].name;
                    ac->insert_matches[ac->match_count] = point_param_pnames[j].name;
                    g_ac_func_matches[ac->match_count] = NULL;
                    ac->match_count++;
                }
            }
            if (ac->match_count > 0) {
                g_ac_mode = AC_MODE_POINT_PARAM;
                g_ac_token_len = alen;
                snprintf(g_ac_suffix, sizeof(g_ac_suffix), ", ");
                update_selected_autocomplete_preview();
                return;
            }
        }
    }

    /* Enum-based commands completion. */
    const ReplEnumCommandSpec *enum_cmds = repl_enum_command_specs();
    for (int i = 0; enum_cmds[i].name; i++) {
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%s(", enum_cmds[i].name);
        int plen = (int)strlen(prefix);

        if (strncmp(input, prefix, plen) == 0 && input_len > plen) {
            const char *after = input + plen;
            int alen = input_len - plen;
            char *comma = strchr(after, ',');

            if (!comma) {
                /* Complete enum1. */
                for (int j = 0; enum_cmds[i].enums1 && enum_cmds[i].enums1[j].name && ac->match_count < MAX_AC_MATCHES; j++) {
                    if (strncmp(enum_cmds[i].enums1[j].name, after, alen) == 0 &&
                        (int)strlen(enum_cmds[i].enums1[j].name) > alen) {
                        ac->matches[ac->match_count] = enum_cmds[i].enums1[j].name;
                        ac->insert_matches[ac->match_count] = enum_cmds[i].enums1[j].name;
                        g_ac_func_matches[ac->match_count] = NULL;
                        ac->match_count++;
                    }
                }
                if (ac->match_count > 0) {
                    g_ac_mode = AC_MODE_ENUM_ARG1;
                    g_ac_token_len = alen;
                    if (abs(enum_cmds[i].num_args) == 1)
                        snprintf(g_ac_suffix, sizeof(g_ac_suffix), ")");
                    else if (abs(enum_cmds[i].num_args) == 2)
                        snprintf(g_ac_suffix, sizeof(g_ac_suffix), ", ");
                    update_selected_autocomplete_preview();
                }
                return;
            } else {
                /* Complete enum2. */
                if (abs(enum_cmds[i].num_args) == 2 && enum_cmds[i].enums2) {
                    const char *arg2 = comma + 1;
                    while (*arg2 == ' ') arg2++;
                    int arg2_len = input_len - (int)(arg2 - input);

                    for (int j = 0; enum_cmds[i].enums2[j].name && ac->match_count < MAX_AC_MATCHES; j++) {
                        if (strncmp(enum_cmds[i].enums2[j].name, arg2, arg2_len) == 0 &&
                            (int)strlen(enum_cmds[i].enums2[j].name) > arg2_len) {
                            ac->matches[ac->match_count] = enum_cmds[i].enums2[j].name;
                            ac->insert_matches[ac->match_count] = enum_cmds[i].enums2[j].name;
                            g_ac_func_matches[ac->match_count] = NULL;
                            ac->match_count++;
                        }
                    }
                    if (ac->match_count > 0) {
                        g_ac_mode = AC_MODE_ENUM_ARG2;
                        g_ac_token_len = arg2_len;
                        snprintf(g_ac_suffix, sizeof(g_ac_suffix), ")");
                        update_selected_autocomplete_preview();
                    }
                    return;
                }
            }
        }
    }

    /* Complete function names. */
    const ReplFuncCompletion *completions = repl_func_completions();
    for (int i = 0; completions[i].insert_text && ac->match_count < MAX_AC_MATCHES; i++) {
        if (strncmp(completions[i].insert_text, input, (size_t)input_len) == 0 &&
            (int)strlen(completions[i].insert_text) > input_len) {
            ac->matches[ac->match_count] = completions[i].display_text;
            ac->insert_matches[ac->match_count] = completions[i].insert_text;
            g_ac_func_matches[ac->match_count] = &completions[i];
            ac->match_count++;
        }
    }
    if (ac->match_count > 0) {
        g_ac_mode = AC_MODE_FUNC_PREFIX;
        update_selected_autocomplete_preview();
        return;
    }

    update_input_param_hint();
}

void accept_autocomplete(void) {
    ReplAutocompleteState ac = editor_state_autocomplete();

    int ghost_len = (int)strlen(ac.ghost);
    {
        ReplEditorInputState *inp = editor_state_input_mut();
        if (inp->input_len + ghost_len < MAX_INPUT_LEN - 1) {
            strcat(inp->input, ac.ghost);
            inp->input_len += ghost_len;
            editor_cursor_pos_set(inp->input_len);
        }
    }
    clear_autocomplete_state();
    g_ac_mode = AC_MODE_NONE;
    g_ac_token_len = 0;
    g_ac_suffix[0] = '\0';
}

/* --- EditorCompletionProvider hookup ---
 *
 * Phase G commit 36. Editor input dispatch invokes
 * editor_completion_update / _update_selected_preview / _clear; we
 * register the existing repl_autocomplete entry points here so the
 * editor stays decoupled from REPL grammar specifics. */

static void repl_autocomplete_provider_clear(void) {
    clear_autocomplete_state();
}

static const EditorCompletionProvider g_repl_autocomplete_provider = {
    .update                  = update_autocomplete,
    .update_selected_preview = update_selected_autocomplete_preview,
    .clear                   = repl_autocomplete_provider_clear,
};

void repl_autocomplete_register_provider(void) {
    editor_completion_register(&g_repl_autocomplete_provider);
}
