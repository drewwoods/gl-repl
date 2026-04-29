#include "sample.h"
#include "repl_code_panel_layout.h"
#include "repl_core_internal.h"
#include "repl_command_store.h"
#include "repl_config.h"
#include "repl_pipeline.h"
#include "repl_parser.h"
#include "repl_source_scope.h"
#include "repl_state.h"
#include "repl_layout.h"

#define IMPORT_EXPORT_STATE (repl_state_import_export_mut())
#define g_workspace_header_lines (IMPORT_EXPORT_STATE->workspace_header_lines)
#define g_workspace_header_line_count (IMPORT_EXPORT_STATE->workspace_header_line_count)
#define g_render_state_lines (IMPORT_EXPORT_STATE->render_state_lines)
#define g_cam_lines (IMPORT_EXPORT_STATE->cam_lines)
#define g_export_scene_name_hint (IMPORT_EXPORT_STATE->export_scene_name_hint)
#define g_pending_scene_name (IMPORT_EXPORT_STATE->pending_scene_name)
#define g_pending_workspace_dir (IMPORT_EXPORT_STATE->pending_workspace_dir)

const char *g_header_pre[] = {
    "#include <gl_includes.h>",
    "#include <math.h>",
    "",
    "#ifndef M_PI",
    "#define M_PI 3.14159265358979323846",
    "#endif",
    "",
    "static float g_angle = 0.0f;",
    "static int   g_rotating = 0;",
    "static GLUquadric *g_quadric = NULL;",
    "",
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
typedef struct { char name[16]; float value; } DeferredVar;
static DeferredVar g_deferred_var_values[MAX_DEFERRED_VAR_VALUES];
static int         g_deferred_var_count = 0;

static void workspace_slug_from_name(const char *name, char *out, size_t out_sz) {
    size_t out_idx = 0;
    for (size_t name_idx = 0; name[name_idx] && out_idx + 1 < out_sz; name_idx++) {
        unsigned char c = (unsigned char)name[name_idx];
        if (isspace(c) || c == '-' || c == '/') out[out_idx++] = '_';
        else if (isalnum(c))                    out[out_idx++] = (char)tolower(c);
        else if (c == '_')                      out[out_idx++] = '_';
    }
    out[out_idx] = '\0';
}

static void workspace_format_float(char *buf, size_t n, float v) {
    snprintf(buf, n, "%g", (double)v);
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
    if (strcmp(slug, "top_code_panel") == 0) {
        repl_config_set(REPL_CONFIG_CODE_PANEL_LAYOUT,
                        val ? CODE_PANEL_LAYOUT_TOP : CODE_PANEL_LAYOUT_LEFT);
        return 1;
    }
    int cfg_count = 0;
    const ReplConfigItem *items = repl_config_items(&cfg_count);
    for (int item_idx = 0; item_idx < cfg_count; item_idx++) {
        const ReplConfigItem *item = &items[item_idx];
        if (item->section_header || item->key == REPL_CONFIG_NONE)
            continue;
        char item_slug[32];
        workspace_slug_from_name(item->label, item_slug, sizeof(item_slug));
        if (strcmp(item_slug, slug) == 0) {
            repl_config_set(item->key, val);
            return 1;
        }
    }
    /* Unknown slug: still consume so unrelated directives don't claim it. */
    return 1;
}

static void emit_cfgs(int *n) {
    int cfg_count = 0;
    const ReplConfigItem *items = repl_config_items(&cfg_count);
    for (int item_idx = 0; item_idx < cfg_count && *n < MAX_WORKSPACE_HEADER_LINES; item_idx++) {
        const ReplConfigItem *item = &items[item_idx];
        if (item->section_header || item->key == REPL_CONFIG_NONE)
            continue;
        char slug[32];
        workspace_slug_from_name(item->label, slug, sizeof(slug));
        snprintf(g_workspace_header_lines[(*n)++], WORKSPACE_HEADER_LINE_LEN,
                 "// @cfg %s = %d", slug, repl_config_get(item->key));
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
    ReplConfigKey toggle_key;
} InitBootstrapEntry;

static const InitBootstrapEntry g_init_bootstrap_repl[] = {
    { "glEnable(GL_COLOR_MATERIAL);", REPL_CONFIG_NONE },
    { "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);", REPL_CONFIG_NONE },
    { "glEnable(GL_BLEND);", REPL_CONFIG_NONE },
    { "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);", REPL_CONFIG_NONE },
#ifndef NO_POINT_PARAMETER
    { "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1.0, 0.0, 0.02);",
      REPL_CONFIG_POINT_ATTENUATION },
#endif
};
#define NUM_INIT_BOOTSTRAP \
    ((int)(sizeof(g_init_bootstrap_repl) / sizeof(g_init_bootstrap_repl[0])))

static GLCmd g_init_bootstrap_cmds[NUM_INIT_BOOTSTRAP];
static int   g_init_bootstrap_ready = 0;

static const char *g_init_host_only_visible_c[] = {
    "  GLfloat lm_amb[] = { 0.15f, 0.15f, 0.20f, 1.0f };",
    "  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);",
    "  g_quadric = gluNewQuadric();",
    "  gluQuadricNormals(g_quadric, GLU_SMOOTH);",
    "  gluQuadricTexture(g_quadric, GL_FALSE);",
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
                                   const GLCmd *cmd, int translate_exprs);

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
        GLCmd cmd;
        ReplParseContext parse_ctx = { 0, NULL, 0, 0 };
        memset(&cmd, 0, sizeof(cmd));
        if (!repl_parser_parse_command_ctx(g_init_bootstrap_repl[bootstrap_idx].repl_line,
                                    &cmd, &parse_ctx)) {
            fprintf(stderr, "init bootstrap parse failed: %s\n",
                    g_init_bootstrap_repl[bootstrap_idx].repl_line);
            abort();
        }
        g_init_bootstrap_cmds[bootstrap_idx] = cmd;
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
        if (g_init_bootstrap_repl[bootstrap_idx].toggle_key != REPL_CONFIG_NONE &&
            !repl_config_get(g_init_bootstrap_repl[bootstrap_idx].toggle_key)) {
            if (g_init_bootstrap_cmds[bootstrap_idx].type == CMD_POINT_PARAMETER_FV &&
                g_init_bootstrap_cmds[bootstrap_idx].mode == GL_POINT_DISTANCE_ATTENUATION) {
                GLCmd disabled = g_init_bootstrap_cmds[bootstrap_idx];
                disabled.args[0] = 1.0f;
                disabled.args[1] = 0.0f;
                disabled.args[2] = 0.0f;
                apply_state_cmd(&disabled, 1.0f);
            }
            continue;
        }
        apply_state_cmd(&g_init_bootstrap_cmds[bootstrap_idx], 1.0f);
    }
}

