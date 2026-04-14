#include "sample.h"
#include "repl_core_internal.h"

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

char g_workspace_header_lines[MAX_WORKSPACE_HEADER_LINES][WORKSPACE_HEADER_LINE_LEN];
int  g_workspace_header_line_count = 0;

static void workspace_slug_from_name(const char *name, char *out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isspace(c) || c == '-' || c == '/') out[j++] = '_';
        else if (isalnum(c))                    out[j++] = (char)tolower(c);
        else if (c == '_')                      out[j++] = '_';
    }
    out[j] = '\0';
}

static void workspace_format_float(char *buf, size_t n, float v) {
    snprintf(buf, n, "%g", (double)v);
}

void refresh_workspace_header_lines(void) {
    int n = 0;
    if (n < MAX_WORKSPACE_HEADER_LINES) {
        snprintf(g_workspace_header_lines[n++], WORKSPACE_HEADER_LINE_LEN,
                 "// @workspace: REPL state (auto-saved)");
    }
    for (int i = 0; i < g_num_predef_vars && n < MAX_WORKSPACE_HEADER_LINES; i++) {
        char vbuf[32];
        workspace_format_float(vbuf, sizeof(vbuf), g_predef_vars[i].value);
        snprintf(g_workspace_header_lines[n++], WORKSPACE_HEADER_LINE_LEN,
                 "// @var %s = %s", g_predef_vars[i].name, vbuf);
    }
    for (int i = 0; i < CFG_ITEM_COUNT && n < MAX_WORKSPACE_HEADER_LINES; i++) {
        char slug[32];
        workspace_slug_from_name(g_cfg_items[i].label, slug, sizeof(slug));
        snprintf(g_workspace_header_lines[n++], WORKSPACE_HEADER_LINE_LEN,
                 "// @cfg %s = %d", slug, *g_cfg_items[i].value);
    }
    g_workspace_header_line_count = n;
}

int parse_workspace_header_line(const char *line) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p[0] != '/' || p[1] != '/') return 0;
    p += 2;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '@') return 0;
    p++;

    if (strncmp(p, "workspace", 9) == 0) return 1;

    if (strncmp(p, "var", 3) == 0 && isspace((unsigned char)p[3])) {
        p += 4;
        while (*p && isspace((unsigned char)*p)) p++;
        char name[16];
        int ni = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
               ni < (int)sizeof(name) - 1)
            name[ni++] = *p++;
        name[ni] = '\0';
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '=') return 0;
        p++;
        ExprCtx ctx = { p, NULL, 0 };
        float val = eval_expr(&ctx);
        for (int i = 0; i < g_num_predef_vars; i++) {
            if (strcmp(g_predef_vars[i].name, name) == 0) {
                g_predef_vars[i].value = val;
                return 1;
            }
        }
        return 1;
    }

    if (strncmp(p, "cfg", 3) == 0 && isspace((unsigned char)p[3])) {
        p += 4;
        while (*p && isspace((unsigned char)*p)) p++;
        char slug[32];
        int si = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
               si < (int)sizeof(slug) - 1)
            slug[si++] = *p++;
        slug[si] = '\0';
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '=') return 0;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        int val = (int)strtol(p, NULL, 10);
        for (int i = 0; i < CFG_ITEM_COUNT; i++) {
            char item_slug[32];
            workspace_slug_from_name(g_cfg_items[i].label, item_slug, sizeof(item_slug));
            if (strcmp(item_slug, slug) == 0) {
                int max_v = g_cfg_items[i].n_states > 0
                            ? g_cfg_items[i].n_states - 1 : 1;
                if (val < 0) val = 0;
                if (val > max_v) val = max_v;
                *g_cfg_items[i].value = val;
                return 1;
            }
        }
        return 1;
    }

    return 0;
}

char g_render_state_lines[RENDER_STATE_LINE_COUNT][64] = {
    "  glEnable(GL_MULTISAMPLE);",
    "  glDisable(GL_LINE_SMOOTH);"
};

char g_cam_lines[CAM_LINE_COUNT][96] = {
    "  glTranslatef(0.0000f, 0.0000f, -5.0000f);",
    "  glRotatef(20.0000f, 1.0f, 0.0f, 0.0f);",
    "  glRotatef(30.0000f, 0.0f, 1.0f, 0.0f);",
    "  glTranslatef(0.0000f, 0.0000f, 0.0000f);"
};

const char *g_header_post[] = {
    NULL
};

typedef struct {
    const char *repl_line;
    const int  *toggle;
} InitBootstrapEntry;

int g_init_attenuate_points = CFG_DEFAULT_ATTENUATE_POINTS;

