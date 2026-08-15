/*
 * src/repl/import.c - Reader half of the export/import file format.
 *
 * The writer (file emit, code-panel dump, header-line refresh) stays in
 * export.c. This file
 * owns:
 *
 *   - The file-import state machine (line accumulation, handler dispatch,
 *     diagnostics, pending cfg, and stashed variable values).
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
 * Shared format strings and state access macros live in
 * export_format_shared.h. The two TUs do NOT share static helpers - the
 * WORKSPACE_DIRECTIVES dispatcher table here pairs each name with its reader
 * only, and export.c carries its own emit-only table.
 *
 * DEFERRED (repl-clarity-review.md finding 8, in docs/plans/partial/): this
 * file is one ~3,200-line TU facing a writer half split across six. The
 * five responsibilities listed above are the seam list. **The plan's advice
 * is not "split it" - it is "split item 5 first and nothing else"**: lift
 * the C-to-REPL line translators (import_make_repl_* and their parsing
 * helpers, roughly the run from import_make_repl_for_header to
 * import_make_repl_scratch_block_line) into import_line_translators.c plus
 * a narrow internal header, exposing only the body-line and function-header
 * translators the state machine needs. They have no ImportState dependency,
 * and the result pairs naturally with export_cmd_writer.c.
 *
 * Leave the state machine (import_process_line -> import_try_*) alone until
 * something needs to touch it - most likely
 * docs/plans/not-started/one-scene-loader.md, which retires
 * example_loader.c's own .glr walk. Rewriting the machine on the assumption
 * that there are two .glr walkers should wait for that plan or be done as
 * part of it. Renaming the reader API (repl_export_load_* ->
 * repl_import_load_*) and cutting a thin import.h is optional and should
 * ride that change, not lead it.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "repl/export.h"          /* public reader API */
#include "repl/export_format_shared.h"
#include "source_document.h"      /* source_document_insert_line */
#include "repl/load.h"            /* repl_load_apply_line */
#include "config.h"
#include "repl/command_store.h"
#include <string.h>
#include "repl/host_effects.h"
#include "repl/reformat.h"
#include "repl/scenes.h"
#include "repl/state_notify.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/camera_header.h"
#include "repl/doc_order.h"
#include "repl/text_helpers.h"
#include "repl/executor.h"
#include "repl/parser.h"
#include "repl/util.h"            /* repl_format_fits / _copy_string_fits / _append_clamped */
#include "repl/pipeline.h"
#include "repl/line_scan.h"       /* statement boundaries, shared with glr_extedit */
#include "repl/scene_load.h"      /* ReplSceneLoadOpts - explicit load policy */
#include "repl/scene_snapshot.h"  /* ATOMIC rollback */
#include "repl/source_scope.h"

#include "repl/cfg_baseline.h"

#ifndef NO_COLOR_OUTPUT
#define COLOR_WARNING_START "\033[33m"
#define COLOR_WARNING_END   "\033[0m"
#else
#define COLOR_WARNING_START ""
#define COLOR_WARNING_END   ""
#endif

static const char kWarningPrefix[] = COLOR_WARNING_START "Warning:" COLOR_WARNING_END " could not parse line";

#define g_import_cfg_bridge (repl_config_bridge())

typedef struct ImportState ImportState;

typedef struct {
    ReplConfigBag cfg_accumulator;
    ImportState  *owner;
} ImportWorkspaceAccum;

typedef struct {
    char name[REPL_PREDEF_NAME_MAX];
    float value;
} ImportFloatStash;

typedef struct ImportStagedFuncLine {
    char text[MAX_LINE_LEN];
    /* The physical file row this line came from. Staging is why it has to be
     * carried: a pre-snippet function body is buffered here and replayed long
     * after its rows went by, so at flush time `ImportState.line_no` names the
     * flush point instead. Restoring it is what lets the row map - and a
     * warning about a rejected body line - name the row the user is looking
     * at. 0 for a line the importer synthesized rather than read. */
    int  line_no;
    struct ImportStagedFuncLine *next;
} ImportStagedFuncLine;

typedef struct {
    int present;
    int flushed;
    ImportStagedFuncLine *head;
    ImportStagedFuncLine *tail;
} ImportStagedFunction;

#define IMPORT_MAX_PENDING_COMMENTS 16

/* DEFERRED (repl-clarity-review.md finding 8, in docs/plans/partial/): phase,
 * format policy and scanner state sit in adjacent scalar fields here, and the
 * phase is implicit in handler order plus the in_snippet / past_snippet pair
 * rather than named. The plan's scope is narrow on purpose: **replace only
 * in_snippet / past_snippet with a named phase**, and keep func_depth (a
 * counter), func_block_comment, active_staged_func_slot (a tagged index) and
 * the format-policy flags explicit - they are legitimately orthogonal, not
 * one Cartesian product. Do this when something else brings you into the
 * state machine, likely one-scene-loader.md. */
struct ImportState {
    ReplCameraHeader camera;          /* the one camera-header reader */
    ReplDocOrder     order;           /* canonical document order (.glr only) */
    ReplSceneLoadOpts opts;           /* format / policy / metadata policy */
    int              check_order;     /* .glr source: the authored format */
    int              order_failed;
    /* ATOMIC only. `load_failed` latches the first rejected line and stops the
     * walk; `atomic_stash` is the pre-load document the abort restores. The
     * stash is NULL under TOLERANT (nothing to roll back to) and also when the
     * ~1.2 MB allocation fails - the load then still runs and still aborts at
     * the first bad line, it just cannot undo the rows already inserted. That
     * degradation is deliberate: refusing to load at all because a scratch
     * buffer would not fit is worse than the partial document the caller's own
     * stash (repl_load_scene_via_loader, glr_extedit) already covers. */
    int              load_failed;
    int              failed_line_no;
    SceneSnapshot   *atomic_stash;
    const char      *source_label;    /* for diagnostics; never retained */
    int in_snippet;
    int past_snippet;
    int func_depth;                   /* depth inside a function definition */
    int func_block_comment;           /* block-comment state for the brace scan */
    int allow_raw_scene;              /* markerless files are plain REPL source */
    int has_func_body_markers;         /* exported snippets can place staged funcs */
    int active_staged_func_slot;       /* pre-snippet function currently buffered */
    int loaded;
    int warnings;
    int edit_line;                    /* caller-owned cursor */
    ImportWorkspaceAccum workspace;
    char pending_comments[IMPORT_MAX_PENDING_COMMENTS][MAX_LINE_LEN];
    int  pending_comment_line[IMPORT_MAX_PENDING_COMMENTS];
    int  pending_comment_count;
    int  pending_blank_run;
    int  line_no;
    ImportFloatStash float_stash[MAX_PREDEF_VARS];
    int              float_stash_count;
    ImportStagedFunction staged_funcs[REPL_FUNC_SLOT_COUNT];
    int staged_func_order[REPL_FUNC_SLOT_COUNT];
    int staged_func_order_count;
};

/* Public single-line parser batching state. repl_export_load_from_file uses
 * ImportState.workspace instead, so failed file imports cannot leak @cfg or
 * stashed variable data into later public repl_export_apply_pending_cfg() calls. */
static ImportWorkspaceAccum g_public_workspace_accum;

static void import_workspace_accum_reset(ImportWorkspaceAccum *accum) {
    if (!accum)
        return;
    repl_config_bag_clear(&accum->cfg_accumulator);
}

static void import_workspace_accum_init(ImportWorkspaceAccum *accum,
                                        ImportState *owner) {
    if (!accum)
        return;
    accum->owner = owner;
    import_workspace_accum_reset(accum);
}

static void import_workspace_cfg_apply_and_reset(ImportWorkspaceAccum *accum) {
    if (!accum)
        return;
    if (g_import_cfg_bridge && g_import_cfg_bridge->apply &&
        accum->cfg_accumulator.count > 0) {
        g_import_cfg_bridge->apply(&accum->cfg_accumulator);
    }
    repl_config_bag_clear(&accum->cfg_accumulator);
}

void repl_export_apply_pending_cfg(void) {
    import_workspace_cfg_apply_and_reset(&g_public_workspace_accum);
}

/* Where a reconstructed `// @declare` row goes: exactly where the marker
 * sits in the file. write_canonical_cmd_as_c emits the marker at the
 * declaration's own document position, and the body is read in order, so
 * the store's current end *is* the recorded position - no rule needs to
 * re-derive it, and whatever the author wrote above the declaration stays
 * above it.
 *
 * This is why the exporter's ordering is load-bearing rather than
 * cosmetic: a use that preceded its `@declare` would fail to resolve the
 * name. The interactive compiler cannot produce that shape
 * (compile_decl_prologue_end keeps a declaration above every reference),
 * so only a hand-edited file can, and it fails the same way the REPL
 * would reject the line if typed. */
static int import_decl_insert_pos(const ReplCommandStore *store) {
    if (!store || !store->cmds || !store->count)
        return 0;
    return *store->count;
}

static void import_format_decl_float(char *buf, size_t n, float v) {
    repl_format_source_float(buf, (int)n, v);
}

/* ========================================================================= */
/* Workspace header directive table - reader side.                            */
/*                                                                            */
/* The writer half (refresh_workspace_header_lines + emit_*) lives in         */
/* src/repl/export.c with its own emit-only directive table. The two halves   */
/* are intentionally independent so neither TU has to forward-declare into    */
/* the other; both walks key off the same `@name` strings and a regression    */
/* test catches any drift.                                                    */
/* ========================================================================= */

typedef int (*WorkspaceParseFn)(ImportWorkspaceAccum *accum, const char *args);
typedef int (*SnippetParseFn)(const char *args, ImportState *s);

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

/* Emit one import diagnostic to stderr with the shared "Warning: " prefix
 * and trailing newline. Centralises the format so the @func /
 * @declare / scene-name / workspace-dir overflow notices read identically. */
static void import_vwarn(const char *fmt, va_list ap) {
    fputs("Warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static void import_state_vwarn(ImportState *s, const char *fmt, va_list ap) {
    import_vwarn(fmt, ap);
    if (s)
        s->warnings++;
}

static void import_state_warn(ImportState *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    import_state_vwarn(s, fmt, ap);
    va_end(ap);
}

/* Emit a workspace-header warning and count it toward the per-load
 * "(N warnings)" total when parsing under repl_export_load_from_file().
 * Public repl_state_parse_workspace_header_line() callers still get the
 * warning printed immediately and can drain cfg through
 * repl_export_apply_pending_cfg(). */
static void import_workspace_warn(ImportWorkspaceAccum *accum,
                                  const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (accum && accum->owner) {
        import_state_vwarn(accum->owner, fmt, ap);
    } else {
        import_vwarn(fmt, ap);
    }
    va_end(ap);
}

static void import_state_warn_parse_line(ImportState *s, const char *line,
                                         const char *detail) {
    if (detail && detail[0])
        fprintf(stderr, "%s (line %d): %s (%s)\n",
                kWarningPrefix, s ? s->line_no : 0, line, detail);
    else
        fprintf(stderr, "%s (line %d): %s\n",
                kWarningPrefix, s ? s->line_no : 0, line);
    if (s)
        s->warnings++;
}

static void import_state_warn_parse_status(ImportState *s, int line_no,
                                           const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s (line %d): ", kWarningPrefix, line_no);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (s)
        s->warnings++;
}

static void import_flush_staged_function(ImportState *s, int slot);
static void import_flush_all_staged_functions(ImportState *s);

static int import_staged_func_append_line(ImportState *s, int slot,
                                          const char *line, int line_no) {
    ImportStagedFunction *fn;
    ImportStagedFuncLine *node;

    if (!s || slot < 0 || slot >= REPL_FUNC_SLOT_COUNT)
        return 0;

    fn = &s->staged_funcs[slot];
    node = (ImportStagedFuncLine *)malloc(sizeof(*node));
    if (!node) {
        import_state_warn(s, "out of memory while staging func%d", slot);
        return 0;
    }
    snprintf(node->text, sizeof(node->text), "%s", line ? line : "");
    node->line_no = line_no;
    node->next = NULL;

    if (fn->tail)
        fn->tail->next = node;
    else
        fn->head = node;
    fn->tail = node;
    if (!fn->present &&
        s->staged_func_order_count < REPL_FUNC_SLOT_COUNT)
        s->staged_func_order[s->staged_func_order_count++] = slot;
    fn->present = 1;
    return 1;
}

static void import_staged_functions_clear(ImportState *s) {
    if (!s)
        return;

    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        ImportStagedFuncLine *line = s->staged_funcs[slot].head;
        while (line) {
            ImportStagedFuncLine *next = line->next;
            free(line);
            line = next;
        }
        s->staged_funcs[slot].present = 0;
        s->staged_funcs[slot].flushed = 0;
        s->staged_funcs[slot].head = NULL;
        s->staged_funcs[slot].tail = NULL;
    }
    s->staged_func_order_count = 0;
}

static int import_repl_func_line_slot(const char *line) {
    const char *p = line;
    int slot = -1;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (!repl_parse_func_name_token(&p, &slot))
        return -1;
    if (slot < 0 || slot >= REPL_FUNC_SLOT_COUNT)
        return -1;
    return slot;
}

/* --- workspace-dir --------------------------------------------------------- */

