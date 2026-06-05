/*
 * src/repl/import.c — Reader half of the export/import file format.
 *
 * Split out of src/repl/export.c (audit #69 in
 * plans/active/src-repl-code-smell-audit-2.md). The writer (file emit,
 * code-panel dump, header-line refresh) stays in export.c. This file
 * owns:
 *
 *   - The pending-cfg accumulator (parse_cfg drains it into the cfg
 *     bridge once a header batch is parsed).
 *   - The deferred-@var table (workspace-header @var values that must
 *     survive a snippet's // @declare round-trip).
 *   - The workspace header directive readers
 *     (parse_workspace_dir / parse_scene_name / parse_var /
 *     parse_func_alias / parse_cfg) and the parser dispatcher
 *     repl_state_parse_workspace_header_line.
 *   - The camera bridge import-line consumers.
 *   - The snippet directive table (@declare reader).
 *   - The C-to-REPL line translators (for-headers, function headers,
 *     tess lines, point-parameter lines, label() lines, predef-decl
 *     scans) and the import driver
 *     repl_export_load_from_file.
 *
 * The IMPORT_EXPORT_STATE macros are duplicated verbatim from
 * src/repl/export.c so each TU can reach the shared workspace-header
 * / pending-name / cam-line storage owned by the state-owner facade.
 * The two TUs do NOT share static helpers — the WORKSPACE_DIRECTIVES
 * dispatcher table here pairs each name with its reader only, and
 * export.c carries its own emit-only table.
 */
#include <stdarg.h>
#include <stdio.h>
#include "repl/export.h"          /* public reader API */
#include "source_document.h"      /* source_document_insert_line */
#include "repl/load.h"            /* repl_load_apply_line — step 5b */
#include "config.h"
#include "repl/command_store.h"
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/executor.h"
#include "repl/parser.h"
#include "repl/pipeline.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"

/* Mirrors the IMPORT_EXPORT_STATE block at the top of src/repl/export.c.
 * The two definitions must stay in lockstep — both TUs reach the same
 * state-owner facade (repl_state_import_export_mut). When you change
 * the macro list here, change it there too. */
#define IMPORT_EXPORT_STATE (repl_state_import_export_mut())
#define g_workspace_header_lines (IMPORT_EXPORT_STATE->workspace_header_lines)
#define g_workspace_header_line_count (IMPORT_EXPORT_STATE->workspace_header_line_count)
#define g_render_state_lines (IMPORT_EXPORT_STATE->render_state_lines)
#define g_cam_lines (IMPORT_EXPORT_STATE->cam_lines)
#define g_export_scene_name_hint (IMPORT_EXPORT_STATE->export_scene_name_hint)
#define g_pending_scene_name (IMPORT_EXPORT_STATE->pending_scene_name)
#define g_pending_workspace_dir (IMPORT_EXPORT_STATE->pending_workspace_dir)

#include "repl/cfg_baseline.h"

#define g_import_cfg_bridge (repl_config_bridge())

/* Pending @cfg accumulator: parse_cfg() during import populates this; the
 * import driver drains it via the bridge after parse completes. */
static ReplConfigBag g_import_cfg_accumulator;

static void import_cfg_accumulator_reset(void) {
    repl_config_bag_clear(&g_import_cfg_accumulator);
}

static void import_cfg_accumulator_apply_and_reset(void) {
    if (g_import_cfg_bridge && g_import_cfg_bridge->apply &&
        g_import_cfg_accumulator.count > 0) {
        g_import_cfg_bridge->apply(&g_import_cfg_accumulator);
    }
    import_cfg_accumulator_reset();
}

void repl_export_apply_pending_cfg(void) {
    import_cfg_accumulator_apply_and_reset();
}

/* Deferred @var values: set by parse_workspace_header_line alongside the
 * normal auto-declare+set-value path.  load_from_file re-applies them after
 * the snippet is processed so that // @declare markers in the snippet can
 * undeclare and re-declare variables (creating CMD_VAR_DECLARE commands)
 * without losing the saved values from the workspace header. */
#define MAX_DEFERRED_VAR_VALUES MAX_PREDEF_VARS
typedef struct { char name[REPL_PREDEF_NAME_MAX]; float value; } DeferredVar;
static DeferredVar g_deferred_var_values[MAX_DEFERRED_VAR_VALUES];
static int         g_deferred_var_count = 0;

static int import_first_non_decl(const ReplCommandStore *store) {
    int pos = 0;

    if (!store || !store->cmds || !store->count)
        return 0;

    while (pos < *store->count &&
           store->cmds[pos].type == CMD_VAR_DECLARE)
        pos++;
    return pos;
}

static void import_format_decl_float(char *buf, size_t n, float v) {
    repl_format_source_float(buf, (int)n, v);
}

/* ========================================================================= */
/* Workspace header directive table — reader side.                            */
/*                                                                            */
/* The writer half (refresh_workspace_header_lines + emit_*) lives in         */
/* src/repl/export.c with its own emit-only directive table. The two halves   */
/* are intentionally independent so neither TU has to forward-declare into    */
/* the other; both walks key off the same `@name` strings and a regression    */
/* test catches any drift.                                                    */
/* ========================================================================= */

typedef int (*WorkspaceParseFn)(const char *args);
typedef int (*SnippetParseFn)(const char *args, int *loaded,
                              int *warnings, int *edit_line_inout);

typedef struct {
    const char       *name;      /* directive name without leading `@` */
    size_t            name_len;
    WorkspaceParseFn  parse;     /* parse(args-after-name-and-space) */
} WorkspaceDirective;

typedef struct {
    const char    *name;
    size_t         name_len;
    SnippetParseFn parse;
} SnippetDirective;

static const char k_snippet_directive_declare[] = "declare";

/* Emit one import diagnostic to stderr with the shared "Warning: " prefix
 * and trailing newline. Centralises the format so the @var / @func /
 * @declare / scene-name / workspace-dir overflow notices read identically
 * (and a future routing change has a single site). The header-directive
 * parsers (parse_var / parse_func_alias / ...) can't reach
 * ImportState.warnings, so these print but are not added to the
 * "(N warnings)" tally; the snippet parsers additionally bump that counter. */