static const InitBootstrapEntry g_init_bootstrap_repl[] = {
    { "glEnable(GL_COLOR_MATERIAL);", NULL },
    { "glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);", NULL },
    { "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);", NULL },
    { "glEnable(GL_BLEND);", NULL },
    { "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);", NULL },
    { "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1.0, 0.0, 0.02);",
      &g_init_attenuate_points },
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
    "  glEnable(GL_COLOR_MATERIAL);",
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
    int saved_edit_line = g_edit_line;

    if (g_init_bootstrap_ready)
        return;

    g_edit_line = 0;
    for (int i = 0; i < NUM_INIT_BOOTSTRAP; i++) {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        if (!repl_parse_command(g_init_bootstrap_repl[i].repl_line, &cmd)) {
            fprintf(stderr, "init bootstrap parse failed: %s\n",
                    g_init_bootstrap_repl[i].repl_line);
            abort();
        }
        g_init_bootstrap_cmds[i] = cmd;
    }
    g_edit_line = saved_edit_line;
    g_init_bootstrap_ready = 1;
}

void ensure_init_bootstrap_ready(void) {
    if (!g_init_bootstrap_ready)
        parse_init_bootstrap();
}

void apply_init_bootstrap(void) {
    ensure_init_bootstrap_ready();

    for (int i = 0; i < NUM_INIT_BOOTSTRAP; i++) {
        if (g_init_bootstrap_repl[i].toggle &&
            *g_init_bootstrap_repl[i].toggle == 0) {
            if (g_init_bootstrap_cmds[i].type == CMD_POINT_PARAMETER_FV &&
                g_init_bootstrap_cmds[i].mode == GL_POINT_DISTANCE_ATTENUATION) {
                GLCmd disabled = g_init_bootstrap_cmds[i];
                disabled.args[0] = 1.0f;
                disabled.args[1] = 0.0f;
                disabled.args[2] = 0.0f;
                apply_state_cmd(&disabled, 1.0f);
            }
            continue;
        }
        apply_state_cmd(&g_init_bootstrap_cmds[i], 1.0f);
    }
}

int init_section_line_count(void) {
    int count = init_host_only_line_count();

    ensure_init_bootstrap_ready();
    for (int i = 0; i < NUM_INIT_BOOTSTRAP; i++) {
        if (g_init_bootstrap_repl[i].toggle &&
            *g_init_bootstrap_repl[i].toggle == 0)
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
    for (int idx = 0; idx < NUM_INIT_BOOTSTRAP; idx++) {
        if (g_init_bootstrap_repl[idx].toggle &&
            *g_init_bootstrap_repl[idx].toggle == 0)
            continue;
        if (enabled_idx == i) {
            format_cmd_source_as_c(buf, n, &g_init_bootstrap_cmds[idx], 0);
            return;
        }
        enabled_idx++;
    }

    buf[0] = '\0';
}

static void emit_export_init_section_to_file(FILE *f, int include_tess) {
    char line[MAX_LINE_LEN];

    for (int i = 0; g_init_host_only_visible_c[i]; i++)
        fprintf(f, "%s\n", g_init_host_only_visible_c[i]);
    if (include_tess)
        for (int i = 0; g_init_host_only_tess_c[i]; i++)
            fprintf(f, "%s\n", g_init_host_only_tess_c[i]);

    ensure_init_bootstrap_ready();
    for (int idx = 0; idx < NUM_INIT_BOOTSTRAP; idx++) {
        if (g_init_bootstrap_repl[idx].toggle &&
            *g_init_bootstrap_repl[idx].toggle == 0)
            continue;
        format_cmd_source_as_c(line, sizeof(line), &g_init_bootstrap_cmds[idx], 0);
        fprintf(f, "%s\n", line);
    }
}

static void emit_export_header_pre(FILE *f) {
    char angle_line[64];

    snprintf(angle_line, sizeof(angle_line),
             "static float g_angle = %.4ff;", g_cam_ry);

    for (int i = 0; g_header_pre[i]; i++) {
        if (strcmp(g_header_pre[i], "void display() {") == 0)
            break;
        if (strcmp(g_header_pre[i], "static float g_angle = 0.0f;") == 0) {
            fprintf(f, "%s\n", angle_line);
            continue;
        }
        fprintf(f, "%s\n", g_header_pre[i]);
    }
}

static void emit_export_cam_lines(FILE *f) {
    fprintf(f, "  glTranslatef(0.0000f, 0.0000f, %.4ff);\n", -g_cam_dist);
    fprintf(f, "  glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);\n", g_cam_rx);
    fprintf(f, "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);\n");
    fprintf(f, "  glTranslatef(%.4ff, %.4ff, %.4ff);\n",
            -g_cam_tx, -g_cam_ty, -g_cam_tz);
}

void update_render_state_strings(void) {
    snprintf(g_render_state_lines[0], sizeof(g_render_state_lines[0]),
             "  gl%s(GL_MULTISAMPLE);",
             g_multisample_enabled ? "Enable" : "Disable");
    snprintf(g_render_state_lines[1], sizeof(g_render_state_lines[1]),
             "  gl%s(GL_LINE_SMOOTH);",
             g_line_smooth_enabled ? "Enable" : "Disable");
}

/* Legacy: invert the old gluLookAt-based forward transform so pre-refactor
 * output.c files keep loading. Recovers (rx, ry, dist, tx, ty, tz) from eye
 * and center, assuming up = (0, 1, 0). Returns 1 on success. */
static int legacy_lookat_to_cam_state(float ex, float ey, float ez,
                                      float cx, float cy, float cz) {
    float vx = ex - cx, vy = ey - cy, vz = ez - cz;
    float d = sqrtf(vx * vx + vy * vy + vz * vz);
    if (d < 1e-6f) return 0;

    float srx = vy / d;
    if (srx >  1.0f) srx =  1.0f;
    if (srx < -1.0f) srx = -1.0f;
    float rx = asinf(srx);

    float ry = atan2f(vx, vz);

    g_cam_rx   = rx * 180.0f / (float)M_PI;
    g_cam_ry   = ry * 180.0f / (float)M_PI;
    g_cam_dist = d;
    g_cam_tx   = cx;
    g_cam_ty   = cy;
    g_cam_tz   = cz;
    return 1;
}

/* Extracts the first 6 floats (eye xyz + center xyz) from text containing
 * a gluLookAt(...) call and applies them to the camera state via the
 * legacy inversion. Returns 1 if parsed+applied, 0 otherwise. */
int import_parse_lookat_block(const char *text) {
    const char *p = strstr(text, "gluLookAt");
    if (!p) return 0;
    p = strchr(p, '(');
    if (!p) return 0;
    p++;

    float v[6];
    for (int i = 0; i < 6; i++) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) return 0;
        char *end = NULL;
        v[i] = strtof(p, &end);
        if (end == p) return 0;
        p = end;
    }

    return legacy_lookat_to_cam_state(v[0], v[1], v[2], v[3], v[4], v[5]);
}

