#include "repl_export.h"
#include "./include/gl_2d.h"
#include "repl_load.h"           /* repl_load_apply_line — step 5b */
/* glr_camera.h removed in step 4a: the export pipeline no longer
 * references glr_camera_*. Camera state flows through the
 * controller-installed ReplExportCameraBridge (see repl_export.h).
 * glr_config.h was already dropped in step 4 for the same reason. */
#include "outline_offset.h"
#include "repl_command_store.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_parser.h"
#include "repl_pipeline.h"
#include "repl_source_scope.h"
#include "repl_state_owners.h"
#include "ui/code_panel_layout.h"
#include "ui/layout.h"
#include "ui/metrics.h"

#define IMPORT_EXPORT_STATE (repl_state_import_export_mut())
#define g_workspace_header_lines (IMPORT_EXPORT_STATE->workspace_header_lines)
#define g_workspace_header_line_count (IMPORT_EXPORT_STATE->workspace_header_line_count)
#define g_render_state_lines (IMPORT_EXPORT_STATE->render_state_lines)
#define g_cam_lines (IMPORT_EXPORT_STATE->cam_lines)
#define g_export_scene_name_hint (IMPORT_EXPORT_STATE->export_scene_name_hint)
#define g_pending_scene_name (IMPORT_EXPORT_STATE->pending_scene_name)
#define g_pending_workspace_dir (IMPORT_EXPORT_STATE->pending_workspace_dir)

/* ----- Neutral header-config bag (step 4 decouple) ------------------------ */

void repl_export_config_clear(ReplExportConfig *cfg) {
    if (!cfg) return;
    cfg->count = 0;
}

int repl_export_config_set(ReplExportConfig *cfg,
                           const char *key, const char *value) {
    if (!cfg || !key || !value) return 0;
    /* Replace if present. */
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->items[i].key, key) == 0) {
            snprintf(cfg->items[i].value, REPL_EXPORT_CFG_VALUE_MAX, "%s", value);
            return 1;
        }
    }
    if (cfg->count >= REPL_EXPORT_CFG_MAX_ITEMS) return 0;
    snprintf(cfg->items[cfg->count].key,   REPL_EXPORT_CFG_KEY_MAX,   "%s", key);
    snprintf(cfg->items[cfg->count].value, REPL_EXPORT_CFG_VALUE_MAX, "%s", value);
    cfg->count++;
    return 1;
}

int repl_export_config_set_int(ReplExportConfig *cfg, const char *key, int value) {
    char buf[REPL_EXPORT_CFG_VALUE_MAX];
    snprintf(buf, sizeof(buf), "%d", value);
    return repl_export_config_set(cfg, key, buf);
}

const char *repl_export_config_get(const ReplExportConfig *cfg, const char *key) {
    if (!cfg || !key) return NULL;
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->items[i].key, key) == 0)
            return cfg->items[i].value;
    }
    return NULL;
}

int repl_export_config_get_int(const ReplExportConfig *cfg,
                               const char *key, int fallback) {
    const char *s = repl_export_config_get(cfg, key);
    if (!s) return fallback;
    return (int)strtol(s, NULL, 10);
}

int repl_export_config_count(const ReplExportConfig *cfg) {
    return cfg ? cfg->count : 0;
}

int repl_export_config_at(const ReplExportConfig *cfg, int idx,
                          const char **key_out, const char **value_out) {
    if (!cfg || idx < 0 || idx >= cfg->count) return 0;
    if (key_out)   *key_out   = cfg->items[idx].key;
    if (value_out) *value_out = cfg->items[idx].value;
    return 1;
}

/* Bridge installation: file-static pointer the controller installs at startup.
 * NULL bridge = no @cfg emission/parsing, which is what the demo wants. */
static const ReplExportConfigBridge *g_export_cfg_bridge = NULL;

void repl_export_install_config_bridge(const ReplExportConfigBridge *bridge) {
    g_export_cfg_bridge = bridge;
}

const ReplExportConfigBridge *repl_export_config_bridge(void) {
    return g_export_cfg_bridge;
}

/* Camera bridge — same shape as the cfg bridge. Step 4a moved camera-block
 * emission and parsing through this interface so repl_export.c no longer
 * references glr_camera_*. The default bridge is installed by
 * glr_app_install_app_services. */
static const ReplExportCameraBridge *g_export_camera_bridge = NULL;

void repl_export_install_camera_bridge(const ReplExportCameraBridge *bridge) {
    g_export_camera_bridge = bridge;
}

const ReplExportCameraBridge *repl_export_camera_bridge(void) {
    return g_export_camera_bridge;
}

/* Pending @cfg accumulator: parse_cfg() during import populates this; the
 * import driver drains it via the bridge after parse completes. */
static ReplExportConfig g_import_cfg_accumulator;

