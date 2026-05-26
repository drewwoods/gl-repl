#include "repl/export.h"
#include "source_document.h"     /* source_document_insert_line */
#include "repl/load.h"           /* repl_load_apply_line — step 5b */
/* glr_camera.h removed in step 4a: the export pipeline no longer
 * references glr_camera_*. Camera state flows through the
 * controller-installed ReplExportCameraBridge (see src/repl/export.h).
 * glr_config.h was already dropped in step 4 for the same reason. */
#include "config.h"             /* REPL_OUTLINE_POLYGON_OFFSET_{FACTOR,UNITS} */
#include "repl/command_store.h"
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/executor.h"        /* repl_apply_state_cmd */
#include "repl/parser.h"
#include "repl/pipeline.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"

#define IMPORT_EXPORT_STATE (repl_state_import_export_mut())
#define g_workspace_header_lines (IMPORT_EXPORT_STATE->workspace_header_lines)
#define g_workspace_header_line_count (IMPORT_EXPORT_STATE->workspace_header_line_count)
#define g_render_state_lines (IMPORT_EXPORT_STATE->render_state_lines)
#define g_cam_lines (IMPORT_EXPORT_STATE->cam_lines)
#define g_export_scene_name_hint (IMPORT_EXPORT_STATE->export_scene_name_hint)
#define g_pending_scene_name (IMPORT_EXPORT_STATE->pending_scene_name)
#define g_pending_workspace_dir (IMPORT_EXPORT_STATE->pending_workspace_dir)

#include "repl/cfg_baseline.h"

#define g_export_cfg_bridge (repl_config_bridge())

static const char k_cfg_slug_point_attenuation[] = "point_attenuation";
static const char k_cfg_slug_msaa[] = "msaa";
static const char k_cfg_slug_line_smooth[] = "line_smooth";
static const char k_cfg_slug_vertex_outlines[] = "vertex_outlines";
static const char k_cfg_slug_vertex_points[] = "vertex_points";

/* Camera bridge — same shape as the cfg bridge. Step 4a moved camera-block
 * emission and parsing through this interface so src/repl/export.c no longer
 * references glr_camera_*. The default bridge is installed by
 * glr_ctrl_install_app_services. */
static const ReplExportCameraBridge *g_export_camera_bridge = NULL;

void repl_export_install_camera_bridge(const ReplExportCameraBridge *bridge) {
    g_export_camera_bridge = bridge;
}

const ReplExportCameraBridge *repl_export_camera_bridge(void) {
    return g_export_camera_bridge;
}

/* Reshape-projection bridge — same install/getter shape as the camera
 * bridge. NULL on the demo / in tests, where the perspective default is
 * emitted. */
static const ReplExportProjectionBridge *g_export_projection_bridge = NULL;

void repl_export_install_projection_bridge(const ReplExportProjectionBridge *bridge) {
    g_export_projection_bridge = bridge;
}

const ReplExportProjectionBridge *repl_export_projection_bridge(void) {
    return g_export_projection_bridge;
}

/* Resolves to live scene state via the bridge. Callable directly only
 * from single-pass, off-frame-loop consumers (the file writer). The code
 * panel must NOT call this — the controller resolves it once per frame
 * into UiRenderSnapshot.reshape_proj_lines so the panel's row-count and
 * render passes (which straddle scene_render_3d_scene) agree. See
 * ARCHITECTURE.md, "Rule — where a per-frame dynamic value is resolved". */
int repl_export_reshape_projection_lines(const char *out[REPL_EXPORT_PROJ_LINES]) {
    static char buf[REPL_EXPORT_PROJ_LINES][REPL_EXPORT_PROJ_LINE_MAX];
    int count = 0;

    if (g_export_projection_bridge &&
        g_export_projection_bridge->fill_reshape_block) {
        ReplExportProjectionBlock blk;
        blk.count = 0;
        g_export_projection_bridge->fill_reshape_block(&blk);
        if (blk.count > 0 && blk.count <= REPL_EXPORT_PROJ_LINES) {
            for (int i = 0; i < blk.count; i++) {
                snprintf(buf[i], sizeof buf[i], "%s", blk.lines[i]);
                out[i] = buf[i];
            }
            count = blk.count;
        }
    }

    if (count == 0) {
        /* Canonical default: the steady 3D frustum with the *correct*
         * far plane (the historical literal said 100.0; the live scene
         * uses 200.0). */
        snprintf(buf[0], sizeof buf[0],
                 "  gluPerspective(45.0, (float)w/(float)h, 0.1, 200.0);");
        out[0] = buf[0];
        count = 1;
    }
    return count;
}

/* Pending @cfg accumulator: parse_cfg() during import populates this; the
 * import driver drains it via the bridge after parse completes. */
static ReplConfigBag g_import_cfg_accumulator;

static void import_cfg_accumulator_reset(void) {
    repl_config_bag_clear(&g_import_cfg_accumulator);
}

static void import_cfg_accumulator_apply_and_reset(void) {
    if (g_export_cfg_bridge && g_export_cfg_bridge->apply &&
        g_import_cfg_accumulator.count > 0) {
        g_export_cfg_bridge->apply(&g_import_cfg_accumulator);
    }
    import_cfg_accumulator_reset();
}

void repl_export_apply_pending_cfg(void) {
    import_cfg_accumulator_apply_and_reset();
}

/* File-scope C boilerplate: includes, macros, and the rotation globals.
 * Lines here appear in BOTH the code panel header AND the exported C
 * file's file-scope region (above display()). They never run at REPL
 * time — see g_display_header for the display() opening, and the
 * controller (src/app/glr_ctrl.c) for what actually executes. */
/* Wrap each common <math.h> identifier so a user-declared predef of
 * the same name doesn't collide with the math.h function symbol at
 * compile time. Pattern: `#define X _X` BEFORE `#include <math.h>`
 * renames math.h's `X` to `_X`; `#undef X` AFTER the include restores
 * the bare name for the user's declarations below. The exported user
 * code (`static float X = ...;`) is then distinct from math.h's
 * (now-renamed) `_X`.
 *
 * Coverage: Bessel functions (j0/j1/jn, y0/y1/yn) and the gamma
 * family (gamma/lgamma/tgamma) — all single- or two-letter math.h
 * functions that are plausible user variable names. The y0/y1 pair
 * was the original motivator; jn/yn/gamma/lgamma/tgamma are added
 * defensively after the audit in #7 of the bug investigation.
 *
 * If a math.h identifier isn't actually declared on the host platform
 * (e.g. `pow10` is GNU-only), the `#define X _X` is harmless — the
 * preprocessor rewrite has nothing to act on, and the `#undef`
 * leaves the name back where it started. */
const char *g_header_pre[] = {
    "#define j0 _j0",
    "#define j1 _j1",
    "#define jn _jn",
    "#define y0 _y0",
    "#define y1 _y1",
    "#define yn _yn",
    "#define gamma _gamma",
    "#define lgamma _lgamma",
    "#define tgamma _tgamma",
    "#include \"gl_includes.h\"",
    "#include <math.h>",
    "#include <stdlib.h>",
    "#undef j0",
    "#undef j1",
    "#undef jn",
    "#undef y0",
    "#undef y1",
    "#undef yn",
    "#undef gamma",
    "#undef lgamma",
    "#undef tgamma",
    "",
    "#ifndef M_PI",
    "#define M_PI 3.14159265358979323846",
    "#endif",
    "",
    "static float g_angle = 0.0f;",
    "static int   g_rotating = 0;",
    "",
    NULL
};

/* display() opening: shared by the code panel header and by
 * emit_export_display_begin. Keeping these as literal strings in one
 * array (rather than hardcoded fprintf calls in the exporter) is what
 * keeps panel and export byte-identical for the opening lines. */
const char *g_display_header[] = {
    "void display() {",
    "  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "  glLoadIdentity();",
    "  glPushAttrib(GL_ALL_ATTRIB_BITS);",
    NULL
};

/* Deferred @var values: set by parse_workspace_header_line alongside the
 * normal auto-declare+set-value path.  load_from_file re-applies them after
 * the snippet is processed so that // @declare markers in the snippet can
 * undeclare and re-declare variables (creating CMD_VAR_DECLARE commands)
 * without losing the saved values from the workspace header. */
#define MAX_DEFERRED_VAR_VALUES 64
typedef struct { char name[REPL_PREDEF_NAME_MAX]; float value; } DeferredVar;
static DeferredVar g_deferred_var_values[MAX_DEFERRED_VAR_VALUES];
static int         g_deferred_var_count = 0;

/* workspace_slug_from_name moved out of src/repl/export.c. The bridge
 * implementation in glr_export.c owns slug derivation now; src/repl/export.c
 * just emits/parses the (slug, value) pairs the bridge produces
 * (implemented in step 4 of the decouple plan). */

static void workspace_format_float(char *buf, size_t n, float v) {
    snprintf(buf, n, "%.9g", (double)v);
}

static void export_format_decl_float(char *buf, size_t n, float v) {
    repl_format_source_float(buf, (int)n, v);
}

/* Per-call editor-text view, set by the public entry points
 * (`repl_export_save_output`, `repl_dump_code_panel_text`) before they
 * invoke any helper that reads source text. Static helpers route
 * through `export_document_text` instead of calling `editor_buffer_line`
 * directly so the source-text dependency is declared at the API
 * boundary as a SourceTextView parameter rather than a hidden
 * global reach-through. */
static SourceTextView s_export_text_view;

static const char *export_document_text(int cmd_idx) {
    const char *text;

    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return "";

    text = source_text_line(s_export_text_view, cmd_idx);
    return (text && text[0]) ? text : "";
}

/* ========================================================================= */
/* Workspace header directive table                                           */
/*                                                                            */
/* Reader and writer share a directive table so every `@name` that            */
/* load_from_file can parse has a matching emit step in                       */
/* refresh_workspace_header_lines (and vice versa).  Each entry pairs a       */
/* parse(args) with an emit(append into g_workspace_header_lines) step.       */
/* Order in this table determines emit order.                                 */
/* ========================================================================= */

typedef int  (*WorkspaceParseFn)(const char *args);
typedef void (*WorkspaceEmitFn)(int *n);
typedef int  (*SnippetParseFn)(const char *args, int *loaded,
                               int *warnings, int *edit_line_inout);

typedef struct {
    const char       *name;      /* directive name without leading `@` */
    size_t            name_len;
    WorkspaceParseFn  parse;     /* parse(args-after-name-and-space) */
    WorkspaceEmitFn   emit;      /* append zero or more lines, bumping *n */
} WorkspaceDirective;

typedef struct {
    const char    *name;      /* snippet marker name without leading `@` */
    size_t         name_len;
    SnippetParseFn parse;     /* parse(args-after-name-and-space) */
} SnippetDirective;

static const char k_snippet_directive_declare[] = "declare";

/* --- workspace-dir --------------------------------------------------------- */

static int parse_workspace_dir(const char *args) {
    size_t char_idx = 0;
    while (*args && char_idx < REPL_WORKSPACE_DIR_MAX - 1)
        g_pending_workspace_dir[char_idx++] = *args++;
    g_pending_workspace_dir[char_idx] = '\0';
    while (char_idx > 0 && isspace((unsigned char)g_pending_workspace_dir[char_idx - 1]))
        g_pending_workspace_dir[--char_idx] = '\0';
    return 1;
}

static void emit_workspace_dir(int *n) {
    const char *workspace_dir = repl_state_workspace_dir();

    if (workspace_dir[0] && *n < MAX_WORKSPACE_HEADER_LINES) {
        snprintf(g_workspace_header_lines[(*n)++], WORKSPACE_HEADER_LINE_LEN,
                 "// @workspace-dir %s", workspace_dir);
    }
}

/* --- scene-name ------------------------------------------------------------ */