int init_section_line_count(void) {
    int count = init_host_only_line_count();

    ensure_init_bootstrap_ready();
    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        if (g_init_bootstrap_repl[bootstrap_idx].toggle_key != REPL_CONFIG_NONE &&
            !repl_config_get(g_init_bootstrap_repl[bootstrap_idx].toggle_key))
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
        if (g_init_bootstrap_repl[bootstrap_idx].toggle_key != REPL_CONFIG_NONE &&
            !repl_config_get(g_init_bootstrap_repl[bootstrap_idx].toggle_key))
            continue;
        if (enabled_idx == i) {
            format_cmd_source_as_c(buf, n, &g_init_bootstrap_cmds[bootstrap_idx], 0);
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
        if (g_init_bootstrap_repl[bootstrap_idx].toggle_key != REPL_CONFIG_NONE &&
            !repl_config_get(g_init_bootstrap_repl[bootstrap_idx].toggle_key))
            continue;
        format_cmd_source_as_c(line, sizeof(line), &g_init_bootstrap_cmds[bootstrap_idx], 0);
        fprintf(f, "%s\n", line);
    }
}

static void emit_export_header_pre(FILE *f) {
    char angle_line[64];

    snprintf(angle_line, sizeof(angle_line),
             "static float g_angle = %.4ff;", repl_state_camera().ry);

    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++) {
        if (strcmp(g_header_pre[line_idx], "void display() {") == 0)
            break;
        if (strcmp(g_header_pre[line_idx], "static float g_angle = 0.0f;") == 0) {
            fprintf(f, "%s\n", angle_line);
            continue;
        }
        fprintf(f, "%s\n", g_header_pre[line_idx]);
    }
}