static void import_warn(const char *fmt, ...) {
    va_list ap;
    fputs("Warning: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* --- workspace-dir --------------------------------------------------------- */

static int parse_workspace_dir(const char *args) {
    size_t char_idx = 0;
    while (*args && char_idx < REPL_WORKSPACE_DIR_MAX - 1)
        g_pending_workspace_dir[char_idx++] = *args++;
    g_pending_workspace_dir[char_idx] = '\0';
    /* More source left over the cap means the path was clipped — a
     * truncated path points at the wrong directory, so don't do it silently. */
    if (*args)
        import_warn("@workspace-dir path exceeds the %d-char limit; truncated",
                    (int)REPL_WORKSPACE_DIR_MAX - 1);
    while (char_idx > 0 && isspace((unsigned char)g_pending_workspace_dir[char_idx - 1]))
        g_pending_workspace_dir[--char_idx] = '\0';
    return 1;
}

/* --- scene-name ------------------------------------------------------------ */

static int parse_scene_name(const char *args) {
    size_t char_idx = 0;
    while (*args && char_idx < USER_SCENE_NAME_MAX - 1)
        g_pending_scene_name[char_idx++] = *args++;
    g_pending_scene_name[char_idx] = '\0';
    if (*args)
        import_warn("@scene-name exceeds the %d-char limit; truncated to '%s'",
                    (int)USER_SCENE_NAME_MAX - 1, g_pending_scene_name);
    while (char_idx > 0 && isspace((unsigned char)g_pending_scene_name[char_idx - 1]))
        g_pending_scene_name[--char_idx] = '\0';
    return 1;
}

/* --- var ------------------------------------------------------------------- */

static int parse_var(const char *args) {
    const char *p = args;
    char name[REPL_PREDEF_NAME_MAX];
    int name_char_idx = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
           name_char_idx < (int)sizeof(name) - 1)
        name[name_char_idx++] = *p++;
    name[name_char_idx] = '\0';
    /* Name longer than the predef-name buffer: it was clipped to `name`.
     * Warn, then skip the overflow so the `= value` still parses and the
     * (truncated) var registers — in-code references truncate identically,
     * so the variable stays usable, just under the shorter name. Pre-fix the
     * leftover chars broke the `=` parse and the whole @var was dropped. */
    if (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        import_warn("@var name exceeds the %d-char limit; truncated to '%s'",
                    REPL_PREDEF_NAME_MAX - 1, name);
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;
    ExprCtx ctx = { p, NULL, 0, NULL, 0 };
    float val = repl_eval_expr(&ctx);
    int idx = repl_eval_find_predef_var_idx(name);
    if (idx < 0) {
        char err[REPL_DIAG_TEXT_MAX];
        err[0] = '\0';
        if (!repl_eval_declare_predef_var(name, err, sizeof(err))) {
            /* e.g. reserved name or predef table full. Pre-fix this `err`
             * was computed and thrown away — surface it. */
            import_warn("@var '%s' could not be declared: %s",
                        name, err[0] ? err : "rejected");
            return 0;
        }
        idx = repl_eval_find_predef_var_idx(name);
        if (idx < 0)
            return 0;
    }
    g_predef_vars_mut[idx].value = val;
    /* Also defer the value so that if a // @declare marker in the snippet
     * undeclares and re-declares this var, the value is restored afterwards
     * (see load_from_file deferred-apply step). */
    if (g_deferred_var_count < MAX_DEFERRED_VAR_VALUES) {
        repl_copy_string_fits(g_deferred_var_values[g_deferred_var_count].name,
                              sizeof(g_deferred_var_values[0].name),
                              name);
        g_deferred_var_values[g_deferred_var_count].value = val;
        g_deferred_var_count++;
    }
    return 1;
}

/* --- func aliases ---------------------------------------------------------- */

static int parse_func_alias(const char *args) {
    const char *p = args;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p)) return 0;
    int slot = 0;
    while (isdigit((unsigned char)*p)) {
        slot = slot * 10 + (*p - '0');
        p++;
    }
    if (slot < 0 || slot >= REPL_FUNC_SLOT_COUNT) return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!isalpha((unsigned char)*p) && *p != '_') return 0;
    char name[REPL_FUNC_NAME_MAX];
    int len = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
           len < REPL_FUNC_NAME_MAX - 1) {
        name[len++] = *p++;
    }
    name[len] = '\0';
    if (len == 0) return 0;
    /* The alias name is longer than a slot can hold: it was just truncated,
     * so the matching `static void <name>(...)` definition below will fail to
     * map back to this slot and be dropped. Warn rather than drop in silence
     * (the historical behaviour) — the only fix is a shorter func name or a
     * larger REPL_FUNC_NAME_MAX. */
    if (*p && (isalnum((unsigned char)*p) || *p == '_'))
        import_warn("@func %d alias '%s' exceeds the %d-char name limit; "
                    "its definition will be dropped on import",
                    slot, name, REPL_FUNC_NAME_MAX - 1);
    repl_func_alias_set(slot, name);
    return 1;
}

/* --- cfg ------------------------------------------------------------------- */

static int parse_cfg(const char *args) {
    char slug[32];
    const char *p = NULL;
    if (!repl_config_extract_slug(args, slug, sizeof(slug), &p))
        return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    /* Copy the raw value token (trimmed). Used to be strtol →
     * snprintf("%d") here; now passed through verbatim so the bridge
     * can resolve symbolic enum names like "GRID_THEME_RADAR"
     * alongside legacy integer-form workspaces. The bridge's
     * resolve_text / strtol fallback runs at apply time. */
    char value[REPL_CFG_VALUE_MAX];
    int vi = 0;
    while (*p && !isspace((unsigned char)*p) &&
           vi < (int)sizeof(value) - 1) {
        value[vi++] = *p++;
    }
    value[vi] = '\0';
    if (vi == 0) return 0;
    repl_config_bag_set(&g_import_cfg_accumulator, slug, value);
    return 1;
}

/* --- directive table (reader half) ----------------------------------------- */

#define WS_DIR(name, parse_fn) { name, sizeof(name) - 1, parse_fn }

static const WorkspaceDirective WORKSPACE_DIRECTIVES[] = {
    WS_DIR("scene-name",    parse_scene_name),
    WS_DIR("workspace-dir", parse_workspace_dir),
    WS_DIR("var",           parse_var),
    WS_DIR("func",          parse_func_alias),
    WS_DIR("cfg",           parse_cfg),
};
#define WORKSPACE_DIRECTIVE_COUNT \
    ((int)(sizeof(WORKSPACE_DIRECTIVES) / sizeof(WORKSPACE_DIRECTIVES[0])))

#undef WS_DIR

#define SNIPPET_DIR(name, parse_fn) \
    { name, sizeof(name) - 1, parse_fn }

int repl_state_parse_workspace_header_line(const char *line) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p[0] != '/' || p[1] != '/') return 0;
    p += 2;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '@') return 0;
    p++;

    /* Banner line: `// @workspace: REPL state ...` - recognised, no payload. */
    if (strncmp(p, "workspace:", 10) == 0) return 1;
    if (strncmp(p, "workspace", 9) == 0 &&
        !isalnum((unsigned char)p[9]) && p[9] != '_' && p[9] != '-')
        return 1;

    for (int dir_idx = 0; dir_idx < WORKSPACE_DIRECTIVE_COUNT; dir_idx++) {
        const WorkspaceDirective *d = &WORKSPACE_DIRECTIVES[dir_idx];
        if (strncmp(p, d->name, d->name_len) != 0) continue;
        unsigned char follow = (unsigned char)p[d->name_len];
        if (follow != '\0' && !isspace(follow)) continue;
        const char *args = p + d->name_len;
        while (*args && isspace((unsigned char)*args)) args++;
        return d->parse(args);
    }
    return 0;
}

/* The camera-block parser state machine lives in the bridge
 * implementation (glr_camera_export.c). src/repl/import.c just delegates
 * import-side line consumption and reset to the bridge (implemented in
 * step 4a). */
static void import_cam_parser_reset(void) {
    const ReplExportCameraBridge *bridge = repl_export_camera_bridge();
    if (bridge && bridge->reset_import)
        bridge->reset_import();
}

