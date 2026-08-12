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
#include "repl/state_views.h"
#include "repl/text_helpers.h"
#include "repl/command.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "repl/host_effects.h"
#include "repl/command_spec.h"
#include "editor/completion.h"
#include "app/glr_completion.h"
#include "subsystems/tutorial/tutorial.h"
static const ReplFuncCompletion *g_ac_func_matches[MAX_AC_MATCHES];
/* Func slot behind each match, or -1 when the candidate is not a user
 * function alias. Parallel to ac->matches / ac->insert_matches (and
 * permuted with them by sort_autocomplete_matches) so the preview can
 * build a parameter hint from the alias's live CMD_FUNC_DEF row. */
static int g_ac_alias_slots[MAX_AC_MATCHES];
/* Insert text for alias candidates (`drawCube(`). The alias table is
 * runtime REPL state and carries no trailing '(', so the candidate
 * strings are materialized here; ac->insert_matches borrows these
 * pointers, hence file-static storage rather than a local. Indexed by
 * func slot, so a rebuild can't invalidate a still-referenced entry. */
static char g_ac_alias_text[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX + 2];

typedef enum {
    AC_MODE_NONE = 0,
    AC_MODE_POINT_PARAM,
    AC_MODE_ENUM_SLOT,
    AC_MODE_FUNC_PREFIX
} AutocompleteMode;

static AutocompleteMode g_ac_mode = AC_MODE_NONE;
static int g_ac_token_len = 0;
/* Absolute offset into the live input buffer where the token being
 * completed starts. With end-of-input completion this is always
 * input_len - g_ac_token_len; with mid-line enum completion (cursor
 * parked at a token end with trailing args after it) it marks the
 * splice point the accept path replaces. */
static int g_ac_token_start = 0;
static char g_ac_suffix[8] = "";
/* Offset into the live input buffer where the completion prefix
 * starts. Non-zero when the user has typed `... = ` and the matcher
 * is treating the RHS as the prefix rather than the whole line.
 * The accept path (and update_selected_autocomplete_preview) uses
 * this to compute the ghost's `chars-already-typed` length. */
static int g_ac_input_offset = 0;

/* Case-insensitive prefix test: 1 if `cand` begins with the first `n`
 * characters of `pre` (ASCII case-folded). Lets autocomplete match a
 * command/enum typed in any case (e.g. "glco" -> "glColor3f("); the
 * canonical-cased candidate is what gets inserted on accept, so the
 * wrong-case prefix is corrected. A local helper rather than
 * strncasecmp keeps the old-gcc/-std=c99 build free of <strings.h>. */
static int ac_prefix_match_ci(const char *cand, const char *pre, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char a = (unsigned char)cand[i];
        if (a == '\0')
            return 0;
        if (tolower(a) != tolower((unsigned char)pre[i]))
            return 0;
    }
    return 1;
}

/* Gate for mid-line completion. Accepts a cursor tail that is nothing
 * but the remainder of the current call's argument list -
 * `[ws] [, args...] ) [;] [ws]` - so completion can finish a prior enum
 * arg (`glColorMaterial(GL_FR|, GL_DIFFUSE);`) without firing inside
 * arbitrary expressions. The closing `)` may be absent (call still
 * being typed). Depth covers nested (), {}, [] the same way the
 * param-hint walker does, so a trailing compound literal doesn't end
 * the scan early. */
static int tail_is_only_trailing_args(const char *p) {
    int depth = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ',' && *p != ')')
        return 0;
    for (; *p; p++) {
        char ch = *p;
        if (depth == 0 && ch == ')')
            break;
        if (ch == '(' || ch == '{' || ch == '[')      depth++;
        else if ((ch == ')' || ch == '}' || ch == ']') && depth > 0) depth--;
    }
    if (*p == '\0')
        return 1; /* still-unclosed call: `, GL_...` with no `)` yet */
    p++; /* past the top-level ')' */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ';') p++;
    while (*p == ' ' || *p == '\t') p++;
    return *p == '\0';
}