static int parse_workspace_dir(ImportWorkspaceAccum *accum, const char *args) {
    size_t char_idx = 0;
    while (*args && char_idx < REPL_WORKSPACE_DIR_MAX - 1)
        IMPORT_EXPORT_WRITABLE->pending_workspace_dir[char_idx++] = *args++;
    IMPORT_EXPORT_WRITABLE->pending_workspace_dir[char_idx] = '\0';
    /* More source left over the cap means the path was clipped - a
     * truncated path points at the wrong directory, so don't do it silently. */
    if (*args)
        import_workspace_warn(accum,
                              "@workspace-dir path exceeds the %d-char limit; truncated",
                              (int)REPL_WORKSPACE_DIR_MAX - 1);
    {
        char *dir = IMPORT_EXPORT_WRITABLE->pending_workspace_dir;
        while (char_idx > 0 && isspace((unsigned char)dir[char_idx - 1]))
            dir[--char_idx] = '\0';
    }
    return 1;
}

/* --- scene-name ------------------------------------------------------------ */

static int parse_scene_name(ImportWorkspaceAccum *accum, const char *args) {
    size_t char_idx = 0;
    while (*args && char_idx < USER_SCENE_NAME_MAX - 1)
        IMPORT_EXPORT_WRITABLE->pending_scene_name[char_idx++] = *args++;
    IMPORT_EXPORT_WRITABLE->pending_scene_name[char_idx] = '\0';
    if (*args)
        import_workspace_warn(accum,
                              "@scene-name exceeds the %d-char limit; truncated to '%s'",
                              (int)USER_SCENE_NAME_MAX - 1,
                              IMPORT_EXPORT_VIEW.pending_scene_name);
    {
        char *name = IMPORT_EXPORT_WRITABLE->pending_scene_name;
        while (char_idx > 0 && isspace((unsigned char)name[char_idx - 1]))
            name[--char_idx] = '\0';
    }
    return 1;
}

/* --- var ------------------------------------------------------------------- */

static int parse_var(ImportWorkspaceAccum *accum, const char *args) {
    (void)accum;
    (void)args;
    return 1;
}

/* --- func aliases ---------------------------------------------------------- */

/* Parse the `@func <slot> = <name>` workspace-header directive args and
 * register the alias, so the later `static void <name>(...)` definition
 * in the file body maps back to its funcN slot. Returns 1 when
 * consumed, 0 on malformed args. */
static int parse_func_alias(ImportWorkspaceAccum *accum, const char *args) {
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
     * (the historical behaviour) - the only fix is a shorter func name or a
     * larger REPL_FUNC_NAME_MAX. */
    if (*p && (isalnum((unsigned char)*p) || *p == '_'))
        import_workspace_warn(accum,
                              "@func %d alias '%s' exceeds the %d-char name limit; "
                              "its definition will be dropped on import",
                              slot, name, REPL_FUNC_NAME_MAX - 1);
    repl_func_alias_set(slot, name);
    return 1;
}

/* --- cfg ------------------------------------------------------------------- */

static int parse_cfg(ImportWorkspaceAccum *accum, const char *args) {
    char slug[32];
    const char *p = NULL;
    if (!repl_config_extract_slug(args, slug, sizeof(slug), &p))
        return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    /* Copy the raw value token (trimmed). Used to be strtol ->
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
    if (accum)
        repl_config_bag_set(&accum->cfg_accumulator, slug, value);
    return 1;
}

/* --- directive table (reader half) ----------------------------------------- */

#define WS_DIR(name, parse_fn) { name, sizeof(name) - 1, parse_fn }

static const WorkspaceDirective WORKSPACE_DIRECTIVES[] = {
    /* name,                                       parse */
    WS_DIR(REPL_WORKSPACE_DIRECTIVE_SCENE_NAME,    parse_scene_name),
    WS_DIR(REPL_WORKSPACE_DIRECTIVE_WORKSPACE_DIR, parse_workspace_dir),
    WS_DIR(REPL_WORKSPACE_DIRECTIVE_VAR,           parse_var),
    WS_DIR(REPL_WORKSPACE_DIRECTIVE_FUNC,          parse_func_alias),
    WS_DIR(REPL_WORKSPACE_DIRECTIVE_CFG,           parse_cfg),
};
#define WORKSPACE_DIRECTIVE_COUNT \
    ((int)(sizeof(WORKSPACE_DIRECTIVES) / sizeof(WORKSPACE_DIRECTIVES[0])))

#undef WS_DIR

#define SNIPPET_DIR(name, parse_fn) \
    { name, sizeof(name) - 1, parse_fn }

static int import_parse_workspace_header_line(ImportWorkspaceAccum *accum,
                                              const char *line) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p[0] != '/' || p[1] != '/') return 0;
    p += 2;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '@') return 0;
    p++;

    /* Banner line: @workspace: REPL state ... - recognised, no payload.
     * C89 block-comment directives are normalized to // form by file import
     * before reaching this parser; legacy callers may still pass // directly. */
    if (strncmp(p, REPL_WORKSPACE_BANNER_DIRECTIVE_PREFIX,
                sizeof(REPL_WORKSPACE_BANNER_DIRECTIVE_PREFIX) - 1) == 0)
        return 1;
    if (strncmp(p, REPL_WORKSPACE_BANNER_DIRECTIVE,
                sizeof(REPL_WORKSPACE_BANNER_DIRECTIVE) - 1) == 0 &&
        !isalnum((unsigned char)p[sizeof(REPL_WORKSPACE_BANNER_DIRECTIVE) - 1]) &&
        p[sizeof(REPL_WORKSPACE_BANNER_DIRECTIVE) - 1] != '_' &&
        p[sizeof(REPL_WORKSPACE_BANNER_DIRECTIVE) - 1] != '-')
        return 1;

    for (int dir_idx = 0; dir_idx < WORKSPACE_DIRECTIVE_COUNT; dir_idx++) {
        const WorkspaceDirective *d = &WORKSPACE_DIRECTIVES[dir_idx];
        if (strncmp(p, d->name, d->name_len) != 0) continue;
        unsigned char follow = (unsigned char)p[d->name_len];
        if (follow != '\0' && !isspace(follow)) continue;
        const char *args = p + d->name_len;
        while (*args && isspace((unsigned char)*args)) args++;
        return d->parse(accum, args);
    }
    return 0;
}

int repl_state_parse_workspace_header_line(const char *line) {
    return import_parse_workspace_header_line(&g_public_workspace_accum, line);
}

/* Route one camera-reader diagnostic to the importer's sink. The reader is
 * neutral code and knows neither the file name nor the severity policy, so it
 * hands back a rule and the caller writes the sentence. */
static void import_camera_diag(void *userdata, const ReplCameraDiag *diag,
                               const char *rule_text) {
    const char *label = (const char *)userdata;
    char msg[REPL_STATUS_TEXT_MAX];

    if (diag->rule == REPL_CAMERA_RULE_MISSING_ROLE)
        return;   /* a partial header is a note, not a scolding */
    /* The goal statement promises a diagnostic that names the file *and* the
     * line, so the importer supplies the name the reader cannot know. */
    snprintf(msg, sizeof(msg), "%s:%d: %s",
             label ? label : "<memory>", diag->line_no, rule_text);
    if (repl_camera_rule_severity(diag->rule) == REPL_CAMERA_SEVERITY_REJECTION)
        repl_set_status_error(msg);
    else
        repl_set_status(msg);
    fprintf(stderr, "%s\n", msg);
}

static void import_camera_report_missing(const ReplCameraFinish *finish,
                                         const char *label) {
    char roles[64];
    char msg[REPL_STATUS_TEXT_MAX];

    if (!repl_camera_header_format_missing_roles(finish, roles, sizeof(roles)))
        return;
    snprintf(msg, sizeof(msg), "%s: camera header: %s not set; keeping current",
             label ? label : "<memory>", roles);
    repl_set_status(msg);
    fprintf(stderr, "%s\n", msg);
}

/* Recognize an exported file-scope global decl
 * `[static] float a = 1, b = 2.5;` (the write_predef_var_globals
 * output) and copy each initializer into the already-registered predef
 * var of the same name. Declaration/registration itself happens via the
 * `static float` declarations / `@declare` directives - this only restores values. Returns 1
 * when at least one var was updated. */