static int parse_scene_name(const char *args) {
    size_t char_idx = 0;
    while (*args && char_idx < USER_SCENE_NAME_MAX - 1)
        g_pending_scene_name[char_idx++] = *args++;
    g_pending_scene_name[char_idx] = '\0';
    while (char_idx > 0 && isspace((unsigned char)g_pending_scene_name[char_idx - 1]))
        g_pending_scene_name[--char_idx] = '\0';
    return 1;
}

static void emit_scene_name(int *n) {
    /* Explicit export hint overrides the active-slot name. */
    const char *scene_name = g_export_scene_name_hint;
    if ((!scene_name || !*scene_name) && repl_active_user_scene() >= 0)
        scene_name = repl_user_scene_name(repl_active_user_scene());
    if (scene_name && *scene_name && *n < MAX_WORKSPACE_HEADER_LINES) {
        snprintf(g_workspace_header_lines[(*n)++], WORKSPACE_HEADER_LINE_LEN,
                 "// @scene-name %s", scene_name);
    }
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
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;
    ExprCtx ctx = { p, NULL, 0 };
    float val = repl_eval_expr(&ctx);
    int idx = repl_eval_find_predef_var_idx(name);
    if (idx < 0) {
        char err[REPL_DIAG_TEXT_MAX];
        if (!repl_eval_declare_predef_var(name, err, sizeof(err)))
            return 0;
        idx = repl_eval_find_predef_var_idx(name);
        if (idx < 0)
            return 0;
    }
    g_predef_vars[idx].value = val;
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

static void emit_vars(int *n) {
    for (int var_idx = 0; var_idx < g_num_predef_vars && *n < MAX_WORKSPACE_HEADER_LINES; var_idx++) {
        char vbuf[32];
        workspace_format_float(vbuf, sizeof(vbuf), g_predef_vars[var_idx].value);
        if (repl_format_fits(g_workspace_header_lines[*n], WORKSPACE_HEADER_LINE_LEN,
                             "// @var %s = %s", g_predef_vars[var_idx].name, vbuf))
            (*n)++;
    }
}

/* --- func aliases ----------------------------------------------------------
 * Round-trip the func-alias table through the workspace header so a saved
 * `drawCube` definition reloads into the same slot. Format:
 *   `// @func 0 = drawCube`
 * Slots without an alias don't emit a line. */

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
    repl_func_alias_set(slot, name);
    return 1;
}

static void emit_func_aliases(int *n) {
    for (int slot = 0;
         slot < REPL_FUNC_SLOT_COUNT && *n < MAX_WORKSPACE_HEADER_LINES;
         slot++) {
        const char *alias = repl_func_alias_get(slot);
        if (!alias) continue;
        if (repl_format_fits(g_workspace_header_lines[*n],
                             WORKSPACE_HEADER_LINE_LEN,
                             "// @func %d = %s", slot, alias))
            (*n)++;
    }
}



static int parse_cfg(const char *args) {
    char slug[32];
    if (!repl_config_extract_slug(args, slug, sizeof(slug)))
        return 0;
    const char *p = args;
    while (*p && (isalnum((unsigned char)*p) || *p == '_'))
        p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    int val = (int)strtol(p, NULL, 10);
    /* Apply immediately via the bridge: callers (tests, the importer,
     * the example loader) expect line-by-line @cfg parsing to update
     * live state synchronously. We also accumulate so callers that
     * want to drain at the end of a batch (repl_export_apply_pending_cfg)
     * see the same set of (slug, val) pairs — but the live state has
     * already been updated by the per-line apply. */
    if (g_export_cfg_bridge && g_export_cfg_bridge->apply) {
        ReplConfigBag single;
        repl_config_bag_clear(&single);
        repl_config_bag_set_int(&single, slug, val);
        g_export_cfg_bridge->apply(&single);
    }
    repl_config_bag_set_int(&g_import_cfg_accumulator, slug, val);
    return 1;
}

static void emit_cfgs(int *n) {
    /* The bridge populates the bag with (slug, value) pairs. If no
     * bridge is installed (the demo case), the bag stays empty and
     * no @cfg lines are emitted. */
    if (!g_export_cfg_bridge || !g_export_cfg_bridge->fill_all)
        return;
    ReplConfigBag cfg;
    repl_config_bag_clear(&cfg);
    g_export_cfg_bridge->fill_all(&cfg);
    for (int i = 0; i < cfg.count && *n < MAX_WORKSPACE_HEADER_LINES; i++) {
        snprintf(g_workspace_header_lines[(*n)++], WORKSPACE_HEADER_LINE_LEN,
                 "// @cfg %s = %s", cfg.items[i].key, cfg.items[i].value);
    }
}

/* --- directive table (source of truth) ------------------------------------- */

#define WS_DIR(name, parse_fn, emit_fn) \
    { name, sizeof(name) - 1, parse_fn, emit_fn }

static const WorkspaceDirective WORKSPACE_DIRECTIVES[] = {
    /* Emit order matches this array.  Reader dispatches by name. */
    WS_DIR("scene-name",    parse_scene_name,    emit_scene_name),
    WS_DIR("workspace-dir", parse_workspace_dir, emit_workspace_dir),
    WS_DIR("var",           parse_var,           emit_vars),
    WS_DIR("func",          parse_func_alias,    emit_func_aliases),
    WS_DIR("cfg",           parse_cfg,           emit_cfgs),
};
#define WORKSPACE_DIRECTIVE_COUNT \
    ((int)(sizeof(WORKSPACE_DIRECTIVES) / sizeof(WORKSPACE_DIRECTIVES[0])))

#undef WS_DIR

#define SNIPPET_DIR(name, parse_fn) \
    { name, sizeof(name) - 1, parse_fn }

void repl_state_refresh_workspace_header_lines(void) {
    int line_count = 0;
    if (line_count < MAX_WORKSPACE_HEADER_LINES) {
        snprintf(g_workspace_header_lines[line_count++], WORKSPACE_HEADER_LINE_LEN,
                 "// @workspace: REPL state (auto-saved)");
    }
    for (int dir_idx = 0; dir_idx < WORKSPACE_DIRECTIVE_COUNT; dir_idx++)
        WORKSPACE_DIRECTIVES[dir_idx].emit(&line_count);
    g_workspace_header_line_count = line_count;
}

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

const char *g_header_post[] = {
    NULL
};

typedef struct {
    const char *repl_line;
    /* Slug name of the cfg toggle that gates this line, or NULL if
     * unconditional. Looked up via the export config bridge —
     * src/repl/export.c does not call glr_config_get directly. */
    const char *toggle_slug;
} InitBootstrapEntry;

/* Bootstrap commands are REPL lines that run once at startup through
 * repl_apply_state_cmd. Each frame's glPushAttrib/glPopAttrib bracket then
 * preserves this state across user commands. Comment entries (// ...)
 * parse as CMD_COMMENT — repl_apply_state_cmd no-ops them, but the editor
 * and exporter render them as section headers in the init() body. */
static const InitBootstrapEntry g_init_bootstrap_repl[] = {
    { "// Background color used by glClear at the start of every frame.", NULL },
    { "glClearColor(0.10, 0.10, 0.10, 1.0);", NULL },
    { "// Color material: glColor* values drive ambient + diffuse so user", NULL },
    { "// code can tint lit geometry without explicit glMaterial calls.", NULL },
    { "glEnable(GL_COLOR_MATERIAL);", NULL },
    { "glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);", NULL },
    { "// Light both sides of each face, and set non-zero specular and", NULL },
    { "// shininess defaults so user-enabled lighting matches the REPL", NULL },
    { "// preview instead of the GL-default black specular.", NULL },
    { "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);", NULL },
    { "glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, (GLfloat[]){0.4, 0.4, 0.4, 1.0});", NULL },
    { "glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, (GLfloat[]){30.0});", NULL },
    { "// Blending: standard src-over for translucent geometry.", NULL },
    { "glEnable(GL_BLEND);", NULL },
    { "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);", NULL },
        { "// Point attenuation: distance-based point-size falloff.", k_cfg_slug_point_attenuation },
    { "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1.0, 0.0, 0.02);",
            k_cfg_slug_point_attenuation },
};

/* Helper: look up a cfg toggle via the installed bridge. Returns the
 * fallback when no bridge is installed (the demo case) or the slug is
 * unknown. */
static int init_bootstrap_toggle_get(const char *slug, int fallback) {
    if (!slug) return 1;  /* unconditional entries are always "on" */
    if (!g_export_cfg_bridge || !g_export_cfg_bridge->get_int)
        return fallback;
    return g_export_cfg_bridge->get_int(slug, fallback);
}

/* True when a bootstrap entry must be skipped *entirely* — not applied,
 * not emitted, not counted — because the runtime GL context lacks the
 * entry point it depends on. Only the point-attenuation entry has such
 * a dependency (glPointParameterfv). This is deliberately independent
 * of and ahead of the toggle-slug disable path in the loops below:
 * that path only *neutralizes* attenuation by re-applying a
 * no-falloff CMD_POINT_PARAMETER_FV, which would still invoke the
 * missing glPointParameterfv. When unsupported we must not touch it
 * at all. */
static int init_bootstrap_entry_unsupported(const InitBootstrapEntry *entry) {
    return entry->toggle_slug
    && strcmp(entry->toggle_slug, k_cfg_slug_point_attenuation) == 0
        && !repl_executor_point_parameter_supported();
}
#define NUM_INIT_BOOTSTRAP \
    ((int)(sizeof(g_init_bootstrap_repl) / sizeof(g_init_bootstrap_repl[0])))

static ReplParsedLine g_init_bootstrap_cmds[NUM_INIT_BOOTSTRAP];
static int            g_init_bootstrap_ready = 0;

static const char *g_init_host_only_visible_c[] = {
    "  GLfloat lm_amb[] = { 0.15f, 0.15f, 0.20f, 1.0f };",
    "  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);",
    NULL
};

static const char *g_init_host_only_tess_c[] = {
    "  g_tess = gluNewTess();",
    "  gluTessCallback(g_tess, GLU_TESS_BEGIN, (_GluCb)_tess_vtx_begin_cb);",
    "  gluTessCallback(g_tess, GLU_TESS_END, (_GluCb)_tess_vtx_end_cb);",
    "  gluTessCallback(g_tess, GLU_TESS_VERTEX, (_GluCb)_tess_vtx_cb);",
    "  gluTessCallback(g_tess, GLU_TESS_COMBINE, (_GluCb)_tess_comb_cb);",
    "  gluTessCallback(g_tess, GLU_TESS_ERROR, (_GluCb)_tess_err_cb);",
    "  gluTessCallback(g_tess, GLU_TESS_EDGE_FLAG, (_GluCb)glEdgeFlag);",
    NULL
};

static void format_cmd_source_as_c(char *out, size_t out_sz,
                                   const char *source_text,
                                   int translate_exprs);

const char *g_footer_pre_init[] = {
    "",
    "  glPopAttrib();",
    "  glutSwapBuffers();",
    "}",
    "",
    "void reshape(int w, int h) {",
    "  glViewport(0, 0, w, h);",
    "  glMatrixMode(GL_PROJECTION);",
    "  glLoadIdentity();",
    /* Dynamic: every consumer expands this via
     * repl_export_reshape_projection_lines() so the saved file and the
     * live code panel reflect the scene's current projection. */
    REPL_EXPORT_RESHAPE_PROJ_SENTINEL,
    "  glMatrixMode(GL_MODELVIEW);",
    "}",
    "",
    "void keyboard(unsigned char key, int x, int y) {",
    "  (void)x; (void)y;",
    "  if (key == ' ') g_rotating = !g_rotating;",
    "  if (key == 27) exit(0);",
    "}",
    "",
    "void tick(int v) {",
    "  (void)v;",
    "  /* Fixed-step time advance, matching the live REPL's",
    "   * repl_state_time_advance(0.016) timer. Keeps tDelta = (t -",
    "   * tLast) * 10 constant across frames, independent of how long",
    "   * each render actually takes. */",
    "  t += 0.016f;",
    "  if (g_rotating) g_angle += 0.5f;",
    "  glutPostRedisplay();",
    "  glutTimerFunc(16, tick, 0);",
    "}",
    "",
    "void init() {",
    "  glLineWidth(1.5f);",
    NULL
};