static void import_cfg_accumulator_reset(void) {
    repl_export_config_clear(&g_import_cfg_accumulator);
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

const char *g_header_pre[] = {
    "#define y0 _y0",
    "#define y1 _y1",
    "#include <gl_includes.h>",
    "#include <math.h>",
    "#include <stdlib.h>",
    "#undef y0",
    "#undef y1",
    "",
    "#ifndef M_PI",
    "#define M_PI 3.14159265358979323846",
    "#endif",
    "",
    "static float g_angle = 0.0f;",
    "static int   g_rotating = 0;",
    "",
    "void display() {",
    "  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "  glLoadIdentity();",
    "  glPushAttrib(GL_ALL_ATTRIB_BITS);",
    REPL_CODE_PANEL_SCRATCH_DECL_LINE,
    NULL
};

/* Deferred @var values: set by parse_workspace_header_line alongside the
 * normal auto-declare+set-value path.  load_from_file re-applies them after
 * the snippet is processed so that // @declare markers in the snippet can
 * undeclare and re-declare variables (creating CMD_VAR_DECLARE commands)
 * without losing the saved values from the workspace header. */
#define MAX_DEFERRED_VAR_VALUES 64
typedef struct { char name[16]; float value; } DeferredVar;
static DeferredVar g_deferred_var_values[MAX_DEFERRED_VAR_VALUES];
static int         g_deferred_var_count = 0;

/* Step 4 of the decouple plan moved workspace_slug_from_name out of
 * repl_export.c. The bridge implementation in glr_export.c owns slug
 * derivation now; repl_export.c just emits/parses the (slug, value)
 * pairs the bridge produces. */

static void workspace_format_float(char *buf, size_t n, float v) {
    snprintf(buf, n, "%g", (double)v);
}

/* Per-call editor-text view, set by the public entry points
 * (`repl_export_save_output`, `repl_dump_code_panel_text`,
 * `repl_dump_code_panel_visual_text`) before they invoke any helper
 * that reads source text. Static helpers route through
 * `export_document_text` instead of calling `editor_buffer_line`
 * directly so the source-text dependency is declared at the API
 * boundary as an EditorBufferView parameter rather than a hidden
 * global reach-through. */
static EditorBufferView s_export_text_view;

static const char *export_document_text(int cmd_idx) {
    const char *text;

    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return "";

    text = editor_buffer_view_line(s_export_text_view, cmd_idx);
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

typedef struct {
    const char       *name;      /* directive name without leading `@` */
    size_t            name_len;
    WorkspaceParseFn  parse;     /* parse(args-after-name-and-space) */
    WorkspaceEmitFn   emit;      /* append zero or more lines, bumping *n */
} WorkspaceDirective;

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
    char name[16];
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
        char err[128];
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

/* --- cfg ------------------------------------------------------------------- */

static int parse_cfg(const char *args) {
    const char *p = args;
    char slug[32];
    int slug_char_idx = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
           slug_char_idx < (int)sizeof(slug) - 1)
        slug[slug_char_idx++] = *p++;
    slug[slug_char_idx] = '\0';
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    int val = (int)strtol(p, NULL, 10);
    /* Legacy slug alias: `top_code_panel = 1` translates to
     * `code_panel = TOP`. CODE_PANEL_LAYOUT_TOP / _LEFT enum values
     * are stable across builds (defined in ui/layout.h). */
    const char *out_slug = slug;
    int         out_val  = val;
    if (strcmp(slug, "top_code_panel") == 0) {
        out_slug = "code_panel";
        out_val  = val ? CODE_PANEL_LAYOUT_TOP : CODE_PANEL_LAYOUT_LEFT;
    }
    /* Apply immediately via the bridge: callers (tests, the importer,
     * the example loader) expect line-by-line @cfg parsing to update
     * live state synchronously. We also accumulate so callers that
     * want to drain at the end of a batch (repl_export_apply_pending_cfg)
     * see the same set of (slug, val) pairs — but the live state has
     * already been updated by the per-line apply. */
    if (g_export_cfg_bridge && g_export_cfg_bridge->apply) {
        ReplExportConfig single;
        repl_export_config_clear(&single);
        repl_export_config_set_int(&single, out_slug, out_val);
        g_export_cfg_bridge->apply(&single);
    }
    repl_export_config_set_int(&g_import_cfg_accumulator, out_slug, out_val);
    return 1;
}

static void emit_cfgs(int *n) {
    /* The bridge populates the bag with (slug, value) pairs. If no
     * bridge is installed (the demo case), the bag stays empty and
     * no @cfg lines are emitted. */
    if (!g_export_cfg_bridge || !g_export_cfg_bridge->fill_all)
        return;
    ReplExportConfig cfg;
    repl_export_config_clear(&cfg);
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

void refresh_workspace_header_lines(void) {
    int line_count = 0;
    if (line_count < MAX_WORKSPACE_HEADER_LINES) {
        snprintf(g_workspace_header_lines[line_count++], WORKSPACE_HEADER_LINE_LEN,
                 "// @workspace: REPL state (auto-saved)");
    }
    for (int dir_idx = 0; dir_idx < WORKSPACE_DIRECTIVE_COUNT; dir_idx++)
        WORKSPACE_DIRECTIVES[dir_idx].emit(&line_count);
    g_workspace_header_line_count = line_count;
}

int parse_workspace_header_line(const char *line) {
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
     * repl_export.c does not call glr_config_get directly. */
    const char *toggle_slug;
} InitBootstrapEntry;

static const InitBootstrapEntry g_init_bootstrap_repl[] = {
    { "glEnable(GL_COLOR_MATERIAL);", NULL },
    { "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);", NULL },
    { "glEnable(GL_BLEND);", NULL },
    { "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);", NULL },
#ifndef NO_POINT_PARAMETER
    { "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1.0, 0.0, 0.02);",
      "point_attenuation" },
#endif
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
    "  gluTessCallback(g_tess, GLU_TESS_BEGIN, (void (*)())_tess_vtx_begin_cb);",
    "  gluTessCallback(g_tess, GLU_TESS_END, (void (*)())_tess_vtx_end_cb);",
    "  gluTessCallback(g_tess, GLU_TESS_VERTEX, (void (*)())_tess_vtx_cb);",
    "  gluTessCallback(g_tess, GLU_TESS_COMBINE, (void (*)())_tess_comb_cb);",
    "  gluTessCallback(g_tess, GLU_TESS_ERROR, (void (*)())_tess_err_cb);",
    "  gluTessCallback(g_tess, GLU_TESS_EDGE_FLAG, (void (*)())glEdgeFlag);",
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
    "  gluPerspective(45.0, (float)w/(float)h, 0.1, 100.0);",
    "  glMatrixMode(GL_MODELVIEW);",
    "}",
    "",
    "void keyboard(unsigned char key, int x, int y) {",
    "  (void)x; (void)y;",
    "  if (key == ' ') g_rotating = !g_rotating;",
    "  if (key == 27) exit(0);",
    "}",
    "",
    "void idle() {",
    "  if (g_rotating) g_angle += 0.5f;",
    "  glutPostRedisplay();",
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
    "  glutIdleFunc(idle);",
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
        "  glutIdleFunc(idle);\n"
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

void ensure_init_bootstrap_ready(void) {
    if (!g_init_bootstrap_ready)
        parse_init_bootstrap();
}

void apply_init_bootstrap(void) {
    ensure_init_bootstrap_ready();

    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        const InitBootstrapEntry *entry = &g_init_bootstrap_repl[bootstrap_idx];
        if (entry->toggle_slug && !init_bootstrap_toggle_get(entry->toggle_slug, 1)) {
            if (g_init_bootstrap_cmds[bootstrap_idx].cmd.type == CMD_POINT_PARAMETER_FV &&
                g_init_bootstrap_cmds[bootstrap_idx].cmd.mode == GL_POINT_DISTANCE_ATTENUATION) {
                GLCmd disabled = g_init_bootstrap_cmds[bootstrap_idx].cmd;
                disabled.args[0] = 1.0f;
                disabled.args[1] = 0.0f;
                disabled.args[2] = 0.0f;
                apply_state_cmd(&disabled, 1.0f);
            }
            continue;
        }
        apply_state_cmd(&g_init_bootstrap_cmds[bootstrap_idx].cmd, 1.0f);
    }
}

int init_section_line_count(void) {
    int count = init_host_only_line_count();

    ensure_init_bootstrap_ready();
    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        const InitBootstrapEntry *entry = &g_init_bootstrap_repl[bootstrap_idx];
        if (entry->toggle_slug && !init_bootstrap_toggle_get(entry->toggle_slug, 1))
            continue;
        count++;
    }

    return count;
}

void init_section_line(int i, char *buf, size_t n) {
    int host_count = init_host_only_line_count();
    int enabled_idx = 0;

    if (!buf || n == 0)
        return;

    ensure_init_bootstrap_ready();
    if (i < 0 || i >= init_section_line_count()) {
        buf[0] = '\0';
        return;
    }

    if (i < host_count) {
        snprintf(buf, n, "%s", g_init_host_only_visible_c[i]);
        return;
    }

    i -= host_count;
    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        const InitBootstrapEntry *entry = &g_init_bootstrap_repl[bootstrap_idx];
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

    ensure_init_bootstrap_ready();
    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        const InitBootstrapEntry *entry = &g_init_bootstrap_repl[bootstrap_idx];
        if (entry->toggle_slug && !init_bootstrap_toggle_get(entry->toggle_slug, 1))
            continue;
        format_cmd_source_as_c(line, sizeof(line),
                               g_init_bootstrap_cmds[bootstrap_idx].text,
                               0);
        fprintf(f, "%s\n", line);
    }
}

