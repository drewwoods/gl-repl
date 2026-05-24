/*
 * glr_completion.c -- Controller-side completion provider.
 *
 * Cross-domain adapter: reads REPL state (command spec, predefined-
 * variable table, source CMD_FUNC_DEF entries) and pushes results into
 * the editor's completion system. Registers itself via
 * glr_completion_register_provider() at startup; the editor calls it
 * through editor_completion_update / _clear / etc.
 *
 * Runtime storage (matches, ghost, hint) lives on EditorState and is
 * accessed through the typed autocomplete facade.
 */
#include "editor/state.h"        /* EditorBufferView, EditorAutocompleteState, editor_state_* */
#include "repl/state_owners.h"
#include "repl/core_internal.h"
#include "repl/command_spec.h"
#include "editor/completion.h"
#include "app/glr_completion.h"
#include "subsystems/tutorial/tutorial.h"
static const ReplFuncCompletion *g_ac_func_matches[MAX_AC_MATCHES];

typedef enum {
    AC_MODE_NONE = 0,
    AC_MODE_POINT_PARAM,
    AC_MODE_ENUM_SLOT,
    AC_MODE_FUNC_PREFIX
} AutocompleteMode;

static AutocompleteMode g_ac_mode = AC_MODE_NONE;
static int g_ac_token_len = 0;
static char g_ac_suffix[8] = "";
/* Offset into the live input buffer where the completion prefix
 * starts. Non-zero when the user has typed `... = ` and the matcher
 * is treating the RHS as the prefix rather than the whole line.
 * The accept path (and update_selected_autocomplete_preview) uses
 * this to compute the ghost's `chars-already-typed` length. */
static int g_ac_input_offset = 0;

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

    /* Single combined depth covers (), {}, and []: a comma counts as
     * an arg separator only at top level, so compound literals like
     * `(GLfloat[]){r, g, b, a}` and nested function calls like
     * `cos(t + phase)` don't mis-advance the slot pointer. */
    for (const char *p = after; *p; p++) {
        unsigned char ch = (unsigned char)*p;

        if (depth == 0 && ch == ')')
            return;
        if (depth == 0 && ch == ',') {
            arg_index++;
            arg_has_text = 0;
            continue;
        }

        if (ch == '(' || ch == '{' || ch == '[')      depth++;
        else if ((ch == ')' || ch == '}' || ch == ']') && depth > 0) depth--;

        if (!isspace(ch))
            arg_has_text = 1;
    }

    if (arg_index > param_count)
        return;

    int next_param = arg_has_text ? arg_index + 1 : arg_index;
    if (next_param > param_count)
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
    EditorAutocompleteState *ac = editor_state_autocomplete_mut();
    if (builtin) {
        build_param_hint_text(builtin->params, builtin->param_count,
                              after, ac->hint, (int)sizeof(ac->hint));
        return;
    }

    {
        char param_storage[MAX_EXPR_VARS][16];
        int param_count = 0;

        if (find_defined_func_call_params(input, &after, &param_count, param_storage)) {
            const char *params[MAX_EXPR_VARS];
            for (int j = 0; j < param_count; j++)
                params[j] = param_storage[j];
            build_param_hint_text(params, param_count, after,
                                  ac->hint, (int)sizeof(ac->hint));
        }
    }
}

static void update_selected_autocomplete_preview(void) {
    EditorInputView inp = editor_state_input();
    EditorAutocompleteState *ac = editor_state_autocomplete_mut();

    ac->ghost[0] = '\0';
    ac->hint[0] = '\0';

    if (ac->match_count <= 0 || !ac->insert_matches[ac->selected_idx])
        return;

    if (g_ac_mode == AC_MODE_FUNC_PREFIX) {
        const char *after = NULL;
        char param_storage[MAX_EXPR_VARS][16];
        int param_count = 0;

        /* Already-typed length of the candidate is the post-`=` prefix
         * length, not the whole input length. */
        int typed_len = inp.input_len - g_ac_input_offset;
        if (typed_len < 0) typed_len = 0;
        snprintf(ac->ghost, sizeof(ac->ghost), "%s",
                 ac->insert_matches[ac->selected_idx] + typed_len);
        if (g_ac_func_matches[ac->selected_idx] && g_ac_func_matches[ac->selected_idx]->param_count > 0) {
            build_param_hint_text(g_ac_func_matches[ac->selected_idx]->params,
                                  g_ac_func_matches[ac->selected_idx]->param_count,
                                  "", ac->hint, (int)sizeof(ac->hint));
        } else if (find_defined_func_call_params(inp.input, &after, &param_count, param_storage)) {
            const char *params[MAX_EXPR_VARS];
            for (int j = 0; j < param_count; j++)
                params[j] = param_storage[j];
            ac->ghost[0] = '\0';
            build_param_hint_text(params, param_count, after,
                                  ac->hint, (int)sizeof(ac->hint));
        }
        return;
    }

    if (g_ac_mode == AC_MODE_POINT_PARAM ||
        g_ac_mode == AC_MODE_ENUM_SLOT) {
        snprintf(ac->ghost, sizeof(ac->ghost), "%s%s",
                 ac->insert_matches[ac->selected_idx] + g_ac_token_len, g_ac_suffix);
    }
}