const char *g_footer_post_init[] = {
    "}",
    "",
    "int main(int argc, char **argv) {",
    "  glutInit(&argc, argv);",
    "  glutInitDisplayMode(GLUT_DOUBLE|GLUT_RGB|GLUT_DEPTH|GLUT_MULTISAMPLE);",
    "  glutInitWindowSize(800, 600);",
    "  glutCreateWindow(\"OpenGL REPL\");",
    "  init();",
    "  glutDisplayFunc(display);",
    "  glutReshapeFunc(reshape);",
    "  glutKeyboardFunc(keyboard);",
    "  glutTimerFunc(16, tick, 0);",
    "  glutMainLoop();",
    "  return 0;",
    "}",
    NULL
};

static void emit_footer_post_init(FILE *f, int win_w, int win_h) {
    fprintf(f,
        "}\n"
        "\n"
        "int main(int argc, char **argv) {\n"
        "  glutInit(&argc, argv);\n"
        "  glutInitDisplayMode(GLUT_DOUBLE|GLUT_RGB|GLUT_DEPTH|GLUT_MULTISAMPLE);\n"
        "  glutInitWindowSize(%d, %d);\n"
        "  glutCreateWindow(\"OpenGL REPL\");\n"
        "  init();\n"
        "  glutDisplayFunc(display);\n"
        "  glutReshapeFunc(reshape);\n"
        "  glutKeyboardFunc(keyboard);\n"
        "  glutTimerFunc(16, tick, 0);\n"
        "  glutMainLoop();\n"
        "  return 0;\n"
        "}\n",
        win_w, win_h);
}

static int init_host_only_line_count(void) {
    int count = 0;
    while (g_init_host_only_visible_c[count])
        count++;
    return count;
}

static void parse_init_bootstrap(void) {
    if (g_init_bootstrap_ready)
        return;

    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        char bootstrap_err[REPL_STATUS_TEXT_MAX];
        bootstrap_err[0] = '\0';
        ReplParseContext parse_ctx = {
            .err_buf = bootstrap_err,
            .err_sz  = (int)sizeof(bootstrap_err),
        };
        ReplParsedLine pl;
        if (!repl_parser_parse_command_ctx(g_init_bootstrap_repl[bootstrap_idx].repl_line,
                                    &pl, &parse_ctx)) {
            fprintf(stderr, "init bootstrap parse failed: %s%s%s\n",
                    g_init_bootstrap_repl[bootstrap_idx].repl_line,
                    bootstrap_err[0] ? " — " : "",
                    bootstrap_err);
            abort();
        }
        g_init_bootstrap_cmds[bootstrap_idx] = pl;
    }
    g_init_bootstrap_ready = 1;
}

void repl_ensure_init_bootstrap_ready(void) {
    if (!g_init_bootstrap_ready)
        parse_init_bootstrap();
}

void repl_apply_init_bootstrap(void) {
    repl_ensure_init_bootstrap_ready();

    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        const InitBootstrapEntry *entry = &g_init_bootstrap_repl[bootstrap_idx];
        if (init_bootstrap_entry_unsupported(entry))
            continue;  /* runtime lacks glPointParameterfv: skip, don't neutralize */
        if (entry->toggle_slug && !init_bootstrap_toggle_get(entry->toggle_slug, 1)) {
            if (g_init_bootstrap_cmds[bootstrap_idx].cmd.type == CMD_POINT_PARAMETER_FV &&
                (GLenum)g_init_bootstrap_cmds[bootstrap_idx].cmd.args[0] ==
                    GL_POINT_DISTANCE_ATTENUATION) {
                /* args[0]=pname; args[1..3]=const/linear/quadratic.
                 * Neutralize attenuation to a constant 1 (no falloff). */
                GLCmd disabled = g_init_bootstrap_cmds[bootstrap_idx].cmd;
                disabled.args[1] = 1.0f;
                disabled.args[2] = 0.0f;
                disabled.args[3] = 0.0f;
                repl_apply_state_cmd(&disabled, 1.0f);
            }
            continue;
        }
        repl_apply_state_cmd(&g_init_bootstrap_cmds[bootstrap_idx].cmd, 1.0f);
    }
}

int repl_export_init_section_line_count(void) {
    int count = init_host_only_line_count() + repl_export_lights_init_line_count();

    repl_ensure_init_bootstrap_ready();
    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        const InitBootstrapEntry *entry = &g_init_bootstrap_repl[bootstrap_idx];
        if (init_bootstrap_entry_unsupported(entry))
            continue;
        if (entry->toggle_slug && !init_bootstrap_toggle_get(entry->toggle_slug, 1))
            continue;
        count++;
    }

    return count;
}

void repl_export_init_section_line(int i, char *buf, size_t n) {
    int host_count = init_host_only_line_count();
    int lights_count = repl_export_lights_init_line_count();
    int enabled_idx = 0;

    if (!buf || n == 0)
        return;

    repl_ensure_init_bootstrap_ready();
    if (i < 0 || i >= repl_export_init_section_line_count()) {
        buf[0] = '\0';
        return;
    }

    if (i < host_count) {
        snprintf(buf, n, "%s", g_init_host_only_visible_c[i]);
        return;
    }

    i -= host_count;
    if (i < lights_count) {
        repl_export_lights_init_line(i, buf, n);
        return;
    }

    i -= lights_count;
    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        const InitBootstrapEntry *entry = &g_init_bootstrap_repl[bootstrap_idx];
        if (init_bootstrap_entry_unsupported(entry))
            continue;
        if (entry->toggle_slug && !init_bootstrap_toggle_get(entry->toggle_slug, 1))
            continue;
        if (enabled_idx == i) {
            format_cmd_source_as_c(buf, n,
                                   g_init_bootstrap_cmds[bootstrap_idx].text,
                                   0);
            return;
        }
        enabled_idx++;
    }

    buf[0] = '\0';
}

static void emit_export_init_section_to_file(FILE *f, int include_tess) {
    char line[MAX_LINE_LEN];

    for (int line_idx = 0; g_init_host_only_visible_c[line_idx]; line_idx++)
        fprintf(f, "%s\n", g_init_host_only_visible_c[line_idx]);
    if (include_tess)
        for (int line_idx = 0; g_init_host_only_tess_c[line_idx]; line_idx++)
            fprintf(f, "%s\n", g_init_host_only_tess_c[line_idx]);

    int n_lights = repl_export_lights_init_line_count();
    for (int lights_idx = 0; lights_idx < n_lights; lights_idx++) {
        repl_export_lights_init_line(lights_idx, line, sizeof(line));
        fprintf(f, "%s\n", line);
    }

    repl_ensure_init_bootstrap_ready();
    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        const InitBootstrapEntry *entry = &g_init_bootstrap_repl[bootstrap_idx];
        if (init_bootstrap_entry_unsupported(entry))
            continue;
        if (entry->toggle_slug && !init_bootstrap_toggle_get(entry->toggle_slug, 1))
            continue;
        format_cmd_source_as_c(line, sizeof(line),
                               g_init_bootstrap_cmds[bootstrap_idx].text,
                               0);
        fprintf(f, "%s\n", line);
    }
}

static void emit_export_header_pre(FILE *f) {
    /* Ask the camera bridge for the g_angle preamble line.
     * Without a bridge installed (the demo case) we emit the
     * placeholder unchanged so the file is still valid C. */
    char angle_line[REPL_EXPORT_CAMERA_PREAMBLE_MAX];
    angle_line[0] = '\0';
    if (g_export_camera_bridge && g_export_camera_bridge->fill_save_preamble)
        g_export_camera_bridge->fill_save_preamble(angle_line, (int)sizeof(angle_line));

    /* NOTE: resolving a dynamic boilerplate line at the consumer site
     * (as here) is safe ONLY because this one consumer is the file
     * writer — a single pass off the frame loop. Do NOT copy this shape
     * for a line the code panel reads: panel row-count and render
     * straddle scene_render_3d_scene() and would diverge. Resolve once
     * into UiRenderSnapshot instead. See ARCHITECTURE.md, "Rule — where
     * a per-frame dynamic value is resolved". */
    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++) {
        if (strcmp(g_header_pre[line_idx], "static float g_angle = 0.0f;") == 0) {
            if (angle_line[0])
                fprintf(f, "%s\n", angle_line);
            else
                fprintf(f, "%s\n", g_header_pre[line_idx]);
            continue;
        }
        fprintf(f, "%s\n", g_header_pre[line_idx]);
    }
}

static void emit_export_cam_lines(FILE *f) {
    /* The bridge owns the camera-line format. Without a
     * bridge (demo case) the // camera block is omitted from the
     * exported file — that's fine, the demo doesn't export
     * (implemented in step 4a of the decouple plan). */
    if (!g_export_camera_bridge || !g_export_camera_bridge->fill_save_block)
        return;
    ReplExportCameraBlock block;
    memset(&block, 0, sizeof(block));
    g_export_camera_bridge->fill_save_block(&block);
    if (!block.present) return;
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++) {
        if (block.lines[i][0])
            fprintf(f, "%s\n", block.lines[i]);
    }
}

void repl_refresh_render_state_strings(void) {
    /* Render-config toggles moved to glr_state. Read them
     * via the bridge's slug-keyed get_int — same opaque path the rest
     * of the export pipeline uses for cfg state. The demo doesn't
     * install a bridge, so the toggles fall back to "Enable" /
     * "Disable" defaults below; the demo never exports anyway
     * (implemented in step 7a). */
    int msaa_on        = g_export_cfg_bridge && g_export_cfg_bridge->get_int
                         ? g_export_cfg_bridge->get_int(k_cfg_slug_msaa, 1)
                         : 1;
    int line_smooth_on = g_export_cfg_bridge && g_export_cfg_bridge->get_int
                         ? g_export_cfg_bridge->get_int(k_cfg_slug_line_smooth, 0)
                         : 0;
    snprintf(g_render_state_lines[0], sizeof(g_render_state_lines[0]),
             "  gl%s(GL_MULTISAMPLE);",
             msaa_on ? "Enable" : "Disable");
    snprintf(g_render_state_lines[1], sizeof(g_render_state_lines[1]),
             "  gl%s(GL_LINE_SMOOTH);",
             line_smooth_on ? "Enable" : "Disable");
}

/* The camera-block parser state machine moved to the bridge
 * implementation (glr_camera_export.c). src/repl/export.c just delegates
 * import-side line consumption and reset to the bridge (implemented in
 * step 4a). */
void import_cam_parser_reset(void) {
    if (g_export_camera_bridge && g_export_camera_bridge->reset_import)
        g_export_camera_bridge->reset_import();
}

int import_parse_cam_line(const char *text) {
    if (!g_export_camera_bridge || !g_export_camera_bridge->try_consume_import_line)
        return 0;
    return g_export_camera_bridge->try_consume_import_line(text);
}