static void emit_export_header_pre(FILE *f) {
    /* Step 4a: ask the camera bridge for the g_angle preamble line.
     * Without a bridge installed (the demo case) we emit the
     * placeholder unchanged so the file is still valid C. */
    char angle_line[REPL_EXPORT_CAMERA_PREAMBLE_MAX];
    angle_line[0] = '\0';
    if (g_export_camera_bridge && g_export_camera_bridge->fill_save_preamble)
        g_export_camera_bridge->fill_save_preamble(angle_line, (int)sizeof(angle_line));

    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++) {
        if (strcmp(g_header_pre[line_idx], "void display() {") == 0)
            break;
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
    /* Step 4a: the bridge owns the camera-line format. Without a
     * bridge (demo case) the // camera block is omitted from the
     * exported file — that's fine, the demo doesn't export. */
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

void update_render_state_strings(void) {
    snprintf(g_render_state_lines[0], sizeof(g_render_state_lines[0]),
             "  gl%s(GL_MULTISAMPLE);",
             repl_state_render().multisample_enabled ? "Enable" : "Disable");
    snprintf(g_render_state_lines[1], sizeof(g_render_state_lines[1]),
             "  gl%s(GL_LINE_SMOOTH);",
             repl_state_render().line_smooth_enabled ? "Enable" : "Disable");
    snprintf(g_render_state_lines[2], sizeof(g_render_state_lines[2]),
             "  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);");
}

/* Step 4a: the camera-block parser state machine moved to the bridge
 * implementation (glr_camera_export.c). repl_export.c just delegates
 * import-side line consumption and reset to the bridge. */
void import_cam_parser_reset(void) {
    if (g_export_camera_bridge && g_export_camera_bridge->reset_import)
        g_export_camera_bridge->reset_import();
}

int import_parse_cam_line(const char *text) {
    if (!g_export_camera_bridge || !g_export_camera_bridge->try_consume_import_line)
        return 0;
    return g_export_camera_bridge->try_consume_import_line(text);
}

void update_cam_lines(void) {
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

static void write_light_setup(FILE *f) {
    static const char *light_names[] = {
        "GL_LIGHT0", "GL_LIGHT1", "GL_LIGHT2", "GL_LIGHT3"
    };
    ReplRenderState render = repl_state_render();

    int first_light = 1;

    /* Iterate through configured lights and export their setup to C code. */
    for (int light_idx = 0; light_idx < MAX_LIGHTS; light_idx++) {
        const SceneLight *l = &render.lights[light_idx];
        const char *ln = light_names[light_idx];

        if (!l->enabled) continue;

        if (first_light) {
            fprintf(f, "\n  /* Light setup */\n");
            first_light = 0;
        }

        fprintf(f, "  {\n");
        fprintf(f, "    GLfloat pos[]  = { %.2ff, %.2ff, %.2ff, %.2ff };\n",
                l->pos[0], l->pos[1], l->pos[2], l->pos[3]);
        fprintf(f, "    GLfloat dif[]  = { %.2ff, %.2ff, %.2ff, 1.0f };\n",
                l->diffuse[0], l->diffuse[1], l->diffuse[2]);
        fprintf(f, "    GLfloat amb[]  = { %.2ff, %.2ff, %.2ff, 1.0f };\n",
                l->ambient[0], l->ambient[1], l->ambient[2]);
        fprintf(f, "    GLfloat spec[] = { %.2ff, %.2ff, %.2ff, 1.0f };\n",
                l->specular[0], l->specular[1], l->specular[2]);
        fprintf(f, "    glDisable(%s);\n", ln); // lights are disabled by default, each scene enables them as needed
        fprintf(f, "    glLightfv(%s, GL_POSITION, pos);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_DIFFUSE,  dif);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_AMBIENT,  amb);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_SPECULAR, spec);\n", ln);
        fprintf(f, "  }\n");
    }
}

static void write_for_begin_as_c(FILE *f, const GLCmd *cmd,
                                 const char *source_text) {
    char var_name[16];
    const char *p = source_text;
    int indent = 0;
    while (p[indent] && isspace((unsigned char)p[indent])) indent++;

    char ind[32];
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
            int depth = 0;
            while (*hp && !(*hp == ',' && depth == 0) && !(*hp == ')' && depth == 0)) {
                if (*hp == '(') depth++;
                else if (*hp == ')') depth--;
                hp++;
            }
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
            fprintf(f, "%sfor (float %s = %g; %s < %g; %s += 1.0f) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name);
        } else if (step_v == -1.0f) {
            fprintf(f, "%sfor (float %s = %g; %s > %g; %s -= 1.0f) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name);
        } else if (step_v > 0) {
            fprintf(f, "%sfor (float %s = %g; %s < %g; %s += %gf) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name, step_v);
        } else {
            fprintf(f, "%sfor (float %s = %g; %s > %g; %s += %gf) {\n",
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

void trim_in_place(char *s) {
    int start = 0;
    int len = (int)strlen(s);
    while (start < len && isspace((unsigned char)s[start])) start++;
    while (len > start && isspace((unsigned char)s[len - 1])) len--;
    if (start > 0) memmove(s, s + start, (size_t)(len - start));
    s[len - start] = '\0';
}

int repl_extract_paren_payload(const char *src, char *out, int out_sz) {
    const char *p = strchr(src, '(');
    if (!p) return 0;
    p++;
    const char *start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if (depth > 0) p++;
    }
    if (depth != 0) return 0;
    int n = (int)(p - start);
    if (n > out_sz - 1) n = out_sz - 1;
    memcpy(out, start, (size_t)n);
    out[n] = '\0';
    trim_in_place(out);
    return 1;
}

int extract_for_args_text(const char *src,
                          char *var, int var_sz,
                          char *args, int args_sz) {
    const char *p = strchr(src, '(');
    if (!p) return 0;
    p++;

    while (*p && isspace((unsigned char)*p)) p++;

    int vi = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && vi < var_sz - 1)
        var[vi++] = *p++;
    var[vi] = '\0';
    if (vi == 0) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ',') return 0;
    p++;

    const char *start = p;
    int depth = 0;
    while (*p) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            if (depth == 0) break;
            depth--;
        }
        p++;
    }
    if (*p != ')') return 0;

    int n = (int)(p - start);
    if (n > args_sz - 1) n = args_sz - 1;
    memcpy(args, start, (size_t)n);
    args[n] = '\0';
    trim_in_place(args);
    return 1;
}

static int parse_identifier_list(const char *src,
                                 char names[][16], int max_names) {
    const char *p = src;
    int count = 0;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (count >= max_names) return -1;
        if (!isalpha((unsigned char)*p) && *p != '_') return -1;

        int ni = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            if (ni >= 15) return -1;
            names[count][ni++] = *p++;
        }
        names[count][ni] = '\0';
        count++;

        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p != ',') return -1;
        p++;
    }

    return count;
}