static int import_parse_cam_line(const char *text) {
    const ReplExportCameraBridge *bridge = repl_export_camera_bridge();
    if (!bridge || !bridge->try_consume_import_line)
        return 0;
    return bridge->try_consume_import_line(text);
}

static int import_parse_predef_decl(const char *line) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    /* Optional canonical `static ` prefix (see format_decl_text). */
    if (strncmp(p, "static", 6) == 0 && isspace((unsigned char)p[6])) {
        p += 6;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (strncmp(p, "float ", 6) != 0) return 0;
    p += 6;

    int updated = 0;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;

        char name[REPL_PREDEF_NAME_MAX];
        int ni = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
               ni < (int)sizeof(name) - 1)
            name[ni++] = *p++;
        name[ni] = '\0';
        if (ni == 0) break;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '=') break;
        p++;

        ExprCtx ctx = { p, NULL, 0, NULL, 0 };
        float val = repl_eval_expr(&ctx);
        p = ctx.p;

        /* Look up and update the predefined variable value. */
        for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
            if (strcmp(g_predef_vars[var_idx].name, name) == 0) {
                g_predef_vars_mut[var_idx].value = val;
                updated = 1;
                break;
            }
        }

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') {
            p++;
            continue;
        }
        break;
    }
    return updated;
}

/* 1 if `s` carries a whole-token `@tune` (the trailing knob tag the exporter
 * appends to a `// @declare` marker), else 0. Bounded by whitespace/end so it
 * never matches a name like `@tuned`. */
static int declare_args_have_tune_tag(const char *s) {
    for (const char *q = s; (q = strstr(q, "@tune")) != NULL; q += 5) {
        char prev = (q == s) ? ' ' : q[-1];
        char next = q[5];
        if (isspace((unsigned char)prev) &&
            (next == '\0' || isspace((unsigned char)next)))
            return 1;
    }
    return 0;
}

/* Parse a snippet-scoped `@declare` marker written by write_canonical_cmd_as_c()
 * and reconstruct the corresponding CMD_VAR_DECLARE command. Variables that are
 * already registered in g_predef_vars (e.g. from @var auto-declare or from
 * declare_test_vars in tests) are kept at their current indices so that any
 * CMD_VAR_ASSIGN commands already loaded with those indices remain valid.
 * Vars not yet registered are declared. */
static int parse_snippet_declare(const char *args, int *loaded,
                                 int *warnings, int *edit_line_inout) {
    const char *p = args;
    while (*p && isspace((unsigned char)*p)) p++;

    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type      = CMD_VAR_DECLARE;
    cmd.valid     = 1;
    int count     = 0;

    /* Names that THIS call newly declares in the predef table — kept
     * separate so a later source/cmd-store insert failure can undeclare
     * them without touching names that were already registered (e.g.
     * by @var auto-declare or by test setup). */
    char newly_declared[MAX_NAMES_PER_DECL][REPL_PREDEF_NAME_MAX];
    int  new_count = 0;

    /* Build the canonical source string and collect names. Tokens take the
     * form `name` or `name=value`; the optional value carries the inline
     * initializer through the round-trip so the canonical decl text matches
     * the original source byte-for-byte. */
    char decl_line[MAX_LINE_LEN];
    /* Canonical form matches format_decl_text: `  static float ...`.
     * The exporter writes file-scope `static float` lines anyway, so
     * the keyword here reinforces that lifetime in the code panel. */
    int off = snprintf(decl_line, sizeof(decl_line), "  static float");
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (!isalpha((unsigned char)*p) && *p != '_') break;
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int len = (int)(p - start);
        if (len <= 0) break;  /* no more names */
        if (len >= REPL_PREDEF_NAME_MAX) {
            import_warn("@declare name '%.*s' exceeds the %d-char limit; "
                        "dropped (with any names after it)",
                        len, start, REPL_PREDEF_NAME_MAX - 1);
            if (warnings) (*warnings)++;
            break;
        }
        if (count >= MAX_NAMES_PER_DECL) {
            import_warn("@declare lists more than %d names; the rest are dropped",
                        MAX_NAMES_PER_DECL);
            if (warnings) (*warnings)++;
            break;
        }
        char name[REPL_PREDEF_NAME_MAX];
        memcpy(name, start, (size_t)len);
        name[len] = '\0';
        /* Optional `=value` rider. */
        int has_init = 0;
        float init_val = 0;
        if (*p == '=') {
            p++;
            char *endp = NULL;
            init_val = strtof(p, &endp);
            if (endp && endp != p) {
                has_init = 1;
                p = endp;
            }
        }
        /* Declare the var if not yet registered. Record newly-declared
         * names so we can undeclare them if a downstream step (name
         * copy overflow, source/cmd-store insert failure) bails. */
        int was_registered = (repl_eval_find_predef_var_idx(name) >= 0);
        if (!was_registered) {
            if (!repl_eval_declare_predef_var(name, NULL, 0)) {
                if (warnings) (*warnings)++;
                continue;
            }
            memcpy(newly_declared[new_count], name, (size_t)len);
            newly_declared[new_count][len] = '\0';
            new_count++;
        }
        if (!repl_copy_string_fits(cmd.payload.decl.names[count],
                                   sizeof(cmd.payload.decl.names[count]), name)) {
            /* Roll back the just-declared entry so the predef table
             * doesn't carry a name the document never gets. */
            if (!was_registered) {
                repl_eval_undeclare_predef_var(name);
                new_count--;
            }
            if (warnings) (*warnings)++;
            continue;
        }
        off += snprintf(decl_line + off, sizeof(decl_line) - (size_t)off,
                        count == 0 ? " %.*s" : ", %.*s", len, start);
        if (has_init) {
            char vbuf[32];
            import_format_decl_float(vbuf, sizeof(vbuf), init_val);
            off += snprintf(decl_line + off, sizeof(decl_line) - (size_t)off,
                            " = %s", vbuf);
        }
        count++;
    }
    if (count == 0) {
        /* Nothing accepted; undeclare any names this call registered. */
        for (int i = 0; i < new_count; i++)
            repl_eval_undeclare_predef_var(newly_declared[i]);
        return 0;
    }
    /* Re-attach the @tune knob tag so the reconstructed decl line is the
     * in-app source of truth (badge + re-export both read the trailing
     * comment). */
    if (declare_args_have_tune_tag(args))
        snprintf(decl_line + off, sizeof(decl_line) - (size_t)off, "; // @tune");
    else
        snprintf(decl_line + off, sizeof(decl_line) - (size_t)off, ";");
    cmd.payload.decl.count = count;

    /* Insert the command directly, bypassing editor_try_commit_float_decl so we
     * don't reject vars that are already registered.  Keep declarations in the
     * same leading zone used by interactive float declarations, even though
     * exported // @declare markers are encountered later in the snippet. */
    {
        ReplCommandStore store = repl_command_store_live();
        int decl_pos = import_first_non_decl(&store);

        /* Source text first; cmd-store second so a text-write failure
         * leaves no orphan GLCmd row. cmd-store failure rolls the
         * inserted line back AND undeclares the newly-registered names
         * so the eval table can't drift out of sync with the document. */
        if (!source_document_insert_line(decl_pos, decl_line)) {
            for (int i = 0; i < new_count; i++)
                repl_eval_undeclare_predef_var(newly_declared[i]);
            if (warnings) (*warnings)++;
            return 1;
        }
        /* Caller-owned cursor threaded through ImportState
         * (implemented in phase 3.6.4; see the
         * edit-line-ownership plan doc). */
        ReplStoreMutOpts opts = {
            .flags        = REPL_COMMAND_STORE_ADJUST_EDIT_LINE,
            .cursor_inout = edit_line_inout,
        };
        if (!repl_command_store_insert_one(
                &store, decl_pos, &cmd, &opts)) {
            SourceTextChange rollback = {
                .kind         = SOURCE_TEXT_DELETE_RANGE,
                .pos          = decl_pos,
                .count        = 1,
                .delete_pos   = -1,
                .delete_count = 0,
            };
            source_document_apply_change(&rollback);
            for (int i = 0; i < new_count; i++)
                repl_eval_undeclare_predef_var(newly_declared[i]);
            if (warnings) (*warnings)++;
            return 1;
        }
        (*loaded)++;
    }
    return 1;
}