/* Skip whitespace, commas, and trailing 'f' / 'F' float suffixes so strtof
 * can march through a call like `glTranslatef(0.0f, 0.0f, -5.0f);`. */
static const char *cam_line_skip_sep(const char *p) {
    while (*p == 'f' || *p == 'F' || *p == ',' || *p == ' ' || *p == '\t')
        p++;
    return p;
}

static int cam_line_read_floats(const char *p, float *out, int n) {
    for (int i = 0; i < n; i++) {
        p = cam_line_skip_sep(p);
        char *end = NULL;
        out[i] = strtof(p, &end);
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

    g_cam_ry = v;
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
        g_cam_dist = -v[2];
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
        g_cam_rx = v[0];
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
        g_cam_ry = v[0];
        g_cam_parse_state = 3;
        return 1;
    }

    if (g_cam_parse_state == 3 && strncmp(p, "glRotatef", 9) == 0) {
        /* Literal `glRotatef(g_angle, 0,1,0)` animation hook — no scalars
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
        g_cam_tx = -v[0];
        g_cam_ty = -v[1];
        g_cam_tz = -v[2];
        g_cam_parse_state = 5;
        return 1;
    }

    return 0;
}

void update_cam_lines(void) {
    snprintf(g_cam_lines[0], sizeof(g_cam_lines[0]),
             "  glTranslatef(0.0000f, 0.0000f, %.4ff);", -g_cam_dist);
    snprintf(g_cam_lines[1], sizeof(g_cam_lines[1]),
             "  glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);", g_cam_rx);
    snprintf(g_cam_lines[2], sizeof(g_cam_lines[2]),
             "  glRotatef(%.4ff, 0.0f, 1.0f, 0.0f);", g_cam_ry);
    snprintf(g_cam_lines[3], sizeof(g_cam_lines[3]),
             "  glTranslatef(%.4ff, %.4ff, %.4ff);",
             -g_cam_tx, -g_cam_ty, -g_cam_tz);
}

static void write_light_setup(FILE *f) {
    static const char *light_names[] = {
        "GL_LIGHT0", "GL_LIGHT1", "GL_LIGHT2", "GL_LIGHT3"
    };

    int first_light = 1;

    for (int i = 0; i < MAX_LIGHTS; i++) {
        const SceneLight *l = &g_lights[i];
        const char *ln = light_names[i];

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
        int ni = 0;
        while (*hp && (isalnum((unsigned char)*hp) || *hp == '_') &&
               ni < (int)sizeof(var_name) - 1)
            var_name[ni++] = *hp++;
        var_name[ni] = '\0';
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
        repl_expr_to_c(start_s, c_start, sizeof(c_start));
        repl_expr_to_c(end_s, c_end, sizeof(c_end));
        if (step_s[0])
            repl_expr_to_c(step_s, c_step, sizeof(c_step));
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
    if (parse_for_header(p, var_name, sizeof(var_name),
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
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid)
            continue;
        if (cmd_type_is_tess(g_cmds[i].type))
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
        strncpy(out, src, (size_t)out_sz - 1);
        out[out_sz - 1] = '\0';
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
        float value = eval_expr(&ctx);
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
        for (int i = 0; i < param_count && written < out_sz; i++) {
            written += snprintf(out + written, out_sz - written, "%s%s",
                                i == 0 ? "" : ", ", param_names[i]);
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
        for (int i = 0; i < num_vars; i++) {
            int nlen = (int)strlen(vars[i].name);
            if (nlen == len && strncmp(start, vars[i].name, len) == 0)
                return 1;
        }
    }
    return 0;
}

int input_has_any_visible_vars(const char *s, ExprVar *vars, int num_vars) {
    return input_has_predef_vars(s) || input_has_expr_vars(s, vars, num_vars);
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

    for (int i = 0; i < arg_count; i++)
        repl_expr_to_c(raw_args[i], c_args[i], sizeof(c_args[i]));

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
        repl_expr_to_c(cmd->source, c_src, sizeof(c_src));
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

    for (int i = begin_idx + 1; i < g_num_cmds; i++) {
        CmdType t = g_cmds[i].type;
        if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) depth++;
        else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
            if (--depth == 0)
                return i;
        }
    }

    return g_num_cmds;
}

static void write_canonical_cmd_as_c(FILE *f, const GLCmd *cmd, int for_depth,
                                     int *tess_depth) {
    switch (cmd->type) {
    case CMD_COMMENT:
        fprintf(f, "%s\n", cmd->source);
        break;
    case CMD_VAR_ASSIGN: {
        char c_src[MAX_LINE_LEN];
        repl_expr_to_c(cmd->source, c_src, sizeof(c_src));
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

    for (int i = start; i < end_idx && i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        switch (g_cmds[i].type) {
        case CMD_FOR_BEGIN:
            write_for_begin_as_c(f, &g_cmds[i]);
            for_depth++;
            break;
        case CMD_FOR_END:
            for_depth--;
            fprintf(f, "%s\n", g_cmds[i].source);
            break;
        case CMD_FUNC_DEF:
            if (skip_func_defs)
                i = find_export_block_end(i);
            break;
        case CMD_FUNC_END:
            break;
        default:
            write_canonical_cmd_as_c(f, &g_cmds[i], for_depth, &tess_depth);
            break;
        }
    }
}

static void write_predef_var_globals(FILE *f) {
    if (g_num_predef_vars <= 0) return;
    fprintf(f, "\n/* Predefined REPL variables (file scope for func access) */\n");
    for (int i = 0; i < g_num_predef_vars; i++) {
        fprintf(f, "static float %s = 0.0f;\n", g_predef_vars[i].name);
    }
}

static void write_predef_var_reset_func(FILE *f) {
    fprintf(f, "\nstatic void reset_repl_vars(void) {\n");
    for (int i = 0; i < g_num_predef_vars; i++) {
        if (strcmp(g_predef_vars[i].name, "t") == 0) {
            fprintf(f, "  %s = 0.001f * (float)glutGet(GLUT_ELAPSED_TIME);\n",
                    g_predef_vars[i].name);
        } else {
            fprintf(f, "  %s = %g;\n",
                    g_predef_vars[i].name, g_predef_vars[i].value);
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
    write_render_body_range_as_c(f, 0, g_num_cmds, 1);
    int bb = 0;
    for (int i = 0; i < g_num_cmds; i++) {
        if (g_cmds[i].valid && g_cmds[i].type == CMD_BEGIN) bb++;
        else if (g_cmds[i].valid && g_cmds[i].type == CMD_END) bb--;
    }
    if (bb > 0)
        fprintf(f, "  glEnd();\n");
    fprintf(f, "  // Snippet end\n");
    fprintf(f, "}\n");
}

static void write_func_defs_as_c(FILE *f) {
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid || g_cmds[i].type != CMD_FUNC_DEF) continue;
        int fn = (int)g_cmds[i].args[0];
        int parsed_fn = fn;
        int param_count = 0;
        char param_names[MAX_EXPR_VARS][16];
        int fe = find_export_block_end(i);
        if (parse_repl_func_signature(g_cmds[i].source, &parsed_fn,
                                      param_names, MAX_EXPR_VARS,
                                      &param_count) && param_count > 0) {
            fprintf(f, "\nstatic void func%d(", parsed_fn);
            for (int p = 0; p < param_count; p++)
                fprintf(f, "%sfloat %s", p == 0 ? "" : ", ", param_names[p]);
            fprintf(f, ") {\n");
        } else {
            fprintf(f, "\nstatic void func%d(void) {\n", fn);
        }
        write_render_body_range_as_c(f, i + 1, fe, 0);
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
        float val = eval_expr(&ctx);
        p = ctx.p;

        for (int i = 0; i < g_num_predef_vars; i++) {
            if (strcmp(g_predef_vars[i].name, name) == 0) {
                g_predef_vars[i].value = val;
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

static int import_make_repl_for_header(const char *line, char *out, int out_sz) {
    char var[16];
    float start_v, end_v, step_v;
    if (!parse_c_for_header(line, var, sizeof(var), &start_v, &end_v, &step_v))
        return 0;

    char repl_line[MAX_LINE_LEN];
    c_expr_to_repl(line, repl_line, sizeof(repl_line));

    if (input_has_predef_vars(repl_line)) {
        const char *rp = repl_line;
        while (*rp && *rp != '=') rp++;
        if (!*rp) return 0;
        rp++;
        while (*rp && isspace((unsigned char)*rp)) rp++;

        const char *se_start = rp;
        int depth = 0;
        while (*rp) {
            if (*rp == '(') depth++;
            else if (*rp == ')') depth--;
            if (*rp == ';' && depth == 0) break;
            rp++;
        }
        if (*rp != ';') return 0;

        char start_expr[96];
        int sl = (int)(rp - se_start);
        if (sl > (int)sizeof(start_expr) - 1) sl = (int)sizeof(start_expr) - 1;
        memcpy(start_expr, se_start, (size_t)sl);
        start_expr[sl] = '\0';
        trim_in_place(start_expr);

        rp++;
        while (*rp && isspace((unsigned char)*rp)) rp++;
        while (*rp && (isalnum((unsigned char)*rp) || *rp == '_')) rp++;
        while (*rp && isspace((unsigned char)*rp)) rp++;
        while (*rp && (*rp == '<' || *rp == '>' || *rp == '=' || *rp == '!')) rp++;
        while (*rp && isspace((unsigned char)*rp)) rp++;

        const char *ee_start = rp;
        depth = 0;
        while (*rp) {
            if (*rp == '(') depth++;
            else if (*rp == ')') depth--;
            if (*rp == ';' && depth == 0) break;
            rp++;
        }
        if (*rp != ';') return 0;

        char end_expr[96];
        int el = (int)(rp - ee_start);
        if (el > (int)sizeof(end_expr) - 1) el = (int)sizeof(end_expr) - 1;
        memcpy(end_expr, ee_start, (size_t)el);
        end_expr[el] = '\0';
        trim_in_place(end_expr);

        if (step_v != 1.0f) {
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
    for (int i = 0; i < count && written < out_sz; i++)
        written += snprintf(out + written, out_sz - written, "%s%s",
                            i == 0 ? "" : ", ", names[i]);
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
        float nv[3] = {0, 0, 1};
        const char *np = p;
        for (int i = 0; i < 3; i++) {
            const char *eq = strchr(np, '=');
            if (!eq) break;
            eq++;
            ExprCtx ctx = { eq, NULL, 0 };
            nv[i] = eval_expr(&ctx);
            np = ctx.p;
        }
        snprintf(out, out_sz, "gluNormal(%g, %g, %g);", nv[0], nv[1], nv[2]);
        return 1;
    }

    if (strncmp(p, "{ _tc[", 6) == 0) {
        float cv[4] = {1, 1, 1, 1};
        const char *cp = p;
        for (int i = 0; i < 4; i++) {
            const char *eq = strchr(cp, '=');
            if (!eq) break;
            eq++;
            ExprCtx ctx = { eq, NULL, 0 };
            cv[i] = eval_expr(&ctx);
            cp = ctx.p;
        }
        snprintf(out, out_sz, "gluColor(%g, %g, %g, %g);",
                 cv[0], cv[1], cv[2], cv[3]);
        return 1;
    }

    if (strstr(p, "TessVertex") != NULL && strstr(p, "gluTessVertex") != NULL) {
        float vv[3] = {0, 0, 0};
        const char *vp = strstr(p, "_v->pos[0]");
        if (!vp) return 0;
        for (int i = 0; i < 3; i++) {
            const char *eq = strchr(vp, '=');
            if (!eq) break;
            eq++;
            ExprCtx ctx = { eq, NULL, 0 };
            vv[i] = eval_expr(&ctx);
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

    for (int i = 0; i < count; i++)
        c_expr_to_repl(raw_args[i], repl_args[i], sizeof(repl_args[i]));

    snprintf(out, out_sz, "glPointParameterfv(%s, %s, %s, %s);",
             pname, repl_args[0], repl_args[1], repl_args[2]);
    return 1;
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

    for (int i = 0; names[i]; i++) {
        const char *name = names[i];
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

        c_expr_to_repl(tmp, out, out_sz);
        return 1;
    }

    return 0;
}

static void import_feed_one_line(const char *line, int *loaded, int *warnings) {
    char repl_line[MAX_LINE_LEN];
    int before = g_num_cmds;
    int handled = 0;

    if (import_make_repl_for_header(line, repl_line, sizeof(repl_line))) {
        handled = feed_line(repl_line);
    } else if (import_make_repl_tess_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_point_parameter_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_quadric_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_label(line, repl_line, sizeof(repl_line))) {
        handled = feed_line(repl_line);
    } else {
        c_expr_to_repl(line, repl_line, sizeof(repl_line));
        handled = feed_line(repl_line);
    }

    if (g_num_cmds > before) *loaded += (g_num_cmds - before);
    if (!handled) {
        fprintf(stderr, "Warning: could not parse line: %s\n", line);
        (*warnings)++;
    }
}

void save_output(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        set_status("Error: cannot write output.c");
        return;
    }

    int needs_tess = export_uses_tess_commands();
    int needs_rand = 0;
    for (int i = 0; i < g_num_cmds; i++)
        if (g_cmds[i].valid && strstr(g_cmds[i].source, "rand(") != NULL)
            needs_rand = 1;

    update_render_state_strings();
    update_cam_lines();
    refresh_workspace_header_lines();

    for (int i = 0; i < g_workspace_header_line_count; i++)
        fprintf(f, "%s\n", g_workspace_header_lines[i]);
    if (g_workspace_header_line_count > 0)
        fprintf(f, "\n");

    emit_export_header_pre(f);
    write_predef_var_globals(f);
    if (needs_rand)
        write_rand_helper(f);
    if (needs_tess)
        write_tess_preamble(f);
    write_predef_var_reset_func(f);
    write_func_defs_as_c(f);
    write_render_helper_as_c(f, "render_repl_geometry");

    fprintf(f, "\nvoid display() {\n");
    fprintf(f, "  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);\n");
    fprintf(f, "  glLoadIdentity();\n");
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        fprintf(f, "%s\n", g_render_state_lines[i]);
    emit_export_cam_lines(f);
    for (int i = 0; g_header_post[i]; i++)
        fprintf(f, "%s\n", g_header_post[i]);
    write_light_setup(f);

    fprintf(f, "\n  /* Vertex Fill Pass */\n");
    fprintf(f, "  glPushMatrix();\n");
    fprintf(f, "  reset_repl_vars();\n");
    fprintf(f, "  render_repl_geometry();\n");
    fprintf(f, "  glPopMatrix();\n");

    if (g_show_outlines) {
        fprintf(f, "\n  /* Vertex Outline Pass */\n");
        fprintf(f, "  glColor3f(0.0f, 0.0f, 0.0f);\n");
        fprintf(f, "  glDisable(GL_COLOR_MATERIAL);\n");
        fprintf(f, "  glEnable(GL_POLYGON_OFFSET_LINE);\n");
        fprintf(f, "  glPolygonOffset(%#.6gf, %#.6gf);\n",
                      (double)REPL_OUTLINE_POLYGON_OFFSET_FACTOR, (double)REPL_OUTLINE_POLYGON_OFFSET_UNITS);
        fprintf(f, "  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);\n");
        fprintf(f, "  glLineWidth(1.2f);\n");
        fprintf(f, "  glEnable(GL_LIGHTING);\n");
        fprintf(f, "  glPushMatrix();\n");
        fprintf(f, "  reset_repl_vars();\n");
        fprintf(f, "  render_repl_geometry();\n");
        fprintf(f, "  glPopMatrix();\n");
        fprintf(f, "  glLineWidth(1.0f);\n");
        fprintf(f, "  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);\n");
        fprintf(f, "  glDisable(GL_POLYGON_OFFSET_LINE);\n");
    }

    if (g_show_vpoints) {
        fprintf(f, "\n  /* Vertex Point Pass */\n");
        fprintf(f, "  glColor3f(0.0f, 0.0f, 0.0f);\n");
        fprintf(f, "  glDisable(GL_COLOR_MATERIAL);\n");
        fprintf(f, "  glPointSize(8.0f);\n");
        fprintf(f, "  glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);\n");
        fprintf(f, "  glEnable(GL_LIGHTING);\n");
        fprintf(f, "  glPushMatrix();\n");
        fprintf(f, "  reset_repl_vars();\n");
        fprintf(f, "  render_repl_geometry();\n");
        fprintf(f, "  glPopMatrix();\n");
        fprintf(f, "  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);\n");
        fprintf(f, "  glPointSize(1.0f);\n");
    }

    for (int i = 0; g_footer_pre_init[i]; i++)
        fprintf(f, "%s\n", g_footer_pre_init[i]);
    emit_export_init_section_to_file(f, needs_tess);
    for (int i = 0; g_footer_post_init[i]; i++)
        fprintf(f, "%s\n", g_footer_post_init[i]);

    fclose(f);

    char msg[128];
    snprintf(msg, sizeof(msg), "Saved to output.c (%d commands)", g_num_cmds);
    set_status(msg);
}

int load_from_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return 0;

    char line[MAX_LINE_LEN];
    int in_snippet = 0;
    int past_snippet = 0;
    int import_func_depth = 0;
    int loaded = 0;
    int warnings = 0;

    char lookat_buf[512];
    int  lookat_active = 0;
    int  lookat_len = 0;

    import_cam_parser_reset();

    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;

        /* Legacy gluLookAt format: accumulate 3 lines then invert */
        if (lookat_active || strstr(p, "gluLookAt") != NULL) {
            if (!lookat_active) {
                lookat_active = 1;
                lookat_len = 0;
                lookat_buf[0] = '\0';
            }
            int add = (int)strlen(p);
            if (lookat_len + add + 2 < (int)sizeof(lookat_buf)) {
                memcpy(lookat_buf + lookat_len, p, (size_t)add);
                lookat_len += add;
                lookat_buf[lookat_len++] = ' ';
                lookat_buf[lookat_len] = '\0';
            }
            if (strchr(p, ';')) {
                import_parse_lookat_block(lookat_buf);
                lookat_active = 0;
                lookat_len = 0;
            }
            continue;
        }

        if (!in_snippet && import_parse_export_angle_init(p))
            continue;

        /* Export camera format: compact glTranslatef/glRotatef lines in display() */
        if (!in_snippet && import_parse_cam_line(p))
            continue;

        if (past_snippet)
            continue;

        if (!in_snippet) {
            if (parse_workspace_header_line(p))
                continue;
            if (import_func_depth > 0) {
                import_feed_one_line(p, &loaded, &warnings);
                for (const char *bp = p; *bp; bp++) {
                    if (*bp == '{') import_func_depth++;
                    else if (*bp == '}') import_func_depth--;
                }
                continue;
            }

            char repl_func_line[MAX_LINE_LEN];
            if (import_make_repl_func_header(p, repl_func_line, sizeof(repl_func_line))) {
                int before = g_num_cmds;
                int handled = feed_line(repl_func_line);
                if (g_num_cmds > before) loaded += (g_num_cmds - before);
                if (!handled) {
                    fprintf(stderr, "Warning: could not parse line: %s\n", line);
                    warnings++;
                }
                import_func_depth = 1;
                continue;
            }

            if (strncmp(p, "// Snippet start", 16) == 0)
                in_snippet = 1;
            continue;
        }

        if (strncmp(p, "// Snippet end", 14) == 0) {
            in_snippet = 0;
            past_snippet = 1;
            continue;
        }

        if (len == 0 || *p == '\0') continue;
        if (import_parse_predef_decl(p))
            continue;
        import_feed_one_line(p, &loaded, &warnings);
    }

    fclose(f);

    if (loaded > 0) {
        depth_cache_invalidate();
        repl_reformat_commands();
        char msg[256];
        if (warnings > 0)
            snprintf(msg, sizeof(msg),
                     "Loaded %d commands from %s (%d warnings)",
                     loaded, filename, warnings);
        else
            snprintf(msg, sizeof(msg),
                     "Loaded %d commands from %s", loaded, filename);
        set_status(msg);
        fprintf(stderr, "%s\n", msg);
    }
    return loaded > 0;
}

static int dump_code_panel_available_chars(int panel_w, int x) {
    int avail_px = panel_w - x - 4;
    if (avail_px < FONT_W)
        return 0;
    return avail_px / FONT_W;
}

static int dump_code_panel_cont_indent_chars(const char *text) {
    const char *src = text ? text : "";
    int leading = 0;

    while (src[leading] && isspace((unsigned char)src[leading]))
        leading++;

    const char *paren = strchr(src, '(');
    if (paren && paren[1] != '\0') {
        int align = (int)(paren - src) + 1;
        int max_align = leading + 12;
        if (align > max_align)
            align = max_align;
        return align;
    }

    return leading + 4;
}

static int dump_code_panel_is_secondary_break(char c) {
    return c == ')' || c == ' ' || c == '+' || c == '*' || c == '-' || c == '/';
}

static int dump_code_panel_find_wrap_break(const char *text, int start,
                                           int max_chars, int len) {
    int end = start + max_chars - 1;
    int search_start = start;
    if (end >= len)
        end = len - 1;

    while (search_start < len &&
           isspace((unsigned char)text[search_start]))
        search_start++;

    for (int i = end; i > search_start; i--) {
        if (text[i] == ',')
            return i;
    }

    for (int i = end; i > search_start; i--) {
        if (dump_code_panel_is_secondary_break(text[i]))
            return i;
    }

    for (int i = end + 1; i < len; i++) {
        if (text[i] == ',' ||
            (i > search_start && dump_code_panel_is_secondary_break(text[i])))
            return i;
    }

    return -1;
}

static void dump_code_panel_wrapped_line(FILE *dst, const char *text,
                                         int first_x, int panel_w) {
    const char *src = text ? text : "";
    int len = (int)strlen(src);
    int pos = 0;
    int x = first_x;
    int cont_indent_chars = dump_code_panel_cont_indent_chars(src);
    int cont_x = first_x + cont_indent_chars * FONT_W;

    if (len == 0) {
        fputc('\n', dst);
        return;
    }

    for (;;) {
        int width_chars = dump_code_panel_available_chars(panel_w, x);
        int remaining = len - pos;
        int seg_len = remaining;
        int prefix_chars = (x - first_x) / FONT_W;

        if (g_wrap_at_comma && width_chars >= 1 && remaining > width_chars) {
            int break_idx = dump_code_panel_find_wrap_break(src, pos,
                                                            width_chars, len);
            if (break_idx >= 0)
                seg_len = break_idx - pos + 1;
        }

        fprintf(dst, "%*s%.*s\n", prefix_chars, "", seg_len, src + pos);

        if (!g_wrap_at_comma || width_chars < 1 || seg_len == remaining)
            break;

        pos += seg_len;
        x = cont_x;
    }
}

void repl_dump_code_panel_text(FILE *out) {
    FILE *dst = out ? out : stdout;

    update_render_state_strings();
    update_cam_lines();

    fprintf(dst, "--- header_pre ---\n");
    for (int i = 0; g_header_pre[i]; i++)
        fprintf(dst, "%s\n", g_header_pre[i]);

    fprintf(dst, "--- render_state ---\n");
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        fprintf(dst, "%s\n", g_render_state_lines[i]);

    fprintf(dst, "--- camera ---\n");
    for (int i = 0; i < CAM_LINE_COUNT; i++)
        fprintf(dst, "%s\n", g_cam_lines[i]);

    fprintf(dst, "--- header_post ---\n");
    for (int i = 0; g_header_post[i]; i++)
        fprintf(dst, "%s\n", g_header_post[i]);

    fprintf(dst, "--- source ---\n");
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        fprintf(dst, "%s\n", g_cmds[i].source);
    }

    fflush(dst);
}

void repl_dump_code_panel_visual_text(FILE *out) {
    FILE *dst = out ? out : stdout;
    int panel_w = (int)(g_win_w * g_panel_frac);
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;

    update_render_state_strings();
    update_cam_lines();

    fprintf(dst, "--- header_pre ---\n");
    for (int i = 0; g_header_pre[i]; i++)
        dump_code_panel_wrapped_line(dst, g_header_pre[i], text_x, panel_w);

    fprintf(dst, "--- render_state ---\n");
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        dump_code_panel_wrapped_line(dst, g_render_state_lines[i], text_x, panel_w);

    fprintf(dst, "--- camera ---\n");
    for (int i = 0; i < CAM_LINE_COUNT; i++)
        dump_code_panel_wrapped_line(dst, g_cam_lines[i], text_x, panel_w);

    fprintf(dst, "--- header_post ---\n");
    for (int i = 0; g_header_post[i]; i++)
        dump_code_panel_wrapped_line(dst, g_header_post[i], text_x, panel_w);

    fprintf(dst, "--- source ---\n");
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        dump_code_panel_wrapped_line(dst, g_cmds[i].source, text_x, panel_w);
    }

    fflush(dst);
}