static void reset_ac_statics(void) {
    g_ac_mode = AC_MODE_NONE;
    g_ac_token_len = 0;
    g_ac_token_start = 0;
    g_ac_input_offset = 0;
    g_ac_suffix[0] = '\0';
    for (int i = 0; i < MAX_AC_MATCHES; i++) {
        g_ac_func_matches[i] = NULL;
        g_ac_alias_slots[i] = -1;
    }
}

/* Case-insensitive strcmp with a case-sensitive tiebreak, so the sort
 * order is stable across equal-fold names. Local helper keeps the
 * -std=c99 build free of <strings.h> (like ac_prefix_match_ci). */
static int ac_name_cmp(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
    }
    if (*a != *b)
        return (unsigned char)*a - (unsigned char)*b;
    return strcmp(a, b);
}

/* Sort the four parallel match arrays (display text, insert text,
 * func-completion pointer, and alias slot) alphabetically by display
 * text so the popup reads in order regardless of each source table's
 * native ordering. Insertion sort over the small (<= MAX_AC_MATCHES)
 * match set; called with selected_idx at its post-clear 0, before the
 * preview builds. */
static void sort_autocomplete_matches(EditorAutocompleteState *ac) {
    for (int i = 1; i < ac->match_count; i++) {
        const char *disp = ac->matches[i];
        const char *ins = ac->insert_matches[i];
        const ReplFuncCompletion *fn = g_ac_func_matches[i];
        int slot = g_ac_alias_slots[i];
        int j = i - 1;
        while (j >= 0 && ac_name_cmp(ac->matches[j], disp) > 0) {
            ac->matches[j + 1] = ac->matches[j];
            ac->insert_matches[j + 1] = ac->insert_matches[j];
            g_ac_func_matches[j + 1] = g_ac_func_matches[j];
            g_ac_alias_slots[j + 1] = g_ac_alias_slots[j];
            j--;
        }
        ac->matches[j + 1] = disp;
        ac->insert_matches[j + 1] = ins;
        g_ac_func_matches[j + 1] = fn;
        g_ac_alias_slots[j + 1] = slot;
    }
}

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

        if (!isspace((unsigned char)ch))
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

/* Signature of the live CMD_FUNC_DEF row for func slot `fn`. Returns 1
 * when the slot is defined in the current document, filling
 * param_storage / *count_out with its parameter list (a zero-parameter
 * definition still returns 1 with count 0). The document scan is also
 * the liveness test for alias candidates: repl_apply_alias_ops only ever
 * *sets* alias names, so a name can outlive the definition that bound
 * it and must not be offered once the row is gone. */
static int func_slot_signature(int fn, int *count_out,
                               char param_storage[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX]) {
    EditorBufferView text = editor_buffer_view();
    const GLCmd *cmds = repl_state_document_cmds();

    if (fn < 0 || fn >= REPL_FUNC_SLOT_COUNT)
        return 0;

    for (int i = 0; i < repl_state_document_count(); i++) {
        int parsed_fn = -1;
        int param_count = 0;
        if (!cmds[i].valid || cmds[i].type != CMD_FUNC_DEF)
            continue;
        if ((int)cmds[i].args[0] != fn)
            continue;
        {
            const char *func_text = editor_buffer_view_line(text, i);
            if (!parse_repl_func_signature(func_text ? func_text : "", &parsed_fn,
                                           param_storage, MAX_EXPR_VARS,
                                           &param_count))
                continue;
        }
        if (parsed_fn != fn)
            continue;
        if (count_out)
            *count_out = param_count;
        return 1;
    }

    return 0;
}

/* Parameter list for a call being typed at the head of `input`. Accepts
 * both spellings of the callee - bare `func3(` and an alias-named
 * `drawCube(` - via the shared name-token scanner, so aliases get the
 * same param hint their slot does. Only definitions with at least one
 * parameter report a match; a zero-parameter call has nothing to hint. */