static void emit_export_cam_lines(FILE *f) {
    ReplCameraState cam = repl_state_camera();
    fprintf(f, "  glTranslatef(0.0000f, 0.0000f, %.4ff);\n", -cam.dist);
    fprintf(f, "  glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);\n", cam.rx);
    fprintf(f, "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);\n");
    fprintf(f, "  glTranslatef(%.4ff, %.4ff, %.4ff);\n",
            -cam.tx, -cam.ty, -cam.tz);
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

/* Skip whitespace, commas, and trailing 'f' / 'F' float suffixes so strtof
 * can march through a call like `glTranslatef(0.0f, 0.0f, -5.0f);`. */
static const char *cam_line_skip_sep(const char *p) {
    while (*p == 'f' || *p == 'F' || *p == ',' || *p == ' ' || *p == '\t')
        p++;
    return p;
}

static int cam_line_read_floats(const char *p, float *out, int n) {
    for (int float_idx = 0; float_idx < n; float_idx++) {
        p = cam_line_skip_sep(p);
        char *end = NULL;
        out[float_idx] = strtof(p, &end);
        if (end == p) return 0;
        p = end;
    }
    return 1;
}

/* Camera-block parser state: exported camera lines appear contiguously inside
 * display(). Older exports use translate(dist), rotate(rx), rotate(ry),
 * rotate(g_angle), translate(target); newer exports fold ry into the initial
 * g_angle value and omit the literal rotate(ry) line. We walk a tiny state
 * machine so the parser only consumes lines during the expected sequence,
 * and never bites into user code elsewhere in the file. Reset at load. */
static int g_cam_parse_state = 0;    /* 0..5, stops consuming at 5 */

void import_cam_parser_reset(void) {
    g_cam_parse_state = 0;
}

static int import_parse_export_angle_init(const char *text) {
    const char *p = text;
    const char *eq;
    char *end = NULL;
    float v;

    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "static float g_angle", 20) != 0)
        return 0;

    eq = strchr(p, '=');
    if (!eq)
        return 0;
    eq++;

    v = strtof(eq, &end);
    if (end == eq)
        return 0;

    repl_state_camera_set_orbit(repl_state_camera().rx, v);
    return 1;
}

/* Per-line sniffer for the new 4-line camera block emitted into output.c.
 * Each call updates at most one camera scalar; the caller invokes this for
 * every line in the loaded file. Returns 1 if a camera line was consumed. */