void repl_refresh_camera_lines(void) {
    /* Bridge-driven preview: the bridge formats the 4-line block from
     * current camera state with numeric ry (no g_angle placeholder).
     * Without a bridge installed (the demo case), g_cam_lines stays
     * empty — the demo doesn't render a code panel. */
    if (!g_export_camera_bridge || !g_export_camera_bridge->fill_display_block) {
        for (int i = 0; i < REPL_EXPORT_CAMERA_LINES &&
                        i < (int)(sizeof(g_cam_lines) / sizeof(g_cam_lines[0])); i++)
            g_cam_lines[i][0] = '\0';
        return;
    }
    ReplExportCameraBlock block;
    memset(&block, 0, sizeof(block));
    g_export_camera_bridge->fill_display_block(&block);
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES &&
                    i < (int)(sizeof(g_cam_lines) / sizeof(g_cam_lines[0])); i++) {
        snprintf(g_cam_lines[i], sizeof(g_cam_lines[i]), "%s", block.lines[i]);
    }
}

/* Light-text generators. Per the "fewer surprises" principle, every GL
 * state change should be visible to the editor; the lights are the
 * largest hidden block. Init lines (colors + baseline disable) belong in
 * init() because they don't depend on the modelview. Position lines
 * belong in display() after the camera transforms because
 * glLightfv(GL_POSITION) snapshots the active modelview — calling them
 * in init() would lock positions to whatever modelview was current there
 * (identity), so they wouldn't orbit with the scene.
 *
 * The same text appears in the editor's code panel and the exported C.
 * Format uses inline `(GLfloat[]){...}` literals matching the bootstrap
 * REPL commands' rendered C; `%.9g` keeps float32 round-trip safety
 * while still trimming redundant trailing zeros. */

static const char *const k_light_names[MAX_LIGHTS] = {
    "GL_LIGHT0", "GL_LIGHT1", "GL_LIGHT2", "GL_LIGHT3"
};

#define LIGHT_INIT_LINES_PER_LIGHT 4  /* DIFFUSE, AMBIENT, SPECULAR, glDisable */

/* Section-header comment lines emitted above the generated GL calls.
 * Pure text — the editor renders them as dimmed init/header lines and
 * the export writes them verbatim as C comments. */
static const char *const k_lights_init_header[] = {
    "  // Per-light colors. Light POSITION is set in display() after the camera",
    "  // transforms (glLightfv(GL_POSITION) snapshots the active modelview).",
};
#define LIGHTS_INIT_HEADER_LINES \
    ((int)(sizeof(k_lights_init_header) / sizeof(k_lights_init_header[0])))

static const char *const k_lights_display_header[] = {
    "  // Light positions, set after the camera so they stay anchored in world space.",
};
#define LIGHTS_DISPLAY_HEADER_LINES \
    ((int)(sizeof(k_lights_display_header) / sizeof(k_lights_display_header[0])))

int repl_export_lights_init_line_count(void) {
    return LIGHTS_INIT_HEADER_LINES + MAX_LIGHTS * LIGHT_INIT_LINES_PER_LIGHT;
}

void repl_export_lights_init_line(int i, char *buf, size_t n) {
    if (!buf || n == 0) return;
    if (i < 0 || i >= repl_export_lights_init_line_count()) {
        buf[0] = '\0';
        return;
    }
    if (i < LIGHTS_INIT_HEADER_LINES) {
        snprintf(buf, n, "%s", k_lights_init_header[i]);
        return;
    }
    i -= LIGHTS_INIT_HEADER_LINES;
    int light_idx = i / LIGHT_INIT_LINES_PER_LIGHT;
    int sub_idx   = i % LIGHT_INIT_LINES_PER_LIGHT;
    ReplRenderState render = repl_state_render();
    const SceneLight *l = &render.lights[light_idx];
    const char *ln = k_light_names[light_idx];
    switch (sub_idx) {
    case 0:
        snprintf(buf, n,
                 "  glLightfv(%s, GL_DIFFUSE,  (GLfloat[]){%.9g, %.9g, %.9g, %.9g});",
                 ln, l->diffuse[0], l->diffuse[1], l->diffuse[2], l->diffuse[3]);
        break;
    case 1:
        snprintf(buf, n,
                 "  glLightfv(%s, GL_AMBIENT,  (GLfloat[]){%.9g, %.9g, %.9g, %.9g});",
                 ln, l->ambient[0], l->ambient[1], l->ambient[2], l->ambient[3]);
        break;
    case 2:
        snprintf(buf, n,
                 "  glLightfv(%s, GL_SPECULAR, (GLfloat[]){%.9g, %.9g, %.9g, %.9g});",
                 ln, l->specular[0], l->specular[1], l->specular[2], l->specular[3]);
        break;
    case 3:
        snprintf(buf, n, "  glDisable(%s);", ln);
        break;
    }
}

int repl_export_lights_display_line_count(void) {
    return LIGHTS_DISPLAY_HEADER_LINES + MAX_LIGHTS;
}

void repl_export_lights_display_line(int i, char *buf, size_t n) {
    if (!buf || n == 0) return;
    if (i < 0 || i >= repl_export_lights_display_line_count()) {
        buf[0] = '\0';
        return;
    }
    if (i < LIGHTS_DISPLAY_HEADER_LINES) {
        snprintf(buf, n, "%s", k_lights_display_header[i]);
        return;
    }
    i -= LIGHTS_DISPLAY_HEADER_LINES;
    ReplRenderState render = repl_state_render();
    const SceneLight *l = &render.lights[i];
    const char *ln = k_light_names[i];
    snprintf(buf, n,
             "  glLightfv(%s, GL_POSITION, (GLfloat[]){%.9g, %.9g, %.9g, %.9g});",
             ln, l->pos[0], l->pos[1], l->pos[2], l->pos[3]);
}