static int find_defined_func_call_params(const char *input, const char **after_out,
                                         int *count_out,
                                         char param_storage[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX]) {
    const char *p = input;
    char ident[REPL_FUNC_NAME_MAX];
    int fn = -1;
    int param_count = 0;
    int kind = repl_scan_func_name_token(&p, &fn, ident);

    if (kind == 0)
        return 0;
    if (kind == 2) {
        fn = repl_func_alias_lookup_slot(ident);
        if (fn < 0)
            return 0;
    }
    if (*p != '(')
        return 0;

    if (after_out)
        *after_out = p + 1;

    if (!func_slot_signature(fn, &param_count, param_storage) || param_count <= 0)
        return 0;
    if (count_out)
        *count_out = param_count;
    return 1;
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
        char param_storage[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
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
        char param_storage[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
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
        } else if (g_ac_alias_slots[ac->selected_idx] >= 0) {
            /* Alias candidate: the parameter names come from the slot's
             * definition row, not the static table. Ghost stays (the
             * untyped tail of the name), matching the built-in shape. */
            if (func_slot_signature(g_ac_alias_slots[ac->selected_idx],
                                    &param_count, param_storage) && param_count > 0) {
                const char *params[MAX_EXPR_VARS];
                for (int j = 0; j < param_count; j++)
                    params[j] = param_storage[j];
                build_param_hint_text(params, param_count, "",
                                      ac->hint, (int)sizeof(ac->hint));
            }
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

/* While a tutorial is active AND the cursor sits on the expected
 * commit line, autocomplete matches/hints would compete with the
 * tutorial UI. Suppress normal completion and emit the
 * expected-command shadow text as the ghost suffix instead - the
 * active-input renderer already draws ghost in dimmed color
 * after the cursor. Empty input + tutorial active yields the
 * full expected line as the ghost. On any other line the user
 * is editing unrelated code, so fall through to the normal
 * autocomplete path.
 *
 * REQUIRE_VAR has no expected_commit_line (the user is free to
 * type the satisfying `name = expr;` anywhere), so the cursor
 * check above doesn't gate it. Show the synthesized ghost on any
 * row while a REQUIRE_VAR step is active - tutorial_shadow_suffix
 * builds the `name = target` string for that case.
 *
 * Returns 1 when the ghost was installed and normal completion
 * should stop. */
static int ac_try_tutorial_ghost(const char *raw_input,
                                 EditorAutocompleteState *ac) {
    int show_tutorial_ghost = 0;

    if (!tutorial_active())
        return 0;
    if (editor_state_edit_line() == tutorial_expected_commit_line())
        show_tutorial_ghost = 1;
    else if (tutorial_current_step_kind() == TUTORIAL_STEP_KIND_REQUIRE_VAR)
        show_tutorial_ghost = 1;
    if (!show_tutorial_ghost)
        return 0;

    tutorial_shadow_suffix(raw_input, ac->ghost, sizeof(ac->ghost));
    /* Sibling of the shadow suffix: when the typed input fully
     * matches the expected command, refresh the status bar with a
     * "press Enter or ';' to commit" reminder so the user knows the
     * line is ready. No-op while still typing or off the COMMAND
     * step. */
    tutorial_refresh_input_hint(raw_input);
    return 1;
}

/* Skip past a leading `... = ` so the matcher works on the RHS
 * rather than the whole line. Examples:
 *   "x = rand"        -> RHS = "rand"
 *   "float x = sin"   -> RHS = "sin"
 *   "A[0] = glC"      -> RHS = "glC"
 * Distinguishes assignment `=` from `==`/`<=`/`>=`/`!=` so a
 * partial conditional doesn't get treated as an assignment.
 *
 * Mid-line, only an `=` before the cursor can establish the
 * RHS-prefix context for the token being completed; scanning
 * past it would push g_ac_input_offset beyond the cursor and
 * break the offset math below. */
static void ac_resolve_rhs_context(const char *raw_input, int scan_len) {
    int last_eq = -1;

    for (int i = 0; i < scan_len; i++) {
        if (raw_input[i] != '=') continue;
        /* skip both chars of '==' so it isn't read as assignment */
        if (i + 1 < scan_len && raw_input[i + 1] == '=') { i++; continue; }
        if (i > 0 && (raw_input[i - 1] == '<' ||
                      raw_input[i - 1] == '>' ||
                      raw_input[i - 1] == '!' ||
                      raw_input[i - 1] == '=')) continue;
        last_eq = i;
    }
    if (last_eq >= 0) {
        int o = last_eq + 1;
        while (o < scan_len && isspace((unsigned char)raw_input[o])) o++;
        g_ac_input_offset = o;
    }
}

/* glPointParameterfv enum completion (custom: 1 enum + 3 floats).
 * Returns 1 when matches were installed and the preview updated. */
static int ac_try_point_param_completion(const char *raw_input,
                                         const char *input, int input_len,
                                         int interior, int cursor,
                                         EditorAutocompleteState *ac) {
    static const char prefix[] = "glPointParameterfv(";
    const ReplEnumEntry *point_param_pnames = repl_point_param_pname_entries();
    int plen = (int)sizeof(prefix) - 1;
    /* Completion stops at the cursor: mid-line the pname token ends
     * there and trailing `, const, linear, quadratic)` text is kept;
     * at end-of-input effective_len is the whole (post-`=`) input,
     * matching the historic behavior. Only the pname slot completes,
     * so any comma before the cursor disqualifies. */
    int effective_len = interior ? (cursor - g_ac_input_offset) : input_len;
    int alen = effective_len - plen;

    if (strncmp(input, prefix, plen) != 0 || input_len <= plen ||
        alen < 0 || memchr(input + plen, ',', (size_t)alen) != NULL)
        return 0;

    {
        const char *after = input + plen;
        for (int j = 0; point_param_pnames[j].name && ac->match_count < MAX_AC_MATCHES; j++) {
            if (ac_prefix_match_ci(point_param_pnames[j].name, after, alen) &&
                (int)strlen(point_param_pnames[j].name) > alen) {
                ac->matches[ac->match_count] = point_param_pnames[j].name;
                ac->insert_matches[ac->match_count] = point_param_pnames[j].name;
                g_ac_func_matches[ac->match_count] = NULL;
                ac->match_count++;
            }
        }
    }
    if (ac->match_count <= 0)
        return 0;

    g_ac_mode = AC_MODE_POINT_PARAM;
    g_ac_token_len = alen;
    g_ac_token_start = (int)((input + plen) - raw_input);
    snprintf(g_ac_suffix, sizeof(g_ac_suffix), "%s",
             interior ? "" : ", ");
    sort_autocomplete_matches(ac);
    update_selected_autocomplete_preview();
    return 1;
}

/* Enum-based commands completion (slot-indexed).
 *
 * One path for every positional enum slot: the active slot is the
 * count of top-level commas between '(' and the cursor; the token
 * being completed is the segment after the last such comma, ending
 * at the cursor (== end of input in the historic case; mid-line the
 * trailing `, args...)` text is left alone and the accept path
 * splices at the cursor). Matches come from def->args[slot].enums plus
 * an optional bitfield-all alias.
 * the accept suffix is ")" for the last enum slot, ", " otherwise,
 * and "" mid-line (the trailing text already has the separator).
 * abs(num_args) is the slot count so the custom glMaterialfv row
 * (num_args -2) still offers face/param completion even though the
 * parser skips it.
 *
 * Returns 1 when a command prefix matched (even with zero matches):
 * the caller must not fall through to function-name completion. */
static int ac_try_enum_slot_completion(const char *raw_input,
                                       const char *input, int input_len,
                                       int interior, int cursor,
                                       EditorAutocompleteState *ac) {
    const ReplEnumCommandSpec *enum_cmds = repl_enum_command_specs();

    for (int i = 0; enum_cmds[i].name; i++) {
        char prefix[64];
        int plen;
        int nargs;
        int effective_len;
        const char *after;
        const char *end;
        int slot = 0;
        const char *seg;
        int depth = 0;
        int seg_len;
        const ReplEnumEntry *tbl;

        snprintf(prefix, sizeof(prefix), "%s(", enum_cmds[i].name);
        plen = (int)strlen(prefix);

        if (strncmp(input, prefix, plen) != 0 || input_len <= plen)
            continue;

        nargs = abs(enum_cmds[i].num_args);
        if (nargs < 1)
            return 1; /* command matched but declares no enum slots */
        if (nargs > MAX_ENUM_ARGS)
            nargs = MAX_ENUM_ARGS;

        effective_len = interior ? (cursor - g_ac_input_offset) : input_len;
        if (effective_len < plen)
            return 1; /* cursor inside the command name - nothing to complete */

        /* Depth-aware top-level comma scan up to the cursor, mirroring
         * build_param_hint_text: commas inside nested (), {}, [] (a
         * compound literal, `cos(a, b)`) must not advance the slot. */
        after = input + plen;
        end = input + effective_len;
        seg = after;
        for (const char *q = after; q < end; q++) {
            char ch = *q;
            if (depth == 0 && ch == ',') { slot++; seg = q + 1; continue; }
            /* A `|` starts a new term inside the *same* slot: the
             * bitfield slot (glClear) is the only place it can
             * appear, and completion there should offer the mask
             * tokens again rather than treat `A | GL_D` as one
             * unmatchable prefix. */
            if (depth == 0 && ch == '|') { seg = q + 1; continue; }
            if (ch == '(' || ch == '{' || ch == '[')      depth++;
            else if ((ch == ')' || ch == '}' || ch == ']') && depth > 0) depth--;
        }
        if (slot >= nargs) {
            /* Past the last enum slot. For positive num_args the call
             * is done (no more args to suggest). For custom-parser
             * rows (negative num_args) there are still trailing args
             * the func-prefix param-hint walker can describe - fall
             * through to it so e.g. glMaterialfv(GL_FRONT, GL_AMBIENT,
             * still shows the compound-literal hint. */
            if (enum_cmds[i].num_args < 0)
                update_input_param_hint();
            return 1;
        }
        while (seg < end && *seg == ' ') seg++;
        seg_len = (int)(end - seg);
        if (seg_len < 0) seg_len = 0;

        tbl = enum_cmds[i].args[slot].enums;
        for (int j = 0; tbl && tbl[j].name && ac->match_count < MAX_AC_MATCHES; j++) {
            if (ac_prefix_match_ci(tbl[j].name, seg, seg_len) &&
                (int)strlen(tbl[j].name) > seg_len) {
                ac->matches[ac->match_count] = tbl[j].name;
                ac->insert_matches[ac->match_count] = tbl[j].name;
                g_ac_func_matches[ac->match_count] = NULL;
                ac->match_count++;
            }
        }
        {
            const char *alias = enum_cmds[i].args[slot].bitfield_all_alias;
            if (alias && ac->match_count < MAX_AC_MATCHES &&
                ac_prefix_match_ci(alias, seg, seg_len) &&
                (int)strlen(alias) > seg_len) {
                ac->matches[ac->match_count] = alias;
                ac->insert_matches[ac->match_count] = alias;
                g_ac_func_matches[ac->match_count] = NULL;
                ac->match_count++;
            }
        }
        if (ac->match_count > 0) {
            g_ac_mode = AC_MODE_ENUM_SLOT;
            g_ac_token_len = seg_len;
            g_ac_token_start = (int)(seg - raw_input);
            if (interior) {
                /* Mid-line the trailing text already supplies the
                 * separator / closing paren - accept splices the bare
                 * token at the cursor. */
                g_ac_suffix[0] = '\0';
            } else {
                /* Negative num_args (custom-parser rows like
                 * glMaterialfv, num_args = -2) means the function has
                 * *more* args after the last enum slot (e.g. the
                 * compound literal). Always use ", " as the accept
                 * suffix for those - closing with ")" would strand the
                 * user mid-call. Positive num_args are exhaustive: the
                 * last enum slot IS the last arg. */
                int more_args_after = (enum_cmds[i].num_args < 0);
                snprintf(g_ac_suffix, sizeof(g_ac_suffix), "%s",
                         (slot + 1 == nargs && !more_args_after) ? ")" : ", ");
            }
            sort_autocomplete_matches(ac);
            update_selected_autocomplete_preview();
        }
        return 1;
    }
    return 0;
}

static void update_autocomplete(void) {
    EditorInputView inp = editor_state_input();
    EditorAutocompleteState *ac = editor_state_autocomplete_mut();
    const char *raw_input = inp.input;
    int raw_input_len = inp.input_len;
    int cursor;
    int interior;
    const char *input;
    int input_len;
    const ReplFuncCompletion *completions;

    editor_state_autocomplete_clear();
    reset_ac_statics();

    if (ac_try_tutorial_ghost(raw_input, ac))
        return;

    if (raw_input_len == 0) return;

    /* Completions historically fire only with the cursor at the end of
     * input. One relaxation: the enum-slot modes also fire mid-line
     * when the cursor sits at the end of the token being completed and
     * everything after it is only trailing call arguments (e.g.
     * `glColorMaterial(GL_FR|, GL_DIFFUSE);` offers GL_FRONT). The
     * other modes (function names, param hints) keep the historic
     * end-of-input-only behavior - `interior` gates them off below. */
    cursor = editor_cursor_pos();
    interior = (cursor != raw_input_len);
    if (interior) {
        if (cursor <= 0) return;
        if (!tail_is_only_trailing_args(raw_input + cursor)) return;
    }

    ac_resolve_rhs_context(raw_input, interior ? cursor : raw_input_len);

    input = raw_input + g_ac_input_offset;
    input_len = raw_input_len - g_ac_input_offset;
    if (input_len <= 0) return;

    if (ac_try_point_param_completion(raw_input, input, input_len,
                                      interior, cursor, ac))
        return;

    if (ac_try_enum_slot_completion(raw_input, input, input_len,
                                    interior, cursor, ac))
        return;

    /* Mid-line completion is enum-slot-only: function-name completion
     * and the param hint keep their end-of-input-only behavior (their
     * accept/ghost mechanics are append-shaped). */
    if (interior)
        return;

    /* Complete function names. */
    completions = repl_func_completions();
    for (int i = 0; completions[i].insert_text && ac->match_count < MAX_AC_MATCHES; i++) {
        if (ac_prefix_match_ci(completions[i].insert_text, input, input_len) &&
            (int)strlen(completions[i].insert_text) > input_len) {
            ac->matches[ac->match_count] = completions[i].display_text;
            ac->insert_matches[ac->match_count] = completions[i].insert_text;
            g_ac_func_matches[ac->match_count] = &completions[i];
            ac->match_count++;
        }
    }

    /* Complete user function aliases (`drawCube(`). The bare funcN
     * spellings live in the static completion table; an alias is
     * runtime state, so its candidate text is materialized here. Both
     * are offered for an aliased slot - the alias sorts under its own
     * initial, so neither hides the other. */
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT && ac->match_count < MAX_AC_MATCHES; slot++) {
        const char *alias = repl_func_alias_get(slot);
        char *cand = g_ac_alias_text[slot];
        char params[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];

        if (!alias || !alias[0])
            continue;
        snprintf(cand, sizeof(g_ac_alias_text[slot]), "%s(", alias);
        if (!ac_prefix_match_ci(cand, input, input_len) ||
            (int)strlen(cand) <= input_len)
            continue;
        if (!func_slot_signature(slot, NULL, params))
            continue;

        ac->matches[ac->match_count] = cand;
        ac->insert_matches[ac->match_count] = cand;
        g_ac_func_matches[ac->match_count] = NULL;
        g_ac_alias_slots[ac->match_count] = slot;
        ac->match_count++;
    }

    if (ac->match_count > 0) {
        g_ac_mode = AC_MODE_FUNC_PREFIX;
        sort_autocomplete_matches(ac);
        update_selected_autocomplete_preview();
        return;
    }

    update_input_param_hint();
}

void glr_completion_accept_autocomplete(void) {
    const EditorAutocompleteState *ac = editor_state_autocomplete();
    EditorInputState *inp = editor_state_input_mut();

    if (ac->match_count > 0 && ac->insert_matches[ac->selected_idx]) {
        /* Accept a command/enum completion by REPLACING the already-typed
         * token with the canonical candidate, not by appending the ghost
         * suffix. The two are identical when the user typed the right
         * case, but replacement also corrects the case (matching is
         * case-insensitive) - e.g. "glco" -> "glColor3f(",
         * "gl_depth_test" -> "GL_DEPTH_TEST". `already_typed` is the
         * length of the token the candidate replaces, starting at
         * `tok_start`; `suffix` is the post-candidate text (", " / ")"
         * for an end-of-input enum slot, nothing for a function-name
         * completion or a mid-line enum slot). Any text after the token
         * (the trailing args of a mid-line enum completion) is preserved
         * by the splice; end-of-input completions have an empty tail, so
         * this is the historic replace-and-append there. */
        int already_typed;
        int tok_start;
        const char *suffix;
        if (g_ac_mode == AC_MODE_FUNC_PREFIX) {
            already_typed = inp->input_len - g_ac_input_offset;
            tok_start = g_ac_input_offset;
            suffix = "";
        } else { /* AC_MODE_ENUM_SLOT / AC_MODE_POINT_PARAM */
            already_typed = g_ac_token_len;
            tok_start = g_ac_token_start;
            suffix = g_ac_suffix;
        }
        if (already_typed < 0) already_typed = 0;
        if (tok_start < 0) tok_start = 0;
        if (tok_start > inp->input_len) tok_start = inp->input_len;
        if (already_typed > inp->input_len - tok_start)
            already_typed = inp->input_len - tok_start;

        const char *cand = ac->insert_matches[ac->selected_idx];
        int cand_len   = (int)strlen(cand);
        int suffix_len = (int)strlen(suffix);
        int tail_start = tok_start + already_typed;
        int tail_len   = inp->input_len - tail_start;
        int new_len    = tok_start + cand_len + suffix_len + tail_len;

        if (new_len < MAX_INPUT_LEN - 1) {
            memmove(inp->input + tok_start + cand_len + suffix_len,
                    inp->input + tail_start, (size_t)tail_len);
            memcpy(inp->input + tok_start, cand, (size_t)cand_len);
            memcpy(inp->input + tok_start + cand_len, suffix,
                   (size_t)suffix_len);
            inp->input_len = new_len;
            inp->input[new_len] = '\0';
            editor_cursor_pos_set(tok_start + cand_len + suffix_len);
        } else {
            repl_set_status_error("Input buffer full!");
        }
    } else {
        /* No command/enum match: append the ghost verbatim. This is the
         * tutorial shadow-suffix path (tutorial_shadow_suffix sets
         * ac->ghost with no matches - e.g. the REQUIRE_VAR declaration's
         * trailing instruction comment), and any other ghost-only case. */
        int ghost_len = (int)strlen(ac->ghost);
        if (inp->input_len + ghost_len < MAX_INPUT_LEN - 1) {
            memcpy(inp->input + inp->input_len, ac->ghost, (size_t)ghost_len + 1);
            inp->input_len += ghost_len;
            editor_cursor_pos_set(inp->input_len);
        } else {
            repl_set_status_error("Input buffer full!");
        }
    }
    editor_state_autocomplete_clear();
    reset_ac_statics();
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
 * provider-private statics. */

static void glr_completion_provider_clear(void) {
    reset_ac_statics();
}

static const EditorCompletionProvider g_glr_completion_provider = {
    .update                  = update_autocomplete,
    .update_selected_preview = update_selected_autocomplete_preview,
    .clear                   = glr_completion_provider_clear,
    .accept                  = glr_completion_accept_autocomplete,
};

void glr_completion_register_provider(void) {
    editor_completion_register(&g_glr_completion_provider);
}