int parse_expr_list_exact(const char *src, float *out_vals, int max_vals,
                          ExprVar *vars, int num_vars, int *out_count) {
    const char *p = src;
    int count = 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) {
        if (out_count) *out_count = 0;
        return 1;
    }

    for (;;) {
        ExprCtx ctx = { p, vars, num_vars };
        float value = repl_eval_expr(&ctx);
        if (ctx.p == p) return 0;
        if (count >= max_vals) return 0;
        if (out_vals) out_vals[count] = value;
        count++;

        p = ctx.p;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p != ',') return 0;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) return 0;
    }

    if (out_count) *out_count = count;
    return 1;
}

/* Parse a leading function name token into its slot index. Returns 1
 * and writes *fn on success; returns 0 on no match.
 *
 * Accepts the bare slot form `funcN` (N=0..9) plus any alias name
 * registered through repl_func_alias_set. Advances *p_inout past the
 * matched identifier on success. */
static int parse_func_name_token(const char **p_inout, int *fn) {
    const char *p = *p_inout;
    while (*p && isspace((unsigned char)*p)) p++;
    /* Bare funcN form. */
    if (strncmp(p, "func", 4) == 0 &&
        p[4] >= '0' && p[4] <= '9' &&
        !isalnum((unsigned char)p[5]) && p[5] != '_') {
        if (fn) *fn = p[4] - '0';
        p += 5;
        *p_inout = p;
        return 1;
    }
    /* Alias form: pull a C identifier and look it up. */
    if (!isalpha((unsigned char)*p) && *p != '_') return 0;
    char ident[REPL_FUNC_NAME_MAX];
    int len = 0;
    const char *id_start = p;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
           len < REPL_FUNC_NAME_MAX - 1) {
        ident[len++] = *p++;
    }
    /* If the identifier overflowed the alias-name buffer, the rest
     * couldn't possibly be a registered alias. */
    if (*p && (isalnum((unsigned char)*p) || *p == '_')) return 0;
    ident[len] = '\0';
    if (len == 0) return 0;
    int slot = repl_func_alias_lookup_slot(ident);
    if (slot < 0) return 0;
    if (fn) *fn = slot;
    *p_inout = p;
    (void)id_start;
    return 1;
}

int parse_repl_func_signature(const char *src, int *fn,
                              char param_names[][16], int max_params,
                              int *param_count) {
    const char *p = src;
    if (!parse_func_name_token(&p, fn)) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '{' || *p == '\0') {
        if (param_count) *param_count = 0;
        return 1;
    }
    if (*p != '(') return 0;

    const char *payload_start = ++p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if (depth > 0) p++;
    }
    if (depth != 0) return 0;

    char payload[MAX_LINE_LEN];
    int n = (int)(p - payload_start);
    if (n > (int)sizeof(payload) - 1) n = (int)sizeof(payload) - 1;
    memcpy(payload, payload_start, (size_t)n);
    payload[n] = '\0';
    trim_in_place(payload);

    while (*p == ')') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' && *p != '\0') return 0;

    if (!payload[0]) {
        if (param_count) *param_count = 0;
        return 1;
    }

    int count = parse_identifier_list(payload, param_names, max_params);
    if (count < 0) return 0;
    if (param_count) *param_count = count;
    return 1;
}

int extract_func_call_args_text(const char *src, int *fn,
                                char *args, int args_sz) {
    const char *p = src;
    if (!parse_func_name_token(&p, fn)) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') return 0;
    p++;
    const char *start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if (depth > 0) p++;
    }
    if (depth != 0) return 0;

    int n = (int)(p - start);
    if (n > args_sz - 1) n = args_sz - 1;
    memcpy(args, start, (size_t)n);
    args[n] = '\0';
    trim_in_place(args);

    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0' && *p != ';') return 0;
    return 1;
}