static void write_for_begin_as_c(FILE *f, const GLCmd *cmd,
                                 const char *source_text) {
    char var_name[REPL_PREDEF_NAME_MAX];
    const char *p = source_text;
    int indent = 0;
    while (p[indent] && isspace((unsigned char)p[indent])) indent++;

    char ind[REPL_INDENT_TEXT_MAX];
    if (indent > (int)sizeof(ind) - 1) indent = (int)sizeof(ind) - 1;
    memset(ind, ' ', (size_t)indent);
    ind[indent] = '\0';

    if (cmd->has_vars) {
        const char *hp = p;
        while (*hp && *hp != '(') hp++;
        if (*hp) hp++;
        while (*hp && isspace((unsigned char)*hp)) hp++;
        int var_name_idx = 0;
        while (*hp && (isalnum((unsigned char)*hp) || *hp == '_') &&
               var_name_idx < (int)sizeof(var_name) - 1)
            var_name[var_name_idx++] = *hp++;
        var_name[var_name_idx] = '\0';
        while (*hp && isspace((unsigned char)*hp)) hp++;
        if (*hp == ',') hp++;

        char start_s[128] = "", end_s[128] = "", step_s[128] = "";
        int nargs = 0;
        char *dests[] = { start_s, end_s, step_s };
        int dsizes[] = { (int)sizeof(start_s), (int)sizeof(end_s), (int)sizeof(step_s) };
        while (*hp && *hp != ')' && nargs < 3) {
            while (*hp && isspace((unsigned char)*hp)) hp++;
            const char *as = hp;
            hp = repl_scan_next_arg_delim(hp);
            int alen = (int)(hp - as);
            while (alen > 0 && isspace((unsigned char)as[alen - 1])) alen--;
            if (alen > dsizes[nargs] - 1) alen = dsizes[nargs] - 1;
            memcpy(dests[nargs], as, (size_t)alen);
            dests[nargs][alen] = '\0';
            nargs++;
            if (*hp == ',') hp++;
        }

        char c_start[128], c_end[128], c_step[128];
        repl_eval_expr_to_c(start_s, c_start, sizeof(c_start));
        repl_eval_expr_to_c(end_s, c_end, sizeof(c_end));
        if (step_s[0])
            repl_eval_expr_to_c(step_s, c_step, sizeof(c_step));
        else
            strncpy(c_step, "1.0f", sizeof(c_step));

        float step_v = cmd->args[2];
        if (step_v >= 0) {
            fprintf(f, "%sfor (float %s = %s; %s < %s; %s += %s) {\n",
                    ind, var_name, c_start, var_name, c_end, var_name, c_step);
        } else {
            fprintf(f, "%sfor (float %s = %s; %s > %s; %s += %s) {\n",
                    ind, var_name, c_start, var_name, c_end, var_name, c_step);
        }
        return;
    }

    float start_v, end_v, step_v;
    const char *body;
    if (repl_eval_parse_for_header(p, var_name, sizeof(var_name),
                         &start_v, &end_v, &step_v, &body)) {
        if (step_v == 1.0f) {
            fprintf(f, "%sfor (float %s = %.9g; %s < %.9g; %s += 1.0f) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name);
        } else if (step_v == -1.0f) {
            fprintf(f, "%sfor (float %s = %.9g; %s > %.9g; %s -= 1.0f) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name);
        } else if (step_v > 0) {
            fprintf(f, "%sfor (float %s = %.9g; %s < %.9g; %s += %.9gf) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name, step_v);
        } else {
            fprintf(f, "%sfor (float %s = %.9g; %s > %.9g; %s += %.9gf) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name, step_v);
        }
    } else {
        fprintf(f, "%s\n", source_text);
    }
}

static int cmd_type_is_tess(CmdType t) {
    return t == CMD_TESS_BEGIN_POLYGON ||
           t == CMD_TESS_BEGIN_CONTOUR ||
           t == CMD_TESS_END ||
           t == CMD_TESS_NORMAL ||
           t == CMD_TESS_COLOR ||
           t == CMD_TESS_VERTEX;
}

static int export_uses_tess_commands(void) {
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds_mut()[cmd_idx].valid)
            continue;
        if (cmd_type_is_tess(repl_state_document_cmds_mut()[cmd_idx].type))
            return 1;
    }

    return 0;
}

static int write_tess_source_as_c(FILE *f, const GLCmd *cmd,
                                  const char *source_text) {
    char payload[MAX_LINE_LEN];
    char raw_args[4][MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int arg_count;

    if (!repl_extract_paren_payload(source_text, payload, sizeof(payload)))
        return 0;

    arg_count = split_top_level_args(payload, raw_args, 4);
    if (arg_count < 0)
        return 0;

    for (int arg_idx = 0; arg_idx < arg_count; arg_idx++)
        repl_eval_expr_to_c(raw_args[arg_idx], c_args[arg_idx], sizeof(c_args[arg_idx]));

    switch (cmd->type) {
    case CMD_TESS_NORMAL:
        if (arg_count != 3)
            return 0;
        fprintf(f, "      { _tn[0]=%s; _tn[1]=%s; _tn[2]=%s; }\n",
                c_args[0], c_args[1], c_args[2]);
        return 1;
    case CMD_TESS_COLOR:
        if (arg_count == 3) {
            strncpy(c_args[3], "1", sizeof(c_args[3]) - 1);
            c_args[3][sizeof(c_args[3]) - 1] = '\0';
            arg_count = 4;
        }
        if (arg_count != 4)
            return 0;
        fprintf(f, "      { _tc[0]=%s; _tc[1]=%s; _tc[2]=%s; _tc[3]=%s; }\n",
                c_args[0], c_args[1], c_args[2], c_args[3]);
        return 1;
    case CMD_TESS_VERTEX:
        if (arg_count != 3)
            return 0;
        fprintf(f,
                "      { TessVertex *_v=&_tv[_tv_n++];"
                " _v->pos[0]=%s;_v->pos[1]=%s;_v->pos[2]=%s;"
                " memcpy(_v->normal,_tn,24); memcpy(_v->color,_tc,32);"
                " gluTessVertex(g_tess,_v->pos,_v); }\n",
                c_args[0], c_args[1], c_args[2]);
        return 1;
    default:
        return 0;
    }
}

static void format_cmd_source_as_c(char *out, size_t out_sz,
                                   const char *source_text,
                                   int translate_exprs) {
    char c_src[MAX_LINE_LEN];

    if (!out || out_sz == 0)
        return;

    if (translate_exprs)
        repl_eval_expr_to_c(source_text, c_src, sizeof(c_src));
    else {
        strncpy(c_src, source_text, sizeof(c_src) - 1);
        c_src[sizeof(c_src) - 1] = '\0';
    }

    snprintf(out, out_sz, "%s", c_src);
}

static void write_cmd_source_as_c(FILE *f, const char *source_text,
                                  int translate_exprs) {
    char out[MAX_LINE_LEN];

    format_cmd_source_as_c(out, sizeof(out), source_text, translate_exprs);
    fprintf(f, "%s\n", out);
}

static int find_export_block_end(int begin_idx) {
    int depth = 1;

    for (int cmd_idx = begin_idx + 1; cmd_idx < repl_state_document_count(); cmd_idx++) {
        CmdType t = repl_state_document_cmds_mut()[cmd_idx].type;
        if (repl_cmd_is_block_head(t)) depth++;
        else if (repl_cmd_is_block_end(t)) {
            if (--depth == 0)
                return cmd_idx;
        }
    }

    return repl_state_document_count();
}

static int comment_run_attached_func_idx(int start, int end_idx) {
    int cmd_idx = start;
    while (cmd_idx < end_idx && cmd_idx < repl_state_document_count() &&
           repl_state_document_cmds_mut()[cmd_idx].valid &&
           (repl_state_document_cmds_mut()[cmd_idx].type == CMD_COMMENT ||
            repl_state_document_cmds_mut()[cmd_idx].type == CMD_EMPTY))
        cmd_idx++;
    if (cmd_idx > start && cmd_idx < end_idx && cmd_idx < repl_state_document_count() &&
        repl_state_document_cmds_mut()[cmd_idx].valid && repl_state_document_cmds_mut()[cmd_idx].type == CMD_FUNC_DEF)
        return cmd_idx;
    return -1;
}

static void write_canonical_cmd_as_c(FILE *f, const GLCmd *cmd, int cmd_idx,
                                     int for_depth, int *tess_depth) {
    const char *source_text = export_document_text(cmd_idx);

    switch (cmd->type) {
    case CMD_EMPTY:
        fputc('\n', f);
        break;
    case CMD_COMMENT:
        fprintf(f, "%s\n", source_text);
        break;
    case CMD_VAR_DECLARE: {
        /* Variables are emitted as file-scope statics by write_predef_var_globals().
         * We cannot write a local float declaration here because it would shadow
         * the file-scope global.  Instead, emit a special REPL marker comment so
         * the importer can recreate the CMD_VAR_DECLARE when loading back into the
         * REPL without creating a C local variable.
         *
         * Inline initializers (`float x = 5;`) ride along as `name=value` so
         * the canonical decl text round-trips byte-exact through export+import. */
        float inits[MAX_NAMES_PER_DECL];
        int   has_init[MAX_NAMES_PER_DECL];
        for (int di = 0; di < MAX_NAMES_PER_DECL; di++) {
            inits[di] = 0;
            has_init[di] = 0;
        }
        {
            const char *p = source_text;
            while (*p && isspace((unsigned char)*p)) p++;
            /* Optional canonical `static ` prefix. */
            if (strncmp(p, "static", 6) == 0 && isspace((unsigned char)p[6])) {
                p += 6;
                while (*p && isspace((unsigned char)*p)) p++;
            }
            if (strncmp(p, "float", 5) == 0 &&
                (p[5] == ' ' || p[5] == '\t')) {
                p += 5;
                int idx = 0;
                while (*p && *p != ';' && idx < cmd->var_decl_count) {
                    while (*p && isspace((unsigned char)*p)) p++;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (*p == '=') {
                        p++;
                        while (*p && isspace((unsigned char)*p)) p++;
                        char *endp = NULL;
                        float v = strtof(p, &endp);
                        if (endp && endp != p) {
                            inits[idx] = v;
                            has_init[idx] = 1;
                            p = endp;
                        }
                    }
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (*p == ',') p++;
                    idx++;
                }
            }
        }
        int off = fprintf(f, "  // @%s", k_snippet_directive_declare);
        for (int di = 0; di < cmd->var_decl_count; di++) {
            if (has_init[di]) {
                char vbuf[32];
                export_format_decl_float(vbuf, sizeof(vbuf), inits[di]);
                off += fprintf(f, " %s=%s", cmd->var_names[di], vbuf);
            } else
                off += fprintf(f, " %s", cmd->var_names[di]);
        }
        (void)off;
        fprintf(f, "\n");
        break;
    }
    case CMD_VAR_ASSIGN: {
        char c_src[MAX_LINE_LEN];
        repl_eval_expr_to_c(source_text, c_src, sizeof(c_src));
        fprintf(f, "%s\n", c_src);
        break;
    }
    case CMD_SCRATCH_ASSIGN: {
        char c_src[MAX_LINE_LEN];
        repl_eval_expr_to_c(source_text, c_src, sizeof(c_src));
        fprintf(f, "%s\n", c_src);
        break;
    }
    case CMD_CALL:
        write_cmd_source_as_c(f, source_text, 1);
        break;
    case CMD_TESS_BEGIN_POLYGON:
        fprintf(f, "  { _tv_n=0; gluTessBeginPolygon(g_tess,NULL); }\n");
        *tess_depth = 1;
        break;
    case CMD_TESS_BEGIN_CONTOUR:
        fprintf(f, "    gluTessBeginContour(g_tess);\n");
        *tess_depth = 2;
        break;
    case CMD_TESS_END:
        if (*tess_depth == 2) {
            fprintf(f, "    gluTessEndContour(g_tess);\n");
            *tess_depth = 1;
        } else {
            fprintf(f, "  gluTessEndPolygon(g_tess);\n");
            *tess_depth = 0;
        }
        break;
    case CMD_TESS_NORMAL:
        if (!write_tess_source_as_c(f, cmd, source_text)) {
            fprintf(f, "      { _tn[0]=%.9g; _tn[1]=%.9g; _tn[2]=%.9g; }\n",
                    cmd->args[0], cmd->args[1], cmd->args[2]);
        }
        break;
    case CMD_TESS_COLOR:
        if (!write_tess_source_as_c(f, cmd, source_text)) {
            fprintf(f, "      { _tc[0]=%.9g; _tc[1]=%.9g; _tc[2]=%.9g; _tc[3]=%.9g; }\n",
                    cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
        }
        break;
    case CMD_TESS_VERTEX:
        if (!write_tess_source_as_c(f, cmd, source_text)) {
            fprintf(f,
                    "      { TessVertex *_v=&_tv[_tv_n++];"
                    " _v->pos[0]=%.9g;_v->pos[1]=%.9g;_v->pos[2]=%.9g;"
                    " memcpy(_v->normal,_tn,24); memcpy(_v->color,_tc,32);"
                    " gluTessVertex(g_tess,_v->pos,_v); }\n",
                    cmd->args[0], cmd->args[1], cmd->args[2]);
        }
        break;
    case CMD_LABEL: {
        /* Split format / post-args, translate the post-args
         * through repl_eval_expr_to_c, and re-emit with the
         * format string preserved byte-exact. The default branch
         * can't be used: repl_eval_expr_to_c walks the whole line
         * (including string contents) and would rewrite substrings
         * like "sin" or "PI" appearing inside the format string.
         *
         * Emits `label(...)`, a REPL-specific primitive whose
         * standalone-C definition is provided by a static wrapper
         * emitted in the file's prologue when needs_label is set.
         * See write_label_helper. */
        const char *open_p = strchr(source_text, '(');
        const char *close_p = open_p ? strrchr(source_text, ')') : NULL;
        if (open_p && close_p && close_p > open_p) {
            int prefix_len = (int)(open_p - source_text) + 1;
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
            if (repl_label_split_args(args_str,
                                      fmt, (int)sizeof(fmt),
                                      post, (int)sizeof(post),
                                      split_err, (int)sizeof(split_err))) {
                char post_c[MAX_LINE_LEN] = "";
                if (post[0])
                    repl_eval_expr_to_c(post, post_c, sizeof(post_c));
                fwrite(source_text, 1, (size_t)prefix_len, f);
                fprintf(f, "\"%s\"%s%s);\n",
                        fmt, post_c[0] ? ", " : "", post_c);
                break;
            }
        }
        /* Fallback: emit raw text — the importer handles it via the
         * default repl_eval_c_expr_to_repl path. */
        fprintf(f, "%s\n", source_text);
        break;
    }
    default:
        write_cmd_source_as_c(f, source_text, for_depth > 0 || cmd->has_vars);
        break;
    }
}

static void write_render_body_range_as_c(FILE *f, int start, int end_idx,
                                         int skip_func_defs) {
    int for_depth = 0;
    int tess_depth = 0;

    for (int cmd_idx = start; cmd_idx < end_idx && cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds_mut()[cmd_idx].valid) continue;
        if (skip_func_defs &&
            (repl_state_document_cmds_mut()[cmd_idx].type == CMD_COMMENT ||
             repl_state_document_cmds_mut()[cmd_idx].type == CMD_EMPTY)) {
            int attached_func = comment_run_attached_func_idx(cmd_idx, end_idx);
            if (attached_func >= 0) {
                cmd_idx = find_export_block_end(attached_func);
                continue;
            }
        }
        switch (repl_state_document_cmds_mut()[cmd_idx].type) {
        case CMD_FOR_BEGIN:
            write_for_begin_as_c(f, &repl_state_document_cmds_mut()[cmd_idx],
                                 export_document_text(cmd_idx));
            for_depth++;
            break;
        case CMD_FOR_END:
            for_depth--;
            fprintf(f, "%s\n", export_document_text(cmd_idx));
            break;
        case CMD_FUNC_DEF:
            if (skip_func_defs)
                cmd_idx = find_export_block_end(cmd_idx);
            break;
        case CMD_FUNC_END:
            break;
        default:
            write_canonical_cmd_as_c(f, &repl_state_document_cmds_mut()[cmd_idx],
                                     cmd_idx, for_depth, &tess_depth);
            break;
        }
    }
}

/* Number of geometry passes display() will emit. Always at least 1
 * (Vertex Fill); outline / vertex-point passes are gated by cfg
 * toggles. The save / restore helpers and the wrapping save call in
 * display() are emitted only when this returns >1, so single-pass
 * exports stay zero-overhead. */
static int export_count_enabled_passes(void) {
    int count = 1;
    if (g_export_cfg_bridge && g_export_cfg_bridge->get_int) {
        if (g_export_cfg_bridge->get_int(k_cfg_slug_vertex_outlines, 0)) count++;
        if (g_export_cfg_bridge->get_int(k_cfg_slug_vertex_points, 0)) count++;
    }
    return count;
}

/* Predef vars other than `t` carry their snapshot value forward into
 * the next frame. `t` is set per-frame from glutGet at the top of
 * display(), so its static initializer is irrelevant. */
static int export_predef_var_persists(int var_idx) {
    return strcmp(g_predef_vars[var_idx].name, "t") != 0;
}

static int export_has_persistent_predef_vars(void) {
    for (int i = 0; i < g_num_predef_vars; i++)
        if (export_predef_var_persists(i))
            return 1;
    return 0;
}

static void write_predef_var_globals(FILE *f) {
    if (g_num_predef_vars <= 0) return;
    fprintf(f, "\n/* Predefined REPL variables (file scope for func access).\n"
               " * Initializers are the live snapshot at export time so the\n"
               " * exported binary starts in the same state the REPL ended in;\n"
               " * the live REPL preserves these mutations across frames and\n"
               " * the exported display() does the same (no per-frame reset). */\n");
    for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
        const char *name = g_predef_vars[var_idx].name;
        if (!export_predef_var_persists(var_idx)) {
            /* `t` is overwritten each frame in display() from glutGet. */
            fprintf(f, "static float %s = 0.0f;\n", name);
        } else {
            char vbuf[32];
            export_format_decl_float(vbuf, sizeof(vbuf),
                                     g_predef_vars[var_idx].value);
            fprintf(f, "static float %s = %s;\n", name, vbuf);
        }
    }
}

/* Multipass save/restore: capture the predef-var state once at the top
 * of each frame and restore it before every pass after the first, so
 * each pass sees the same starting state but the LAST pass's mutations
 * persist into the next frame. Mirrors the live REPL's per-frame
 * save/restore around the executor. `t` is not saved — every pass
 * sees the same per-frame `t` value set in display(). */
static void write_save_restore_helpers(FILE *f) {
    if (!export_has_persistent_predef_vars())
        return;

    fprintf(f, "\n/* Per-frame snapshot of predef vars for multipass rendering. */\n");
    for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
        if (!export_predef_var_persists(var_idx)) continue;
        fprintf(f, "static float _saved_%s;\n", g_predef_vars[var_idx].name);
    }

    fprintf(f, "\nstatic void save_repl_vars(void) {\n");
    for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
        if (!export_predef_var_persists(var_idx)) continue;
        const char *name = g_predef_vars[var_idx].name;
        fprintf(f, "  _saved_%s = %s;\n", name, name);
    }
    fprintf(f, "}\n");

    fprintf(f, "\nstatic void restore_repl_vars(void) {\n");
    for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
        if (!export_predef_var_persists(var_idx)) continue;
        const char *name = g_predef_vars[var_idx].name;
        fprintf(f, "  %s = _saved_%s;\n", name, name);
    }
    fprintf(f, "}\n");
}