static int import_parse_predef_decl_common(const char *line, ImportFloatStash *out_stash, int *out_count, int max_stash,
                                           ReplCameraHeader *hdr, int decl_line_no) {
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

        if (out_stash && out_count) {
            if (*out_count < max_stash) {
                repl_copy_string_fits(out_stash[*out_count].name,
                                      sizeof(out_stash[*out_count].name),
                                      name);
                out_stash[*out_count].value = val;
                (*out_count)++;
                updated = 1;

                /* Register the predefined variable immediately so helper functions
                 * defined before display() can successfully reference it on compile.
                 * Skip system scaffold variables like g_angle. */
                /* g_angle's initializer is non-semantic and must stay 0.0f:
                 * the compiled C animates from 0 while the pose rides on the
                 * `@camera ry` row, so a hand-edited value makes the two views
                 * disagree. The shared reader cannot catch this - the
                 * declaration carries no tag, and teaching it to inspect
                 * untagged lines would resurrect the preamble reader this
                 * format deleted - so the warning lives here, at the guard
                 * that already singles the name out. The example, tutorial and
                 * demo paths never see a g_angle line. */
                if (hdr && strcmp(name, "g_angle") == 0 && val != 0.0f)
                    repl_camera_header_record(hdr, REPL_CAMERA_ROLE_SPIN,
                                              REPL_CAMERA_RULE_G_ANGLE_INIT,
                                              decl_line_no);
                if (strcmp(name, "g_angle") != 0) {
                    int was_registered = (repl_eval_find_predef_var_idx(name) >= 0);
                    if (!was_registered) {
                        repl_eval_declare_predef_var(name, NULL, 0);
                    }
                    int idx = repl_eval_find_predef_var_idx(name);
                    if (idx >= 0) {
                        g_predef_vars_mut[idx].value = val;
                    }
                }
            }
        } else {
            /* Look up and update the predefined variable value. */
            for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
                if (strcmp(g_predef_vars[var_idx].name, name) == 0) {
                    g_predef_vars_mut[var_idx].value = val;
                    updated = 1;
                    break;
                }
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

static int import_parse_predef_decl(const char *line) {
    return import_parse_predef_decl_common(line, NULL, NULL, 0, NULL, 0);
}

static int import_try_stash_predef_decl(ImportState *s, const char *line) {
    return import_parse_predef_decl_common(line, s->float_stash, &s->float_stash_count,
                                           MAX_PREDEF_VARS, &s->camera, s->line_no);
}

/* 1 if `s` carries the whole-token `tag` (a trailing tag the exporter
 * appends to a `// @declare` marker - `@tune` / `@config`), else 0. Bounded
 * by whitespace/end so it never matches a name like `@tuned`. */
static int declare_args_have_tag(const char *s, const char *tag,
                                 size_t tag_len) {
    for (const char *q = s; (q = strstr(q, tag)) != NULL; q += tag_len) {
        char prev = (q == s) ? ' ' : q[-1];
        char next = q[tag_len];
        if (isspace((unsigned char)prev) &&
            (next == '\0' || isspace((unsigned char)next)))
            return 1;
    }
    return 0;
}

/* Parse a snippet-scoped `@declare` marker written by write_canonical_cmd_as_c()
 * and reconstruct the corresponding CMD_VAR_DECLARE command. Variables that are
 * already registered in g_predef_vars (e.g. from auto-declare or from
 * declare_test_vars in tests) are kept at their current indices so that any
 * CMD_VAR_ASSIGN commands already loaded with those indices remain valid.
 * Vars not yet registered are declared. */
static int parse_snippet_declare(const char *args, ImportState *s) {
    const char *p = args;
    while (*p && isspace((unsigned char)*p)) p++;

    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type      = CMD_VAR_DECLARE;
    cmd.valid     = 1;
    int count     = 0;

    /* Names that THIS call newly declares in the predef table - kept
     * separate so a later source/cmd-store insert failure can undeclare
     * them without touching names that were already registered (e.g.
     * by auto-declare or by test setup). */
    char newly_declared[MAX_NAMES_PER_DECL][REPL_PREDEF_NAME_MAX];
    int  new_count = 0;

    /* Build the canonical source string and collect names. Tokens take the
     * form `name` or `name=value`; the optional value carries the inline
     * initializer through the round-trip so the canonical decl text matches
     * the original source byte-for-byte. */
    char decl_line[MAX_LINE_LEN];
    /* Canonical form matches format_decl_text: `  static float ...`.
     * The exporter writes file-scope `static float` lines anyway, so
     * the keyword here reinforces that lifetime in the code panel.
     *
     * The reconstruction is *wider than the marker it came from*: names get
     * `, ` separators and each stashed value is re-rendered from the float
     * (up to REPL_SOURCE_FLOAT_TEXT_MAX - 1 chars, e.g. a 31-digit
     * fixed-point spelling), none of which the `// @declare a b c` line paid
     * for. Appends go through repl_append_clamped and never `off += snprintf`
     * - see src/repl/util.h for why the raw idiom writes out of bounds. */
    int truncated = 0;
    int off = repl_append_clamped(decl_line, sizeof(decl_line), 0, &truncated,
                                  "  static float");
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (!isalpha((unsigned char)*p) && *p != '_') break;
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int len = (int)(p - start);
        if (len <= 0) break;  /* no more names */
        if (len >= REPL_PREDEF_NAME_MAX) {
            import_state_warn(s,
                              "@declare name '%.*s' exceeds the %d-char limit; "
                              "dropped (with any names after it)",
                              len, start, REPL_PREDEF_NAME_MAX - 1);
            break;
        }
        if (count >= MAX_NAMES_PER_DECL) {
            import_state_warn(s,
                              "@declare lists more than %d names; the rest are dropped",
                              MAX_NAMES_PER_DECL);
            break;
        }
        char name[REPL_PREDEF_NAME_MAX];
        memcpy(name, start, (size_t)len);
        name[len] = '\0';
        /* Declare the var if not yet registered. Record newly-declared
         * names so we can undeclare them if a downstream step (name
         * copy overflow, source/cmd-store insert failure) bails. */
        int was_registered = (repl_eval_find_predef_var_idx(name) >= 0);
        if (!was_registered) {
            if (!repl_eval_declare_predef_var(name, NULL, 0)) {
                if (s) s->warnings++;
                continue;
            }
            memcpy(newly_declared[new_count], name, (size_t)len);
            newly_declared[new_count][len] = '\0';
            new_count++;
        }
        int idx = repl_eval_find_predef_var_idx(name);
        int has_stashed_val = 0;
        float stashed_val = 0.0f;
        if (idx >= 0 && s) {
            for (int si = 0; si < s->float_stash_count; si++) {
                if (strcmp(s->float_stash[si].name, name) == 0) {
                    stashed_val = s->float_stash[si].value;
                    has_stashed_val = 1;
                    break;
                }
            }
        }
        if (has_stashed_val && idx >= 0) {
            g_predef_vars_mut[idx].value = stashed_val;
        }

        if (!repl_copy_string_fits(cmd.payload.decl.names[count],
                                   sizeof(cmd.payload.decl.names[count]), name)) {
            /* Roll back the just-declared entry so the predef table
             * doesn't carry a name the document never gets. */
            if (!was_registered) {
                repl_eval_undeclare_predef_var(name);
                new_count--;
            }
            if (s) s->warnings++;
            continue;
        }
        off = repl_append_clamped(decl_line, sizeof(decl_line), off, &truncated,
                                  count == 0 ? " %.*s" : ", %.*s", len, start);
        if (has_stashed_val) {
            char vbuf[32];
            import_format_decl_float(vbuf, sizeof(vbuf), stashed_val);
            off = repl_append_clamped(decl_line, sizeof(decl_line), off,
                                      &truncated, " = %s", vbuf);
        }
        count++;
    }
    if (count == 0) {
        /* Nothing accepted; undeclare any names this call registered. */
        for (int i = 0; i < new_count; i++)
            repl_eval_undeclare_predef_var(newly_declared[i]);
        return 0;
    }
    /* Re-attach the @tune / @config tags so the reconstructed decl line is
     * the in-app source of truth (badge, panel dimming, and re-export all
     * read the trailing comment). */
    {
        int has_tune = declare_args_have_tag(args, "@tune", 5);
        int has_config = declare_args_have_tag(args, "@config", 7);
        off = repl_append_clamped(decl_line, sizeof(decl_line), off, &truncated,
                                  ";");
        /* Last append: the offset is not carried further, only `truncated`. */
        if (has_tune || has_config)
            repl_append_clamped(decl_line, sizeof(decl_line), off,
                                &truncated, " //%s%s",
                                has_tune ? " @tune" : "",
                                has_config ? " @config" : "");
    }

    /* A clipped row is unusable: a dropped name stays registered in the predef
     * table while vanishing from the document, and a dropped @tune/@config tag
     * silently changes what the variable panel shows. Roll the registrations
     * back and drop the directive, mirroring the insert-failure paths below. */
    if (truncated) {
        import_state_warn(s,
                          "@%s line does not fit in %d chars once formatted; "
                          "dropped",
                          REPL_SNIPPET_DIRECTIVE_DECLARE, MAX_LINE_LEN - 1);
        for (int i = 0; i < new_count; i++)
            repl_eval_undeclare_predef_var(newly_declared[i]);
        return 1;
    }

    cmd.payload.decl.count = count;

    /* Insert the command directly, bypassing editor_try_commit_float_decl so we
     * don't reject vars that are already registered, and at the position the
     * `// @declare` marker occupies in the file rather than a re-derived one. */
    {
        ReplCommandStore store = repl_command_store_live();
        int decl_pos = import_decl_insert_pos(&store);

        /* Source text first; cmd-store second so a text-write failure
         * leaves no orphan GLCmd row. cmd-store failure rolls the
         * inserted line back AND undeclares the newly-registered names
         * so the eval table can't drift out of sync with the document. */
        if (!source_document_insert_line(decl_pos, decl_line)) {
            for (int i = 0; i < new_count; i++)
                repl_eval_undeclare_predef_var(newly_declared[i]);
            if (s) s->warnings++;
            return 1;
        }
        /* Thread the caller-owned cursor through ImportState. */
        ReplStoreMutOpts opts = {
            .flags        = REPL_COMMAND_STORE_ADJUST_EDIT_LINE,
            .cursor_inout = s ? &s->edit_line : NULL,
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
            if (s) s->warnings++;
            return 1;
        }
        if (s) s->loaded++;
    }
    return 1;
}

static int parse_snippet_func_body(const char *args, ImportState *s) {
    const char *p = args;
    int slot = 0;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (!isdigit((unsigned char)*p)) {
        import_state_warn(s, "@%s missing function slot",
                          REPL_SNIPPET_DIRECTIVE_FUNC_BODY);
        return 1;
    }
    while (isdigit((unsigned char)*p)) {
        slot = slot * 10 + (*p - '0');
        p++;
    }
    if (slot < 0 || slot >= REPL_FUNC_SLOT_COUNT) {
        import_state_warn(s, "@%s slot %d is out of range",
                          REPL_SNIPPET_DIRECTIVE_FUNC_BODY, slot);
        return 1;
    }

    import_flush_staged_function(s, slot);
    return 1;
}

static const SnippetDirective SNIPPET_DIRECTIVES[] = {
    /* name,                                    parse */
    SNIPPET_DIR(REPL_SNIPPET_DIRECTIVE_DECLARE,   parse_snippet_declare),
    SNIPPET_DIR(REPL_SNIPPET_DIRECTIVE_FUNC_BODY, parse_snippet_func_body),
};

#define SNIPPET_DIRECTIVE_COUNT \
    ((int)(sizeof(SNIPPET_DIRECTIVES) / sizeof(SNIPPET_DIRECTIVES[0])))

#undef SNIPPET_DIR

static int import_parse_snippet_directive(ImportState *s, const char *line) {
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
        return d->parse(p, s);
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

/* C-to-REPL line translator: `static void funcN(float a, float b) {`
 * (or an aliased name resolved via @func) back to `funcN(a, b) {`.
 * `void` / empty parameter lists become the zero-arg `funcN {` form.
 * Returns 1 with the REPL line in `out`, 0 if the line isn't an
 * exported function header. */
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

/* C-to-REPL line translator for the exported tessellator scaffolding.
 * Each independent matcher maps one exported C shape back to its REPL
 * source line: the gluTessBegin/gluTessEnd calls to gluBegin/gluEnd,
 * and the `{ _tn[...] }` / `{ _tc[...] }` / `_v->pos[...]` assignment
 * blocks to gluNormal/gluColor/gluVertex. Expression args are recovered
 * verbatim from the brace block when possible (preserving vars like
 * `t`), else re-evaluated to literals; a default-opaque alpha collapses
 * back to the 3-arg gluColor form. Returns 1 on a match. */
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

/* Widest value list any of the readers below accepts: glMultMatrixf's
 * 4x4. Bounds both the caller's repl_args[] and the scratch split. */
#define IMPORT_PAYLOAD_ARGS_MAX REPL_MATRIX_CELL_COUNT

/* Shared skeleton for the glPointParameterfv / glClipPlane /
 * glMaterialfv / glMultMatrixf C-to-REPL readers. Matches `prefix` after
 * leading whitespace, extracts the payload between the call's outer
 * parens, peels `token_count` leading comma-delimited tokens (trimmed)
 * into tokens[], then reads the value list from either a `{...}` compound
 * literal or the first matching exporter helper call in helpers[],
 * splits it into at most `max_args` top-level args, and converts each
 * through the C-to-REPL expression converter into repl_args[]. Returns
 * the converted arg count, or 0 on any structural mismatch (callers check
 * the count they expect). */
static int import_parse_payload_call(const char *line, const char *prefix,
                                     char tokens[][64], int token_count,
                                     const char *const *helpers,
                                     int helper_count,
                                     char repl_args[][MAX_LINE_LEN],
                                     int max_args) {
    const char *p = line;
    const char *open;
    const char *close;
    const char *cursor;
    const char *brace_open;
    const char *brace_close;
    char payload[MAX_LINE_LEN];
    char values[MAX_LINE_LEN];
    char raw_args[IMPORT_PAYLOAD_ARGS_MAX][MAX_LINE_LEN];
    int payload_len;
    int values_len;
    int count;

    if (max_args < 0 || max_args > IMPORT_PAYLOAD_ARGS_MAX)
        return 0;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, prefix, strlen(prefix)) != 0)
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

    cursor = payload;
    for (int tok = 0; tok < token_count; tok++) {
        const char *comma = strchr(cursor, ',');
        int tok_len;
        if (!comma)
            return 0;
        tok_len = (int)(comma - cursor);
        if (tok_len <= 0 || tok_len >= 64)
            return 0;
        memcpy(tokens[tok], cursor, (size_t)tok_len);
        tokens[tok][tok_len] = '\0';
        trim_in_place(tokens[tok]);
        cursor = comma + 1;
    }

    brace_open = strchr(cursor, '{');
    brace_close = strrchr(cursor, '}');
    if (brace_open && brace_close && brace_close > brace_open) {
        values_len = (int)(brace_close - brace_open - 1);
        if (values_len <= 0 || values_len >= (int)sizeof(values))
            return 0;
        memcpy(values, brace_open + 1, (size_t)values_len);
        values[values_len] = '\0';
    } else {
        const char *helper = NULL;
        const char *helper_open;
        const char *helper_close;
        for (int h = 0; h < helper_count && !helper; h++)
            helper = strstr(cursor, helpers[h]);
        if (!helper)
            return 0;
        helper_open = strchr(helper, '(');
        helper_close = helper_open ? strrchr(helper_open, ')') : NULL;
        if (!helper_open || !helper_close || helper_close <= helper_open + 1)
            return 0;
        values_len = (int)(helper_close - helper_open - 1);
        if (values_len <= 0 || values_len >= (int)sizeof(values))
            return 0;
        memcpy(values, helper_open + 1, (size_t)values_len);
        values[values_len] = '\0';
    }

    count = split_top_level_args(values, raw_args, max_args);
    if (count <= 0)
        return 0;

    /* Convert parsed C expressions back to REPL syntax. */
    for (int arg_idx = 0; arg_idx < count; arg_idx++)
        repl_eval_c_expr_to_repl(raw_args[arg_idx], repl_args[arg_idx],
                                 sizeof(repl_args[arg_idx]));
    return count;
}

/* C-to-REPL line translator: exported
 * `glPointParameterfv(pname, (GLfloat[]){c, l, q})` (or the GLfloat3
 * helper form) back to the REPL's flat 4-arg
 * `glPointParameterfv(pname, c, l, q);` spelling. Returns 1 on a
 * match. */
static int import_make_repl_point_parameter_line(const char *line, char *out, int out_sz) {
    static const char *const helpers[] = { REPL_EXPORT_GLFLOAT3_HELPER };
    char pname[1][64];
    char repl_args[4][MAX_LINE_LEN];

    if (import_parse_payload_call(line, "glPointParameterfv(",
                                  pname, 1, helpers, 1, repl_args, 4) != 3)
        return 0;
    return repl_format_fits(out, (size_t)out_sz,
                            "glPointParameterfv(%s, %s, %s, %s);",
                            pname[0], repl_args[0], repl_args[1], repl_args[2]);
}

/* C-to-REPL line translator: `glClipPlane(plane, <equation>)` where
 * <equation> is either a compound literal `(GLdouble[]){...}` or the
 * exporter's repl_gldouble4 helper call. The plane token carries over
 * verbatim; the 4 coefficient expressions run through the C-to-REPL
 * converter and re-emit in the canonical compound-literal form. */
static int import_make_repl_clip_plane_line(const char *line, char *out, int out_sz) {
    static const char *const helpers[] = { REPL_EXPORT_GLDOUBLE4_HELPER };
    char plane[1][64];
    char repl_args[4][MAX_LINE_LEN];

    if (import_parse_payload_call(line, "glClipPlane(",
                                  plane, 1, helpers, 1, repl_args, 4) != 4)
        return 0;
    return repl_format_fits(out, (size_t)out_sz,
                            "glClipPlane(%s, (GLdouble[]){%s, %s, %s, %s});",
                            plane[0],
                            repl_args[0], repl_args[1], repl_args[2], repl_args[3]);
}

/* C-to-REPL line translator: `glFogfv(GL_FOG_COLOR, <color>)` where
 * <color> is either a compound literal `(GLfloat[]){...}` or the
 * exporter's repl_glfloat4 helper call. The pname token carries over
 * verbatim; the 4 channel expressions run through the C-to-REPL
 * converter and re-emit in the canonical compound-literal form. */
static int import_make_repl_fog_fv_line(const char *line, char *out, int out_sz) {
    static const char *const helpers[] = { REPL_EXPORT_GLFLOAT4_HELPER };
    char pname[1][64];
    char repl_args[4][MAX_LINE_LEN];

    if (import_parse_payload_call(line, "glFogfv(",
                                  pname, 1, helpers, 1, repl_args, 4) != 4)
        return 0;
    return repl_format_fits(out, (size_t)out_sz,
                            "glFogfv(%s, (GLfloat[]){%s, %s, %s, %s});",
                            pname[0],
                            repl_args[0], repl_args[1], repl_args[2], repl_args[3]);
}