static void update_autocomplete(void) {
    EditorInputView inp = editor_state_input();
    EditorAutocompleteState *ac = editor_state_autocomplete_mut();
    const char *raw_input = inp.input;
    int raw_input_len = inp.input_len;

    editor_state_autocomplete_clear();
    g_ac_mode = AC_MODE_NONE;
    g_ac_token_len = 0;
    g_ac_input_offset = 0;
    g_ac_suffix[0] = '\0';

    /* While a tutorial is active AND the cursor sits on the expected
     * commit line, autocomplete matches/hints would compete with the
     * tutorial UI. Suppress normal completion and emit the
     * expected-command shadow text as the ghost suffix instead — the
     * active-input renderer already draws ghost in dimmed color
     * after the cursor. Empty input + tutorial active yields the
     * full expected line as the ghost. On any other line the user
     * is editing unrelated code, so fall through to the normal
     * autocomplete path. */
    if (tutorial_active() &&
        editor_state_edit_line() == tutorial_expected_commit_line()) {
        tutorial_shadow_suffix(raw_input, ac->ghost, sizeof(ac->ghost));
        /* Sibling of the shadow suffix: when the typed input fully
         * matches the expected command, refresh the status bar with a
         * "press Enter or ';' to commit" reminder so the user knows the
         * line is ready. No-op while still typing or off the COMMAND
         * step. */
        tutorial_refresh_input_hint(raw_input);
        return;
    }

    if (raw_input_len == 0) return;

    /* Only offer completions when cursor is at the end of input. */
    if (editor_cursor_pos() != raw_input_len) return;

    /* Skip past a leading `... = ` so the matcher works on the RHS
     * rather than the whole line. Examples:
     *   "x = rand"        -> RHS = "rand"
     *   "float x = sin"   -> RHS = "sin"
     *   "A[0] = glC"      -> RHS = "glC"
     * Distinguishes assignment `=` from `==`/`<=`/`>=`/`!=` so a
     * partial conditional doesn't get treated as an assignment. */
    {
        int last_eq = -1;
        for (int i = 0; i < raw_input_len; i++) {
            if (raw_input[i] != '=') continue;
            /* skip both chars of '==' so it isn't read as assignment */
            if (i + 1 < raw_input_len && raw_input[i + 1] == '=') { i++; continue; }
            if (i > 0 && (raw_input[i - 1] == '<' ||
                          raw_input[i - 1] == '>' ||
                          raw_input[i - 1] == '!' ||
                          raw_input[i - 1] == '=')) continue;
            last_eq = i;
        }
        if (last_eq >= 0) {
            int o = last_eq + 1;
            while (o < raw_input_len && isspace((unsigned char)raw_input[o])) o++;
            g_ac_input_offset = o;
        }
    }

    const char *input = raw_input + g_ac_input_offset;
    int input_len = raw_input_len - g_ac_input_offset;
    if (input_len <= 0) return;

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

    /* Enum-based commands completion (slot-indexed).
     *
     * One path for every positional enum slot: the active slot is the
     * count of top-level commas between '(' and the cursor; the token
     * being completed is the trailing segment after the last comma.
     * Matches come from def->args[slot].enums; the accept suffix is
     * ")" for the last enum slot, ", " otherwise. abs(num_args) is the
     * slot count so the custom glMaterialfv row (num_args -2) still
     * offers face/param completion even though the parser skips it. */
    const ReplEnumCommandSpec *enum_cmds = repl_enum_command_specs();
    for (int i = 0; enum_cmds[i].name; i++) {
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%s(", enum_cmds[i].name);
        int plen = (int)strlen(prefix);

        if (strncmp(input, prefix, plen) != 0 || input_len <= plen)
            continue;

        int nargs = abs(enum_cmds[i].num_args);
        if (nargs < 1)
            return; /* command matched but declares no enum slots */
        if (nargs > MAX_ENUM_ARGS)
            nargs = MAX_ENUM_ARGS;

        const char *after = input + plen;
        int slot = 0;
        const char *seg = after;
        for (const char *q = after; *q; q++) {
            if (*q == ',') { slot++; seg = q + 1; }
        }
        if (slot >= nargs) {
            /* Past the last enum slot. For positive num_args the call
             * is done (no more args to suggest). For custom-parser
             * rows (negative num_args) there are still trailing args
             * the func-prefix param-hint walker can describe — fall
             * through to it so e.g. glMaterialfv(GL_FRONT, GL_AMBIENT,
             * still shows the compound-literal hint. */
            if (enum_cmds[i].num_args < 0)
                update_input_param_hint();
            return;
        }
        while (*seg == ' ') seg++;
        int seg_len = input_len - (int)(seg - input);
        if (seg_len < 0) seg_len = 0;

        const ReplEnumEntry *tbl = enum_cmds[i].args[slot].enums;
        for (int j = 0; tbl && tbl[j].name && ac->match_count < MAX_AC_MATCHES; j++) {
            if (strncmp(tbl[j].name, seg, (size_t)seg_len) == 0 &&
                (int)strlen(tbl[j].name) > seg_len) {
                ac->matches[ac->match_count] = tbl[j].name;
                ac->insert_matches[ac->match_count] = tbl[j].name;
                g_ac_func_matches[ac->match_count] = NULL;
                ac->match_count++;
            }
        }
        if (ac->match_count > 0) {
            g_ac_mode = AC_MODE_ENUM_SLOT;
            g_ac_token_len = seg_len;
            /* Negative num_args (custom-parser rows like glMaterialfv,
             * num_args = -2) means the function has *more* args after
             * the last enum slot (e.g. the compound literal). Always
             * use ", " as the accept suffix for those — closing with
             * ")" would strand the user mid-call. Positive num_args
             * are exhaustive: the last enum slot IS the last arg. */
            int more_args_after = (enum_cmds[i].num_args < 0);
            snprintf(g_ac_suffix, sizeof(g_ac_suffix), "%s",
                     (slot + 1 == nargs && !more_args_after) ? ")" : ", ");
            update_selected_autocomplete_preview();
        }
        return;
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

void glr_completion_accept_autocomplete(void) {
    EditorAutocompleteState ac = editor_state_autocomplete();

    int ghost_len = (int)strlen(ac.ghost);
    {
        EditorInputState *inp = editor_state_input_mut();
        if (inp->input_len + ghost_len < MAX_INPUT_LEN - 1) {
            strcat(inp->input, ac.ghost);
            inp->input_len += ghost_len;
            editor_cursor_pos_set(inp->input_len);
        }
    }
    editor_state_autocomplete_clear();
    g_ac_mode = AC_MODE_NONE;
    g_ac_token_len = 0;
    g_ac_suffix[0] = '\0';
}

/* --- EditorCompletionProvider hookup ---
 *
 * Editor input dispatch invokes editor_completion_update /
 * _update_selected_preview / _clear; we register the existing
 * glr_completion entry points here so the editor stays decoupled from
 * REPL grammar specifics.
 *
 * editor_completion_clear() owns the slice wipe (it lives on
 * EditorState), so this provider hook only resets the
 * provider-private statics.
 *
 * (Wired in Phase G commit 36.) */

static void glr_completion_provider_clear(void) {
    g_ac_mode = AC_MODE_NONE;
    g_ac_token_len = 0;
    g_ac_suffix[0] = '\0';
}

static const EditorCompletionProvider g_glr_completion_provider = {
    .update                  = update_autocomplete,
    .update_selected_preview = update_selected_autocomplete_preview,
    .clear                   = glr_completion_provider_clear,
};

void glr_completion_register_provider(void) {
    editor_completion_register(&g_glr_completion_provider);
}