static void write_rand_helper(FILE *f) {
    fprintf(f,
        "\nstatic float repl_randf(float seed, float iter) {\n"
        "  float h = sinf(seed * 12.9898f + iter * 78.233f) * 43758.5453f;\n"
        "  float frac = h - floorf(h);\n"
        "  if (frac < 0.0f) frac += 1.0f;\n"
        "  return frac;\n"
        "}\n"
        "\nstatic float repl_rand2f(float seed, float iter) {\n"
        "  return repl_randf(seed, iter) * 2.0f - 1.0f;\n"
        "}\n");
}

/* Wrapper for the REPL `label("fmt", ...)` primitive. Walks the format
 * string and substitutes each `%f` with `%g`-formatted output (matching
 * the REPL's CMD_LABEL executor case in src/repl/executor.c) so the live
 * REPL and exported binary render identical text. Using vsnprintf with
 * the raw format would print `1.000000` for `%f` while the REPL prints
 * `1` — that divergence breaks visual round-trips.
 *
 * Float args promote to double through the variadic call, so user
 * expressions don't need explicit casts at the call site.
 *
 * The two extra includes (<stdarg.h>, <stdio.h>) are emitted here
 * rather than in the global header so non-label exports stay
 * byte-identical to their pre-label form. */
static void write_label_helper(FILE *f) {
    /* fprintf format escaping: every literal `%` in the emitted C source
     * must be doubled (`%%`) so fprintf doesn't treat it as a conversion
     * specifier. The `%%g` below emits literal `%g` into the C output. */
    fprintf(f,
        "\n#include <stdarg.h>\n"
        "#include <stdio.h>\n"
        "\nstatic void label(const char *fmt, ...) {\n"
        "  char __b[128];\n"
        "  int __off = 0;\n"
        "  va_list __ap;\n"
        "  va_start(__ap, fmt);\n"
        "  while (*fmt && __off < (int)sizeof(__b) - 1) {\n"
        "    if (fmt[0] == '%%' && fmt[1] == 'f') {\n"
        "      double __v = va_arg(__ap, double);\n"
        "      __off += snprintf(__b + __off, sizeof(__b) - (size_t)__off,\n"
        "                        \"%%g\", __v);\n"
        "      if (__off >= (int)sizeof(__b)) __off = (int)sizeof(__b) - 1;\n"
        "      fmt += 2;\n"
        "    } else if (fmt[0] == '%%' && fmt[1] == '%%') {\n"
        "      __b[__off++] = '%%';\n"
        "      fmt += 2;\n"
        "    } else {\n"
        "      __b[__off++] = *fmt++;\n"
        "    }\n"
        "  }\n"
        "  __b[__off] = '\\0';\n"
        "  va_end(__ap);\n"
        "  for (const char *__p = __b; *__p; __p++)\n"
        "    glutBitmapCharacter(GLUT_BITMAP_9_BY_15, (unsigned char)*__p);\n"
        "}\n");
}

static void write_render_helper_as_c(FILE *f, const char *name) {
    fprintf(f, "\nstatic void %s(void) {\n", name);
    fprintf(f, "  // Snippet start\n");
    write_render_body_range_as_c(f, 0, repl_state_document_count(), 1);
    int bb = 0;
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (repl_state_document_cmds_mut()[cmd_idx].valid && repl_state_document_cmds_mut()[cmd_idx].type == CMD_BEGIN) bb++;
        else if (repl_state_document_cmds_mut()[cmd_idx].valid && repl_state_document_cmds_mut()[cmd_idx].type == CMD_END) bb--;
    }
    if (bb > 0)
        fprintf(f, "  glEnd();\n");
    fprintf(f, "  // Snippet end\n");
    fprintf(f, "}\n");
}

static void write_func_defs_as_c(FILE *f) {
    /* Iterate through all document commands looking for function definitions. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds_mut()[cmd_idx].valid || repl_state_document_cmds_mut()[cmd_idx].type != CMD_FUNC_DEF) continue;
        int comment_start = cmd_idx;
        while (comment_start > 0 &&
               repl_state_document_cmds_mut()[comment_start - 1].valid &&
               (repl_state_document_cmds_mut()[comment_start - 1].type == CMD_COMMENT ||
                repl_state_document_cmds_mut()[comment_start - 1].type == CMD_EMPTY))
            comment_start--;
        /* Emit any preceding comment lines. */
        for (int comment_idx = comment_start; comment_idx < cmd_idx; comment_idx++)
            fprintf(f, "\n%s\n", export_document_text(comment_idx));

        int fn = (int)repl_state_document_cmds_mut()[cmd_idx].args[0];
        int parsed_fn = fn;
        int param_count = 0;
        char param_names[MAX_EXPR_VARS][16];
        int fe = find_export_block_end(cmd_idx);
        /* Emit the C function under the user's alias when one is
         * registered (round-tripped via the `// @func N = name`
         * directive), falling back to the bare `funcN` slot name. The
         * call sites keep their canonical alias text, so defining the
         * body under the same name is what keeps the standalone .c
         * self-consistent and compilable. */
        char fn_name[REPL_FUNC_NAME_MAX + 8];
        if (parse_repl_func_signature(export_document_text(cmd_idx), &parsed_fn,
                                      param_names, MAX_EXPR_VARS,
                                      &param_count) && param_count > 0) {
            const char *alias = repl_func_alias_get(parsed_fn);
            if (alias) snprintf(fn_name, sizeof(fn_name), "%s", alias);
            else       snprintf(fn_name, sizeof(fn_name), "func%d", parsed_fn);
            fprintf(f, "\nstatic void %s(", fn_name);
            /* Emit function parameters. */
            for (int param_idx = 0; param_idx < param_count; param_idx++)
                fprintf(f, "%sfloat %s", param_idx == 0 ? "" : ", ", param_names[param_idx]);
            fprintf(f, ") {\n");
        } else {
            const char *alias = repl_func_alias_get(fn);
            if (alias) snprintf(fn_name, sizeof(fn_name), "%s", alias);
            else       snprintf(fn_name, sizeof(fn_name), "func%d", fn);
            fprintf(f, "\nstatic void %s(void) {\n", fn_name);
        }
        write_render_body_range_as_c(f, cmd_idx + 1, fe, 0);
        fprintf(f, "}\n");
    }
}