/* C-to-REPL line translator: `glMaterialfv(face, pname, <values>)`
 * where <values> is either a compound literal `(GLfloat[]){...}` or one
 * of the exporter's GLfloat1/GLfloat4 helper calls. The face/pname
 * tokens carry over verbatim; the 1 (GL_SHININESS) or 4 (RGBA) value
 * expressions run through the C-to-REPL converter and re-emit in the
 * canonical compound-literal form. Returns 1 on a match. */
static int import_make_repl_materialfv_line(const char *line, char *out, int out_sz) {
    static const char *const helpers[] = {
        REPL_EXPORT_GLFLOAT4_HELPER, REPL_EXPORT_GLFLOAT1_HELPER
    };
    char tokens[2][64];  /* face, pname */
    char repl_args[4][MAX_LINE_LEN];
    int count;

    count = import_parse_payload_call(line, "glMaterialfv(",
                                      tokens, 2, helpers, 2, repl_args, 4);
    if (count != 1 && count != 4)
        return 0;

    if (count == 1) {
        return repl_format_fits(out, (size_t)out_sz,
                                "glMaterialfv(%s, %s, (GLfloat[]){%s});",
                                tokens[0], tokens[1], repl_args[0]);
    }
    return repl_format_fits(out, (size_t)out_sz,
                            "glMaterialfv(%s, %s, (GLfloat[]){%s, %s, %s, %s});",
                            tokens[0], tokens[1],
                            repl_args[0], repl_args[1], repl_args[2], repl_args[3]);
}

/* C-to-REPL line translator: `glMultMatrixf(<cells>)` where <cells> is
 * either a compound literal `(GLfloat[]){...}` or the exporter's
 * repl_glfloat16 helper call. The 16 expressions run through the
 * C-to-REPL converter and re-emit in the canonical compound-literal
 * form. The scratch-array form `glMultMatrixf(A)` never reaches here:
 * it is already REPL syntax, and a lone name is not a value list, so
 * the split below rejects it and the line falls through unchanged. */
static int import_make_repl_mult_matrixf_line(const char *line, char *out, int out_sz) {
    static const char *const helpers[] = { REPL_EXPORT_GLFLOAT16_HELPER };
    char tokens[1][64];  /* unused: the call has no leading args */
    char repl_args[REPL_MATRIX_CELL_COUNT][MAX_LINE_LEN];

    if (import_parse_payload_call(line, "glMultMatrixf(",
                                  tokens, 0, helpers, 1, repl_args,
                                  REPL_MATRIX_CELL_COUNT)
            != REPL_MATRIX_CELL_COUNT)
        return 0;
    return repl_format_fits(out, (size_t)out_sz,
                            "glMultMatrixf((GLfloat[]){%s, %s, %s, %s, "
                            "%s, %s, %s, %s, %s, %s, %s, %s, "
                            "%s, %s, %s, %s});",
                            repl_args[0], repl_args[1], repl_args[2], repl_args[3],
                            repl_args[4], repl_args[5], repl_args[6], repl_args[7],
                            repl_args[8], repl_args[9], repl_args[10], repl_args[11],
                            repl_args[12], repl_args[13], repl_args[14], repl_args[15]);
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

static void import_state_init(ImportState *s) {
    s->in_snippet = 0;
    s->past_snippet = 0;
    s->func_depth = 0;
    s->func_block_comment = 0;
    s->check_order = 0;
    s->order_failed = 0;
    s->load_failed = 0;
    s->failed_line_no = 0;
    s->atomic_stash = NULL;
    repl_scene_load_opts_init(&s->opts, REPL_EXAMPLE_SOURCE_C);
    s->source_label = NULL;
    s->allow_raw_scene = 0;
    s->has_func_body_markers = 0;
    s->active_staged_func_slot = -1;
    s->loaded = 0;
    s->warnings = 0;
    s->edit_line = 0;
    import_workspace_accum_init(&s->workspace, s);
    s->pending_comment_count = 0;
    s->pending_blank_run = 0;
    s->line_no = 0;
    s->float_stash_count = 0;
    s->staged_func_order_count = 0;
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        s->staged_funcs[slot].present = 0;
        s->staged_funcs[slot].flushed = 0;
        s->staged_funcs[slot].head = NULL;
        s->staged_funcs[slot].tail = NULL;
    }
}

/* Is the span [s, end) exactly a literal zero, in any spelling the
 * exporter or a hand-written C file might use (0, 0.0, .0, 0.0f)? */
static int import_span_is_literal_zero(const char *s, const char *end) {
    char buf[64];
    char *stop = NULL;
    int n;

    while (s < end && isspace((unsigned char)*s)) s++;
    while (end > s && isspace((unsigned char)end[-1])) end--;
    n = (int)(end - s);
    if (n <= 0 || n >= (int)sizeof(buf))
        return 0;
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    if (buf[n - 1] == 'f' || buf[n - 1] == 'F')
        buf[--n] = '\0';
    if (n == 0)
        return 0;
    return strtod(buf, &stop) == 0.0 && stop && *stop == '\0';
}

/* Lower the exporter's generated local declaration back to canonical REPL
 * text: `float a = 0.0f, b = 0.0f;` -> `float a, b;`.
 *
 * The exporter writes those initializers because the REPL binds every
 * local to 0.0f on call entry, so a bare C `float a;` would be undefined
 * where the REPL reads 0 (see write_canonical_cmd_as_c). Import is the
 * right place to undo it - it is already the trusted path that bypasses
 * commit handlers for exactly this class of reason - and *lowering*
 * rather than synthesizing an `a = 0.0f;` statement is what keeps the
 * round-trip a fixed point instead of growing the body by one row per
 * local per pass.
 *
 * Only a literal zero is lowered, and *every* declarator must carry one.
 * A hand-written `float a = 5;` inside a function body still reaches the
 * declaration preflight and is rejected there, so REPL and file semantics
 * stay identical rather than merely compatible - and the partially
 * initialized `float a = 0.0f, b;`, which the exporter never writes, is
 * left alone for the same reason: `b` is indeterminate in C, and lowering
 * it would quietly promise a deterministic REPL zero the source file does
 * not have. `static float` is never touched - that is a global, and the
 * prologue's own handler owns it. */
static int import_make_repl_local_decl(const char *line, char *out,
                                       int out_sz) {
    const char *p;
    const char *names_start;
    const char *semi;
    const char *tail;
    int has_static = 0;
    int indent = 0;
    int names = 0;
    int inits = 0;
    int off;

    if (!line || !out || out_sz <= 0)
        return 0;
    while (line[indent] && isspace((unsigned char)line[indent]))
        indent++;
    p = repl_scan_decl_float_prefix(line, &has_static);
    if (!p || has_static)
        return 0;
    names_start = p;

    /* Validate the whole list before emitting anything: one non-zero or
     * non-literal initializer means this is not the exporter's form and
     * the line must pass through untouched. */
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ';' || *p == '\0')
            break;
        if (!isalpha((unsigned char)*p) && *p != '_')
            return 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        names++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '=') {
            const char *value = ++p;
            while (*p && *p != ',' && *p != ';') p++;
            if (!import_span_is_literal_zero(value, p))
                return 0;
            inits++;
        }
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != ',')
            break;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        /* A comma is a separator, not an optional terminator. Accepting
         * `float a = 0.0f,;` would silently repair invalid C into a valid
         * REPL local even though the exporter can never produce that form. */
        if (*p == ';' || *p == '\0')
            return 0;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    /* Every declarator, or none of them: a mixed line is not the
     * exporter's output and must not be rewritten. */
    if (*p != ';' || names == 0 || inits != names)
        return 0;

    if (indent >= out_sz - 1)
        return 0;
    memset(out, ' ', (size_t)indent);
    off = indent;
    off += snprintf(out + off, (size_t)(out_sz - off), "float ");

    p = names_start;
    for (int first = 1; off < out_sz - 1; first = 0) {
        const char *name_start;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ';' || *p == '\0')
            break;
        name_start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        if (!first)
            off += snprintf(out + off, (size_t)(out_sz - off), ", ");
        if (off < out_sz - 1)
            off += snprintf(out + off, (size_t)(out_sz - off), "%.*s",
                            (int)(p - name_start), name_start);
        while (*p && *p != ',' && *p != ';') p++;
        if (*p != ',')
            break;
        p++;
    }
    if (off >= out_sz - 1)
        return 0;

    /* Everything past the terminator (a trailing comment) rides along. */
    semi = strchr(names_start, ';');
    tail = semi ? semi + 1 : "";
    while (*tail && isspace((unsigned char)*tail)) tail++;
    snprintf(out + off, (size_t)(out_sz - off), ";%s%s",
             *tail ? " " : "", tail);
    return 1;
}

/* Fold the C a scratch block exports to back into the block form:
 *
 *   A[4] = c; A[5] = s; A[6] = 0.0f; A[7] = 0.0f;  ->  A[4] = {c, s, 0, 0};
 *
 * The run has to be one array, literal subscripts, consecutive and
 * ascending, and at least two of them - a lone `A[4] = c;` is the scalar
 * form and must round-trip as itself. Anything else falls through to the
 * generic path, which handles each statement the way it always did.
 *
 * This is the only reader of that shape: nothing else in the exported C
 * puts several statements on one line, which is what makes the run
 * unambiguous without tracking the writer's intent across lines. */
static int import_make_repl_scratch_block_line(const char *line,
                                               char *out, int out_sz) {
    const char *p = line;
    const char *comment;
    char array_name = '\0';
    char cells[REPL_SCRATCH_ARRAY_LEN][MAX_LINE_LEN];
    int indent = 0;
    int base = -1;
    int count = 0;
    int used, k;

    if (!line || !out || out_sz <= 0)
        return 0;

    while (line[indent] == ' ' || line[indent] == '\t')
        indent++;
    p = line + indent;

    comment = repl_line_trailing_comment(p);

    while (*p && (!comment || p < comment)) {
        const char *expr_start;
        const char *semi;
        int subscript = 0;
        int digits = 0;
        int expr_len;
        char c_expr[MAX_LINE_LEN];

        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || (comment && p >= comment))
            break;

        if (*p != 'A' && *p != 'B' && *p != 'C')
            return 0;
        if (array_name && *p != array_name)
            return 0;
        array_name = *p;
        p++;

        if (*p != '[')
            return 0;
        p++;
        while (isdigit((unsigned char)*p)) {
            subscript = subscript * 10 + (*p - '0');
            if (subscript > REPL_SCRATCH_ARRAY_LEN)
                return 0;
            digits++;
            p++;
        }
        if (!digits || *p != ']')
            return 0;
        p++;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '=' || p[1] == '=')
            return 0;
        p++;

        semi = strchr(p, ';');
        if (!semi || (comment && semi > comment))
            return 0;
        expr_start = p;
        while (expr_start < semi && isspace((unsigned char)*expr_start))
            expr_start++;
        expr_len = (int)(semi - expr_start);
        while (expr_len > 0 && isspace((unsigned char)expr_start[expr_len - 1]))
            expr_len--;
        if (expr_len <= 0 || expr_len >= MAX_LINE_LEN)
            return 0;

        if (count == 0)
            base = subscript;
        else if (subscript != base + count)
            return 0;
        if (count >= REPL_SCRATCH_ARRAY_LEN)
            return 0;

        memcpy(c_expr, expr_start, (size_t)expr_len);
        c_expr[expr_len] = '\0';
        repl_eval_c_expr_to_repl(c_expr, cells[count], sizeof(cells[count]));
        count++;

        p = semi + 1;
    }

    if (count < 2 || base < 0 || base + count > REPL_SCRATCH_ARRAY_LEN)
        return 0;

    used = snprintf(out, (size_t)out_sz, "%.*s%c[%d] = {",
                    indent, line, array_name, base);
    if (used < 0 || used >= out_sz)
        return 0;
    for (k = 0; k < count; k++) {
        int wrote = snprintf(out + used, (size_t)(out_sz - used), "%s%s",
                             k ? ", " : "", cells[k]);
        if (wrote < 0 || wrote >= out_sz - used)
            return 0;
        used += wrote;
    }
    {
        int wrote = snprintf(out + used, (size_t)(out_sz - used), "};%s%s",
                             comment ? "  " : "", comment ? comment : "");
        if (wrote < 0 || wrote >= out_sz - used)
            return 0;
    }
    return 1;
}

static void import_translate_repl_line(const char *line,
                                       char *repl_line,
                                       int repl_line_sz) {
    if (import_make_repl_for_header(line, repl_line, repl_line_sz))
        return;
    if (import_make_repl_local_decl(line, repl_line, repl_line_sz) ||
        import_make_repl_tess_line(line, repl_line, repl_line_sz) ||
        import_make_repl_materialfv_line(line, repl_line, repl_line_sz) ||
        import_make_repl_point_parameter_line(line, repl_line, repl_line_sz) ||
        import_make_repl_clip_plane_line(line, repl_line, repl_line_sz) ||
        import_make_repl_fog_fv_line(line, repl_line, repl_line_sz) ||
        import_make_repl_mult_matrixf_line(line, repl_line, repl_line_sz) ||
        import_make_repl_scratch_block_line(line, repl_line, repl_line_sz) ||
        import_make_repl_glut_bitmap_string(line, repl_line, repl_line_sz))
        return;

    repl_eval_c_expr_to_repl(line, repl_line, repl_line_sz);
}

/* Detect and strip the trailing `// @auto` marker export writes on a
 * generated normal. Runs on the incoming line rather than the translated
 * one because the tess translator rebuilds its output from scratch and
 * would drop the marker; the block-comment normalizer has already turned
 * the exported C89 block comment back into `//` form by this point.
 *
 * Returns 1 when the marker was found, with `out` holding the line minus
 * the marker (and minus the whitespace that separated them). */