void format_func_header(char *out, int out_sz, const char *indent,
                        int fn, char param_names[][16], int param_count) {
    /* Prefer the user's alias (from the func-alias table) over the bare
     * funcN form so the canonical source text reflects what the user
     * typed. Falls back to funcN when no alias is registered. */
    const char *alias = repl_func_alias_get(fn);
    int written = alias
        ? snprintf(out, out_sz, "%s%s", indent, alias)
        : snprintf(out, out_sz, "%sfunc%d", indent, fn);
    if (written < 0 || written >= out_sz) {
        if (out_sz > 0) out[out_sz - 1] = '\0';
        return;
    }
    /* Always emit `()` — even for zero-arg decls — so the canonical
     * form mirrors C function syntax. The bare `funcN {` shape that
     * older saves used still parses (parse_repl_func_signature
     * accepts it) but reformat normalises to `funcN() {`. */
    if (written < out_sz)
        written += snprintf(out + written, out_sz - written, "(");
    for (int param_idx = 0; param_idx < param_count && written < out_sz; param_idx++) {
        written += snprintf(out + written, out_sz - written, "%s%s",
                            param_idx == 0 ? "" : ", ", param_names[param_idx]);
    }
    if (written < out_sz)
        written += snprintf(out + written, out_sz - written, ")");
    if (written < out_sz)
        snprintf(out + written, out_sz - written, " {");
}

int input_has_expr_vars(const char *s, ExprVar *vars, int num_vars) {
    while (*s) {
        if (!isalpha((unsigned char)*s) && *s != '_') { s++; continue; }
        const char *start = s;
        while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
        int len = (int)(s - start);
        for (int var_idx = 0; var_idx < num_vars; var_idx++) {
            int nlen = (int)strlen(vars[var_idx].name);
            if (nlen == len && strncmp(start, vars[var_idx].name, len) == 0)
                return 1;
        }
    }
    return 0;
}

int input_has_any_visible_vars(const char *s, ExprVar *vars, int num_vars) {
    return repl_eval_input_has_predef_vars(s) || input_has_expr_vars(s, vars, num_vars);
}

int repl_extract_label_name(const char *src, char *name, int name_sz) {
    const char *p = src;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == ':') p++;
    int n = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && n < name_sz - 1)
        name[n++] = *p++;
    name[n] = '\0';
    return n > 0;
}

int repl_extract_goto_label(const char *src, char *name, int name_sz) {
    const char *p = strstr(src, "goto");
    if (!p) p = src;
    else p += 4;
    while (*p && isspace((unsigned char)*p)) p++;
    int n = 0;
    while (*p && *p != ';' && !isspace((unsigned char)*p) &&
           (isalnum((unsigned char)*p) || *p == '_') && n < name_sz - 1)
        name[n++] = *p++;
    name[n] = '\0';
    return n > 0;
}

int repl_extract_assignment_parts(const char *src,
                                  char *name, int name_sz,
                                  char *rhs, int rhs_sz) {
    char index_expr[MAX_LINE_LEN];

    if (!repl_extract_assignment_target_parts(src,
                                              name, name_sz,
                                              index_expr, sizeof(index_expr),
                                              rhs, rhs_sz))
        return 0;
    return index_expr[0] == '\0';
}

int repl_extract_assignment_target_parts(const char *src,
                                         char *name, int name_sz,
                                         char *index_expr, int index_expr_sz,
                                         char *rhs, int rhs_sz) {
    const char *p = src;
    const char *index_start = NULL;
    const char *index_end = NULL;
    const char *rhs_start;
    const char *rhs_end;
    const char *comment_start;
    int n = 0;

    while (*p && isspace((unsigned char)*p)) p++;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (name && n < name_sz - 1)
            name[n] = *p;
        n++;
        p++;
    }
    if (name && name_sz > 0)
        name[n < name_sz - 1 ? n : name_sz - 1] = '\0';
    if (n == 0)
        return 0;

    if (index_expr && index_expr_sz > 0)
        index_expr[0] = '\0';

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '[') {
        int depth = 1;
        index_start = ++p;
        while (*p && depth > 0) {
            if (*p == '[')
                depth++;
            else if (*p == ']')
                depth--;
            if (depth > 0)
                p++;
        }
        if (depth != 0 || !*p)
            return 0;
        index_end = p;
        p++;

        if (index_expr && index_expr_sz > 0) {
            int idx_len = (int)(index_end - index_start);
            if (idx_len > index_expr_sz - 1)
                idx_len = index_expr_sz - 1;
            memcpy(index_expr, index_start, (size_t)idx_len);
            index_expr[idx_len] = '\0';
            trim_in_place(index_expr);
            if (!index_expr[0])
                return 0;
        }
    }

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=' || p[1] == '=')
        return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p)
        return 0;

    rhs_start = p;
    rhs_end = src + strlen(src);
    comment_start = strstr(rhs_start, "//");
    if (comment_start && comment_start < rhs_end)
        rhs_end = comment_start;
    while (rhs_end > rhs_start && isspace((unsigned char)rhs_end[-1])) rhs_end--;
    if (rhs_end > rhs_start && rhs_end[-1] == ';') rhs_end--;
    while (rhs_end > rhs_start && isspace((unsigned char)rhs_end[-1])) rhs_end--;
    if (rhs_end <= rhs_start)
        return 0;

    if (rhs && rhs_sz > 0) {
        int rn = (int)(rhs_end - rhs_start);
        if (rn > rhs_sz - 1) rn = rhs_sz - 1;
        memcpy(rhs, rhs_start, (size_t)rn);
        rhs[rn] = '\0';
        trim_in_place(rhs);
    }
    return 1;
}