static const SnippetDirective SNIPPET_DIRECTIVES[] = {
    SNIPPET_DIR(k_snippet_directive_declare, parse_snippet_declare),
};

#define SNIPPET_DIRECTIVE_COUNT \
    ((int)(sizeof(SNIPPET_DIRECTIVES) / sizeof(SNIPPET_DIRECTIVES[0])))

#undef SNIPPET_DIR

static int import_parse_snippet_directive(const char *line, int *loaded,
                                          int *warnings,
                                          int *edit_line_inout) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p[0] != '/' || p[1] != '/') return 0;
    p += 2;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '@') return 0;
    p++;

    for (int dir_idx = 0; dir_idx < SNIPPET_DIRECTIVE_COUNT; dir_idx++) {
        const SnippetDirective *d = &SNIPPET_DIRECTIVES[dir_idx];
        if (strncmp(p, d->name, d->name_len) != 0)
            continue;
        if (p[d->name_len] != '\0' &&
            !isspace((unsigned char)p[d->name_len]))
            continue;
        p += d->name_len;
        return d->parse(p, loaded, warnings, edit_line_inout);
    }

    return 0;
}

static int import_expr_has_symbolic_ident(const char *expr) {
    const char *p = expr;
    while (*p) {
        if (isdigit((unsigned char)*p) ||
            (*p == '.' && isdigit((unsigned char)p[1]))) {
            char *end = NULL;
            (void)strtof(p, &end);
            if (end && end != p) {
                p = end;
                if (*p == 'f' || *p == 'F') p++;
                continue;
            }
        }

        const char *start = NULL;
        const char *ident_end = repl_eval_eat_identifier(p, &start);
        if (!ident_end) {
            p++;
            continue;
        }

        p = ident_end;
        int len = (int)(p - start);

        char name[32];
        if (len >= (int)sizeof(name))
            return 1;
        memcpy(name, start, (size_t)len);
        name[len] = '\0';

        const char *q = p;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == '(' && repl_eval_is_builtin_function(name)) {
            continue;
        }

        if (strcmp(name, "PI") == 0 ||
            strcmp(name, "TAU") == 0 ||
            strcmp(name, "float") == 0) {
            continue;
        }

        return 1;
    }
    return 0;
}

static int import_copy_expr_until(const char **pp, char terminator,
                                  char *out, int out_sz) {
    const char *start = *pp;
    const char *p = start;
    int depth = 0;

    while (*p) {
        if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            if (terminator == ')' && depth == 0)
                break;
            if (depth > 0)
                depth--;
        }

        if (*p == terminator && depth == 0)
            break;
        p++;
    }

    if (*p != terminator)
        return 0;

    int len = (int)(p - start);
    if (len > out_sz - 1)
        len = out_sz - 1;
    memcpy(out, start, (size_t)len);
    out[len] = '\0';
    trim_in_place(out);
    *pp = p;
    return 1;
}

/* Parse a C for-header `for (<t> i = START; i <op> END; i <step>)` back
 * into REPL loop parts. Accepts op in { <, <=, >, >= } and step forms
 * i++ / i-- / i += Z / i -= Z. Outputs START/END/STEP expression text
 * plus two flags: *include_end = 1 for <= / >= (inclusive bound),
 * *is_greater = 1 for > / >= (descending loop). Returns 1 on a match,
 * 0 if the line isn't a recognized C for-header. */
static int import_extract_c_for_exprs(const char *line,
                                      char *start_expr, int start_sz,
                                      char *end_expr, int end_sz,
                                      char *step_expr, int step_sz,
                                      int *include_end,
                                      int *is_greater) {
    char repl_line[MAX_LINE_LEN];
    repl_eval_c_expr_to_repl(line, repl_line, sizeof(repl_line));

    const char *p = repl_line;
    while (*p && *p != '=') p++;
    if (*p != '=') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    if (!import_copy_expr_until(&p, ';', start_expr, start_sz))
        return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    while (*p && isspace((unsigned char)*p)) p++;

    if (*p == '<') {
        *is_greater = 0;
        p++;
    } else if (*p == '>') {
        *is_greater = 1;
        p++;
    } else {
        return 0;
    }
    *include_end = 0;
    if (*p == '=') {
        *include_end = 1;
        p++;
    }
    while (*p && isspace((unsigned char)*p)) p++;

    if (!import_copy_expr_until(&p, ';', end_expr, end_sz))
        return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    while (*p && isspace((unsigned char)*p)) p++;

    if (*p == '+' && p[1] == '+') {
        snprintf(step_expr, (size_t)step_sz, "1");
        return 1;
    }
    if (*p == '-' && p[1] == '-') {
        snprintf(step_expr, (size_t)step_sz, "-1");
        return 1;
    }
    if (*p == '+' && p[1] == '=') {
        p += 2;
        while (*p && isspace((unsigned char)*p)) p++;
        return import_copy_expr_until(&p, ')', step_expr, step_sz);
    }
    if (*p == '-' && p[1] == '=') {
        char raw_step[120]; /* leave room for the "-(...)" wrapper below */
        p += 2;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!import_copy_expr_until(&p, ')', raw_step, sizeof(raw_step)))
            return 0;
        snprintf(step_expr, (size_t)step_sz, "-(%s)", raw_step);
        return 1;
    }

    return 0;
}