static int import_take_auto_normal_marker(const char *line, char *out,
                                          size_t out_sz) {
    const char *comment;
    const char *tag;
    size_t keep;

    if (!line || !out || out_sz == 0)
        return 0;

    comment = strstr(line, "//");
    if (!comment)
        return 0;

    tag = comment + 2;
    while (*tag && isspace((unsigned char)*tag))
        tag++;
    if (strncmp(tag, REPL_EXPORT_AUTO_NORMAL_MARKER,
                strlen(REPL_EXPORT_AUTO_NORMAL_MARKER)) != 0)
        return 0;
    tag += strlen(REPL_EXPORT_AUTO_NORMAL_MARKER);
    while (*tag && isspace((unsigned char)*tag))
        tag++;
    /* Only a marker that is the whole comment: `// @auto plus notes` is a
     * user comment that happens to start with the tag, and rewriting the
     * row would lose the notes. */
    if (*tag)
        return 0;

    keep = (size_t)(comment - line);
    while (keep > 0 && isspace((unsigned char)line[keep - 1]))
        keep--;
    if (keep >= out_sz)
        keep = out_sz - 1;
    memcpy(out, line, keep);
    out[keep] = '\0';
    return 1;
}

/* Re-stamp is_auto on the row a marked line just produced. The load path
 * appends, so the new row is the one at `before`; the type check keeps a
 * marker on any other command from doing anything.
 *
 * Writes through the command store: that is the owner boundary for command
 * mutation, and import is not a state-owner file. Same read-modify-replace
 * shape the autonormal pass itself uses. */
static void import_restore_auto_normal(int before) {
    ReplCommandStore store;
    GLCmd marked;

    if (repl_state_document_count() != before + 1)
        return;
    marked = repl_state_document_cmds()[before];
    if (marked.type != CMD_NORMAL3F && marked.type != CMD_TESS_NORMAL)
        return;
    marked.is_auto = 1;
    store = repl_command_store_live();
    repl_command_store_replace_one(&store, before, &marked);
}

/* Record a rejected line. Under TOLERANT this is only a counter bump (the
 * caller has already emitted the per-line diagnostic and carries on); under
 * ATOMIC it latches the abort that stops the walk and rolls the document back.
 * Only the FIRST failure is retained - it is the one the user has to fix, and
 * every later line in an aborted walk is unreached rather than judged. */
/* ----- physical -> document row map (BYOE stage 2.5) ---------------------- */

static void import_row_map_clear(ReplSceneRowMap *map) {
    int i;

    if (!map)
        return;
    map->count        = 0;
    map->hole_row     = -1;
    map->hole_doc_row = -1;
    for (i = 0; i < map->cap && map->doc_row; i++)
        map->doc_row[i] = REPL_ROW_MAP_NONE;
}

void repl_scene_row_map_init(ReplSceneRowMap *map, int *storage, int cap,
                             unsigned long hole_nonce) {
    if (!map)
        return;
    map->doc_row    = storage;
    map->cap        = storage ? cap : 0;
    map->hole_nonce = hole_nonce;
    import_row_map_clear(map);
}

int repl_scene_cursor_hole_line(char *buf, int buf_size, unsigned long nonce) {
    int n;

    if (!buf || buf_size <= 0)
        return 0;
    n = snprintf(buf, (size_t)buf_size, "// %s %lu",
                 REPL_CURSOR_HOLE_MARKER, nonce);
    if (n < 0 || n >= buf_size) {
        buf[0] = '\0';
        return 0;
    }
    return n;
}

/* Physical row `line_no` (1-based) produced its first document row at
 * `doc_row`. First writer wins: a physical row that somehow yields several
 * document rows should place the cursor on the first of them. */
static void import_row_map_note(ImportState *s, int line_no, int doc_row) {
    ReplSceneRowMap *map = s->opts.row_map;

    if (!map || !map->doc_row)
        return;
    if (line_no < 1 || line_no > map->cap)
        return;
    if (map->doc_row[line_no - 1] == REPL_ROW_MAP_NONE)
        map->doc_row[line_no - 1] = doc_row;
    if (line_no > map->count)
        map->count = line_no;
}

/* Record where the rows a just-applied line produced actually landed.
 *
 * `before` is the document count taken before the apply, and that is also
 * where the rows went: **the importer only ever appends.** Every insertion
 * position it uses is the document end - the plain-command tail clamps to
 * `document_count`, `import_decl_insert_pos` returns the count outright, and
 * `// Snippet start` resets the cursor there. Read order and document order
 * can still differ (a staged function body is buffered and replayed later, and
 * pending comments are held until the header they belong to), but *feed* order
 * and document order cannot - which is what makes recording at feed time
 * enough, with no fixup pass over rows already written.
 *
 * If a mid-document insert ever joins the import path, this is the function
 * that has to grow one: every recorded entry at or past the insert point
 * would need shifting, and nothing here would notice on its own. */
static void import_row_map_note_applied(ImportState *s, int before) {
    if (!s->opts.row_map || repl_state_document_count() <= before)
        return;
    import_row_map_note(s, s->line_no, before);
}

/* The transport marker the controller substituted for the row it removed.
 *
 * Consumed only when a nonce was requested and matches, so this is inert on
 * every load path but the watcher's, and a hand-written `// @cursor-hole`
 * stays an ordinary comment even on the watcher's. Anything trailing the
 * number disqualifies the line for the same reason. */
static int import_take_cursor_hole(ImportState *s, const char *line) {
    ReplSceneRowMap *map = s->opts.row_map;
    const char *p = line;
    char *end;
    unsigned long nonce;
    size_t marker_len = strlen(REPL_CURSOR_HOLE_MARKER);

    if (!map || map->hole_nonce == 0 || !line)
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (p[0] != '/' || p[1] != '/')
        return 0;
    p += 2;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, REPL_CURSOR_HOLE_MARKER, marker_len) != 0)
        return 0;
    p += marker_len;
    if (*p != ' ' && *p != '\t')
        return 0;
    nonce = strtoul(p, &end, 10);
    if (end == p || nonce != map->hole_nonce)
        return 0;
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')
        return 0;
    /* The hole lands wherever the next row would be inserted. Reading it here
     * rather than at the physical line is what makes a hole inside a staged
     * function body come out right: the marker travelled the staging list with
     * the rows around it and is replayed at their position, not at the row
     * where the file happened to mention it. */
    map->hole_row     = s->line_no;
    map->hole_doc_row = s->edit_line;
    import_row_map_note(s, s->line_no, s->edit_line);
    return 1;
}

static void import_note_line_rejected(ImportState *s, int line_no) {
    if (!s || s->opts.policy != REPL_SCENE_LOAD_POLICY_ATOMIC)
        return;
    if (s->load_failed)
        return;
    s->load_failed    = 1;
    s->failed_line_no = line_no;
}

/* Feed one already-translated source line into the document. Returns 1 when
 * the line was accepted (or consumed as a snippet directive), 0 when
 * repl_load_apply_line rejected it. Callers that only consume the line do not
 * need the result; the ATOMIC latch is set here regardless. */
static int import_feed_one_line(ImportState *s, const char *line) {
    char repl_line[MAX_LINE_LEN];
    char unmarked[MAX_LINE_LEN];
    int before = repl_state_document_count();
    int handled = 0;
    int was_auto;

    /* ATOMIC has already rejected the file. Guarded here rather than only in
     * import_process_line because import_flush_staged_function replays
     * buffered function bodies straight into this function. */
    if (s->load_failed)
        return 0;

    /* Before every other reading of the line: the controller put this here in
     * place of a row it removed, and it must produce no document row. */
    if (import_take_cursor_hole(s, line))
        return 1;

    /* Snippet directives such as @declare are written by
     * write_canonical_cmd_as_c() and must be handled before the generic
     * C-to-REPL path. */
    if (import_parse_snippet_directive(s, line))
        return 1;

    was_auto = import_take_auto_normal_marker(line, unmarked, sizeof(unmarked));
    if (was_auto)
        line = unmarked;

    /* Feed lines through the non-editor source-load API
     * (repl_load_apply_line in src/repl/compile.c) instead of
     * editor_feed_line. The same compile + apply path runs without editor
     * input dispatch. */
    char load_err[REPL_STATUS_TEXT_MAX] = "";
    import_translate_repl_line(line, repl_line, sizeof(repl_line));
    handled = repl_load_apply_line(repl_line, load_err, (int)sizeof(load_err),
                                   &s->edit_line);

    if (was_auto && handled)
        import_restore_auto_normal(before);

    if (repl_state_document_count() > before) s->loaded += (repl_state_document_count() - before);
    import_row_map_note_applied(s, before);
    if (!handled) {
        /* Surface repl_load_apply_line's per-line diagnostic (e.g.
         * "command store at capacity", a parse-error reason)
         * alongside the offending line. Pre-fix the importer
         * captured load_err but discarded it, so capacity overflows
         * and similar import failures showed up as the generic
         * "could not parse line" with no clue why. */
        import_state_warn_parse_line(s, line, load_err);
        import_note_line_rejected(s, s->line_no);
    }
    return handled;
}

static void import_flush_staged_function(ImportState *s, int slot) {
    ImportStagedFunction *fn;

    if (!s || slot < 0 || slot >= REPL_FUNC_SLOT_COUNT)
        return;

    fn = &s->staged_funcs[slot];
    if (!fn->present || fn->flushed)
        return;

    for (ImportStagedFuncLine *line = fn->head; line; line = line->next) {
        int saved = s->line_no;
        /* Replay under the row the line was read from, not the flush point -
         * see ImportStagedFuncLine.line_no. */
        if (line->line_no > 0)
            s->line_no = line->line_no;
        import_feed_one_line(s, line->text);
        s->line_no = saved;
    }
    fn->flushed = 1;
}

static void import_flush_all_staged_functions(ImportState *s) {
    if (!s)
        return;

    for (int order_idx = 0; order_idx < s->staged_func_order_count; order_idx++) {
        int slot = s->staged_func_order[order_idx];
        import_flush_staged_function(s, slot);
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


static void import_reset_pending_function_prelude(ImportState *s) {
    s->pending_comment_count = 0;
    s->pending_blank_run = 0;
}

/* `line_no` is 0 for the synthesized blank rows below, which stand in for
 * export's formatting blanks and correspond to no physical row. */
static void import_append_pending_function_prelude(ImportState *s,
                                                   const char *line,
                                                   int line_no) {
    if (s->pending_comment_count >= IMPORT_MAX_PENDING_COMMENTS)
        return;
    s->pending_comment_line[s->pending_comment_count] = line_no;
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
        import_append_pending_function_prelude(s, "", 0);
    s->pending_blank_run = 0;
}

/* --- pre-snippet handlers (workspace header, function bodies) ------------ */

static int import_comment_matches_marker(const char *comment,
                                         const char *marker) {
    const char *p = comment;

    if (!p || p[0] != '/' || p[1] != '/')
        return 0;
    p += 2;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, marker, strlen(marker)) != 0)
        return 0;
    p += strlen(marker);
    while (*p && isspace((unsigned char)*p))
        p++;
    return *p == '\0';
}

/* 1 if the line is part of the exporter's C89 loop scaffolding - a
 * bare `{` / `}` tagged with the loop-scope marker comment, or a
 * `float i;  // <marker>` hoisted loop-variable decl - which exists
 * only to keep exported files C89-compilable and must be dropped (not
 * fed as source) on import. */
static int import_is_c89_loop_marker_line(const char *p) {
    const char *comment;
    const char *q;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == '{' || *p == '}') {
        q = p + 1;
        while (*q && isspace((unsigned char)*q))
            q++;
        return import_comment_matches_marker(q, REPL_EXPORT_C89_LOOP_SCOPE_MARKER);
    }

    if (strncmp(p, "float", 5) != 0 || !isspace((unsigned char)p[5]))
        return 0;
    q = p + 5;
    while (*q && isspace((unsigned char)*q))
        q++;
    if (!isalpha((unsigned char)*q) && *q != '_')
        return 0;
    q++;
    while (*q && (isalnum((unsigned char)*q) || *q == '_'))
        q++;
    while (*q && isspace((unsigned char)*q))
        q++;
    if (*q != ';')
        return 0;
    q++;
    while (*q && isspace((unsigned char)*q))
        q++;
    comment = q;
    return import_comment_matches_marker(comment, REPL_EXPORT_C89_LOOP_VAR_MARKER);
}

static int import_try_function_body(ImportState *s, const char *p) {
    if (s->func_depth <= 0) return 0;
    if (import_is_c89_loop_marker_line(p))
        return 1;
    if (s->active_staged_func_slot >= 0) {
        char repl_line[MAX_LINE_LEN];
        import_translate_repl_line(p, repl_line, sizeof(repl_line));
        import_staged_func_append_line(s, s->active_staged_func_slot, repl_line,
                                       s->line_no);
    } else {
        import_feed_one_line(s, p);
    }
    s->func_depth += repl_code_brace_delta(p, &s->func_block_comment);
    if (s->func_depth <= 0)
        s->active_staged_func_slot = -1;
    return 1;
}