int import_parse_cam_line(const char *text) {
    if (g_cam_parse_state >= 5) return 0;

    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;

    if (g_cam_parse_state == 0 && strncmp(p, "glTranslatef", 12) == 0) {
        p = strchr(p, '(');
        if (!p) return 0;
        p++;
        float v[3];
        if (!cam_line_read_floats(p, v, 3)) return 0;
        repl_state_camera_set_distance(-v[2]);
        g_cam_parse_state = 1;
        return 1;
    }

    if (g_cam_parse_state == 1 && strncmp(p, "glRotatef", 9) == 0) {
        p = strchr(p, '(');
        if (!p) return 0;
        p++;
        float v[4];
        if (!cam_line_read_floats(p, v, 4)) return 0;
        if (v[1] != 1.0f || v[2] != 0.0f || v[3] != 0.0f) return 0;
        repl_state_camera_set_orbit(v[0], repl_state_camera().ry);
        g_cam_parse_state = 2;
        return 1;
    }

    if (g_cam_parse_state == 2 && strncmp(p, "glRotatef", 9) == 0) {
        const char *q = strchr(p, '(');
        if (q && strstr(q, "g_angle")) {
            g_cam_parse_state = 4;
            return 1;
        }

        p = strchr(p, '(');
        if (!p) return 0;
        p++;
        float v[4];
        if (!cam_line_read_floats(p, v, 4)) return 0;
        if (v[1] != 0.0f || v[2] != 1.0f || v[3] != 0.0f) return 0;
        repl_state_camera_set_orbit(repl_state_camera().rx, v[0]);
        g_cam_parse_state = 3;
        return 1;
    }

    if (g_cam_parse_state == 3 && strncmp(p, "glRotatef", 9) == 0) {
        /* Literal `glRotatef(g_angle, 0,1,0)` animation hook - no scalars
         * to extract, just advance past it. We also tolerate its absence
         * (fall through to state 4) for files saved before it existed. */
        const char *q = strchr(p, '(');
        if (q && strstr(q, "g_angle")) {
            g_cam_parse_state = 4;
            return 1;
        }
        g_cam_parse_state = 4;
        /* fall through to try target translate on the same line */
    }

    if (g_cam_parse_state == 4 && strncmp(p, "glTranslatef", 12) == 0) {
        p = strchr(p, '(');
        if (!p) return 0;
        p++;
        float v[3];
        if (!cam_line_read_floats(p, v, 3)) return 0;
        repl_state_camera_set_pan(-v[0], -v[1], -v[2]);
        g_cam_parse_state = 5;
        return 1;
    }

    return 0;
}

void update_cam_lines(void) {
    ReplCameraState cam = repl_state_camera();
    snprintf(g_cam_lines[0], sizeof(g_cam_lines[0]),
             "  glTranslatef(0.0000f, 0.0000f, %.4ff);", -cam.dist);
    snprintf(g_cam_lines[1], sizeof(g_cam_lines[1]),
             "  glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);", cam.rx);
    snprintf(g_cam_lines[2], sizeof(g_cam_lines[2]),
             "  glRotatef(%.4ff, 0.0f, 1.0f, 0.0f);", cam.ry);
    snprintf(g_cam_lines[3], sizeof(g_cam_lines[3]),
             "  glTranslatef(%.4ff, %.4ff, %.4ff);",
            -cam.tx, -cam.ty, -cam.tz);
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
        fprintf(f, "    glEnable(%s);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_POSITION, pos);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_DIFFUSE,  dif);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_AMBIENT,  amb);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_SPECULAR, spec);\n", ln);
        fprintf(f, "  }\n");
    }
}

static void write_for_begin_as_c(FILE *f, const GLCmd *cmd) {
    char var_name[16];
    const char *p = cmd->source;
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
        fprintf(f, "%s\n", cmd->source);
    }
}