static int import_make_repl_for_header(const char *line, char *out, int out_sz) {
    char var[16];
    float start_v, end_v, step_v;
    if (!repl_eval_parse_c_for_header(line, var, sizeof(var), &start_v, &end_v, &step_v))
        return 0;

    char start_expr[128];
    char end_expr[128];
    char step_expr[128];
    int include_end = 0;
    int is_greater = 0;
    if (import_extract_c_for_exprs(line,
                                   start_expr, sizeof(start_expr),
                                   end_expr, sizeof(end_expr),
                                   step_expr, sizeof(step_expr),
                                   &include_end, &is_greater) &&
        (import_expr_has_symbolic_ident(start_expr) ||
         import_expr_has_symbolic_ident(end_expr) ||
         import_expr_has_symbolic_ident(step_expr))) {
        int symbolic_step = import_expr_has_symbolic_ident(step_expr);
        if (include_end) {
            char adjusted[sizeof(end_expr) + 8];
            int an = snprintf(adjusted, sizeof(adjusted), "(%s) %c 1",
                              end_expr, is_greater ? '-' : '+');
            if (an < 0 || (size_t)an >= sizeof(adjusted))
                return 0;
            int en = snprintf(end_expr, sizeof(end_expr), "%s", adjusted);
            if (en < 0 || en >= (int)sizeof(end_expr))
                return 0;
        }

        int n;
        if (symbolic_step) {
            n = snprintf(out, (size_t)out_sz, "for(%s, %s, %s, %s) {",
                         var, start_expr, end_expr, step_expr);
        } else if (step_v != 1.0f) {
            char step_buf[32];
            repl_format_source_float(step_buf, sizeof(step_buf), step_v);
            n = snprintf(out, (size_t)out_sz, "for(%s, %s, %s, %s) {",
                         var, start_expr, end_expr, step_buf);
        } else {
            n = snprintf(out, (size_t)out_sz, "for(%s, %s, %s) {",
                         var, start_expr, end_expr);
        }
        if (n < 0 || n >= out_sz)
            return 0;
        return 1;
    }

    if (step_v != 1.0f) {
        char start_buf[32];
        char end_buf[32];
        char step_buf[32];
        repl_format_source_float(start_buf, sizeof(start_buf), start_v);
        repl_format_source_float(end_buf, sizeof(end_buf), end_v);
        repl_format_source_float(step_buf, sizeof(step_buf), step_v);
        snprintf(out, out_sz, "for(%s, %s, %s, %s) {",
                 var, start_buf, end_buf, step_buf);
    } else {
        char start_buf[32];
        char end_buf[32];
        repl_format_source_float(start_buf, sizeof(start_buf), start_v);
        repl_format_source_float(end_buf, sizeof(end_buf), end_v);
        snprintf(out, out_sz, "for(%s, %s, %s) {",
                 var, start_buf, end_buf);
    }
    return 1;
}

static int import_make_repl_func_header(const char *line, char *out, int out_sz) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    static const char kFuncPrefix[] = "static void ";
    int kFuncPrefixLen = (int)(sizeof(kFuncPrefix) - 1);
    if (strncmp(p, kFuncPrefix, (size_t)kFuncPrefixLen) != 0)
        return 0;
    p += kFuncPrefixLen;
    while (*p && isspace((unsigned char)*p)) p++;

    int fn = -1;
    if (!repl_parse_func_name_token(&p, &fn))
        return 0;
    if (fn < 0 || fn >= REPL_FUNC_SLOT_COUNT)
        return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(')
        return 0;
    p++;
    const char *start = p;
    p = repl_scan_to_matching_paren(p);
    if (*p != ')')
        return 0;

    char payload[MAX_LINE_LEN];
    int n = (int)(p - start);
    if (n > (int)sizeof(payload) - 1) n = (int)sizeof(payload) - 1;
    memcpy(payload, start, (size_t)n);
    payload[n] = '\0';
    trim_in_place(payload);

    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{')
        return 0;

    if (!payload[0] || strcmp(payload, "void") == 0) {
        snprintf(out, out_sz, "func%d {", fn);
        return 1;
    }

    char names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
    int count = repl_parse_identifier_list(payload, "float",
                                           names, MAX_EXPR_VARS);
    if (count < 0)
        return 0;

    int written = snprintf(out, out_sz, "func%d(", fn);
    /* Append comma-separated parameter names. */
    for (int param_idx = 0; param_idx < count && written < out_sz; param_idx++)
        written += snprintf(out + written, out_sz - written, "%s%s",
                            param_idx == 0 ? "" : ", ", names[param_idx]);
    if (written < out_sz)
        snprintf(out + written, out_sz - written, ") {");
    return 1;
}

static int import_make_repl_label(const char *line, char *out, int out_sz) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;

    int llen = 0;
    while (p[llen] && (isalnum((unsigned char)p[llen]) || p[llen] == '_'))
        llen++;
    if (llen <= 0 || p[llen] != ':')
        return 0;
    if (p[llen + 1] != '\0' && !isspace((unsigned char)p[llen + 1]))
        return 0;

    snprintf(out, out_sz, ":%.*s", llen, p);
    return 1;
}

static int import_extract_assignment_expr(const char *line, const char *key,
                                          char *out, int out_sz) {
    const char *p = strstr(line, key);
    if (!p)
        return 0;
    p = strchr(p, '=');
    if (!p)
        return 0;
    p++;

    char c_expr[MAX_LINE_LEN];
    if (!import_copy_expr_until(&p, ';', c_expr, sizeof(c_expr)))
        return 0;

    repl_eval_c_expr_to_repl(c_expr, out, out_sz);
    trim_in_place(out);
    return out[0] != '\0';
}

/* Extract N `<key>[i] = <expr>;` assignments from the exported tess
 * brace block. Returns 1 if every component was found, 0 on the first
 * miss. `key_fmt` is an `int`-formatted printf template (e.g.
 * `"_tn[%d]"`). On success, exprs[i] holds the per-component RHS text. */
static int parse_tess_brace_block(const char *p, const char *key_fmt,
                                  int n, char exprs[][MAX_LINE_LEN]) {
    for (int i = 0; i < n; i++) {
        char key[24];
        snprintf(key, sizeof(key), key_fmt, i);
        if (!import_extract_assignment_expr(p, key, exprs[i], MAX_LINE_LEN))
            return 0;
    }
    return 1;
}

/* Float fallback when the symbolic extract fails: walk N `= <expr>`
 * fragments forward from `start`, eval each, and write to out[]. Out-of-
 * range components are left at whatever the caller pre-filled. */
static void eval_tess_brace_floats(const char *start, int n, float *out) {
    const char *p = start;
    for (int i = 0; i < n; i++) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        eq++;
        ExprCtx ctx = { eq, NULL, 0, NULL, 0 };
        out[i] = repl_eval_expr(&ctx);
        p = ctx.p;
    }
}