static int import_try_function_header(ImportState *s, const char *p, const char *raw) {
    char repl_func_line[MAX_LINE_LEN];
    if (!import_make_repl_func_header(p, repl_func_line, sizeof(repl_func_line)))
        return 0;
    import_flush_pending_blank_run(s);
    if (!s->allow_raw_scene) {
        int slot = import_repl_func_line_slot(repl_func_line);
        if (slot >= 0) {
            for (int comment_idx = 0; comment_idx < s->pending_comment_count; comment_idx++)
                import_staged_func_append_line(s, slot,
                                               s->pending_comments[comment_idx],
                                               s->pending_comment_line[comment_idx]);
            import_reset_pending_function_prelude(s);
            import_staged_func_append_line(s, slot, repl_func_line, s->line_no);
            s->func_depth = 1;
            s->active_staged_func_slot = slot;
            return 1;
        }
    }
    /* Feed accumulated pending comments before the function header. */
    for (int comment_idx = 0; comment_idx < s->pending_comment_count; comment_idx++) {
        int saved = s->line_no;
        if (s->pending_comment_line[comment_idx] > 0)
            s->line_no = s->pending_comment_line[comment_idx];
        import_feed_one_line(s, s->pending_comments[comment_idx]);
        s->line_no = saved;
    }
    import_reset_pending_function_prelude(s);
    int before = repl_state_document_count();
    char load_err[REPL_STATUS_TEXT_MAX] = "";
    int handled = repl_load_apply_line(repl_func_line, load_err, (int)sizeof(load_err),
                                       &s->edit_line);
    if (repl_state_document_count() > before) s->loaded += (repl_state_document_count() - before);
    import_row_map_note_applied(s, before);
    if (!handled) {
        /* See import_feed_one_line for the load_err rationale. */
        import_state_warn_parse_line(s, raw, load_err);
        import_note_line_rejected(s, s->line_no);
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
    if (!s->has_func_body_markers)
        import_flush_all_staged_functions(s);
    return 1;
}

static int import_try_raw_scene_body(ImportState *s, const char *p) {
    if (!s->allow_raw_scene || s->func_depth != 0)
        return 0;
    import_feed_one_line(s, p);
    return 1;
}

static int import_try_pending_comment(ImportState *s, const char *p) {
    if (*p == '\0') {
        s->pending_blank_run++;
    } else if (p[0] == '/' && p[1] == '/') {
        import_flush_pending_blank_run(s);
        import_append_pending_function_prelude(s, p, s->line_no);
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
    import_flush_all_staged_functions(s);
    s->in_snippet   = 0;
    s->past_snippet = 1;
    return 1;
}

static int import_try_blank(ImportState *s, const char *p) {
    if (*p != '\0')
        return 0;

    import_feed_one_line(s, p);
    return 1;
}

static int import_try_predef_decl(const char *p) {
    return import_parse_predef_decl(p);
}

static int import_try_snippet_body_line(ImportState *s, const char *p) {
    if (import_is_c89_loop_marker_line(p))
        return 1;
    import_feed_one_line(s, p);
    return 1;
}

/* --- dispatch --------------------------------------------------------------- */

typedef enum {
    IMPORT_LINE_WORKSPACE_HEADER,
    IMPORT_LINE_FUNCTION_BODY,
    IMPORT_LINE_FUNCTION_HEADER,
    IMPORT_LINE_PRE_SNIPPET_STASH_DECL,
    IMPORT_LINE_SNIPPET_START,
    IMPORT_LINE_RAW_SCENE_BODY,
    IMPORT_LINE_PENDING_COMMENT,
    IMPORT_LINE_SNIPPET_END,
    IMPORT_LINE_BLANK,
    IMPORT_LINE_PREDEF_DECL,
    IMPORT_LINE_SNIPPET_BODY
} ImportLineKind;

typedef int (*ImportLineHandler)(ImportState *s, const char *p,
                                 const char *raw);

typedef struct {
    ImportLineKind    kind;
    ImportLineHandler handle;
} ImportLineHandlerSpec;

/* The canonical document order covers user code, not just camera rows, so it
 * is the loader's to enforce - and it has to be enforced by *every* loader or
 * the same file is accepted from one entry point and rejected from another,
 * which is the divergence this format exists to end. */
static void import_order_diag(void *userdata, ReplDocOrderRule rule,
                              int line_no, int conflict_line_no,
                              const char *message) {
    const char *label = (const char *)userdata;
    char msg[REPL_STATUS_TEXT_MAX];

    (void)rule;
    (void)conflict_line_no;
    snprintf(msg, sizeof(msg), "%s:%d: %s",
             label ? label : "<memory>", line_no, message);
    repl_set_status_error(msg);
    fprintf(stderr, "%s\n", msg);
}

static int import_handle_workspace_header(ImportState *s, const char *p,
                                          const char *raw) {
    (void)raw;
    return import_parse_workspace_header_line(&s->workspace, p);
}

static int import_handle_function_body(ImportState *s, const char *p,
                                       const char *raw) {
    (void)raw;
    return import_try_function_body(s, p);
}

static int import_handle_function_header(ImportState *s, const char *p,
                                         const char *raw) {
    return import_try_function_header(s, p, raw);
}

static int import_handle_snippet_start(ImportState *s, const char *p,
                                       const char *raw) {
    (void)raw;
    return import_try_snippet_start(s, p);
}

static int import_handle_raw_scene_body(ImportState *s, const char *p,
                                        const char *raw) {
    (void)raw;
    return import_try_raw_scene_body(s, p);
}

static int import_handle_pending_comment(ImportState *s, const char *p,
                                         const char *raw) {
    (void)raw;
    return import_try_pending_comment(s, p);
}

static int import_handle_snippet_end(ImportState *s, const char *p,
                                     const char *raw) {
    (void)raw;
    return import_try_snippet_end(s, p);
}

static int import_handle_blank(ImportState *s, const char *p,
                               const char *raw) {
    (void)raw;
    return import_try_blank(s, p);
}

static int import_handle_predef_decl(ImportState *s, const char *p,
                                     const char *raw) {
    (void)s;
    (void)raw;
    return import_try_predef_decl(p);
}

static int import_handle_stash_predef_decl(ImportState *s, const char *p,
                                           const char *raw) {
    (void)raw;
    /* Only exported-C files split a decl into a file-scope value line plus a
     * snippet `@declare` marker. A markerless file is plain REPL source: its
     * `static float x = 1;` line IS the document row, so it must fall through
     * to the raw-scene-body handler instead of being consumed for its value. */
    if (s->allow_raw_scene)
        return 0;
    return import_try_stash_predef_decl(s, p);
}

static int import_handle_snippet_body(ImportState *s, const char *p,
                                      const char *raw) {
    (void)raw;
    return import_try_snippet_body_line(s, p);
}

static int import_run_handlers(const ImportLineHandlerSpec *handlers,
                               int handler_count,
                               ImportState *s,
                               const char *p,
                               const char *raw) {
    for (int handler_idx = 0; handler_idx < handler_count; handler_idx++) {
        (void)handlers[handler_idx].kind; /* names the ordered phase in tables */
        if (handlers[handler_idx].handle(s, p, raw))
            return 1;
    }
    return 0;
}

#define IMPORT_HANDLER_COUNT(table) ((int)(sizeof(table) / sizeof((table)[0])))

static const ImportLineHandlerSpec IMPORT_PRE_SNIPPET_HANDLERS[] = {
    /* kind,                               handle */
    { IMPORT_LINE_WORKSPACE_HEADER,        import_handle_workspace_header },
    { IMPORT_LINE_FUNCTION_BODY,           import_handle_function_body    },
    { IMPORT_LINE_FUNCTION_HEADER,         import_handle_function_header  },
    { IMPORT_LINE_PRE_SNIPPET_STASH_DECL,  import_handle_stash_predef_decl },
    { IMPORT_LINE_SNIPPET_START,           import_handle_snippet_start    },
    { IMPORT_LINE_RAW_SCENE_BODY,          import_handle_raw_scene_body   },
    { IMPORT_LINE_PENDING_COMMENT,         import_handle_pending_comment  },
};

static const ImportLineHandlerSpec IMPORT_SNIPPET_HANDLERS[] = {
    /* kind,                    handle */
    { IMPORT_LINE_SNIPPET_END,  import_handle_snippet_end  },
    { IMPORT_LINE_BLANK,        import_handle_blank        },
    { IMPORT_LINE_PREDEF_DECL,  import_handle_predef_decl  },
    { IMPORT_LINE_SNIPPET_BODY, import_handle_snippet_body },
};

static void import_process_line(ImportState *s, const char *p, const char *raw) {
    /* Everything after Snippet end is discarded. */
    if (s->past_snippet)
        return;
    /* ATOMIC has already rejected the file; the rest of it is unreached, not
     * judged. Stopping here rather than at the read loop alone also covers the
     * staged-function flush, which replays buffered lines through this path
     * long after their physical rows went by. */
    if (s->load_failed)
        return;

    if (!s->in_snippet) {
        (void)import_run_handlers(IMPORT_PRE_SNIPPET_HANDLERS,
                                  IMPORT_HANDLER_COUNT(IMPORT_PRE_SNIPPET_HANDLERS),
                                  s, p, raw);
        return;
    }

    (void)import_run_handlers(IMPORT_SNIPPET_HANDLERS,
                              IMPORT_HANDLER_COUNT(IMPORT_SNIPPET_HANDLERS),
                              s, p, raw);
}

/* Clear result fields that still live in ReplState because the public
 * per-line workspace-header parser exposes them to legacy callers. The
 * cfg/deferred-var accumulators are per ImportState during file import. */
static void import_clear_pending_result_fields(void) {
    IMPORT_EXPORT_WRITABLE->pending_scene_name[0]    = '\0';
    IMPORT_EXPORT_WRITABLE->pending_workspace_dir[0] = '\0';
}

static const char *import_find_block_comment_start(const char *s) {
    int in_str = 0;
    int in_chr = 0;

    if (!s)
        return NULL;
    for (; *s; s++) {
        if (in_str || in_chr) {
            if (*s == '\\' && s[1]) {
                s++;
                continue;
            }
            if (in_str && *s == '"')
                in_str = 0;
            else if (in_chr && *s == '\'')
                in_chr = 0;
            continue;
        }
        if (*s == '"') {
            in_str = 1;
            continue;
        }
        if (*s == '\'') {
            in_chr = 1;
            continue;
        }
        if (s[0] == '/' && s[1] == '/')
            return NULL;
        if (s[0] == '/' && s[1] == '*')
            return s;
    }
    return NULL;
}

static int import_normalize_c89_comment_line(const char *line,
                                             char *out, size_t out_sz) {
    const char *open;
    const char *close;
    const char *tail;
    size_t prefix_len;
    size_t payload_len;
    size_t off;

    if (!line || !out || out_sz == 0)
        return 0;

    open = import_find_block_comment_start(line);
    if (!open)
        return 0;
    close = strstr(open + 2, "*/");
    if (!close)
        return 0;
    tail = close + 2;
    while (*tail && isspace((unsigned char)*tail))
        tail++;
    if (*tail)
        return 0;

    prefix_len = (size_t)(open - line);
    payload_len = (size_t)(close - (open + 2));
    if (payload_len > 0 && open[2 + payload_len - 1] == ' ')
        payload_len--;
    if (prefix_len + 3 + payload_len >= out_sz)
        payload_len = out_sz > prefix_len + 4 ? out_sz - prefix_len - 4 : 0;

    memcpy(out, line, prefix_len);
    off = prefix_len;
    out[off++] = '/';
    out[off++] = '/';
    memcpy(out + off, open + 2, payload_len);
    off += payload_len;
    out[off] = '\0';
    return 1;
}

/* Strip the parts of `line` that sit inside a C block comment spanning
 * more than one physical line, carrying the open/closed state across
 * calls in `*in_comment`. A bare slash-star opener selects the canonical
 * block form used for consecutive user-authored comments; those payload
 * rows are restored to `//` lines instead. Generated scaffold prose puts
 * text on its opener and therefore remains discard-only. Comments that
 * open and close on one line are left untouched -
 * import_normalize_c89_comment_line rewrites those into `//` form, and
 * the workspace-header directives depend on it.
 *
 * Without this, the opener line of a multi-line comment reads as an
 * unterminated statement, so the line accumulator glues the whole
 * comment onto the next real one. The exporter's globals block is
 * written under exactly such a comment, which is why `static float t =
 * <snapshot>;` - always the first predef decl - never round-tripped.
 *
 * Returns 0 only for a line this strip emptied out (drop it); a line it
 * did not touch is always kept, blank ones included - those still carry
 * a REPL blank command through the snippet body. */
static int import_strip_block_comment_span(char *line, int *in_comment,
                                           int *preserve_comment) {
    int stripped = 0;

    if (*in_comment) {
        char *close = strstr(line, "*/");
        if (*preserve_comment) {
            char *payload = line;
            size_t indent_len;

            while (*payload && isspace((unsigned char)*payload))
                payload++;
            indent_len = (size_t)(payload - line);
            if (close && payload == close) {
                *in_comment = 0;
                *preserve_comment = 0;
                return 0;
            }
            if (*payload == '*') {
                size_t payload_len;
                payload++;
                if (close && close >= payload)
                    *close = '\0';
                payload_len = strlen(payload);
                if (indent_len + 3 >= MAX_LINE_LEN)
                    payload_len = 0;
                else if (indent_len + 2 + payload_len >= MAX_LINE_LEN)
                    payload_len = MAX_LINE_LEN - indent_len - 3;
                memmove(line + indent_len + 2, payload, payload_len);
                line[indent_len] = '/';
                line[indent_len + 1] = '/';
                line[indent_len + 2 + payload_len] = '\0';
                if (close) {
                    *in_comment = 0;
                    *preserve_comment = 0;
                }
                return 1;
            }
        }
        if (!close) {
            line[0] = '\0';
            return 0;
        }
        *in_comment = 0;
        *preserve_comment = 0;
        memmove(line, close + 2, strlen(close + 2) + 1);
        stripped = 1;
    }

    const char *scan = line;
    for (;;) {
        const char *open = import_find_block_comment_start(scan);
        if (!open)
            break;
        const char *close = strstr(open + 2, "*/");
        if (!close) {
            const char *before = line;
            const char *after = open + 2;
            int bare_opener = 1;

            while (before < open && isspace((unsigned char)*before))
                before++;
            if (before != open)
                bare_opener = 0;
            while (*after && isspace((unsigned char)*after))
                after++;
            if (*after)
                bare_opener = 0;
            *in_comment = 1;
            *preserve_comment = bare_opener;
            line[open - line] = '\0';
            stripped = 1;
            break;
        }
        scan = close + 2;
    }

    if (!stripped)
        return 1;

    for (const char *q = line; *q; q++) {
        if (!isspace((unsigned char)*q))
            return 1;
    }
    return 0;
}

static int is_line_comment_or_directive(const char *line) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0') return 1; /* blank */
    if (p[0] == '/' && p[1] == '/') return 1; /* comment or snippet directive */
    if (*p == '#') return 1; /* preprocessor directive */
    return 0;
}

/* A continuation row is known before the logical statement has been parsed,
 * so its final document row is not available yet. Mark it in the caller-owned
 * physical-row map and resolve it when the accumulator flushes. Keeping the
 * marker in that already-per-physical-row storage avoids a second capped row
 * list: every row the caller gave the map can be represented. */
#define IMPORT_ROW_MAP_CONTINUATION (-2)

typedef struct {
    char accum[MAX_LINE_LEN];
    int line_no;
    int depth;
    int truncated;
    int in_block_comment;
    int preserve_block_comment;
} ImportAccum;

static void import_row_map_mark_continuation(ImportState *s, int line_no) {
    ReplSceneRowMap *map;

    if (!s)
        return;
    map = s->opts.row_map;
    if (!map || !map->doc_row || line_no < 1 || line_no > map->cap)
        return;
    if (map->doc_row[line_no - 1] == REPL_ROW_MAP_NONE)
        map->doc_row[line_no - 1] = IMPORT_ROW_MAP_CONTINUATION;
    if (line_no > map->count)
        map->count = line_no;
}

static void import_row_map_resolve_continuations(ImportState *s,
                                                  int first_line,
                                                  int last_line,
                                                  int doc_row) {
    ReplSceneRowMap *map;
    int line_no;

    if (!s)
        return;
    map = s->opts.row_map;
    if (!map || !map->doc_row)
        return;
    if (last_line > map->cap)
        last_line = map->cap;
    for (line_no = first_line + 1; line_no <= last_line; line_no++) {
        if (map->doc_row[line_no - 1] == IMPORT_ROW_MAP_CONTINUATION)
            map->doc_row[line_no - 1] = doc_row;
    }
}

/* Emit the accumulated logical statement through import_process_line,
 * restoring the line number to where the statement began so a warning
 * points at its first physical line. accum is built from already
 * left-trimmed segments, so no leading-whitespace skip is needed.
 *
 * import_process_line records the map against s->line_no, which is the
 * first physical row. Every later code-bearing continuation row produced
 * the same document row and has to be mapped too, or a cursor-only move
 * onto those physical rows is a no-op. Comment / blank lines dropped
 * while the statement was open stay REPL_ROW_MAP_NONE.
 * Clears the accumulator (and its truncation flag). */
static void import_flush_accum(ImportState *s, ImportAccum *acc) {
    int saved;
    int doc;

    if (!acc || !acc->accum[0])
        return;
    saved = s->line_no;
    s->line_no = acc->line_no;
    if (acc->truncated) {
        import_state_warn_parse_status(s, acc->line_no,
                                       "statement exceeded %d chars, truncated",
                                       (int)MAX_LINE_LEN);
    }
    import_process_line(s, acc->accum, acc->accum);
    if (s->opts.row_map &&
        acc->line_no >= 1 && acc->line_no <= s->opts.row_map->cap) {
        doc = s->opts.row_map->doc_row[acc->line_no - 1];
        import_row_map_resolve_continuations(s, acc->line_no, saved, doc);
    }
    s->line_no = saved;
    acc->accum[0]   = '\0';
    acc->truncated  = 0;
}

ReplExampleSourceFormat repl_scene_format_from_path(const char *path) {
    size_t len = path ? strlen(path) : 0;

    return (len > 4 && strcmp(path + len - 4, ".glr") == 0)
        ? REPL_EXAMPLE_SOURCE_GLR
        : REPL_EXAMPLE_SOURCE_C;
}

void repl_scene_load_opts_init(ReplSceneLoadOpts *opts,
                               ReplExampleSourceFormat format) {
    if (!opts)
        return;
    opts->format       = format;
    opts->policy       = REPL_SCENE_LOAD_POLICY_TOLERANT;
    opts->apply_cfg    = 1;
    opts->camera_apply = REPL_CAMERA_APPLY_IMPORT;
    opts->allow_empty  = 0;
    opts->quiet        = 0;
    opts->row_map      = NULL;
}

static void import_begin_load(ImportState *state,
                              const ReplSceneLoadOpts *opts,
                              ReplImportResult *result) {
    if (result) {
        result->scene_name[0]    = '\0';
        result->workspace_dir[0] = '\0';
    }
    import_clear_pending_result_fields();
    import_state_init(state);
    if (opts)
        state->opts = *opts;

    /* Capture BEFORE the func-alias wipe below and before the caller's first
     * inserted row, so the rollback target is the document as it stood when
     * the load was asked for. Callers that clear live state first (the scene
     * slot loaders, the example loader) therefore roll back to *their* cleared
     * state - which is still the right answer at this layer: the reader
     * guarantees "no partially-loaded document", and restoring the previous
     * scene is the caller's own transaction. */
    if (state->opts.policy == REPL_SCENE_LOAD_POLICY_ATOMIC) {
        state->atomic_stash = (SceneSnapshot *)malloc(sizeof(SceneSnapshot));
        if (state->atomic_stash)
            scene_snapshot_capture_live(state->atomic_stash);
    }

    import_row_map_clear(state->opts.row_map);
    repl_camera_header_init(&state->camera);
    repl_doc_order_init(&state->order);
    repl_func_alias_clear_all();
}

/* Name the source. Whether the canonical document order applies is decided by
 * the caller-supplied format, not by this label.
 *
 * It applies to `.glr` - the *authored* format - and not to an exported `.c`,
 * whose layout the exporter fixes and which legitimately violates the phases:
 * reshape() and main() are emitted after display(), so a machine that
 * classified every line would reject every file this project writes. A
 * generated `.c` is canonical by construction; a hand-edited one is already
 * outside the format's guarantees.
 *
 * The label used to decide it, by sniffing `.glr` off its end. That works for
 * a filesystem path and silently fails for everything else: a catalog entry
 * labelled "Torus Knot" got check_order = 0 and lost canonical-order
 * validation with no diagnostic. */
static void import_set_source(ImportState *s, const char *label) {
    s->source_label = label;
    s->check_order  = (s->opts.format == REPL_EXAMPLE_SOURCE_GLR);
    repl_camera_header_set_sink(&s->camera, import_camera_diag,
                                (void *)label);
    repl_doc_order_set_sink(&s->order, import_order_diag, (void *)label);
}

static int import_line_has_snippet_marker(const char *line) {
    return line && strstr(line, "Snippet start") != NULL;
}

static int import_line_has_func_body_marker(const char *line) {
    return line &&
           strstr(line, "@") != NULL &&
           strstr(line, REPL_SNIPPET_DIRECTIVE_FUNC_BODY) != NULL;
}

static int import_lines_have_snippet_marker(const char *const *lines) {
    for (int i = 0; lines && lines[i]; i++) {
        if (import_line_has_snippet_marker(lines[i]))
            return 1;
    }
    return 0;
}

static int import_lines_have_func_body_marker(const char *const *lines) {
    for (int i = 0; lines && lines[i]; i++) {
        if (import_line_has_func_body_marker(lines[i]))
            return 1;
    }
    return 0;
}

static int import_file_has_snippet_marker(FILE *f) {
    char line[MAX_LINE_LEN];
    int found = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (import_line_has_snippet_marker(line)) {
            found = 1;
            break;
        }
    }
    rewind(f);
    return found;
}