static void write_tess_preamble(FILE *f) {
    fprintf(f,
        "#include <string.h>\n"
        "typedef struct { GLdouble pos[3]; GLdouble normal[3]; GLdouble color[4]; } TessVertex;\n"
        "static TessVertex _tv[256];\n"
        "static int _tv_n = 0;\n"
        "static GLdouble _tn[3] = {0.0, 0.0, 1.0};\n"
        "static GLdouble _tc[4] = {1.0, 1.0, 1.0, 1.0};\n"
        "static GLUtesselator *g_tess = NULL;\n"
        "typedef void (*_GluCb)(void);\n"
        "static void _tess_vtx_begin_cb(GLenum mode) { glBegin(mode); }\n"
        "static void _tess_vtx_end_cb(void) { glEnd(); }\n"
        "static void _tess_vtx_cb(void *vd) {\n"
        "    TessVertex *v=(TessVertex*)vd;\n"
        "    glNormal3dv(v->normal); glColor4dv(v->color); glVertex3dv(v->pos);\n"
        "}\n"
        "static void _tess_comb_cb(GLdouble coords[3],void *vd[4],GLfloat w[4],void **out) {\n"
        "    if(_tv_n>=256){*out=NULL;return;}\n"
        "    TessVertex *v=&_tv[_tv_n++];\n"
        "    v->pos[0]=coords[0];v->pos[1]=coords[1];v->pos[2]=coords[2];\n"
        "    for(int c=0;c<3;c++)v->normal[c]=0.0;\n"
        "    for(int c=0;c<4;c++)v->color[c]=0.0;\n"
        "    for(int j=0;j<4;j++){\n"
        "        if(!vd[j])continue;\n"
        "        TessVertex *s=(TessVertex*)vd[j];\n"
        "        for(int c=0;c<3;c++)v->normal[c]+=w[j]*s->normal[c];\n"
        "        for(int c=0;c<4;c++)v->color[c]+=w[j]*s->color[c];\n"
        "    }\n"
        "    double len=sqrt(v->normal[0]*v->normal[0]+v->normal[1]*v->normal[1]+v->normal[2]*v->normal[2]);\n"
        "    if(len>1e-9){v->normal[0]/=len;v->normal[1]/=len;v->normal[2]/=len;}\n"
        "    *out=v;\n"
        "}\n"
        "static void _tess_err_cb(GLenum err) { (void)err; }\n"
    );
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

        ExprCtx ctx = { p, NULL, 0 };
        float val = repl_eval_expr(&ctx);
        p = ctx.p;

        /* Look up and update the predefined variable value. */
        for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
            if (strcmp(g_predef_vars[var_idx].name, name) == 0) {
                g_predef_vars[var_idx].value = val;
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
    char newly_declared[MAX_NAMES_PER_DECL][16];
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
        if (len <= 0 || len >= REPL_PREDEF_NAME_MAX || count >= MAX_NAMES_PER_DECL) break;
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
            if (repl_eval_declare_predef_var(name, NULL, 0) < 0) {
                if (warnings) (*warnings)++;
                continue;
            }
            memcpy(newly_declared[new_count], name, (size_t)len);
            newly_declared[new_count][len] = '\0';
            new_count++;
        }
        if (!repl_copy_string_fits(cmd.var_names[count],
                                   sizeof(cmd.var_names[count]), name)) {
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
            export_format_decl_float(vbuf, sizeof(vbuf), init_val);
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
    snprintf(decl_line + off, sizeof(decl_line) - (size_t)off, ";");
    cmd.var_decl_count = count;

    /* Insert the command directly, bypassing editor_try_commit_float_decl so we
     * don't reject vars that are already registered.  Keep declarations in the
     * same leading zone used by interactive float declarations, even though
     * exported // @declare markers are encountered later in the snippet. */
    {
        ReplCommandStore store = repl_command_store_live();
        int decl_pos = repl_command_store_first_non_decl(&store);

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
    (void)warnings;
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
 * into REPL loop parts. Accepts op ∈ { <, <=, >, >= } and step forms
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

    char names[MAX_EXPR_VARS][16];
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
        int have_exprs = 1;
        /* Iterate through 3D normal vector components (x, y, z). */
        for (int component_idx = 0; component_idx < 3; component_idx++) {
            char key[16];
            snprintf(key, sizeof(key), "_tn[%d]", component_idx);
            if (!import_extract_assignment_expr(p, key, exprs[component_idx], sizeof(exprs[component_idx]))) {
                have_exprs = 0;
                break;
            }
        }
        if (have_exprs) {
            int n = snprintf(out, (size_t)out_sz, "gluNormal(%s, %s, %s);",
                             exprs[0], exprs[1], exprs[2]);
            if (n < 0 || n >= out_sz)
                return 0;
            return 1;
        }

        float nv[3] = {0, 0, 1};
        const char *np = p;
        /* Parse 3 floating-point normal components. */
        for (int component_idx = 0; component_idx < 3; component_idx++) {
            const char *eq = strchr(np, '=');
            if (!eq) break;
            eq++;
            ExprCtx ctx = { eq, NULL, 0 };
            nv[component_idx] = repl_eval_expr(&ctx);
            np = ctx.p;
        }
        snprintf(out, out_sz, "gluNormal(%.9g, %.9g, %.9g);", nv[0], nv[1], nv[2]);
        return 1;
    }

    if (strncmp(p, "{ _tc[", 6) == 0) {
        char exprs[4][MAX_LINE_LEN];
        int have_exprs = 1;
        /* Iterate through 4D color vector components (r, g, b, a). */
        for (int component_idx = 0; component_idx < 4; component_idx++) {
            char key[16];
            snprintf(key, sizeof(key), "_tc[%d]", component_idx);
            if (!import_extract_assignment_expr(p, key, exprs[component_idx], sizeof(exprs[component_idx]))) {
                have_exprs = 0;
                break;
            }
        }
        if (have_exprs) {
            int n;
            if (strcmp(exprs[3], "1") == 0 || strcmp(exprs[3], "1.0") == 0) {
                n = snprintf(out, (size_t)out_sz, "gluColor(%s, %s, %s);",
                             exprs[0], exprs[1], exprs[2]);
            } else {
                n = snprintf(out, (size_t)out_sz, "gluColor(%s, %s, %s, %s);",
                             exprs[0], exprs[1], exprs[2], exprs[3]);
            }
            if (n < 0 || n >= out_sz)
                return 0;
            return 1;
        }

        float cv[4] = {1, 1, 1, 1};
        const char *cp = p;
        /* Parse 4 floating-point color components. */
        for (int component_idx = 0; component_idx < 4; component_idx++) {
            const char *eq = strchr(cp, '=');
            if (!eq) break;
            eq++;
            ExprCtx ctx = { eq, NULL, 0 };
            cv[component_idx] = repl_eval_expr(&ctx);
            cp = ctx.p;
        }
        snprintf(out, out_sz, "gluColor(%.9g, %.9g, %.9g, %.9g);",
                 cv[0], cv[1], cv[2], cv[3]);
        return 1;
    }

    if (strstr(p, "TessVertex") != NULL && strstr(p, "gluTessVertex") != NULL) {
        char exprs[3][MAX_LINE_LEN];
        int have_exprs = 1;
        /* Iterate through 3D vector components (x, y, z). */
        for (int component_idx = 0; component_idx < 3; component_idx++) {
            char key[24];
            snprintf(key, sizeof(key), "_v->pos[%d]", component_idx);
            if (!import_extract_assignment_expr(p, key, exprs[component_idx], sizeof(exprs[component_idx]))) {
                have_exprs = 0;
                break;
            }
        }
        if (have_exprs) {
            int n = snprintf(out, (size_t)out_sz, "gluVertex(%s, %s, %s);",
                             exprs[0], exprs[1], exprs[2]);
            if (n < 0 || n >= out_sz)
                return 0;
            return 1;
        }

        float vv[3] = {0, 0, 0};
        const char *vp = strstr(p, "_v->pos[0]");
        if (!vp) return 0;
        /* Parse 3 floating-point components from the expression. */
        for (int component_idx = 0; component_idx < 3; component_idx++) {
            const char *eq = strchr(vp, '=');
            if (!eq) break;
            eq++;
            ExprCtx ctx = { eq, NULL, 0 };
            vv[component_idx] = repl_eval_expr(&ctx);
            vp = ctx.p;
        }
        snprintf(out, out_sz, "gluVertex(%.9g, %.9g, %.9g);",
                 vv[0], vv[1], vv[2]);
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
        fprintf(stderr, "Warning: could not parse line: %s\n", line);
        (*warnings)++;
    }
}

typedef struct {
    int needs_tess;
    int needs_rand;
    int needs_label;
    int needs_scratch_a;
    int needs_scratch_b;
    int needs_scratch_c;
} ExportNeeds;

typedef struct {
    ExportNeeds              needs;
    const ReplExportLayout  *layout;
} ExportScaffoldContext;

typedef void (*ExportDisplayPassSetupFn)(FILE *f);

typedef struct {
    const char                 *label;
    int                         enabled;
    ExportDisplayPassSetupFn    emit_setup;
} ExportDisplayPassSpec;

/* Word-boundary-aware substring scan: returns 1 iff `needle` appears in
 * `haystack` and the byte immediately before its match position is NOT
 * an identifier character. Avoids false hits like `glRand(` triggering
 * `rand(` detection or `dataA[0]` triggering `A[`. */
static int export_text_uses_token(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t nlen = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        if (p == haystack ||
            (!isalnum((unsigned char)p[-1]) && p[-1] != '_'))
            return 1;
        p += nlen;
    }
    return 0;
}

static ExportNeeds export_collect_needs(void) {
    ExportNeeds needs = {
        .needs_tess = export_uses_tess_commands(),
        .needs_rand = 0,
        .needs_label = 0,
        .needs_scratch_a = 0,
        .needs_scratch_b = 0,
        .needs_scratch_c = 0,
    };

    /* Check each command for rand() / scratch-array / label
     * references. Detection is intentionally a textual scan over
     * source lines (not a CmdType check) so that reads inside arg
     * expressions — e.g. `glVertex3f(A[i], B[i], C[i])` — count
     * alongside writes (`A[0] = ...`). The label wrapper is keyed
     * off the command type rather than text in case a future
     * codegen path emits `label(...)` indirectly. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        const GLCmd *cmd = &repl_state_document_cmds_mut()[cmd_idx];
        if (!cmd->valid) continue;
        if (cmd->type == CMD_LABEL) needs.needs_label = 1;
        const char *src = export_document_text(cmd_idx);
        if (export_text_uses_token(src, "rand(")) needs.needs_rand = 1;
        if (export_text_uses_token(src, "rand2(")) needs.needs_rand = 1;
        if (export_text_uses_token(src, "repl_randf(")) needs.needs_rand = 1;
        if (export_text_uses_token(src, "repl_rand2f(")) needs.needs_rand = 1;
        if (export_text_uses_token(src, "A["))    needs.needs_scratch_a = 1;
        if (export_text_uses_token(src, "B["))    needs.needs_scratch_b = 1;
        if (export_text_uses_token(src, "C["))    needs.needs_scratch_c = 1;
    }

    return needs;
}

static void emit_export_outline_pass_setup(FILE *f) {
    fprintf(f, "  glEnable(GL_COLOR_MATERIAL);\n");
    fprintf(f, "  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);\n");
    fprintf(f, "  glColor3f(0.0f, 0.0f, 0.0f);\n");
    fprintf(f, "  glDisable(GL_COLOR_MATERIAL);\n");
    fprintf(f, "  glEnable(GL_POLYGON_OFFSET_LINE);\n");
    fprintf(f, "  glPolygonOffset(%#.6gf, %#.6gf);\n",
                  (double)REPL_OUTLINE_POLYGON_OFFSET_FACTOR, (double)REPL_OUTLINE_POLYGON_OFFSET_UNITS);
    fprintf(f, "  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);\n");
    fprintf(f, "  glLineWidth(1.2f);\n");
    fprintf(f, "  glEnable(GL_LIGHTING);\n");
}

static void emit_export_point_pass_setup(FILE *f) {
    fprintf(f, "  glEnable(GL_COLOR_MATERIAL);\n");
    fprintf(f, "  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);\n");
    fprintf(f, "  glColor3f(0.0f, 0.0f, 0.0f);\n");
    fprintf(f, "  glDisable(GL_COLOR_MATERIAL);\n");
    fprintf(f, "  glPointSize(8.0f);\n");
    fprintf(f, "  glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);\n");
    fprintf(f, "  glEnable(GL_LIGHTING);\n");
}

static void emit_export_geometry_pass(FILE *f,
                                      const ExportDisplayPassSpec *pass,
                                      int needs_restore) {
    if (!pass || !pass->enabled)
        return;

    fprintf(f, "\n  /* %s */\n", pass->label);
    fprintf(f, "  glPushAttrib(GL_ALL_ATTRIB_BITS);\n");
    if (pass->emit_setup)
        pass->emit_setup(f);
    fprintf(f, "  glPushMatrix();\n");
    if (needs_restore)
        fprintf(f, "  restore_repl_vars();\n");
    fprintf(f, "  render_repl_geometry();\n");
    fprintf(f, "  glPopMatrix();\n");
    fprintf(f, "  glPopAttrib();\n");
}

static void emit_export_display_begin(FILE *f) {
    /* display() opening lines come from g_display_header so the panel
     * (which renders the same array) and the exported file stay
     * byte-identical here. */
    fprintf(f, "\n");
    for (int line_idx = 0; g_display_header[line_idx]; line_idx++)
        fprintf(f, "%s\n", g_display_header[line_idx]);
    /* Emit render state configuration lines (lighting, depth, etc). */
    for (int state_line_idx = 0; state_line_idx < RENDER_STATE_LINE_COUNT; state_line_idx++)
        fprintf(f, "%s\n", g_render_state_lines[state_line_idx]);
    emit_export_cam_lines(f);
    /* Light positions are set after the camera transforms so
     * glLightfv(GL_POSITION) snapshots the post-camera modelview and
     * lights stay anchored in world space as the camera orbits. The
     * non-positional light state (colors + baseline glDisable) is
     * emitted into init() — see emit_export_init_section_to_file.
     *
     * Lights are emitted before g_header_post to match the panel's
     * rendering order; both consumers walk: display_header →
     * render_state → cam → lights → header_post → user code. */
    {
        int n_pos = repl_export_lights_display_line_count();
        for (int pos_idx = 0; pos_idx < n_pos; pos_idx++) {
            char line[MAX_LINE_LEN];
            repl_export_lights_display_line(pos_idx, line, sizeof(line));
            fprintf(f, "%s\n", line);
        }
    }
    /* g_header_post: additional setup after the dynamic state lines. */
    for (int line_idx = 0; g_header_post[line_idx]; line_idx++)
        fprintf(f, "%s\n", g_header_post[line_idx]);
}