static int split_top_level_args(const char *src, char args[][MAX_LINE_LEN], int max_args) {
    const char *p = src;
    int count = 0;

    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (count >= max_args)
            return -1;

        const char *start = p;
        int depth = 0;
        while (*p) {
            if (*p == '(')
                depth++;
            else if (*p == ')') {
                if (depth == 0)
                    break;
                depth--;
            } else if (*p == ',' && depth == 0) {
                break;
            }
            p++;
        }

        int n = (int)(p - start);
        if (n > MAX_LINE_LEN - 1)
            n = MAX_LINE_LEN - 1;
        memcpy(args[count], start, (size_t)n);
        args[count][n] = '\0';
        trim_in_place(args[count]);
        count++;

        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '\0')
            break;
        return -1;
    }

    return count;
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
        if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) depth++;
        else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
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
        int off = fprintf(f, "  // @declare");
        for (int di = 0; di < cmd->var_decl_count; di++) {
            if (has_init[di])
                off += fprintf(f, " %s=%g", cmd->var_names[di], inits[di]);
            else
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
            fprintf(f, "      { _tn[0]=%g; _tn[1]=%g; _tn[2]=%g; }\n",
                    cmd->args[0], cmd->args[1], cmd->args[2]);
        }
        break;
    case CMD_TESS_COLOR:
        if (!write_tess_source_as_c(f, cmd, source_text)) {
            fprintf(f, "      { _tc[0]=%g; _tc[1]=%g; _tc[2]=%g; _tc[3]=%g; }\n",
                    cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
        }
        break;
    case CMD_TESS_VERTEX:
        if (!write_tess_source_as_c(f, cmd, source_text)) {
            fprintf(f,
                    "      { TessVertex *_v=&_tv[_tv_n++];"
                    " _v->pos[0]=%g;_v->pos[1]=%g;_v->pos[2]=%g;"
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
            char split_err[128] = "";
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

static void write_predef_var_globals(FILE *f) {
    if (g_num_predef_vars <= 0) return;
    fprintf(f, "\n/* Predefined REPL variables (file scope for func access) */\n");
    for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
        fprintf(f, "static float %s = 0.0f;\n", g_predef_vars[var_idx].name);
    }
}

static void write_predef_var_reset_func(FILE *f) {
    fprintf(f, "\nstatic void reset_repl_vars(void) {\n");
    for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
        if (strcmp(g_predef_vars[var_idx].name, "t") == 0) {
            fprintf(f, "  %s = 0.001f * (float)glutGet(GLUT_ELAPSED_TIME);\n",
                    g_predef_vars[var_idx].name);
        } else {
            fprintf(f, "  %s = %g;\n",
                    g_predef_vars[var_idx].name, g_predef_vars[var_idx].value);
        }
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
 * the REPL's CMD_LABEL executor case in repl_executor.c) so the live
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
        if (parse_repl_func_signature(export_document_text(cmd_idx), &parsed_fn,
                                      param_names, MAX_EXPR_VARS,
                                      &param_count) && param_count > 0) {
            fprintf(f, "\nstatic void func%d(", parsed_fn);
            /* Emit function parameters. */
            for (int param_idx = 0; param_idx < param_count; param_idx++)
                fprintf(f, "%sfloat %s", param_idx == 0 ? "" : ", ", param_names[param_idx]);
            fprintf(f, ") {\n");
        } else {
            fprintf(f, "\nstatic void func%d(void) {\n", fn);
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
    if (strncmp(p, "float ", 6) != 0) return 0;
    p += 6;

    int updated = 0;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;

        char name[16];
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

/* Parse a // @declare marker written by write_canonical_cmd_as_c() and
 * reconstruct the corresponding CMD_VAR_DECLARE command.  Variables that are
 * already registered in g_predef_vars (e.g. from @var auto-declare or from
 * declare_test_vars in tests) are kept at their current indices so that any
 * CMD_VAR_ASSIGN commands already loaded with those indices remain valid.
 * Vars not yet registered are declared.
 * Returns 1 if the line was a @declare marker (handled), 0 otherwise. */
static int import_parse_declare_marker(const char *line, int *loaded,
                                       int *warnings) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p[0] != '/' || p[1] != '/') return 0;
    p += 2;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "@declare", 8) != 0) return 0;
    p += 8;
    if (*p && !isspace((unsigned char)*p)) return 0;

    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type      = CMD_VAR_DECLARE;
    cmd.valid     = 1;
    int count     = 0;

    /* Build the canonical source string and collect names. Tokens take the
     * form `name` or `name=value`; the optional value carries the inline
     * initializer through the round-trip so the canonical decl text matches
     * the original source byte-for-byte. */
    char decl_line[MAX_LINE_LEN];
    int off = snprintf(decl_line, sizeof(decl_line), "  float");
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (!isalpha((unsigned char)*p) && *p != '_') break;
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int len = (int)(p - start);
        if (len <= 0 || len >= 16 || count >= MAX_NAMES_PER_DECL) break;
        char name[16];
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
        /* Declare the var if not yet registered (noop if already there). */
        int var_idx = repl_eval_find_predef_var_idx(name);
        if (var_idx < 0) {
            var_idx = repl_eval_declare_predef_var(name, NULL, 0);
            if (var_idx < 0) {
                if (warnings) (*warnings)++;
                continue;
            }
        }
        if (!repl_copy_string_fits(cmd.var_names[count],
                                   sizeof(cmd.var_names[count]), name)) {
            if (warnings) (*warnings)++;
            continue;
        }
        off += snprintf(decl_line + off, sizeof(decl_line) - (size_t)off,
                        count == 0 ? " %.*s" : ", %.*s", len, start);
        if (has_init)
            off += snprintf(decl_line + off, sizeof(decl_line) - (size_t)off,
                            " = %g", init_val);
        count++;
    }
    if (count == 0) return 0;
    snprintf(decl_line + off, sizeof(decl_line) - (size_t)off, ";");
    cmd.var_decl_count = count;

    /* Insert the command directly, bypassing try_commit_float_decl so we
     * don't reject vars that are already registered.  Keep declarations in the
     * same leading zone used by interactive float declarations, even though
     * exported // @declare markers are encountered later in the snippet. */
    {
        ReplCommandStore store = repl_command_store_live();
        int decl_pos = repl_command_store_first_non_decl(&store);

        if (!repl_command_store_insert_one(
                &store, decl_pos, &cmd,
                REPL_COMMAND_STORE_ADJUST_EDIT_LINE)) {
            if (warnings) (*warnings)++;
            return 1;
        }
        editor_buffer_insert_line(decl_pos, decl_line);
        (*loaded)++;
    }
    (void)warnings;
    return 1;
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

        if (!isalpha((unsigned char)*p) && *p != '_') {
            p++;
            continue;
        }

        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int len = (int)(p - start);

        char name[32];
        if (len >= (int)sizeof(name))
            return 1;
        memcpy(name, start, (size_t)len);
        name[len] = '\0';

        const char *q = p;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == '(' &&
            (strcmp(name, "sin") == 0 ||
             strcmp(name, "cos") == 0 ||
             strcmp(name, "tan") == 0 ||
             strcmp(name, "sqrt") == 0 ||
             strcmp(name, "abs") == 0 ||
             strcmp(name, "pow") == 0 ||
             strcmp(name, "min") == 0 ||
             strcmp(name, "max") == 0 ||
             strcmp(name, "floor") == 0 ||
             strcmp(name, "ceil") == 0 ||
             strcmp(name, "fmod") == 0 ||
             strcmp(name, "rem") == 0 ||
             strcmp(name, "rand") == 0)) {
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
            n = snprintf(out, (size_t)out_sz, "for(%s, %s, %s, %g) {",
                         var, start_expr, end_expr, step_v);
        } else {
            n = snprintf(out, (size_t)out_sz, "for(%s, %s, %s) {",
                         var, start_expr, end_expr);
        }
        if (n < 0 || n >= out_sz)
            return 0;
        return 1;
    }

    if (step_v != 1.0f)
        snprintf(out, out_sz, "for(%s, %g, %g, %g) {",
                 var, start_v, end_v, step_v);
    else
        snprintf(out, out_sz, "for(%s, %g, %g) {",
                 var, start_v, end_v);
    return 1;
}