static int import_file_has_func_body_marker(FILE *f) {
    char line[MAX_LINE_LEN];
    int found = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (import_line_has_func_body_marker(line)) {
            found = 1;
            break;
        }
    }
    rewind(f);
    return found;
}

/* Signal the region a line sits in, then offer it to the shared camera
 * reader - before block-comment stripping, so the reader sees the file's own
 * text and its brace counter stays in step, and before the snippet and
 * function-depth gates, so a tagged line in either position is consumed with
 * a diagnostic rather than falling through into the document as geometry.
 *
 * Returns 1 when the line was consumed (accepted or rejected). */
static int import_offer_camera_line(ImportState *state, const char *line) {
    const char *p = line;

    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "// Snippet start", 16) == 0 ||
        strncmp(p, "/* Snippet start", 16) == 0)
        repl_camera_header_set_region(&state->camera,
                                      REPL_CAMERA_REGION_SNIPPET);
    else if (strncmp(p, "// Snippet end", 14) == 0 ||
             strncmp(p, "/* Snippet end", 14) == 0)
        repl_camera_header_set_region(&state->camera,
                                      REPL_CAMERA_REGION_POST_SNIPPET);
    else if (strncmp(p, REPL_EXPORT_DISPLAY_OPEN_SIGNATURE,
                     sizeof(REPL_EXPORT_DISPLAY_OPEN_SIGNATURE) - 1) == 0)
        repl_camera_header_set_region(&state->camera,
                                      REPL_CAMERA_REGION_DISPLAY);

    {
        ReplCameraLineResult result =
            repl_camera_header_offer(&state->camera, line, state->line_no);

        if (state->check_order &&
            !repl_doc_order_offer(&state->order, line, state->line_no,
                                  result != REPL_CAMERA_LINE_NOT_CAMERA))
            state->order_failed = 1;
        return result != REPL_CAMERA_LINE_NOT_CAMERA;
    }
}

static void import_process_physical_line(ImportState *state,
                                         char *line,
                                         ImportAccum *acc) {
    char normalized_line[MAX_LINE_LEN];
    const char *proc_line = line;

    if (import_offer_camera_line(state, line))
        return;

    /* C-only scaffolding emitted before exported function bodies. The marker
     * lets generated C declare forward and mutually recursive calls without
     * adding prototype rows to the REPL document on re-import. */
    if (strstr(line, REPL_EXPORT_FORWARD_DECL_MARKER) != NULL)
        return;

    /* Multi-line comments are dropped before anything else looks at the
     * line, so they can never be mistaken for an in-progress statement. */
    if (!import_strip_block_comment_span(line, &acc->in_block_comment,
                                         &acc->preserve_block_comment))
        return;

    if (import_normalize_c89_comment_line(line, normalized_line, sizeof(normalized_line)))
        proc_line = normalized_line;

    if (is_line_comment_or_directive(proc_line)) {
        /* A blank, comment, or directive line that falls inside an
         * in-progress statement (open bracket, or no terminator
         * seen yet) is dropped - keep accumulating rather than
         * flushing a half-built statement. With nothing in progress
         * it is a standalone line, processed as before. */
        if (acc->accum[0])
            return;
        const char *p = proc_line;
        while (*p && isspace((unsigned char)*p)) p++;
        import_process_line(state, p, proc_line);
        return;
    }

    const char *app = proc_line;
    while (*app && isspace((unsigned char)*app)) app++;

    if (!acc->accum[0]) {
        acc->line_no = state->line_no;
        acc->depth = 0;
        acc->truncated = 0;
    } else {
        import_row_map_mark_continuation(state, state->line_no);
    }

    size_t accum_len = strlen(acc->accum);
    if (accum_len > 0 && acc->accum[accum_len - 1] != ' ') {
        if (accum_len + 1 < MAX_LINE_LEN) {
            acc->accum[accum_len++] = ' ';
            acc->accum[accum_len] = '\0';
        } else {
            acc->truncated = 1;
        }
    }

    int code_len = 0;
    /* NULL block-comment state: import_strip_block_comment_span already ran
     * on this line and owns that policy, so nothing here can be inside a
     * block comment. A caller scanning raw lines must pass real state. */
    char last = repl_scan_code_line(app, &acc->depth, NULL, &code_len);
    /* Complete when no bracket is open and the last code char closes
     * a statement (`;`), opens/closes a block (`{`/`}`), or is a stray
     * `:` (see repl_is_stmt_terminator). The depth gate is what stops a split
     * compound literal - `(GLfloat[]){` - or a ternary `:` from flushing
     * mid-expression. */
    int complete = acc->depth <= 0 && repl_is_stmt_terminator(last);

    /* On a continuation line, append only the code portion so a
     * trailing `// ...` can't bleed into the rest of the statement.
     * On the line that completes the statement, append it whole,
     * including any trailing comment - the parser keeps inline
     * comments on a command. */
    size_t seg_len = complete ? strlen(app) : (size_t)code_len;
    size_t avail = MAX_LINE_LEN - 1 - accum_len;
    size_t take = seg_len <= avail ? seg_len : avail;
    if (take < seg_len)
        acc->truncated = 1;
    memcpy(acc->accum + accum_len, app, take);
    acc->accum[accum_len + take] = '\0';

    if (complete) {
        import_flush_accum(state, acc);
        acc->depth = 0;
    }
}

/* Every failure exit runs through here: report, drop the accumulators, and -
 * under ATOMIC - put the document back the way it was found. Returns 0 so
 * call sites read `return import_fail_load(...)`. */