static int import_make_repl_tess_line(const char *line, char *out, int out_sz) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;

    if (strstr(p, "gluTessBeginPolygon") != NULL) {
        snprintf(out, out_sz, "gluBegin(GLU_POLYGON);");
        return 1;
    }
    if (strstr(p, "gluTessBeginContour") != NULL) {
        snprintf(out, out_sz, "gluBegin(GLU_CONTOUR);");
        return 1;
    }
    if (strstr(p, "gluTessEndContour") != NULL ||
        strstr(p, "gluTessEndPolygon") != NULL) {
        snprintf(out, out_sz, "gluEnd();");
        return 1;
    }

    if (strncmp(p, "{ _tn[", 6) == 0) {
        char exprs[3][MAX_LINE_LEN];
        if (parse_tess_brace_block(p, "_tn[%d]", 3, exprs)) {
            int n = snprintf(out, (size_t)out_sz, "gluNormal(%s, %s, %s);",
                             exprs[0], exprs[1], exprs[2]);
            return (n >= 0 && n < out_sz) ? 1 : 0;
        }
        float nv[3] = {0, 0, 1};
        eval_tess_brace_floats(p, 3, nv);
        char x[REPL_SOURCE_FLOAT_TEXT_MAX], y[REPL_SOURCE_FLOAT_TEXT_MAX], z[REPL_SOURCE_FLOAT_TEXT_MAX];
        repl_format_source_float(x, sizeof(x), nv[0]);
        repl_format_source_float(y, sizeof(y), nv[1]);
        repl_format_source_float(z, sizeof(z), nv[2]);
        snprintf(out, out_sz, "gluNormal(%s, %s, %s);", x, y, z);
        return 1;
    }

    if (strncmp(p, "{ _tc[", 6) == 0) {
        char exprs[4][MAX_LINE_LEN];
        if (parse_tess_brace_block(p, "_tc[%d]", 4, exprs)) {
            int n;
            /* Elide the alpha arg when it's the default opaque 1.0 so
             * the round-trip mirrors the user-written `gluColor(r, g, b)`
             * (3-arg) form. */
            if (strcmp(exprs[3], "1") == 0 || strcmp(exprs[3], "1.0") == 0) {
                n = snprintf(out, (size_t)out_sz, "gluColor(%s, %s, %s);",
                             exprs[0], exprs[1], exprs[2]);
            } else {
                n = snprintf(out, (size_t)out_sz, "gluColor(%s, %s, %s, %s);",
                             exprs[0], exprs[1], exprs[2], exprs[3]);
            }
            return (n >= 0 && n < out_sz) ? 1 : 0;
        }
        float cv[4] = {1, 1, 1, 1};
        eval_tess_brace_floats(p, 4, cv);
        char r[REPL_SOURCE_FLOAT_TEXT_MAX], g[REPL_SOURCE_FLOAT_TEXT_MAX],
             b[REPL_SOURCE_FLOAT_TEXT_MAX], a[REPL_SOURCE_FLOAT_TEXT_MAX];
        repl_format_source_float(r, sizeof(r), cv[0]);
        repl_format_source_float(g, sizeof(g), cv[1]);
        repl_format_source_float(b, sizeof(b), cv[2]);
        repl_format_source_float(a, sizeof(a), cv[3]);
        snprintf(out, out_sz, "gluColor(%s, %s, %s, %s);", r, g, b, a);
        return 1;
    }

    if (strstr(p, "TessVertex") != NULL && strstr(p, "gluTessVertex") != NULL) {
        char exprs[3][MAX_LINE_LEN];
        if (parse_tess_brace_block(p, "_v->pos[%d]", 3, exprs)) {
            int n = snprintf(out, (size_t)out_sz, "gluVertex(%s, %s, %s);",
                             exprs[0], exprs[1], exprs[2]);
            return (n >= 0 && n < out_sz) ? 1 : 0;
        }
        const char *vp = strstr(p, "_v->pos[0]");
        if (!vp) return 0;
        float vv[3] = {0, 0, 0};
        eval_tess_brace_floats(vp, 3, vv);
        char x[REPL_SOURCE_FLOAT_TEXT_MAX], y[REPL_SOURCE_FLOAT_TEXT_MAX], z[REPL_SOURCE_FLOAT_TEXT_MAX];
        repl_format_source_float(x, sizeof(x), vv[0]);
        repl_format_source_float(y, sizeof(y), vv[1]);
        repl_format_source_float(z, sizeof(z), vv[2]);
        snprintf(out, out_sz, "gluVertex(%s, %s, %s);", x, y, z);
        return 1;
    }

    return 0;
}

static int import_make_repl_point_parameter_line(const char *line, char *out, int out_sz) {
    const char *p = line;
    const char *open;
    const char *close;
    const char *comma;
    const char *brace_open;
    const char *brace_close;
    char payload[MAX_LINE_LEN];
    char pname[64];
    char coeffs[MAX_LINE_LEN];
    char raw_args[4][MAX_LINE_LEN];
    char repl_args[4][MAX_LINE_LEN];
    int payload_len;
    int pname_len;
    int coeff_len;
    int count;

    while (*p && isspace((unsigned char)*p))
        p++;
    static const char kPointParamPrefix[] = "glPointParameterfv(";
    if (strncmp(p, kPointParamPrefix, sizeof(kPointParamPrefix) - 1) != 0)
        return 0;

    open = strchr(p, '(');
    close = strrchr(p, ')');
    if (!open || !close || close <= open + 1)
        return 0;

    payload_len = (int)(close - open - 1);
    if (payload_len <= 0 || payload_len >= (int)sizeof(payload))
        return 0;
    memcpy(payload, open + 1, (size_t)payload_len);
    payload[payload_len] = '\0';

    comma = strchr(payload, ',');
    if (!comma)
        return 0;
    pname_len = (int)(comma - payload);
    if (pname_len <= 0 || pname_len >= (int)sizeof(pname))
        return 0;
    memcpy(pname, payload, (size_t)pname_len);
    pname[pname_len] = '\0';
    trim_in_place(pname);

    brace_open = strchr(comma + 1, '{');
    brace_close = strrchr(comma + 1, '}');
    if (!brace_open || !brace_close || brace_close <= brace_open + 1)
        return 0;

    coeff_len = (int)(brace_close - brace_open - 1);
    if (coeff_len <= 0 || coeff_len >= (int)sizeof(coeffs))
        return 0;
    memcpy(coeffs, brace_open + 1, (size_t)coeff_len);
    coeffs[coeff_len] = '\0';

    count = split_top_level_args(coeffs, raw_args, 4);
    if (count != 3)
        return 0;

    /* Convert parsed C expressions back to REPL syntax. */
    for (int arg_idx = 0; arg_idx < count; arg_idx++)
        repl_eval_c_expr_to_repl(raw_args[arg_idx], repl_args[arg_idx], sizeof(repl_args[arg_idx]));

    return repl_format_fits(out, (size_t)out_sz,
                            "glPointParameterfv(%s, %s, %s, %s);",
                            pname, repl_args[0], repl_args[1], repl_args[2]);
}

static int import_make_repl_glut_bitmap_string(const char *line,
                                                char *out, int out_sz) {
    /* Match the label prefix (allow leading whitespace).
     * Run the args halves through the C-to-REPL converter while
     * preserving the format string verbatim. */
    const char *p = line ? line : "";
    while (*p && isspace((unsigned char)*p)) p++;
    static const char kPrefix[] = "label";
    int kPrefixLen = (int)(sizeof(kPrefix) - 1);
    if (strncmp(p, kPrefix, (size_t)kPrefixLen) != 0)
        return 0;
    p += kPrefixLen;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') return 0;
    const char *open_p = p;
    const char *close_p = strrchr(p, ')');
    if (!close_p || close_p <= open_p) return 0;

    int args_len = (int)(close_p - (open_p + 1));
    char args_str[MAX_LINE_LEN];
    if (args_len < 0) args_len = 0;
    if (args_len >= (int)sizeof(args_str))
        args_len = (int)sizeof(args_str) - 1;
    memcpy(args_str, open_p + 1, (size_t)args_len);
    args_str[args_len] = '\0';

    char fmt[GLUT_BITMAP_FMT_MAX] = "";
    char post[MAX_LINE_LEN] = "";
    char split_err[REPL_DIAG_TEXT_MAX] = "";
    if (!repl_label_split_args(args_str,
                               fmt, (int)sizeof(fmt),
                               post, (int)sizeof(post),
                               split_err, (int)sizeof(split_err)))
        return 0;

    char post_repl[MAX_LINE_LEN] = "";
    if (post[0])
        repl_eval_c_expr_to_repl(post, post_repl, sizeof(post_repl));

    return repl_format_fits(out, (size_t)out_sz,
                            "label(\"%s\"%s%s);",
                            fmt, post_repl[0] ? ", " : "", post_repl);
}