static int import_make_repl_func_header(const char *line, char *out, int out_sz) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "static void func", 16) != 0)
        return 0;
    p += 16;

    if (*p < '0' || *p > '9')
        return 0;
    int fn = *p - '0';
    p++;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(')
        return 0;
    p++;
    const char *start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if (depth > 0) p++;
    }
    if (depth != 0)
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
    int count = 0;
    char *cursor = payload;
    while (*cursor) {
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (strncmp(cursor, "float", 5) != 0 || !isspace((unsigned char)cursor[5]))
            return 0;
        cursor += 5;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (count >= MAX_EXPR_VARS) return 0;
        if (!isalpha((unsigned char)*cursor) && *cursor != '_') return 0;

        int ni = 0;
        while (*cursor && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
            if (ni >= 15) return 0;
            names[count][ni++] = *cursor++;
        }
        names[count][ni] = '\0';
        count++;

        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (!*cursor) break;
        if (*cursor != ',') return 0;
        cursor++;
    }

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
        snprintf(out, out_sz, "gluNormal(%g, %g, %g);", nv[0], nv[1], nv[2]);
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
        snprintf(out, out_sz, "gluColor(%g, %g, %g, %g);",
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
        snprintf(out, out_sz, "gluVertex(%g, %g, %g);",
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
    if (strncmp(p, "glPointParameterfv(", 19) != 0)
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
    char split_err[128] = "";
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

static void import_feed_one_line(const char *line, int *loaded, int *warnings) {
    char repl_line[MAX_LINE_LEN];
    int before = repl_state_document_count();
    int handled = 0;

    /* @declare markers are written by write_canonical_cmd_as_c() for
     * CMD_VAR_DECLARE and must be handled before the generic C-to-REPL path. */
    if (import_parse_declare_marker(line, loaded, warnings))
        return;

    /* Step 5b: feed lines through the non-editor source-load API
     * (repl_load_apply_line in repl_compile.c) instead of feed_line.
     * Same compile + apply, no editor input dispatch. */
    char load_err[256] = "";
    if (import_make_repl_for_header(line, repl_line, sizeof(repl_line))) {
        handled = repl_load_apply_line(repl_line, load_err, (int)sizeof(load_err));
    } else if (import_make_repl_tess_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_point_parameter_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_label(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_glut_bitmap_string(line, repl_line, sizeof(repl_line))) {
        handled = repl_load_apply_line(repl_line, load_err, (int)sizeof(load_err));
    } else {
        repl_eval_c_expr_to_repl(line, repl_line, sizeof(repl_line));
        handled = repl_load_apply_line(repl_line, load_err, (int)sizeof(load_err));
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
    ExportNeeds needs;
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
                                      const ExportDisplayPassSpec *pass) {
    if (!pass || !pass->enabled)
        return;

    fprintf(f, "\n  /* %s */\n", pass->label);
    fprintf(f, "  glPushAttrib(GL_ALL_ATTRIB_BITS);\n");
    if (pass->emit_setup)
        pass->emit_setup(f);
    fprintf(f, "  glPushMatrix();\n");
    fprintf(f, "  reset_repl_vars();\n");
    fprintf(f, "  render_repl_geometry();\n");
    fprintf(f, "  glPopMatrix();\n");
    fprintf(f, "  glPopAttrib();\n");
}

static void emit_export_display_begin(FILE *f) {
    fprintf(f, "\nvoid display() {\n");
    fprintf(f, "  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);\n");
    fprintf(f, "  glLoadIdentity();\n");
    fprintf(f, "  glPushAttrib(GL_LIGHTING_BIT);\n");
    /* Emit render state configuration lines (lighting, depth, etc). */
    for (int state_line_idx = 0; state_line_idx < RENDER_STATE_LINE_COUNT; state_line_idx++)
        fprintf(f, "%s\n", g_render_state_lines[state_line_idx]);
    emit_export_cam_lines(f);
    /* Emit post-render-state header lines (typically additional setup). */
    for (int line_idx = 0; g_header_post[line_idx]; line_idx++)
        fprintf(f, "%s\n", g_header_post[line_idx]);
    write_light_setup(f);
    /* Per-frame baseline reset to mirror src/scene/render.c:237-244. The
     * REPL forces GL_LIGHTING off every frame so user-typed glEnable
     * lasts only one frame, and resets specular/shininess to its
     * default values so lit geometry uses the same material parameters
     * the live REPL applies. Without this, exported scenes that enable
     * lighting compute lit colors with GL defaults (specular {0,0,0,1},
     * shininess 0) and render visibly different from the REPL — most
     * obviously, glRasterPos3f-driven label() text computed via
     * lighting comes out darker. */
    fprintf(f, "  glDisable(GL_LIGHTING);\n");
    fprintf(f, "  {\n");
    fprintf(f, "    GLfloat _mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };\n");
    fprintf(f, "    GLfloat _mshin[] = { 30.0f };\n");
    fprintf(f, "    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, _mspec);\n");
    fprintf(f, "    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, _mshin);\n");
    fprintf(f, "  }\n");
}

static void emit_export_display_geometry(FILE *f) {
    const ExportDisplayPassSpec passes[] = {
        { "Vertex Fill Pass",    1,               NULL },
        { "Vertex Outline Pass", repl_state_presentation().show_vertex_outlines, emit_export_outline_pass_setup },
        { "Vertex Point Pass",   repl_state_presentation().show_vertex_points,  emit_export_point_pass_setup },
    };

    for (size_t i = 0; i < sizeof(passes) / sizeof(passes[0]); i++)
        emit_export_geometry_pass(f, &passes[i]);
}

static void emit_export_display_tail(FILE *f, const ExportNeeds *needs) {
    int include_tess = needs ? needs->needs_tess : 0;

    for (int line_idx = 0; g_footer_pre_init[line_idx]; line_idx++)
        fprintf(f, "%s\n", g_footer_pre_init[line_idx]);
    emit_export_init_section_to_file(f, include_tess);

    /* Use the actual scene rect so the exported window preserves the REPL
     * viewport's aspect ratio and geometry is never clipped. Fall back to
     * 800x600 when dimensions aren't available (e.g. headless export). */
    int sx, sy, sw, sh;
    ui_layout_scene_rect(&sx, &sy, &sw, &sh);
    if (sw <= 0) sw = 800;
    if (sh <= 0) sh = 600;
    emit_footer_post_init(f, sw, sh);
}

static void emit_export_display(FILE *f, const ExportNeeds *needs) {
    emit_export_display_begin(f);
    emit_export_display_geometry(f);
    emit_export_display_tail(f, needs);
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

static void emit_export_reset_vars_section(FILE *f,
                                           const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_predef_var_reset_func(f);
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
    emit_export_display(f, &ctx->needs);
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
    { "reset vars",         emit_export_reset_vars_section,         export_section_always },
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

void repl_export_save_output(const char *filename, EditorBufferView text) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        set_status("Error: cannot write output.c");
        return;
    }

    s_export_text_view = text;

    ExportScaffoldContext scaffold = {
        .needs = export_collect_needs(),
    };

    update_render_state_strings();
    update_cam_lines();
    refresh_workspace_header_lines();

    emit_export_scaffold(f, &scaffold);

    fclose(f);

    char msg[128];
    snprintf(msg, sizeof(msg), "Saved to output.c (%d commands)", repl_state_document_count());
    set_status(msg);
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
    /* Step 4a: both the g_angle preamble and the body lines flow
     * through a single bridge entry point. The bridge's stateful
     * parser dispatches internally based on which line shape it
     * sees. */
    return import_parse_cam_line(p);
}

static int import_try_function_body(ImportState *s, const char *p) {
    if (s->func_depth <= 0) return 0;
    import_feed_one_line(p, &s->loaded, &s->warnings);
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
        import_feed_one_line(s->pending_comments[comment_idx], &s->loaded, &s->warnings);
    import_reset_pending_function_prelude(s);
    int before = repl_state_document_count();
    char load_err[256] = "";
    int handled = repl_load_apply_line(repl_func_line, load_err, (int)sizeof(load_err));
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
    /* Function/header import may leave the editor cursor in an insertion slot
     * inside existing commands.  Force snippet lines to start appending from
     * the end of the command list. */
    editor_insert_mode_set(0);
    repl_state_edit_line_set(repl_state_document_count());
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

    import_feed_one_line(p, &s->loaded, &s->warnings);
    return 1;
}

static int import_try_predef_decl(const char *p) {
    return import_parse_predef_decl(p);
}

static int import_try_snippet_body_line(ImportState *s, const char *p) {
    import_feed_one_line(p, &s->loaded, &s->warnings);
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
        if (parse_workspace_header_line(p))                    return;
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

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        import_process_line(&state, p, line);
    }

    fclose(f);

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
     * (no glr_config dependency from repl_export.c). */
    import_cfg_accumulator_apply_and_reset();

    if (state.loaded > 0) {
        repl_source_scope_depth_cache_invalidate();
        repl_reformat_program();
        char msg[256];
        if (state.warnings > 0)
            snprintf(msg, sizeof(msg),
                     "Loaded %d commands from %s (%d warnings)",
                     state.loaded, filename, state.warnings);
        else
            snprintf(msg, sizeof(msg),
                     "Loaded %d commands from %s", state.loaded, filename);
        set_status(msg);
        fprintf(stderr, "%s\n", msg);
    }
    return state.loaded > 0;
}

static void dump_code_panel_wrapped_line(FILE *dst, const char *text,
                                         int first_x, int panel_w) {
    const char *src = text ? text : "";
    CodePanelTextLayout layout =
        repl_code_panel_layout_make(panel_w, first_x, FONT_W, repl_state_presentation().wrap_at_comma);
    CodePanelWrapIter it;
    int start, len, x;

    repl_code_panel_wrap_iter_init(&it, src, &layout);
    while (repl_code_panel_wrap_iter_next(&it, &start, &len, &x)) {
        int prefix_chars = (x - first_x) / FONT_W;
        fprintf(dst, "%*s%.*s\n", prefix_chars, "", len, src + start);
    }
}

void repl_dump_code_panel_text(FILE *out, EditorBufferView text) {
    FILE *dst = out ? out : stdout;

    s_export_text_view = text;

    update_render_state_strings();
    update_cam_lines();

    fprintf(dst, "--- header_pre ---\n");
    /* Dump pre-header lines (includes, setup). */
    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++)
        fprintf(dst, "%s\n", g_header_pre[line_idx]);

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

void repl_dump_code_panel_visual_text(FILE *out, EditorBufferView text) {
    FILE *dst = out ? out : stdout;
    int panel_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = repl_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;

    s_export_text_view = text;

    ui_layout_code_panel_rect(NULL, NULL, &panel_w, NULL);
    update_render_state_strings();
    update_cam_lines();

    fprintf(dst, "--- header_pre ---\n");
    /* Dump pre-header lines with code panel wrapping. */
    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++)
        dump_code_panel_wrapped_line(dst, g_header_pre[line_idx], text_x, panel_w);

    fprintf(dst, "--- render_state ---\n");
    /* Dump render state with code panel wrapping. */
    for (int state_line_idx = 0; state_line_idx < RENDER_STATE_LINE_COUNT; state_line_idx++)
        dump_code_panel_wrapped_line(dst, g_render_state_lines[state_line_idx], text_x, panel_w);

    fprintf(dst, "--- camera ---\n");
    /* Dump camera lines with code panel wrapping. */
    for (int cam_line_idx = 0; cam_line_idx < CAM_LINE_COUNT; cam_line_idx++)
        dump_code_panel_wrapped_line(dst, g_cam_lines[cam_line_idx], text_x, panel_w);

    fprintf(dst, "--- header_post ---\n");
    /* Dump post-header lines with code panel wrapping. */
    for (int line_idx = 0; g_header_post[line_idx]; line_idx++)
        dump_code_panel_wrapped_line(dst, g_header_post[line_idx], text_x, panel_w);

    fprintf(dst, "--- source ---\n");
    /* Dump all valid user commands with code panel wrapping. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds_mut()[cmd_idx].valid) continue;
        dump_code_panel_wrapped_line(dst, export_document_text(cmd_idx), text_x, panel_w);
    }

    fflush(dst);
}