static int import_fail_load(ImportState *state, const char *msg) {
    import_workspace_accum_reset(&state->workspace);
    import_clear_pending_result_fields();
    /* The rows this described no longer exist - ATOMIC put the old document
     * back, and under TOLERANT the caller is discarding the load anyway. */
    import_row_map_clear(state->opts.row_map);
    repl_set_status_error(msg);
    /* Order matters: clear the staged buffers before restoring, so nothing is
     * holding a pointer into a document that is about to be replaced. */
    import_staged_functions_clear(state);
    if (state->atomic_stash) {
        (void)scene_snapshot_apply_live(state->atomic_stash,
                                        SCENE_SNAPSHOT_CAMERA_SNAP);
        repl_source_scope_depth_cache_invalidate();
        repl_mark_source_dirty();
    }
    return 0;
}

static void import_release_atomic_stash(ImportState *state) {
    free(state->atomic_stash);
    state->atomic_stash = NULL;
}

static int import_finish_load(ImportState *state,
                              ReplImportResult *result,
                              const char *source_name,
                              int had_read_err,
                              int close_failed,
                              int truncated_line,
                              ImportAccum *acc) {
    const char *label = source_name && source_name[0] ? source_name : "<memory>";
    char msg[REPL_STATUS_TEXT_MAX];
    int ok;

    if (!truncated_line && !state->load_failed)
        import_flush_accum(state, acc);

    /* Caller passes the results of read/close checks from the file-based reader. */
    if (had_read_err || close_failed) {
        snprintf(msg, sizeof(msg), "Error: cannot read %s", label);
        ok = import_fail_load(state, msg);
        goto done;
    }
    if (truncated_line) {
        snprintf(msg, sizeof(msg), "Import failed: line too long in %s", label);
        ok = import_fail_load(state, msg);
        goto done;
    }
    /* Rejection, not a warning: the whole point of a canonical form is that
     * non-canonical files do not accumulate. Every violation has already been
     * reported through the sink, so the summary here just names the count -
     * the migration worklist is the sink's output, not this line. */
    if (state->order_failed) {
        snprintf(msg, sizeof(msg),
                 "Import failed: %s is not in canonical order (%d violations)",
                 label, state->order.violations);
        ok = import_fail_load(state, msg);
        goto done;
    }
    /* ATOMIC's own verdict. The offending line already produced its per-line
     * diagnostic through the warn sink; this names the file and the row so the
     * status bar says which one to go fix. */
    if (state->load_failed) {
        snprintf(msg, sizeof(msg),
                 "Import failed: %s:%d rejected (nothing loaded)",
                 label, state->failed_line_no);
        ok = import_fail_load(state, msg);
        goto done;
    }
    /* A file that parsed cleanly but yielded no document is a failure under
     * either policy - checked here, ahead of the cfg drain, so an ATOMIC load
     * cannot leave a rejected file's `@cfg` side effects behind on the scene
     * that survived. (TOLERANT keeps its historical ordering below: it applies
     * the bag first and reports afterwards.) */
    if (state->opts.policy == REPL_SCENE_LOAD_POLICY_ATOMIC &&
        state->loaded <= 0 && !state->opts.allow_empty) {
        snprintf(msg, sizeof(msg),
                 "Import failed: no commands loaded from %s", label);
        fprintf(stderr, "%s\n", msg);
        ok = import_fail_load(state, msg);
        goto done;
    }

    /* Values are restored directly when @declare is parsed from the stash. */

    /* Drain @cfg accumulator: hand the parsed (slug, val) bag to the
     * controller-installed bridge, which knows how to apply each slug
     * to its owner's state. Without a bridge (the demo case), the
     * accumulator is dropped silently - that's the architectural goal
     * (no glr_config dependency from src/repl/import.c).
     *
     * apply_cfg = 0 (the watched-reload policy, BYOE D3) skips the *apply* and
     * keeps the *reset*. Skipping the whole call would leave a stale
     * accumulator for the next non-watch load to inherit. */
    if (state->opts.apply_cfg)
        import_workspace_cfg_apply_and_reset(&state->workspace);
    else
        import_workspace_accum_reset(&state->workspace);

    if (result) {
        snprintf(result->scene_name, sizeof(result->scene_name),
                 "%s", IMPORT_EXPORT_VIEW.pending_scene_name);
        snprintf(result->workspace_dir, sizeof(result->workspace_dir),
                 "%s", IMPORT_EXPORT_VIEW.pending_workspace_dir);
    }

    /* Resolve and apply the camera exactly once, after the whole file has
     * been offered. IMPORT snaps and adopts the pose as the scene default -
     * what "Reset camera" returns to - which a header-less file must not do,
     * and finish() handles by making no bridge call at all. NONE (the watched
     * reload) still runs the reader's own validation and notes, and still
     * makes no bridge call - see ReplCameraApplyMode. */
    {
        ReplCameraFinish camera_fin =
            repl_camera_header_finish(&state->camera, state->opts.camera_apply);
        import_camera_report_missing(&camera_fin, label);
    }

    if (state->loaded > 0 || state->opts.allow_empty) {
        repl_source_scope_depth_cache_invalidate();
        repl_reformat_program();
        /* Publish the post-import cursor to the host. Without this,
         * downstream callers (e.g. repl_load_scene_as_new_slot, which
         * snapshots via repl_dispatch_edit_line_get right after this
         * returns) see the pre-import value - leaving Load Scene From
         * File parked at line 0 with insert mode off, so the next
         * commit replaces the first imported command instead of appending. */
        repl_dispatch_edit_line_set(state->edit_line);
        if (state->warnings > 0)
            snprintf(msg, sizeof(msg),
                     "Loaded %d commands from %s (%d warnings)",
                     state->loaded, label, state->warnings);
        else
            snprintf(msg, sizeof(msg),
                     "Loaded %d commands from %s", state->loaded, label);
        repl_set_status(msg);
        /* An allow_empty caller that loaded nothing has nothing to announce on
         * stderr - it is mid-keystroke in someone's editor, not a load event.
         * `quiet` says the same thing about a caller that loaded plenty: at
         * WIP typing rate this line is scrollback, not news. */
        if (state->loaded > 0 && !state->opts.quiet)
            fprintf(stderr, "%s\n", msg);
    } else {
        /* File opened and read cleanly but produced no commands - an empty
         * or non-REPL file, or one whose every line failed to parse. Without
         * this the caller (repl_load_initial_commands) silently falls back to
         * the default example, stranding any @cfg side effects already
         * applied above (e.g. a light theme) on the wrong scene. */
        if (state->warnings > 0)
            snprintf(msg, sizeof(msg),
                     "Import failed: no commands loaded from %s (%d unparsed line%s)",
                     label, state->warnings, state->warnings == 1 ? "" : "s");
        else
            snprintf(msg, sizeof(msg),
                     "Import failed: no commands loaded from %s", label);
        repl_set_status_error(msg);
        fprintf(stderr, "%s\n", msg);
    }
    ok = state->loaded > 0 || state->opts.allow_empty;
    import_staged_functions_clear(state);

done:
    import_release_atomic_stash(state);
    return ok;
}

int repl_scene_load_from_lines(const char *const *lines,
                               const char *source_name,
                               const ReplSceneLoadOpts *opts,
                               ReplImportResult *result) {
    ImportState state;
    ReplSceneLoadOpts fallback;

    /* NULL means "the defaults for this source", not "the defaults for
     * exported C": a caller that passes NULL for a `.glr` must not silently
     * lose canonical-order checking, which is the very dropout the explicit
     * format exists to close. */
    if (!opts) {
        repl_scene_load_opts_init(&fallback,
                                  repl_scene_format_from_path(source_name));
        opts = &fallback;
    }
    import_begin_load(&state, opts, result);
    import_set_source(&state, source_name);
    state.allow_raw_scene = !import_lines_have_snippet_marker(lines);
    state.has_func_body_markers = import_lines_have_func_body_marker(lines);

    int truncated_line = 0;
    ImportAccum acc = { .accum = "", .line_no = 0, .depth = 0, .truncated = 0 };

    for (int i = 0; lines && lines[i]; i++) {
        size_t raw_len = strlen(lines[i]);
        if (raw_len >= sizeof(acc.accum)) {
            truncated_line = 1;
            break;
        }
        char line[MAX_LINE_LEN];
        memcpy(line, lines[i], raw_len + 1);
        state.line_no++;
        import_process_physical_line(&state, line, &acc);
        if (state.load_failed)
            break;   /* ATOMIC: the rest of the file is unreached */
    }

    return import_finish_load(&state, result, source_name,
                              0, 0, truncated_line, &acc);
}

int repl_export_load_from_lines(const char *const *lines,
                                const char *source_name,
                                ReplImportResult *result) {
    ReplSceneLoadOpts opts;
    repl_scene_load_opts_init(&opts, repl_scene_format_from_path(source_name));
    return repl_scene_load_from_lines(lines, source_name, &opts, result);
}

/* Parse an already-open seekable stream and take ownership of it. Both named
 * files and stdin's anonymous spool land here, so marker scans, long-line
 * handling, diagnostics, and close-error behavior stay identical. */
static int import_load_seekable_stream(FILE *f, const char *source_name,
                                       const ReplSceneLoadOpts *opts,
                                       ReplImportResult *result) {
    ImportState state;
    import_begin_load(&state, opts, result);
    import_set_source(&state, source_name);
    state.allow_raw_scene = !import_file_has_snippet_marker(f);
    state.has_func_body_markers = import_file_has_func_body_marker(f);

    char line[MAX_LINE_LEN];
    int truncated_line = 0;
    ImportAccum acc = { .accum = "", .line_no = 0, .depth = 0, .truncated = 0 };
    while (fgets(line, sizeof(line), f)) {
        state.line_no++;
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

        import_process_physical_line(&state, line, &acc);
        if (state.load_failed)
            break;   /* ATOMIC: the rest of the file is unreached */
    }

    /* Mirror the save-side ferror/fclose pair: fgets returning NULL
     * conflates EOF with read error, so the read-loop above can't
     * surface I/O failures by itself. Check ferror before closing. */
    int had_read_err = ferror(f);
    int close_failed = fclose(f) != 0;
    return import_finish_load(&state, result, source_name,
                              had_read_err, close_failed, truncated_line,
                              &acc);
}

int repl_scene_load_from_file(const char *filename,
                              const ReplSceneLoadOpts *opts,
                              ReplImportResult *result) {
    ReplSceneLoadOpts fallback;
    FILE *f;

    if (!opts) {   /* see repl_scene_load_from_lines */
        repl_scene_load_opts_init(&fallback,
                                  repl_scene_format_from_path(filename));
        opts = &fallback;
    }
    f = fopen(filename, "r");
    if (!f) {
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof(msg), "Error: cannot open %s: %s",
                 filename && filename[0] ? filename : "<file>", strerror(errno));
        repl_set_status_error(msg);
        fprintf(stderr, "%s\n", msg);
        return 0;
    }
    return import_load_seekable_stream(f, filename, opts, result);
}

int repl_export_load_from_file(const char *filename, ReplImportResult *result) {
    ReplSceneLoadOpts opts;
    /* The filesystem adapter: the one place a format is still derived from a
     * suffix, because a path is all this caller has. */
    repl_scene_load_opts_init(&opts, repl_scene_format_from_path(filename));
    return repl_scene_load_from_file(filename, &opts, result);
}

int repl_export_load_from_stream(FILE *input, const char *source_name,
                                 ReplImportResult *result) {
    const char *label = source_name && source_name[0] ? source_name : "<stream>";
    unsigned char buf[4096];
    size_t nread;
    int copy_failed = 0;
    int saved_errno = 0;

    if (!input) {
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof(msg), "Error: cannot read %s: invalid stream", label);
        repl_set_status_error(msg);
        fprintf(stderr, "%s\n", msg);
        return 0;
    }

    /* The importer makes two marker-detection passes before its parsing pass.
     * Pipes cannot rewind, so spool once to an ISO C anonymous temporary file.
     * tmpfile() is removed automatically when closed, including on failures. */
    FILE *spool = tmpfile();
    if (!spool) {
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof(msg), "Error: cannot buffer %s: %s",
                 label, strerror(errno));
        repl_set_status_error(msg);
        fprintf(stderr, "%s\n", msg);
        return 0;
    }

    while ((nread = fread(buf, 1, sizeof(buf), input)) > 0) {
        if (fwrite(buf, 1, nread, spool) != nread) {
            copy_failed = 1;
            saved_errno = errno;
            break;
        }
    }
    if (!copy_failed && ferror(input)) {
        copy_failed = 1;
        saved_errno = errno;
    }
    if (!copy_failed && fflush(spool) != 0) {
        copy_failed = 1;
        saved_errno = errno;
    }
    if (!copy_failed && fseek(spool, 0, SEEK_SET) != 0) {
        copy_failed = 1;
        saved_errno = errno;
    }

    if (copy_failed) {
        char msg[REPL_STATUS_TEXT_MAX];
        fclose(spool);
        snprintf(msg, sizeof(msg), "Error: cannot buffer %s: %s", label,
                 saved_errno ? strerror(saved_errno) : "I/O error");
        repl_set_status_error(msg);
        fprintf(stderr, "%s\n", msg);
        return 0;
    }

    {
        ReplSceneLoadOpts opts;
        /* A stream label is not a path, but it is all there is; `<stdin>`
         * resolves to exported-C, matching the pre-explicit-format sniff. */
        repl_scene_load_opts_init(&opts, repl_scene_format_from_path(label));
        return import_load_seekable_stream(spool, label, &opts, result);
    }
}