static int cmd_type_is_quadric(CmdType t) {
    return t == CMD_GLU_SPHERE ||
           t == CMD_GLU_CYLINDER ||
           t == CMD_GLU_DISK ||
           t == CMD_GLU_PARTIAL_DISK;
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

static void quadric_source_to_c(const char *src, char *out, int out_sz) {
    const char *p = src;
    const char *open;
    int indent = 0;
    int prefix_len;
    int written;

    if (out_sz <= 0) return;
    out[0] = '\0';

    while (p[indent] == ' ' || p[indent] == '\t')
        indent++;
    p += indent;

    open = strchr(p, '(');
    if (!open) {
        size_t copy_len = strlen(src);
        if (copy_len >= (size_t)out_sz)
            copy_len = (size_t)out_sz - 1;
        memcpy(out, src, copy_len);
        out[copy_len] = '\0';
        return;
    }

    prefix_len = indent + (int)(open - p);
    if (prefix_len > out_sz - 1)
        prefix_len = out_sz - 1;
    memcpy(out, src, (size_t)prefix_len);
    out[prefix_len] = '\0';

    written = snprintf(out + prefix_len, (size_t)(out_sz - prefix_len),
                       "(g_quadric%s%s",
                       open[1] == ')' ? "" : ", ",
                       open + 1);
    if (written < 0 || prefix_len + written >= out_sz)
        out[out_sz - 1] = '\0';
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

int parse_repl_func_signature(const char *src, int *fn,
                              char param_names[][16], int max_params,
                              int *param_count) {
    const char *p = src;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "func", 4) != 0) return 0;
    p += 4;
    if (*p < '0' || *p > '9') return 0;
    if (fn) *fn = *p - '0';
    p++;

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
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "func", 4) != 0) return 0;
    p += 4;
    if (*p < '0' || *p > '9') return 0;
    if (fn) *fn = *p - '0';
    p++;

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
    int written = snprintf(out, out_sz, "%sfunc%d", indent, fn);
    if (written < 0 || written >= out_sz) {
        if (out_sz > 0) out[out_sz - 1] = '\0';
        return;
    }
    if (param_count > 0) {
        written += snprintf(out + written, out_sz - written, "(");
        for (int param_idx = 0; param_idx < param_count && written < out_sz; param_idx++) {
            written += snprintf(out + written, out_sz - written, "%s%s",
                                param_idx == 0 ? "" : ", ", param_names[param_idx]);
        }
        if (written < out_sz)
            written += snprintf(out + written, out_sz - written, ")");
    }
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
    const char *p = src;
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

static int write_tess_source_as_c(FILE *f, const GLCmd *cmd) {
    char payload[MAX_LINE_LEN];
    char raw_args[4][MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int arg_count;

    if (!repl_extract_paren_payload(cmd->source, payload, sizeof(payload)))
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
                                   const GLCmd *cmd, int translate_exprs) {
    char c_src[MAX_LINE_LEN];
    char quadric_src[MAX_LINE_LEN];

    if (!out || out_sz == 0)
        return;

    if (translate_exprs)
        repl_eval_expr_to_c(cmd->source, c_src, sizeof(c_src));
    else {
        strncpy(c_src, cmd->source, sizeof(c_src) - 1);
        c_src[sizeof(c_src) - 1] = '\0';
    }

    if (cmd_type_is_quadric(cmd->type)) {
        quadric_source_to_c(c_src, quadric_src, sizeof(quadric_src));
        snprintf(out, out_sz, "%s", quadric_src);
    } else {
        snprintf(out, out_sz, "%s", c_src);
    }
}

static void write_cmd_source_as_c(FILE *f, const GLCmd *cmd, int translate_exprs) {
    char out[MAX_LINE_LEN];

    format_cmd_source_as_c(out, sizeof(out), cmd, translate_exprs);
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
           repl_state_document_cmds_mut()[cmd_idx].valid && repl_state_document_cmds_mut()[cmd_idx].type == CMD_COMMENT)
        cmd_idx++;
    if (cmd_idx > start && cmd_idx < end_idx && cmd_idx < repl_state_document_count() &&
        repl_state_document_cmds_mut()[cmd_idx].valid && repl_state_document_cmds_mut()[cmd_idx].type == CMD_FUNC_DEF)
        return cmd_idx;
    return -1;
}