static void import_feed_one_line(const char *line, int *loaded, int *warnings,
                                 int *edit_line_inout) {
    char repl_line[MAX_LINE_LEN];
    int before = repl_state_document_count();
    int handled = 0;

    /* Snippet directives such as @declare are written by
     * write_canonical_cmd_as_c() and must be handled before the generic
     * C-to-REPL path. */
    if (import_parse_snippet_directive(line, loaded, warnings,
                                       edit_line_inout))
        return;

    /* Feed lines through the non-editor source-load API
     * (repl_load_apply_line in src/repl/compile.c) instead of
     * editor_feed_line. Same compile + apply, no editor input dispatch
     * (implemented in step 5b). */
    char load_err[REPL_STATUS_TEXT_MAX] = "";
    if (import_make_repl_for_header(line, repl_line, sizeof(repl_line))) {
        handled = repl_load_apply_line(repl_line, load_err, (int)sizeof(load_err),
                                       edit_line_inout);
    } else if (import_make_repl_tess_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_point_parameter_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_label(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_glut_bitmap_string(line, repl_line, sizeof(repl_line))) {
        handled = repl_load_apply_line(repl_line, load_err, (int)sizeof(load_err),
                                       edit_line_inout);
    } else {
        repl_eval_c_expr_to_repl(line, repl_line, sizeof(repl_line));
        handled = repl_load_apply_line(repl_line, load_err, (int)sizeof(load_err),
                                       edit_line_inout);
    }

    if (repl_state_document_count() > before) *loaded += (repl_state_document_count() - before);
    if (!handled) {
        /* Surface repl_load_apply_line's per-line diagnostic (e.g.
         * "command store at capacity", a parse-error reason)
         * alongside the offending line. Pre-fix the importer
         * captured load_err but discarded it, so capacity overflows
         * and similar import failures showed up as the generic
         * "could not parse line" with no clue why. */
        if (load_err[0])
            fprintf(stderr, "Warning: could not parse line: %s (%s)\n",
                    line, load_err);
        else
            fprintf(stderr, "Warning: could not parse line: %s\n", line);
        (*warnings)++;
    }
}

/* ========================================================================= */
/* Import state machine and per-stage handlers                                */
/*                                                                            */
/* load_from_file walks the file line by line and dispatches each line        */
/* through an ordered chain of handlers.  Each handler returns 1 if it        */
/* consumed the line, 0 to fall through to the next.  The state machine       */
/* tracks where in the exported scaffold we are: outside any snippet,         */
/* inside a function definition body, inside the geometry snippet, or         */
/* past the snippet (ignored tail).                                           */
/* ========================================================================= */

#define IMPORT_MAX_PENDING_COMMENTS 16

typedef struct {
    int in_snippet;
    int past_snippet;
    int func_depth;                   /* depth inside a function definition */
    int loaded;
    int warnings;
    int edit_line;                    /* caller-owned cursor (Phase 3.6.4) */
    char pending_comments[IMPORT_MAX_PENDING_COMMENTS][MAX_LINE_LEN];
    int  pending_comment_count;
    int  pending_blank_run;
} ImportState;

static void import_state_init(ImportState *s) {
    s->in_snippet = 0;
    s->past_snippet = 0;
    s->func_depth = 0;
    s->loaded = 0;
    s->warnings = 0;
    s->edit_line = 0;
    s->pending_comment_count = 0;
    s->pending_blank_run = 0;
}

static void import_reset_pending_function_prelude(ImportState *s) {
    s->pending_comment_count = 0;
    s->pending_blank_run = 0;
}

static void import_append_pending_function_prelude(ImportState *s,
                                                   const char *line) {
    if (s->pending_comment_count >= IMPORT_MAX_PENDING_COMMENTS)
        return;
    snprintf(s->pending_comments[s->pending_comment_count++],
             MAX_LINE_LEN, "%s", line);
}

static void import_flush_pending_blank_run(ImportState *s) {
    int logical_blank_count;

    if (s->pending_blank_run <= 0)
        return;

    /* Helper-function export writes one formatting blank line before each
     * emitted prelude line or function header. Every user-authored blank row
     * therefore appears as two raw blank lines, with one extra formatting
     * blank immediately before the next non-empty line. */
    logical_blank_count = (s->pending_blank_run - 1) / 2;
    for (int blank_idx = 0; blank_idx < logical_blank_count; blank_idx++)
        import_append_pending_function_prelude(s, "");
    s->pending_blank_run = 0;
}

/* --- pre-snippet handlers (camera, workspace header, function bodies) ----- */

static int import_try_camera(const char *p) {
    /* Both the g_angle preamble and the body lines flow
     * through a single bridge entry point. The bridge's stateful
     * parser dispatches internally based on which line shape it
     * sees (implemented in step 4a). */
    return import_parse_cam_line(p);
}

static int import_try_function_body(ImportState *s, const char *p) {
    if (s->func_depth <= 0) return 0;
    import_feed_one_line(p, &s->loaded, &s->warnings, &s->edit_line);
    for (const char *bp = p; *bp; bp++) {
        if      (*bp == '{') s->func_depth++;
        else if (*bp == '}') s->func_depth--;
    }
    return 1;
}

static int import_try_function_header(ImportState *s, const char *p, const char *raw) {
    char repl_func_line[MAX_LINE_LEN];
    if (!import_make_repl_func_header(p, repl_func_line, sizeof(repl_func_line)))
        return 0;
    import_flush_pending_blank_run(s);
    /* Feed accumulated pending comments before the function header. */
    for (int comment_idx = 0; comment_idx < s->pending_comment_count; comment_idx++)
        import_feed_one_line(s->pending_comments[comment_idx], &s->loaded, &s->warnings, &s->edit_line);
    import_reset_pending_function_prelude(s);
    int before = repl_state_document_count();
    char load_err[REPL_STATUS_TEXT_MAX] = "";
    int handled = repl_load_apply_line(repl_func_line, load_err, (int)sizeof(load_err),
                                       &s->edit_line);
    if (repl_state_document_count() > before) s->loaded += (repl_state_document_count() - before);
    if (!handled) {
        /* See import_feed_one_line for the load_err rationale. */
        if (load_err[0])
            fprintf(stderr, "Warning: could not parse line: %s (%s)\n",
                    raw, load_err);
        else
            fprintf(stderr, "Warning: could not parse line: %s\n", raw);
        s->warnings++;
    }
    s->func_depth = 1;
    return 1;
}

static int import_try_snippet_start(ImportState *s, const char *p) {
    if (strncmp(p, "// Snippet start", 16) != 0) return 0;
    import_reset_pending_function_prelude(s);
    s->in_snippet = 1;
    /* Function/header import may leave the import cursor in an insertion slot
     * inside existing commands.  Force snippet lines to start appending from
     * the end of the command list. */
    repl_dispatch_insert_mode_off();
    s->edit_line = repl_state_document_count();
    return 1;
}

static int import_try_pending_comment(ImportState *s, const char *p) {
    if (*p == '\0') {
        s->pending_blank_run++;
    } else if (p[0] == '/' && p[1] == '/') {
        import_flush_pending_blank_run(s);
        import_append_pending_function_prelude(s, p);
    } else {
        /* Any non-empty, non-comment line resets the pending buffer so stray
         * comments don't leak onto unrelated lines that follow. */
        import_reset_pending_function_prelude(s);
    }
    return 1; /* always consumes (including blank lines) */
}

/* --- snippet-body handlers -------------------------------------------------- */

static int import_try_snippet_end(ImportState *s, const char *p) {
    if (strncmp(p, "// Snippet end", 14) != 0) return 0;
    s->in_snippet   = 0;
    s->past_snippet = 1;
    return 1;
}

static int import_try_blank(ImportState *s, const char *p) {
    if (*p != '\0')
        return 0;

    import_feed_one_line(p, &s->loaded, &s->warnings, &s->edit_line);
    return 1;
}

static int import_try_predef_decl(const char *p) {
    return import_parse_predef_decl(p);
}

static int import_try_snippet_body_line(ImportState *s, const char *p) {
    import_feed_one_line(p, &s->loaded, &s->warnings, &s->edit_line);
    return 1;
}

/* --- dispatch --------------------------------------------------------------- */

static void import_process_line(ImportState *s, const char *p, const char *raw) {
    /* Camera-state lines appear both in the pre-snippet header and inside the
     * display() body that wraps the snippet, so they are recognised any time
     * we are not already inside a snippet. */
    if (!s->in_snippet && import_try_camera(p))                return;

    /* Everything after Snippet end is discarded. */
    if (s->past_snippet)                                       return;

    if (!s->in_snippet) {
        if (repl_state_parse_workspace_header_line(p))         return;
        if (import_try_function_body(s, p))                    return;
        if (import_try_function_header(s, p, raw))             return;
        if (import_try_snippet_start(s, p))                    return;
        (void)import_try_pending_comment(s, p);
        return;
    }

    /* In-snippet: */
    if (import_try_snippet_end(s, p))                          return;
    if (import_try_blank(s, p))                                return;
    if (import_try_predef_decl(p))                             return;
    (void)import_try_snippet_body_line(s, p);
}

/* Wipe per-load accumulator state populated by parse_workspace_header_line
 * / parse_cfg / @scene-name / @workspace-dir directives. Called at entry
 * (clears whatever a prior failed load may have left behind) and on every
 * failure exit (so a partial parse can't leak @cfg / @var / pending name
 * state into a later call — e.g. example_loader.c's
 * repl_export_apply_pending_cfg() drain, which would otherwise re-apply
 * stale slugs from the failed import). */
static void repl_export_load_reset_accumulators(void) {
    g_deferred_var_count       = 0;
    g_pending_scene_name[0]    = '\0';
    g_pending_workspace_dir[0] = '\0';
    import_cfg_accumulator_reset();
}

int repl_export_load_from_file(const char *filename, ReplImportResult *result) {
    if (result) {
        result->scene_name[0]    = '\0';
        result->workspace_dir[0] = '\0';
    }
    FILE *f = fopen(filename, "r");
    if (!f) return 0;

    repl_export_load_reset_accumulators();

    ImportState state;
    import_state_init(&state);
    import_cam_parser_reset();
    repl_func_alias_clear_all();

    char line[MAX_LINE_LEN];
    int truncated_line = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t raw_len = strlen(line);
        if (raw_len > 0 &&
            line[raw_len - 1] != '\n' &&
            line[raw_len - 1] != '\r' &&
            !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
            truncated_line = 1;
            break;
        }

        int len = (int)raw_len;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        import_process_line(&state, p, line);
    }

    /* Mirror the save-side ferror/fclose pair: fgets returning NULL
     * conflates EOF with read error, so the read-loop above can't
     * surface I/O failures by itself. Check ferror before closing. */
    int had_read_err = ferror(f);
    int close_failed = fclose(f) != 0;
    if (had_read_err || close_failed) {
        repl_export_load_reset_accumulators();
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof(msg), "Error: cannot read %s", filename);
        repl_set_status_error(msg);
        return 0;
    }
    if (truncated_line) {
        repl_export_load_reset_accumulators();
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof(msg), "Import failed: line too long in %s", filename);
        repl_set_status_error(msg);
        return 0;
    }

    /* Re-apply deferred @var values.  // @declare markers in the snippet may
     * have undeclared and re-declared variables (creating CMD_VAR_DECLARE
     * commands), resetting their values to 0.  Reapply the workspace-header
     * values here so they are restored correctly after the round-trip. */
    for (int di = 0; di < g_deferred_var_count; di++) {
        int idx = repl_eval_find_predef_var_idx(g_deferred_var_values[di].name);
        if (idx >= 0)
            g_predef_vars_mut[idx].value = g_deferred_var_values[di].value;
    }
    g_deferred_var_count = 0;

    /* Drain @cfg accumulator: hand the parsed (slug, val) bag to the
     * controller-installed bridge, which knows how to apply each slug
     * to its owner's state. Without a bridge (the demo case), the
     * accumulator is dropped silently — that's the architectural goal
     * (no glr_config dependency from src/repl/import.c). */
    import_cfg_accumulator_apply_and_reset();

    if (result) {
        snprintf(result->scene_name, sizeof(result->scene_name),
                 "%s", g_pending_scene_name);
        snprintf(result->workspace_dir, sizeof(result->workspace_dir),
                 "%s", g_pending_workspace_dir);
    }

    if (state.loaded > 0) {
        repl_source_scope_depth_cache_invalidate();
        repl_reformat_program();
        /* Publish the post-import cursor to the host. Without this,
         * downstream callers (e.g. repl_load_scene_as_new_slot, which
         * snapshots via repl_dispatch_edit_line_get right after this
         * returns) see the pre-import value — leaving Load Scene From
         * File parked at line 0 with insert mode off, so the next
         * commit replaces the first imported command instead of
         * appending (implemented in phase 4; see the
         * edit-line-ownership plan doc). */
        repl_dispatch_edit_line_set(state.edit_line);
        char msg[REPL_STATUS_TEXT_MAX];
        if (state.warnings > 0)
            snprintf(msg, sizeof(msg),
                     "Loaded %d commands from %s (%d warnings)",
                     state.loaded, filename, state.warnings);
        else
            snprintf(msg, sizeof(msg),
                     "Loaded %d commands from %s", state.loaded, filename);
        repl_set_status(msg);
        fprintf(stderr, "%s\n", msg);
    }
    return state.loaded > 0;
}