static void emit_export_display_geometry(FILE *f) {
    /* Presentation toggles moved to glr_state. Read them via
     * the bridge — same opaque path as the rest of the export
     * pipeline. Demo case (no bridge installed) falls back to "off",
     * which is fine because the demo doesn't export (implemented in
     * step 7a). */
    int outlines_on = g_export_cfg_bridge && g_export_cfg_bridge->get_int
                      ? g_export_cfg_bridge->get_int(k_cfg_slug_vertex_outlines, 0)
                      : 0;
    int vpoints_on  = g_export_cfg_bridge && g_export_cfg_bridge->get_int
                      ? g_export_cfg_bridge->get_int(k_cfg_slug_vertex_points, 0)
                      : 0;
    const ExportDisplayPassSpec passes[] = {
        { "Vertex Fill Pass",    1,               NULL },
        { "Vertex Outline Pass", outlines_on, emit_export_outline_pass_setup },
        { "Vertex Point Pass",   vpoints_on,  emit_export_point_pass_setup },
    };

    /* `t` is advanced by the tick() timer at a fixed step (see the
     * footer's tick() / glutTimerFunc setup), mirroring the live
     * REPL's repl_state_time_advance(0.016). display() only renders;
     * it does NOT touch t. Using glutGet(GLUT_ELAPSED_TIME) here
     * would make tDelta = (t - tLast) * 10 frame-rate dependent and
     * diverge from the REPL preview, where tDelta is constant. */

    /* Multipass rendering: snapshot predef vars before the first pass
     * and restore between passes so each pass starts from the same
     * state, while the LAST pass's mutations carry into the next
     * frame. */
    int multipass = export_count_enabled_passes() > 1 &&
                    export_has_persistent_predef_vars();
    if (multipass)
        fprintf(f, "  save_repl_vars();\n");

    int rendered_passes = 0;
    for (size_t i = 0; i < sizeof(passes) / sizeof(passes[0]); i++) {
        if (!passes[i].enabled) continue;
        int needs_restore = multipass && rendered_passes > 0;
        emit_export_geometry_pass(f, &passes[i], needs_restore);
        rendered_passes++;
    }
}

static void emit_export_display_tail(FILE *f, const ExportNeeds *needs,
                                     const ReplExportLayout *layout) {
    int include_tess = needs ? needs->needs_tess : 0;

    for (int line_idx = 0; g_footer_pre_init[line_idx]; line_idx++) {
        if (strcmp(g_footer_pre_init[line_idx],
                   REPL_EXPORT_RESHAPE_PROJ_SENTINEL) == 0) {
            const char *proj[REPL_EXPORT_PROJ_LINES];
            int pn = repl_export_reshape_projection_lines(proj);
            for (int j = 0; j < pn; j++)
                fprintf(f, "%s\n", proj[j]);
            continue;
        }
        fprintf(f, "%s\n", g_footer_pre_init[line_idx]);
    }
    emit_export_init_section_to_file(f, include_tess);

    /* Use the actual scene rect so the exported window preserves the REPL
     * viewport's aspect ratio and geometry is never clipped. Fall back to
     * 800x600 when dimensions aren't available (headless / demo export).
     * Read from the explicit ReplExportLayout struct rather than
     * calling ui_layout_scene_rect directly (implemented in step 7c). */
    int sw = layout ? layout->scene_w : 0;
    int sh = layout ? layout->scene_h : 0;
    if (sw <= 0) sw = 800;
    if (sh <= 0) sh = 600;
    emit_footer_post_init(f, sw, sh);
}

static void emit_export_display(FILE *f, const ExportNeeds *needs,
                                const ReplExportLayout *layout) {
    emit_export_display_begin(f);
    emit_export_display_geometry(f);
    emit_export_display_tail(f, needs, layout);
}

typedef int  (*ExportScaffoldSectionEnabledFn)(const ExportScaffoldContext *ctx);
typedef void (*ExportScaffoldSectionEmitFn)(FILE *f,
                                            const ExportScaffoldContext *ctx);

typedef struct {
    const char                       *name;
    ExportScaffoldSectionEmitFn       emit;
    ExportScaffoldSectionEnabledFn    enabled;
} ExportScaffoldSectionSpec;

static int export_section_always(const ExportScaffoldContext *ctx) {
    (void)ctx;
    return 1;
}

static int export_section_needs_rand(const ExportScaffoldContext *ctx) {
    return ctx && ctx->needs.needs_rand;
}

static int export_section_needs_scratch(const ExportScaffoldContext *ctx) {
    return ctx && (ctx->needs.needs_scratch_a ||
                   ctx->needs.needs_scratch_b ||
                   ctx->needs.needs_scratch_c);
}

static int export_section_needs_tess(const ExportScaffoldContext *ctx) {
    return ctx && ctx->needs.needs_tess;
}

static void emit_export_workspace_metadata_section(FILE *f,
                                                   const ExportScaffoldContext *ctx) {
    (void)ctx;
    /* Emit workspace directives (@scene-name, @workspace-dir, etc). */
    for (int header_line_idx = 0; header_line_idx < g_workspace_header_line_count; header_line_idx++)
        fprintf(f, "%s\n", g_workspace_header_lines[header_line_idx]);
    if (g_workspace_header_line_count > 0)
        fprintf(f, "\n");
}

static void emit_export_header_section(FILE *f,
                                       const ExportScaffoldContext *ctx) {
    (void)ctx;
    emit_export_header_pre(f);
}

static void emit_export_predef_globals_section(FILE *f,
                                               const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_predef_var_globals(f);
}

static void emit_export_scratch_globals_section(FILE *f,
                                                const ExportScaffoldContext *ctx) {
    if (!ctx) return;
    int has_any = ctx->needs.needs_scratch_a ||
                  ctx->needs.needs_scratch_b ||
                  ctx->needs.needs_scratch_c;
    if (!has_any) return;
    fprintf(f, "\n/* Fixed scratch arrays */\n");
    if (ctx->needs.needs_scratch_a)
        fprintf(f, "static float A[%d] = {0};\n", REPL_SCRATCH_ARRAY_LEN);
    if (ctx->needs.needs_scratch_b)
        fprintf(f, "static float B[%d] = {0};\n", REPL_SCRATCH_ARRAY_LEN);
    if (ctx->needs.needs_scratch_c)
        fprintf(f, "static float C[%d] = {0};\n", REPL_SCRATCH_ARRAY_LEN);
}

static void emit_export_rand_helper_section(FILE *f,
                                            const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_rand_helper(f);
}

static int export_section_needs_label(const ExportScaffoldContext *ctx) {
    return ctx && ctx->needs.needs_label;
}

static void emit_export_label_helper_section(FILE *f,
                                             const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_label_helper(f);
}

static void emit_export_tess_preamble_section(FILE *f,
                                              const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_tess_preamble(f);
}

static int export_section_needs_save_restore(const ExportScaffoldContext *ctx) {
    (void)ctx;
    return export_count_enabled_passes() > 1 &&
           export_has_persistent_predef_vars();
}

static void emit_export_save_restore_section(FILE *f,
                                             const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_save_restore_helpers(f);
}

static void emit_export_functions_section(FILE *f,
                                          const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_func_defs_as_c(f);
}

static void emit_export_render_helper_section(FILE *f,
                                              const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_render_helper_as_c(f, "render_repl_geometry");
}

static void emit_export_display_section(FILE *f,
                                        const ExportScaffoldContext *ctx) {
    emit_export_display(f, &ctx->needs, ctx->layout);
}

/* Section order is the exported C ABI: imports and compile tests assume it. */
static const ExportScaffoldSectionSpec EXPORT_SCAFFOLD_SECTIONS[] = {
    { "workspace metadata", emit_export_workspace_metadata_section, export_section_always },
    { "header",             emit_export_header_section,             export_section_always },
    { "predef globals",     emit_export_predef_globals_section,     export_section_always },
    { "scratch globals",    emit_export_scratch_globals_section,    export_section_needs_scratch },
    { "rand helper",        emit_export_rand_helper_section,        export_section_needs_rand },
    { "label helper",       emit_export_label_helper_section,       export_section_needs_label },
    { "tess preamble",      emit_export_tess_preamble_section,      export_section_needs_tess },
    { "save/restore vars",  emit_export_save_restore_section,       export_section_needs_save_restore },
    { "functions",          emit_export_functions_section,          export_section_always },
    { "render helper",      emit_export_render_helper_section,      export_section_always },
    { "display",            emit_export_display_section,            export_section_always },
};

static void emit_export_scaffold(FILE *f, const ExportScaffoldContext *ctx) {
    for (size_t i = 0; i < sizeof(EXPORT_SCAFFOLD_SECTIONS) /
                           sizeof(EXPORT_SCAFFOLD_SECTIONS[0]); i++) {
        const ExportScaffoldSectionSpec *section = &EXPORT_SCAFFOLD_SECTIONS[i];
        if (!section->enabled || section->enabled(ctx))
            section->emit(f, ctx);
    }
}

int repl_export_save_output(const char *filename, SourceTextView text,
                            const ReplExportLayout *layout) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Error: cannot write %s", filename);
        repl_set_status_error(msg);
        return 0;
    }

    s_export_text_view = text;

    ExportScaffoldContext scaffold = {
        .needs  = export_collect_needs(),
        .layout = layout,
    };

    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();
    repl_state_refresh_workspace_header_lines();

    emit_export_scaffold(f, &scaffold);

    int had_error = ferror(f);
    int close_failed = fclose(f) != 0;
    if (had_error || close_failed) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Error: cannot write %s", filename);
        repl_set_status_error(msg);
        return 0;
    }

    char msg[REPL_DIAG_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Saved to output.c (%d commands)", repl_state_document_count());
    repl_set_status(msg);
    return 1;
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

int repl_export_load_from_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return 0;

    /* Reset the deferred-var list; it is populated by parse_workspace_header_line
     * and applied after the snippet is fully processed (see below). */
    g_deferred_var_count       = 0;
    g_pending_scene_name[0]    = '\0';
    g_pending_workspace_dir[0] = '\0';
    /* @cfg accumulator: parse_cfg() populates it during import; we drain
     * it via the bridge after parsing completes. */
    import_cfg_accumulator_reset();

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
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof(msg), "Error: cannot read %s", filename);
        repl_set_status_error(msg);
        return 0;
    }
    if (truncated_line) {
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
            g_predef_vars[idx].value = g_deferred_var_values[di].value;
    }
    g_deferred_var_count = 0;

    /* Drain @cfg accumulator: hand the parsed (slug, val) bag to the
     * controller-installed bridge, which knows how to apply each slug
     * to its owner's state. Without a bridge (the demo case), the
     * accumulator is dropped silently — that's the architectural goal
     * (no glr_config dependency from src/repl/export.c). */
    import_cfg_accumulator_apply_and_reset();

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

void repl_dump_code_panel_text(FILE *out, SourceTextView text) {
    FILE *dst = out ? out : stdout;

    s_export_text_view = text;

    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();

    fprintf(dst, "--- header_pre ---\n");
    /* Dump pre-header lines (includes, setup). */
    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++)
        fprintf(dst, "%s\n", g_header_pre[line_idx]);

    fprintf(dst, "--- display_header ---\n");
    /* Dump display() opening lines (shared with export). */
    for (int line_idx = 0; g_display_header[line_idx]; line_idx++)
        fprintf(dst, "%s\n", g_display_header[line_idx]);
    /* Scratch decoration follows the display() opening — panel-only. */
    fprintf(dst, "%s\n", REPL_CODE_PANEL_SCRATCH_DECL_LINE);

    fprintf(dst, "--- render_state ---\n");
    /* Dump render state configuration. */
    for (int state_line_idx = 0; state_line_idx < RENDER_STATE_LINE_COUNT; state_line_idx++)
        fprintf(dst, "%s\n", g_render_state_lines[state_line_idx]);

    fprintf(dst, "--- camera ---\n");
    /* Dump camera transformation lines. */
    for (int cam_line_idx = 0; cam_line_idx < CAM_LINE_COUNT; cam_line_idx++)
        fprintf(dst, "%s\n", g_cam_lines[cam_line_idx]);

    fprintf(dst, "--- header_post ---\n");
    /* Dump post-header lines (light setup, etc). */
    for (int line_idx = 0; g_header_post[line_idx]; line_idx++)
        fprintf(dst, "%s\n", g_header_post[line_idx]);

    fprintf(dst, "--- source ---\n");
    /* Dump all valid user commands. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds_mut()[cmd_idx].valid) continue;
        fprintf(dst, "%s\n", export_document_text(cmd_idx));
    }

    fflush(dst);
}