static void write_canonical_cmd_as_c(FILE *f, const GLCmd *cmd, int for_depth,
                                     int *tess_depth) {
    switch (cmd->type) {
    case CMD_COMMENT:
        fprintf(f, "%s\n", cmd->source);
        break;
    case CMD_VAR_DECLARE: {
        /* Variables are emitted as file-scope statics by write_predef_var_globals().
         * We cannot write a local float declaration here because it would shadow
         * the file-scope global.  Instead, emit a special REPL marker comment so
         * the importer can recreate the CMD_VAR_DECLARE when loading back into the
         * REPL without creating a C local variable. */
        int off = fprintf(f, "  // @declare");
        for (int di = 0; di < cmd->var_decl_count; di++)
            off += fprintf(f, " %s", cmd->var_names[di]);
        (void)off;
        fprintf(f, "\n");
        break;
    }
    case CMD_VAR_ASSIGN: {
        char c_src[MAX_LINE_LEN];
        repl_eval_expr_to_c(cmd->source, c_src, sizeof(c_src));
        fprintf(f, "%s\n", c_src);
        break;
    }
    case CMD_CALL:
        write_cmd_source_as_c(f, cmd, 1);
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
        if (!write_tess_source_as_c(f, cmd)) {
            fprintf(f, "      { _tn[0]=%g; _tn[1]=%g; _tn[2]=%g; }\n",
                    cmd->args[0], cmd->args[1], cmd->args[2]);
        }
        break;
    case CMD_TESS_COLOR:
        if (!write_tess_source_as_c(f, cmd)) {
            fprintf(f, "      { _tc[0]=%g; _tc[1]=%g; _tc[2]=%g; _tc[3]=%g; }\n",
                    cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
        }
        break;
    case CMD_TESS_VERTEX:
        if (!write_tess_source_as_c(f, cmd)) {
            fprintf(f,
                    "      { TessVertex *_v=&_tv[_tv_n++];"
                    " _v->pos[0]=%g;_v->pos[1]=%g;_v->pos[2]=%g;"
                    " memcpy(_v->normal,_tn,24); memcpy(_v->color,_tc,32);"
                    " gluTessVertex(g_tess,_v->pos,_v); }\n",
                    cmd->args[0], cmd->args[1], cmd->args[2]);
        }
        break;
    default:
        write_cmd_source_as_c(f, cmd, for_depth > 0 || cmd->has_vars);
        break;
    }
}

static void write_render_body_range_as_c(FILE *f, int start, int end_idx,
                                         int skip_func_defs) {
    int for_depth = 0;
    int tess_depth = 0;

    for (int cmd_idx = start; cmd_idx < end_idx && cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds_mut()[cmd_idx].valid) continue;
        if (skip_func_defs && repl_state_document_cmds_mut()[cmd_idx].type == CMD_COMMENT) {
            int attached_func = comment_run_attached_func_idx(cmd_idx, end_idx);
            if (attached_func >= 0) {
                cmd_idx = find_export_block_end(attached_func);
                continue;
            }
        }
        switch (repl_state_document_cmds_mut()[cmd_idx].type) {
        case CMD_FOR_BEGIN:
            write_for_begin_as_c(f, &repl_state_document_cmds_mut()[cmd_idx]);
            for_depth++;
            break;
        case CMD_FOR_END:
            for_depth--;
            fprintf(f, "%s\n", repl_state_document_cmds_mut()[cmd_idx].source);
            break;
        case CMD_FUNC_DEF:
            if (skip_func_defs)
                cmd_idx = find_export_block_end(cmd_idx);
            break;
        case CMD_FUNC_END:
            break;
        default:
            write_canonical_cmd_as_c(f, &repl_state_document_cmds_mut()[cmd_idx], for_depth, &tess_depth);
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
               repl_state_document_cmds_mut()[comment_start - 1].type == CMD_COMMENT)
            comment_start--;
        /* Emit any preceding comment lines. */
        for (int comment_idx = comment_start; comment_idx < cmd_idx; comment_idx++)
            fprintf(f, "\n%s\n", repl_state_document_cmds_mut()[comment_idx].source);

        int fn = (int)repl_state_document_cmds_mut()[cmd_idx].args[0];
        int parsed_fn = fn;
        int param_count = 0;
        char param_names[MAX_EXPR_VARS][16];
        int fe = find_export_block_end(cmd_idx);
        if (parse_repl_func_signature(repl_state_document_cmds_mut()[cmd_idx].source, &parsed_fn,
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

    /* Build the source string and collect names. */
    int off = snprintf(cmd.source, sizeof(cmd.source), "  float");
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
        off += snprintf(cmd.source + off, sizeof(cmd.source) - (size_t)off,
                        count == 0 ? " %.*s" : ", %.*s", len, start);
        count++;
    }
    if (count == 0) return 0;
    snprintf(cmd.source + off, sizeof(cmd.source) - (size_t)off, ";");
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
        char raw_step[128];
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
            char adjusted[128];
            snprintf(adjusted, sizeof(adjusted), "(%s) %c 1",
                     end_expr, is_greater ? '-' : '+');
            strncpy(end_expr, adjusted, sizeof(end_expr) - 1);
            end_expr[sizeof(end_expr) - 1] = '\0';
        }

        if (symbolic_step) {
            snprintf(out, out_sz, "for(%s, %s, %s, %s) {",
                     var, start_expr, end_expr, step_expr);
        } else if (step_v != 1.0f) {
            snprintf(out, out_sz, "for(%s, %s, %s, %g) {",
                     var, start_expr, end_expr, step_v);
        } else {
            snprintf(out, out_sz, "for(%s, %s, %s) {",
                     var, start_expr, end_expr);
        }
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
            snprintf(out, out_sz, "gluNormal(%s, %s, %s);",
                     exprs[0], exprs[1], exprs[2]);
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
            if (strcmp(exprs[3], "1") == 0 || strcmp(exprs[3], "1.0") == 0) {
                snprintf(out, out_sz, "gluColor(%s, %s, %s);",
                         exprs[0], exprs[1], exprs[2]);
            } else {
                snprintf(out, out_sz, "gluColor(%s, %s, %s, %s);",
                         exprs[0], exprs[1], exprs[2], exprs[3]);
            }
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
            snprintf(out, out_sz, "gluVertex(%s, %s, %s);",
                     exprs[0], exprs[1], exprs[2]);
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

static int import_make_repl_quadric_line(const char *line, char *out, int out_sz) {
    static const char *const names[] = {
        "gluSphere",
        "gluCylinder",
        "gluDisk",
        "gluPartialDisk",
        NULL
    };
    const char *p = line;
    int indent = 0;

    while (p[indent] == ' ' || p[indent] == '\t')
        indent++;
    p += indent;

    /* Iterate through supported quadric function names. */
    for (int name_idx = 0; names[name_idx]; name_idx++) {
        const char *name = names[name_idx];
        int name_len = (int)strlen(name);
        const char *args;
        char tmp[MAX_LINE_LEN];
        int prefix_len;

        if (strncmp(p, name, (size_t)name_len) != 0 || p[name_len] != '(')
            continue;

        args = p + name_len + 1;
        while (*args == ' ' || *args == '\t')
            args++;
        if (strncmp(args, "g_quadric", 9) != 0)
            return 0;
        args += 9;
        while (*args == ' ' || *args == '\t')
            args++;
        if (*args == ',')
            args++;
        while (*args == ' ' || *args == '\t')
            args++;

        prefix_len = indent + name_len + 1;
        if (prefix_len >= (int)sizeof(tmp))
            prefix_len = (int)sizeof(tmp) - 1;
        memcpy(tmp, line, (size_t)prefix_len);
        tmp[prefix_len] = '\0';
        strncat(tmp, args, sizeof(tmp) - 1 - strlen(tmp));

        repl_eval_c_expr_to_repl(tmp, out, out_sz);
        return 1;
    }

    return 0;
}

static void import_feed_one_line(const char *line, int *loaded, int *warnings) {
    char repl_line[MAX_LINE_LEN];
    int before = repl_state_document_count();
    int handled = 0;

    /* @declare markers are written by write_canonical_cmd_as_c() for
     * CMD_VAR_DECLARE and must be handled before the generic C-to-REPL path. */
    if (import_parse_declare_marker(line, loaded, warnings))
        return;

    if (import_make_repl_for_header(line, repl_line, sizeof(repl_line))) {
        handled = feed_line(repl_line);
    } else if (import_make_repl_tess_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_point_parameter_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_quadric_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_label(line, repl_line, sizeof(repl_line))) {
        handled = feed_line(repl_line);
    } else {
        repl_eval_c_expr_to_repl(line, repl_line, sizeof(repl_line));
        handled = feed_line(repl_line);
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

static ExportNeeds export_collect_needs(void) {
    ExportNeeds needs = {
        .needs_tess = export_uses_tess_commands(),
        .needs_rand = 0,
    };

    /* Check each command for rand() function calls. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (repl_state_document_cmds_mut()[cmd_idx].valid && strstr(repl_state_document_cmds_mut()[cmd_idx].source, "rand(") != NULL)
            needs.needs_rand = 1;
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

    /* Emit footer lines before init section. */
    for (int line_idx = 0; g_footer_pre_init[line_idx]; line_idx++)
        fprintf(f, "%s\n", g_footer_pre_init[line_idx]);
    emit_export_init_section_to_file(f, include_tess);
    /* Emit footer lines after init section. */
    for (int line_idx = 0; g_footer_post_init[line_idx]; line_idx++)
        fprintf(f, "%s\n", g_footer_post_init[line_idx]);
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

static void emit_export_rand_helper_section(FILE *f,
                                            const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_rand_helper(f);
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
    { "rand helper",        emit_export_rand_helper_section,        export_section_needs_rand },
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

void repl_export_save_output(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        set_status("Error: cannot write output.c");
        return;
    }

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
} ImportState;

static void import_state_init(ImportState *s) {
    s->in_snippet = 0;
    s->past_snippet = 0;
    s->func_depth = 0;
    s->loaded = 0;
    s->warnings = 0;
    s->pending_comment_count = 0;
}

/* --- pre-snippet handlers (camera, workspace header, function bodies) ----- */

static int import_try_camera(const char *p) {
    if (import_parse_export_angle_init(p)) return 1;
    if (import_parse_cam_line(p))          return 1;
    return 0;
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
    /* Feed accumulated pending comments before the function header. */
    for (int comment_idx = 0; comment_idx < s->pending_comment_count; comment_idx++)
        import_feed_one_line(s->pending_comments[comment_idx], &s->loaded, &s->warnings);
    s->pending_comment_count = 0;
    int before = repl_state_document_count();
    int handled = feed_line(repl_func_line);
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
    s->pending_comment_count = 0;
    s->in_snippet = 1;
    /* Function/header import may leave the editor cursor in an insertion slot
     * inside existing commands.  Force snippet lines to start appending from
     * the end of the command list. */
    repl_state_insert_mode_set(0);
    repl_state_edit_line_set(repl_state_document_count());
    return 1;
}

static int import_try_pending_comment(ImportState *s, const char *p) {
    if (p[0] == '/' && p[1] == '/' &&
        s->pending_comment_count < IMPORT_MAX_PENDING_COMMENTS) {
        snprintf(s->pending_comments[s->pending_comment_count++],
                 MAX_LINE_LEN, "%s", p);
    } else if (*p != '\0') {
        /* Any non-empty, non-comment line resets the pending buffer so stray
         * comments don't leak onto unrelated lines that follow. */
        s->pending_comment_count = 0;
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

static int import_try_blank(const char *p) {
    return *p == '\0';
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
    if (import_try_blank(p))                                   return;
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

    if (state.loaded > 0) {
        repl_source_scope_depth_cache_invalidate();
        repl_reformat_commands();
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

void repl_dump_code_panel_text(FILE *out) {
    FILE *dst = out ? out : stdout;

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
        fprintf(dst, "%s\n", repl_state_document_cmds_mut()[cmd_idx].source);
    }

    fflush(dst);
}

void repl_dump_code_panel_visual_text(FILE *out) {
    FILE *dst = out ? out : stdout;
    int panel_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = repl_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;

    repl_layout_code_panel_rect(NULL, NULL, &panel_w, NULL);
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
        dump_code_panel_wrapped_line(dst, repl_state_document_cmds_mut()[cmd_idx].source, text_x, panel_w);
    }

    fflush(dst);
}
