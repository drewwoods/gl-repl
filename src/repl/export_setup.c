#include "repl/export_internal.h"
#include "repl/init_state.h"
#include "repl/text_helpers.h"

/* File-scope C boilerplate: includes, macros, and the rotation globals.
 * Lines here appear in BOTH the code panel header AND the exported C
 * file's file-scope region (above display()). They never run at REPL
 * time - see g_display_header for the display() opening, and the
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
 * family (gamma/lgamma/tgamma) - all single- or two-letter math.h
 * functions that are plausible user variable names. The y0/y1 pair
 * was the original motivator; jn/yn/gamma/lgamma/tgamma are added
 * defensively after the audit in #7 of the bug investigation.
 *
 * The template contains every supported pair, but consumers filter it through
 * repl_export_header_pre_line_visible(), so ordinary scenes pay no preamble
 * cost and a scene declaring (say) y0 emits only the y0 pair. */
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
    "#include <stddef.h>",
    "#if defined(__APPLE__)",
    "#include <OpenGL/gl.h>",
    "#include <OpenGL/glu.h>",
    "#include <GLUT/glut.h>",
    "#else",
    "#define GL_GLEXT_PROTOTYPES",
    "#include <GL/gl.h>",
    "#include <GL/glext.h>",
    "#include <GL/glu.h>",
    "#include <GL/freeglut.h>",
    "#endif",
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

typedef struct {
    const char *name;
    const char *define_line;
    const char *undef_line;
    unsigned    bit;
} MathCollisionWrapper;

static const MathCollisionWrapper g_math_collision_wrappers[] = {
    { "j0",     "#define j0 _j0",         "#undef j0",     1u << 0 },
    { "j1",     "#define j1 _j1",         "#undef j1",     1u << 1 },
    { "jn",     "#define jn _jn",         "#undef jn",     1u << 2 },
    { "y0",     "#define y0 _y0",         "#undef y0",     1u << 3 },
    { "y1",     "#define y1 _y1",         "#undef y1",     1u << 4 },
    { "yn",     "#define yn _yn",         "#undef yn",     1u << 5 },
    { "gamma",  "#define gamma _gamma",   "#undef gamma",  1u << 6 },
    { "lgamma", "#define lgamma _lgamma", "#undef lgamma", 1u << 7 },
    { "tgamma", "#define tgamma _tgamma", "#undef tgamma", 1u << 8 }
};

unsigned repl_export_math_collision_mask(void) {
    unsigned mask = 0;

    for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
        const char *name = g_predef_vars[var_idx].name;
        for (size_t wrapper_idx = 0;
             wrapper_idx < sizeof(g_math_collision_wrappers) /
                           sizeof(g_math_collision_wrappers[0]);
             wrapper_idx++) {
            if (strcmp(name, g_math_collision_wrappers[wrapper_idx].name) == 0) {
                mask |= g_math_collision_wrappers[wrapper_idx].bit;
                break;
            }
        }
    }
    return mask;
}

int repl_export_header_pre_line_visible(int line_idx,
                                        unsigned collision_mask) {
    const char *line;

    if (line_idx < 0 || !(line = g_header_pre[line_idx]))
        return 0;
    for (size_t wrapper_idx = 0;
         wrapper_idx < sizeof(g_math_collision_wrappers) /
                       sizeof(g_math_collision_wrappers[0]);
         wrapper_idx++) {
        const MathCollisionWrapper *wrapper =
            &g_math_collision_wrappers[wrapper_idx];
        if (strcmp(line, wrapper->define_line) == 0 ||
            strcmp(line, wrapper->undef_line) == 0)
            return (collision_mask & wrapper->bit) != 0;
    }
    return 1;
}

/* display() opening: shared by the code panel header and by
 * emit_export_display_begin. Keeping these as literal strings in one
 * array (rather than hardcoded fprintf calls in the exporter) is what
 * keeps panel and export byte-identical for the opening lines. */
const char *g_display_header[] = {
    REPL_EXPORT_DISPLAY_OPEN_LINE,
    "  glLoadIdentity();",
    "  glPushAttrib(GL_ALL_ATTRIB_BITS);",
    NULL
};

/* ========================================================================= */
/* Workspace header directive table - writer half.                            */
/*                                                                            */
/* Each entry pairs a directive name with its emit step                       */
/* (append zero or more lines into g_workspace_header_lines).                 */
/* Order in this table determines emit order. The matching reader-side       */
/* table lives in src/repl/import.c; the two are intentionally               */
/* independent so neither TU has to forward-declare into the other.          */
/* ========================================================================= */

typedef void (*WorkspaceEmitFn)(int *n);

typedef struct {
    const char       *name;      /* directive name without leading `@` */
    WorkspaceEmitFn   emit;      /* append zero or more lines, bumping *n */
} WorkspaceDirective;


/* Resolve slot `slot`'s dimensional light data through the bridge, or zero
 * it when no bridge is installed. */
static void export_light_info(int slot, ReplExportLightInfo *out) {
    const ReplExportLightBridge *bridge = repl_export_light_bridge();

    memset(out, 0, sizeof(*out));
    if (bridge && bridge->fill_slot)
        bridge->fill_slot(slot, out);
}

/* True when the slot's POSITION line must be emitted before the camera
 * transforms (at identity modelview) rather than after them. The flag is set
 * by the scene module's theme presets and carried across the light bridge;
 * the exporter just reads it, which keeps this TU clean of scene includes
 * (check-controller-boundaries forbids them in src/repl/). */
static int export_slot_pos_is_eye_space(int slot) {
    ReplExportLightInfo info;
    export_light_info(slot, &info);
    return info.pos_is_eye_space;
}

/* --- workspace-dir --------------------------------------------------------- */

static void emit_workspace_dir(int *n) {
    const char *workspace_dir = repl_workspace_dir();

    if (workspace_dir[0] && *n < MAX_WORKSPACE_HEADER_LINES) {
        snprintf(g_workspace_header_lines_writable[(*n)++], WORKSPACE_HEADER_LINE_LEN,
                "/* @workspace-dir %s */", workspace_dir);
    }
}

/* --- scene-name ------------------------------------------------------------ */

static void emit_scene_name(int *n) {
    /* Explicit export hint overrides the active-slot name. */
    const char *scene_name = g_export_scene_name_hint;
    if ((!scene_name || !*scene_name) && repl_active_user_scene() >= 0)
        scene_name = repl_user_scene_name(repl_active_user_scene());
    if (scene_name && *scene_name && *n < MAX_WORKSPACE_HEADER_LINES) {
        snprintf(g_workspace_header_lines_writable[(*n)++], WORKSPACE_HEADER_LINE_LEN,
                 "/* @scene-name %s */", scene_name);
    }
}


/* --- func aliases ----------------------------------------------------------
 * Round-trip the func-alias table through the workspace header so a saved
 * `drawCube` definition reloads into the same slot. Exported as a
 * block-comment @func directive carrying the slot number and alias.
 * Slots without an alias don't emit a line. */

static void emit_func_aliases(int *n) {
    for (int slot = 0;
         slot < REPL_FUNC_SLOT_COUNT && *n < MAX_WORKSPACE_HEADER_LINES;
         slot++) {
        const char *alias = repl_func_alias_get(slot);
        if (!alias) continue;
        if (repl_format_fits(g_workspace_header_lines_writable[*n],
                             WORKSPACE_HEADER_LINE_LEN,
                             "/* @func %d = %s */", slot, alias))
            (*n)++;
    }
}

/* --- cfg ------------------------------------------------------------------- */

static void emit_cfgs(int *n) {
    /* The bridge populates the bag with (slug, value) pairs. If no
     * bridge is installed (the demo case), the bag stays empty and
     * no @cfg lines are emitted. */
    if (!g_export_cfg_bridge || !g_export_cfg_bridge->fill_all)
        return;
    ReplConfigBag cfg;
    repl_config_bag_clear(&cfg);
    g_export_cfg_bridge->fill_all(&cfg);
    int i = 0;
    for (; i < cfg.count && *n < MAX_WORKSPACE_HEADER_LINES; i++) {
        snprintf(g_workspace_header_lines_writable[(*n)++], WORKSPACE_HEADER_LINE_LEN,
                 "/* @cfg %s = %s */", cfg.items[i].key, cfg.items[i].value);
    }
    /* The header budget is sized (export_state.h) so this never trips,
     * but warn loudly rather than silently dropping presentation state
     * if a future cfg/var/func growth ever overflows it. */
    if (i < cfg.count) {
        fprintf(stderr,
                "repl_export: workspace header full (%d lines) - dropped %d "
                "@cfg line(s) starting at '%s'; those scene settings will "
                "not round-trip. Raise MAX_WORKSPACE_HEADER_LINES.\n",
                MAX_WORKSPACE_HEADER_LINES, cfg.count - i, cfg.items[i].key);
    }
}

/* --- directive table (writer half) ----------------------------------------- */

#define WS_DIR(name, emit_fn) { name, emit_fn }

static const WorkspaceDirective WORKSPACE_DIRECTIVES[] = {
    /* Emit order matches this array. */
    WS_DIR(REPL_WORKSPACE_DIRECTIVE_SCENE_NAME,    emit_scene_name),
    WS_DIR(REPL_WORKSPACE_DIRECTIVE_WORKSPACE_DIR, emit_workspace_dir),
    WS_DIR(REPL_WORKSPACE_DIRECTIVE_FUNC,          emit_func_aliases),
    WS_DIR(REPL_WORKSPACE_DIRECTIVE_CFG,           emit_cfgs),
};
#define WORKSPACE_DIRECTIVE_COUNT \
    ((int)(sizeof(WORKSPACE_DIRECTIVES) / sizeof(WORKSPACE_DIRECTIVES[0])))

#undef WS_DIR

/* Pin the shared header budget against the worst case so adding cfg
 * items / func slots can't silently reintroduce the
 * @cfg truncation. Worst case = 1 banner + 1 @scene-name + 1
 * @workspace-dir + every @func + every @cfg (capped by the
 * bag's own REPL_CFG_MAX_ITEMS). See export_state.h. */
STATIC_ASSERT(MAX_WORKSPACE_HEADER_LINES >=
                  3 + REPL_FUNC_SLOT_COUNT + REPL_CFG_MAX_ITEMS,
              "workspace header budget too small for worst-case directives");

void repl_state_refresh_workspace_header_lines(void) {
    int line_count = 0;
    if (line_count < MAX_WORKSPACE_HEADER_LINES) {
        snprintf(g_workspace_header_lines_writable[line_count++], WORKSPACE_HEADER_LINE_LEN,
                 REPL_WORKSPACE_HEADER_BANNER);
    }
    for (int dir_idx = 0; dir_idx < WORKSPACE_DIRECTIVE_COUNT; dir_idx++)
        WORKSPACE_DIRECTIVES[dir_idx].emit(&line_count);
    g_workspace_header_line_count_writable = line_count;
}

const char *g_header_post[] = {
    NULL
};

typedef struct {
    const char *repl_line;
    /* Slug name of the cfg toggle that gates this line, or NULL if
     * unconditional. Looked up via the export config bridge -
     * src/repl/export.c does not call glr_config_get directly. */
    const char *toggle_slug;
} InitBootstrapEntry;

/* Bootstrap commands are REPL lines that run once at startup through
 * repl_apply_state_cmd. Each frame's glPushAttrib/glPopAttrib bracket then
 * preserves this state across user commands. Comment entries (// ...)
 * parse as CMD_COMMENT - repl_apply_state_cmd no-ops them, but the editor
 * and exporter render them as section headers in the init() body. */
/* Tiny constant term in the point-attenuation default. Only caps the
 * otherwise-unbounded near-field point blow-up as eye distance d -> 0
 * (the software fallback dodges this by keying off the global orbit
 * distance, which never reaches 0). Negligible past d ~ 1. */
#define POINT_ATTEN_NEAR_CAP 0.01f

/* The point-attenuation bootstrap line, formatted once at startup from
 * REPL_POINT_SIZE_REF_DIST (executor.h) so the hardware default and the
 * software fallback share one reference distance and can't drift. The
 * quadratic coefficient is 1/REF_DIST^2. Filled by parse_init_bootstrap
 * before the table is read; g_init_bootstrap_repl points its entry here. */
static char g_point_atten_bootstrap_line[96];

static const InitBootstrapEntry g_init_bootstrap_repl[] = {
    { "// Background color used by glClear at the start of every frame.", NULL },
    { "glClearColor(0.10, 0.10, 0.10, 1.0);", NULL },
    { "// Blending: standard src-over for translucent geometry.", NULL },
    { "glEnable(GL_BLEND);", NULL },
    { "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);", NULL },
        { "// Point attenuation: points hold a constant world-space footprint,", REPL_EXPORT_CFG_SLUG_POINT_ATTENUATION },
        { "// shrinking as 1/distance like everything else under perspective.", REPL_EXPORT_CFG_SLUG_POINT_ATTENUATION },
    /* Near-pure-quadratic attenuation: derived_size = size/sqrt(a + c*d^2)
     * with d the per-vertex eye distance. The quadratic term c = 1/REF_DIST^2
     * dominates at any normal viewing distance, so size scales as ~1/d ->
     * constant on-screen footprint, matching the software fallback
     * size*REF_DIST/cam_dist. Text formatted at startup from the shared
     * REPL_POINT_SIZE_REF_DIST (see g_point_atten_bootstrap_line). */
    { g_point_atten_bootstrap_line, REPL_EXPORT_CFG_SLUG_POINT_ATTENUATION },
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

/* True when a bootstrap entry must be skipped *entirely* - not applied,
 * not emitted, not counted - because the runtime GL context lacks the
 * entry point it depends on. Only the point-attenuation entry has such
 * a dependency (glPointParameterfv). This is deliberately independent
 * of and ahead of the toggle-slug disable path in the loops below:
 * that path only *neutralizes* attenuation by re-applying a
 * no-falloff CMD_POINT_PARAMETER_FV, which would still invoke the
 * missing glPointParameterfv. When unsupported we must not touch it
 * at all. */
static int init_bootstrap_entry_unsupported(const InitBootstrapEntry *entry) {
    return entry->toggle_slug
    && strcmp(entry->toggle_slug, REPL_EXPORT_CFG_SLUG_POINT_ATTENUATION) == 0
        && !repl_executor_point_parameter_supported();
}
#define NUM_INIT_BOOTSTRAP \
    ((int)(sizeof(g_init_bootstrap_repl) / sizeof(g_init_bootstrap_repl[0])))

static ReplParsedLine g_init_bootstrap_cmds[NUM_INIT_BOOTSTRAP];
static int            g_init_bootstrap_ready = 0;

static const char *g_init_host_only_visible_c[] = {
    "  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, repl_glfloat4(0.15f, 0.15f, 0.20f, 1.0f));",
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
    "void keyboard(unsigned char key, int mouse_x, int mouse_y) {",
    "  (void)mouse_x;",
    "  (void)mouse_y;",
    "  if (key == ' ') g_rotating = !g_rotating;",
    "  if (key == 27) exit(0);",
    "}",
    "",
    "static double timer_now_ms(void) {",
    "  return (double)glutGet(GLUT_ELAPSED_TIME);",
    "}",
    "",
    "static int timer_delay_ms(double *next_deadline_ms) {",
    "  double now = timer_now_ms();",
    "  int delay;",
    "",
    "  if (*next_deadline_ms == 0.0)",
    "    *next_deadline_ms = now;",
    "  *next_deadline_ms += 1000.0 / 60.0;",
    "  if (*next_deadline_ms < now)",
    "    *next_deadline_ms = now;",
    "",
    "  delay = (int)(*next_deadline_ms - now + 0.5);",
    "  if (delay < 1)",
    "    delay = 1;",
    "  return delay;",
    "}",
    "",
    "void tick(int v) {",
    "  static double next_deadline_ms = 0.0;",
    "  int delay;",
    "",
    "  (void)v;",
    "  /* Fixed-step time advance, matching the live REPL's",
    "   * repl_state_time_advance(0.01667) timer. Keeps tDelta = (t -",
    "   * tLast) * 10 constant across frames, independent of how long",
    "   * each render actually takes. */",
    "  t += 0.01667f;",
    "  if (g_rotating) g_angle += 0.5f;",
    "  glutPostRedisplay();",
    "  delay = timer_delay_ms(&next_deadline_ms);",
    "  glutTimerFunc((unsigned int)delay, tick, 0);",
    "}",
    "",
    "void init(void) {",
    "  glLineWidth(1.5f);",
    NULL
};

const char *g_footer_post_init[] = {
    "}",
    "",
    "int main(int argc, char **argv) {",
    "  glutInit(&argc, argv);",
    "  glutInitDisplayMode(GLUT_DOUBLE|GLUT_RGB|GLUT_DEPTH|GLUT_STENCIL|GLUT_MULTISAMPLE);",
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

void emit_footer_post_init(FILE *f, int win_w, int win_h,
                           const char *tick_name) {
    if (!tick_name || !tick_name[0])
        tick_name = "tick";
    fprintf(f,
        "}\n"
        "\n"
        "int main(int argc, char **argv) {\n"
        "  glutInit(&argc, argv);\n"
        "  glutInitDisplayMode(GLUT_DOUBLE|GLUT_RGB|GLUT_DEPTH|GLUT_STENCIL|GLUT_MULTISAMPLE);\n"
        "  glutInitWindowSize(%d, %d);\n"
        "  glutCreateWindow(\"OpenGL REPL\");\n"
        "  init();\n"
        "  glutDisplayFunc(display);\n"
        "  glutReshapeFunc(reshape);\n"
        "  glutKeyboardFunc(keyboard);\n"
        "  glutTimerFunc(16, %s, 0);\n"
        "  glutMainLoop();\n"
        "  return 0;\n"
        "}\n",
        win_w, win_h, tick_name);
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

    /* Format the point-attenuation line from the shared reference distance
     * so it can't drift from the software fallback (executor.c). The
     * quadratic coefficient is 1/REF_DIST^2. Done before the parse loop
     * since g_init_bootstrap_repl points an entry at this buffer. */
    snprintf(g_point_atten_bootstrap_line, sizeof(g_point_atten_bootstrap_line),
             "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, %g, 0.0, %g);",
             (double)POINT_ATTEN_NEAR_CAP,
             (double)(1.0f / (REPL_POINT_SIZE_REF_DIST * REPL_POINT_SIZE_REF_DIST)));

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
                    bootstrap_err[0] ? " - " : "",
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

/* Resolve one bootstrap table entry to the command init() actually applies.
 * Returns 0 for comments, disabled non-state rows, and unsupported commands. */
static int init_bootstrap_effective_state_command(int bootstrap_idx,
                                                   GLCmd *out_cmd) {
    const InitBootstrapEntry *entry;
    GLCmd cmd;

    if (bootstrap_idx < 0 || bootstrap_idx >= NUM_INIT_BOOTSTRAP || !out_cmd)
        return 0;
    entry = &g_init_bootstrap_repl[bootstrap_idx];
    if (init_bootstrap_entry_unsupported(entry))
        return 0;

    cmd = g_init_bootstrap_cmds[bootstrap_idx].cmd;
    if (cmd.type == CMD_COMMENT || cmd.type == CMD_EMPTY)
        return 0;
    if (entry->toggle_slug && !init_bootstrap_toggle_get(entry->toggle_slug, 1)) {
        if (cmd.type != CMD_POINT_PARAMETER_FV ||
            (GLenum)cmd.args[0] != GL_POINT_DISTANCE_ATTENUATION)
            return 0;
        /* args[0]=pname; args[1..3]=const/linear/quadratic. Neutralize
         * attenuation to a constant 1 (no falloff), exactly as init() does. */
        cmd.args[1] = 1.0f;
        cmd.args[2] = 0.0f;
        cmd.args[3] = 0.0f;
    }
    *out_cmd = cmd;
    return 1;
}

int repl_init_bootstrap_state_command_count(void) {
    int count = 0;
    int bootstrap_idx;
    GLCmd cmd;

    repl_ensure_init_bootstrap_ready();
    for (bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP;
         bootstrap_idx++)
        if (init_bootstrap_effective_state_command(bootstrap_idx, &cmd))
            count++;
    return count;
}

int repl_init_bootstrap_state_command_at(int state_command_idx,
                                         GLCmd *out_cmd) {
    int current_idx = 0;
    int bootstrap_idx;
    GLCmd cmd;

    if (state_command_idx < 0 || !out_cmd)
        return 0;
    repl_ensure_init_bootstrap_ready();
    for (bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP;
         bootstrap_idx++) {
        if (!init_bootstrap_effective_state_command(bootstrap_idx, &cmd))
            continue;
        if (current_idx++ == state_command_idx) {
            *out_cmd = cmd;
            return 1;
        }
    }
    return 0;
}

void repl_apply_init_bootstrap(void) {
    int count = repl_init_bootstrap_state_command_count();
    int i;
    GLCmd cmd;

    for (i = 0; i < count; i++)
        if (repl_init_bootstrap_state_command_at(i, &cmd))
            repl_apply_state_cmd(&cmd, 1.0f);
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

/* Idea A clear-color hoist: scan the document for the LAST CMD_CLEAR_COLOR
 * and return its (literal, commit-time) args. The exported per-frame glClear
 * runs before render_repl_geometry() and inside the glPushAttrib(GL_ALL)
 * bracket - so a glClearColor in the body can never drive the visible clear
 * (it runs after the clear and is reverted by glPopAttrib). Mirroring the
 * controller's "last clear color wins" pre-scan (src/app/glr_ctrl.c), we
 * emit that value as init()'s glClearColor instead of the bootstrap default,
 * making it the pushed/restored baseline the clear actually uses. Constant
 * colors only (the common case); an animated/expression clear color is
 * frozen at its current value here. Returns 1 + fills out[4], else 0. */
static int export_document_last_clear_color(float out[4]) {
    int found = 0;
    int count = repl_state_document_count();
    const GLCmd *cmds = repl_state_document_cmds();
    for (int i = 0; i < count; i++) {
        if (!cmds[i].valid || cmds[i].type != CMD_CLEAR_COLOR)
            continue;
        for (int k = 0; k < 4; k++)
            out[k] = cmds[i].args[k];
        found = 1;
    }
    return found;
}

void emit_export_init_section_to_file(FILE *f, int include_tess) {
    char line[MAX_LINE_LEN];

    for (int line_idx = 0; g_init_host_only_visible_c[line_idx]; line_idx++)
        export_write_c89_line(f, g_init_host_only_visible_c[line_idx]);
    if (include_tess)
        for (int line_idx = 0; g_init_host_only_tess_c[line_idx]; line_idx++)
            export_write_c89_line(f, g_init_host_only_tess_c[line_idx]);

    int n_lights = repl_export_lights_init_line_count();
    for (int lights_idx = 0; lights_idx < n_lights; lights_idx++) {
        repl_export_lights_init_line(lights_idx, line, sizeof(line));
        export_write_c89_line(f, line);
    }

    repl_ensure_init_bootstrap_ready();
    for (int bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP; bootstrap_idx++) {
        const InitBootstrapEntry *entry = &g_init_bootstrap_repl[bootstrap_idx];
        if (init_bootstrap_entry_unsupported(entry))
            continue;
        if (entry->toggle_slug && !init_bootstrap_toggle_get(entry->toggle_slug, 1))
            continue;
        /* Hoist a body clear color into the init() default (Idea A). */
        if (g_init_bootstrap_cmds[bootstrap_idx].cmd.type == CMD_CLEAR_COLOR) {
            float cc[4];
            if (export_document_last_clear_color(cc)) {
                char s[4][EXPORT_FLOAT_TEXT_MAX];
                for (int k = 0; k < 4; k++)
                    repl_format_source_float(s[k], sizeof(s[k]), cc[k]);
                fprintf(f, "  glClearColor(%s, %s, %s, %s);\n",
                        s[0], s[1], s[2], s[3]);
                continue;
            }
        }
        if (g_init_bootstrap_cmds[bootstrap_idx].cmd.type == CMD_MATERIALFV &&
            write_materialfv_as_c89(f, g_init_bootstrap_cmds[bootstrap_idx].text))
            continue;
        if (g_init_bootstrap_cmds[bootstrap_idx].cmd.type == CMD_POINT_PARAMETER_FV &&
            write_point_parameterfv_as_c89(f, g_init_bootstrap_cmds[bootstrap_idx].text))
            continue;
        format_cmd_source_as_c(line, sizeof(line),
                               g_init_bootstrap_cmds[bootstrap_idx].text,
                               0);
        export_write_c89_line(f, line);
    }
}

void emit_export_header_pre(FILE *f, const ExportNeeds *needs) {
    /* The label() and @tune helpers need <stdarg.h>/<stdio.h>. Emit them
     * here, grouped with the other system includes, rather than mid-file
     * at each helper's definition - a stray `#include` below file-scope
     * code reads as a sanitization bug even though it compiles. */
    int needs_stdio = needs && (needs->needs_label || needs->tune_count > 0);

    /* Ask the camera bridge for the g_angle preamble line.
     * Without a bridge installed (the demo case) we emit the
     * placeholder unchanged so the file is still valid C. */
    char angle_line[REPL_EXPORT_CAMERA_PREAMBLE_MAX];
    const ReplExportCameraBridge *camera_bridge = repl_export_camera_bridge();
    angle_line[0] = '\0';
    if (camera_bridge && camera_bridge->fill_save_preamble)
        camera_bridge->fill_save_preamble(angle_line, (int)sizeof(angle_line));

    /* NOTE: resolving a dynamic boilerplate line at the consumer site
     * (as here) is safe ONLY because this one consumer is the file
     * writer - a single pass off the frame loop. Do NOT copy this shape
     * for a line the code panel reads: panel row-count and render
     * straddle render3d_draw_scene() and would diverge. Resolve once
     * into UiRenderSnapshot instead. See docs/ARCHITECTURE.md, "Rule - where
     * a per-frame dynamic value is resolved". */
    unsigned collision_mask = repl_export_math_collision_mask();
    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++) {
        if (!repl_export_header_pre_line_visible(line_idx, collision_mask))
            continue;
        if (strcmp(g_header_pre[line_idx], "static float g_angle = 0.0f;") == 0) {
            if (angle_line[0])
                export_write_c89_line(f, angle_line);
            else
                export_write_c89_line(f, g_header_pre[line_idx]);
            continue;
        }
        export_write_c89_line(f, g_header_pre[line_idx]);
        if (needs_stdio &&
            strcmp(g_header_pre[line_idx], "#include <stdlib.h>") == 0) {
            export_write_c89_line(f, "#include <stdarg.h>");
            export_write_c89_line(f, "#include <stdio.h>");
        }
    }
}

void emit_export_cam_lines(FILE *f) {
    /* The bridge owns the camera-line format. Without a
     * bridge (demo case) the // camera block is omitted from the
     * exported file - that's fine, the demo doesn't export
     * (implemented in step 4a of the decouple plan). */
    const ReplExportCameraBridge *camera_bridge = repl_export_camera_bridge();
    if (!camera_bridge || !camera_bridge->fill_save_block)
        return;
    ReplExportCameraBlock block;
    memset(&block, 0, sizeof(block));
    camera_bridge->fill_save_block(&block);
    if (!block.present) return;
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++) {
        if (block.lines[i][0])
            export_write_c89_line(f, block.lines[i]);
    }
}

void repl_refresh_render_state_strings(void) {
    /* Render-config toggles moved to glr_state. Read them
     * via the bridge's slug-keyed get_int - same opaque path the rest
     * of the export pipeline uses for cfg state. The demo doesn't
     * install a bridge, so the toggles fall back to "Enable" /
     * "Disable" defaults below; the demo never exports anyway
     * (implemented in step 7a). */
    int msaa_on = repl_cfg_get_int(REPL_EXPORT_CFG_SLUG_MSAA, 1);
    int line_smooth_on = repl_cfg_get_int(REPL_EXPORT_CFG_SLUG_LINE_SMOOTH, 0);
    snprintf(g_render_state_lines_writable[0], sizeof(g_render_state_lines_writable[0]),
             "  gl%s(GL_MULTISAMPLE);",
             msaa_on ? "Enable" : "Disable");
    snprintf(g_render_state_lines_writable[1], sizeof(g_render_state_lines_writable[1]),
             "  gl%s(GL_LINE_SMOOTH);",
             line_smooth_on ? "Enable" : "Disable");
}

/* The camera-block parser state machine moved to the bridge
 * implementation (glr_camera_export.c). src/repl/import.c delegates
 * import-side line consumption and reset to the bridge (implemented in
 * step 4a). */

void repl_refresh_camera_lines(void) {
    /* Bridge-driven preview: the bridge formats the 4-line block from
     * current camera state with numeric ry (no g_angle placeholder).
     * Without a bridge installed (the demo case), g_cam_lines stays
     * empty - the demo doesn't render a code panel. */
    const ReplExportCameraBridge *camera_bridge = repl_export_camera_bridge();
    if (!camera_bridge || !camera_bridge->fill_display_block) {
        for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++)
            g_cam_lines_writable[i][0] = '\0';
        return;
    }
    ReplExportCameraBlock block;
    memset(&block, 0, sizeof(block));
    camera_bridge->fill_display_block(&block);
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++) {
        snprintf(g_cam_lines_writable[i], sizeof(g_cam_lines_writable[i]), "%s", block.lines[i]);
    }
}

/* Light-text generators. Per the "fewer surprises" principle, every GL
 * state change should be visible to the editor; the lights are the
 * largest hidden block. Init lines (colors + baseline disable) belong in
 * init() because they don't depend on the modelview. Position lines belong
 * in display(): eye-space slots before the camera transforms, world-space
 * slots after them. glLightfv(GL_POSITION) snapshots the active modelview.
 *
 * The same text appears in the editor's code panel and the exported C.
 * Exported C uses repl_glfloatN(...) helpers instead of compound literals;
 * values go through export_format_float_list (repl_format_source_float) so
 * they keep float32 round-trip safety while reading as the shortest exact
 * form (0.8f -> "0.8", not the "0.800000012" a raw %.9g would emit). */

static const char *const k_light_names[REPL_LIGHT_SLOT_COUNT] = {
    "GL_LIGHT0", "GL_LIGHT1", "GL_LIGHT2", "GL_LIGHT3"
};

#define LIGHT_INIT_LINES_PER_LIGHT 4  /* DIFFUSE, AMBIENT, SPECULAR, glDisable */

/* Section-header comment lines emitted above the generated GL calls.
 * Pure text - the editor renders them as dimmed init/header lines and
 * the export writes them verbatim as C comments. */
static const char *const k_lights_init_header[] = {
    "  // Per-light colors. Positions are set per frame in display() because",
    "  // glLightfv(GL_POSITION) snapshots the active modelview matrix.",
};
#define LIGHTS_INIT_HEADER_LINES \
    ((int)(sizeof(k_lights_init_header) / sizeof(k_lights_init_header[0])))

static const char *const k_lights_pre_camera_header[] = {
    "  // Eye-space light positions, set before the camera transform.",
};
#define LIGHTS_PRE_CAMERA_HEADER_LINES \
    ((int)(sizeof(k_lights_pre_camera_header) / \
           sizeof(k_lights_pre_camera_header[0])))

static const char *const k_lights_display_header[] = {
    "  // Light positions, set after the camera so they stay anchored in world space.",
};
#define LIGHTS_DISPLAY_HEADER_LINES \
    ((int)(sizeof(k_lights_display_header) / sizeof(k_lights_display_header[0])))

/* Emit slot `slot`'s init line into buf: DIFFUSE / AMBIENT / SPECULAR /
 * glDisable. All POSITION writes are visible in display(). */
static void lights_init_emit_slot_line(int slot, int sub, char *buf, size_t n) {
    ReplExportLightInfo l;
    export_light_info(slot, &l);
    const char *ln = k_light_names[slot];
    char body[EXPORT_FLOAT_LIST_MAX];
    switch (sub) {
    case 0:
        export_format_float_list(body, sizeof(body), l.diffuse, 4);
        snprintf(buf, n,
                 "  glLightfv(%s, GL_DIFFUSE,  %s(%s));",
                 ln, REPL_EXPORT_GLFLOAT4_HELPER, body);
        return;
    case 1:
        export_format_float_list(body, sizeof(body), l.ambient, 4);
        snprintf(buf, n,
                 "  glLightfv(%s, GL_AMBIENT,  %s(%s));",
                 ln, REPL_EXPORT_GLFLOAT4_HELPER, body);
        return;
    case 2:
        export_format_float_list(body, sizeof(body), l.specular, 4);
        snprintf(buf, n,
                 "  glLightfv(%s, GL_SPECULAR, %s(%s));",
                 ln, REPL_EXPORT_GLFLOAT4_HELPER, body);
        return;
    case 3:
        snprintf(buf, n, "  glDisable(%s);", ln);
        return;
    }
}

int repl_export_lights_init_line_count(void) {
    int n = LIGHTS_INIT_HEADER_LINES;
    for (int s = 0; s < REPL_LIGHT_SLOT_COUNT; s++)
        n += LIGHT_INIT_LINES_PER_LIGHT;
    return n;
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
    for (int slot = 0; slot < REPL_LIGHT_SLOT_COUNT; slot++) {
        if (i < LIGHT_INIT_LINES_PER_LIGHT) {
            lights_init_emit_slot_line(slot, i, buf, n);
            return;
        }
        i -= LIGHT_INIT_LINES_PER_LIGHT;
    }
    buf[0] = '\0';  /* unreachable: index bounds already enforced */
}

static int lights_position_slot_count(int eye_space) {
    int count = 0;
    for (int slot = 0; slot < REPL_LIGHT_SLOT_COUNT; slot++)
        if (!!export_slot_pos_is_eye_space(slot) == !!eye_space)
            count++;
    return count;
}

static void lights_position_line(int i, int eye_space,
                                 char *buf, size_t n) {
    for (int slot = 0; slot < REPL_LIGHT_SLOT_COUNT; slot++) {
        if (!!export_slot_pos_is_eye_space(slot) != !!eye_space)
            continue;
        if (i == 0) {
            ReplExportLightInfo l;
            const char *ln = k_light_names[slot];
            char body[EXPORT_FLOAT_LIST_MAX];
            export_light_info(slot, &l);
            export_format_float_list(body, sizeof(body), l.pos, 4);
            snprintf(buf, n,
                     "  glLightfv(%s, GL_POSITION, %s(%s));",
                     ln, REPL_EXPORT_GLFLOAT4_HELPER, body);
            return;
        }
        i--;
    }
    buf[0] = '\0';
}

int repl_export_lights_pre_camera_line_count(void) {
    int positions = lights_position_slot_count(1);
    return positions ? LIGHTS_PRE_CAMERA_HEADER_LINES + positions : 0;
}

void repl_export_lights_pre_camera_line(int i, char *buf, size_t n) {
    int count = repl_export_lights_pre_camera_line_count();
    if (!buf || n == 0) return;
    if (i < 0 || i >= count) {
        buf[0] = '\0';
        return;
    }
    if (i < LIGHTS_PRE_CAMERA_HEADER_LINES) {
        snprintf(buf, n, "%s", k_lights_pre_camera_header[i]);
        return;
    }
    lights_position_line(i - LIGHTS_PRE_CAMERA_HEADER_LINES, 1, buf, n);
}

int repl_export_lights_display_line_count(void) {
    return LIGHTS_DISPLAY_HEADER_LINES + lights_position_slot_count(0);
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
    lights_position_line(i - LIGHTS_DISPLAY_HEADER_LINES, 0, buf, n);
}

/* ------------------------------------------------------------------------- */
/* Read-only generated GL-state program                                      */

#define GENERATED_INIT_STATE_MAX \
    (2 + REPL_LIGHT_SLOT_COUNT * LIGHT_INIT_LINES_PER_LIGHT + \
     NUM_INIT_BOOTSTRAP)
#define GENERATED_DISPLAY_STATE_MAX \
    (2 + RENDER_STATE_LINE_COUNT + REPL_LIGHT_SLOT_COUNT + \
     REPL_EXPORT_CAMERA_LINES)

static ReplGeneratedStateWrite generated_command_write(CmdType type) {
    ReplGeneratedStateWrite write;
    memset(&write, 0, sizeof(write));
    write.kind = REPL_GENERATED_STATE_COMMAND;
    write.command.type = type;
    write.command.valid = 1;
    write.command.src_cmd_idx = -1;
    write.command.call_src_cmd_idx = -1;
    write.command.root_call_src_cmd_idx = -1;
    return write;
}

static ReplGeneratedStateWrite generated_vector_write(
    ReplGeneratedStateWriteKind kind, GLenum object, GLenum pname,
    const float *value, int value_count) {
    ReplGeneratedStateWrite write;
    int i;

    memset(&write, 0, sizeof(write));
    write.kind = kind;
    write.object = object;
    write.pname = pname;
    write.value_count = value_count;
    for (i = 0; i < value_count && i < 4; i++)
        write.value[i] = value[i];
    return write;
}

static int generated_parse_command_write(const char *line,
                                         ReplGeneratedStateWrite *out) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplParseContext ctx;
    ReplParsedLine parsed;

    if (!line || !line[0] || !out)
        return 0;
    memset(&ctx, 0, sizeof(ctx));
    err[0] = '\0';
    ctx.source_line_idx = -1;
    ctx.err_buf = err;
    ctx.err_sz = (int)sizeof(err);
    if (!repl_parser_parse_command_ctx(line, &parsed, &ctx)) {
        fprintf(stderr, "generated state parse failed: %s%s%s\n", line,
                err[0] ? " - " : "", err);
        return 0;
    }
    *out = generated_command_write(parsed.cmd.type);
    out->command = parsed.cmd;
    out->command.valid = 1;
    out->command.src_cmd_idx = -1;
    out->command.call_src_cmd_idx = -1;
    out->command.root_call_src_cmd_idx = -1;
    return 1;
}

static int generated_init_state_writes(
    ReplGeneratedStateWrite out[GENERATED_INIT_STATE_MAX]) {
    static const float model_ambient[4] = { 0.15f, 0.15f, 0.20f, 1.0f };
    int count = 0;
    int slot;
    int bootstrap_idx;

    out[count] = generated_command_write(CMD_LINE_WIDTH);
    out[count].command.args[0] = 1.5f;
    out[count].command.num_args = 1;
    count++;

    out[count++] = generated_vector_write(
        REPL_GENERATED_STATE_LIGHT_MODEL_FV, 0,
        GL_LIGHT_MODEL_AMBIENT, model_ambient, 4);

    for (slot = 0; slot < REPL_LIGHT_SLOT_COUNT; slot++) {
        ReplExportLightInfo light;
        GLenum object = (GLenum)(GL_LIGHT0 + slot);
        ReplGeneratedStateWrite disable;

        export_light_info(slot, &light);
        out[count++] = generated_vector_write(
            REPL_GENERATED_STATE_LIGHT_FV, object,
            GL_DIFFUSE, light.diffuse, 4);
        out[count++] = generated_vector_write(
            REPL_GENERATED_STATE_LIGHT_FV, object,
            GL_AMBIENT, light.ambient, 4);
        out[count++] = generated_vector_write(
            REPL_GENERATED_STATE_LIGHT_FV, object,
            GL_SPECULAR, light.specular, 4);
        disable = generated_command_write(CMD_DISABLE);
        disable.command.args[0] = (float)object;
        disable.command.num_args = 1;
        out[count++] = disable;
    }

    for (bootstrap_idx = 0; bootstrap_idx < NUM_INIT_BOOTSTRAP;
         bootstrap_idx++) {
        GLCmd cmd;
        if (init_bootstrap_effective_state_command(bootstrap_idx, &cmd)) {
            out[count] = generated_command_write(cmd.type);
            out[count].command = cmd;
            count++;
        }
    }
    return count;
}

static int generated_append_light_positions(
    ReplGeneratedStateWrite *out, int count, int eye_space) {
    int slot;
    for (slot = 0; slot < REPL_LIGHT_SLOT_COUNT; slot++) {
        ReplExportLightInfo light;
        if (!!export_slot_pos_is_eye_space(slot) != !!eye_space)
            continue;
        export_light_info(slot, &light);
        out[count++] = generated_vector_write(
            REPL_GENERATED_STATE_LIGHT_FV,
            (GLenum)(GL_LIGHT0 + slot), GL_POSITION, light.pos, 4);
    }
    return count;
}

static int generated_display_state_writes(
    ReplGeneratedStateWrite out[GENERATED_DISPLAY_STATE_MAX]) {
    const ReplExportCameraBridge *camera_bridge;
    ReplExportCameraBlock camera;
    ReplGeneratedStateWrite write;
    int count = 0;
    int i;

    out[count++] = generated_command_write(CMD_LOAD_IDENTITY);
    memset(&out[count], 0, sizeof(out[count]));
    out[count++].kind = REPL_GENERATED_STATE_PUSH_ATTRIB;

    write = generated_command_write(
        repl_cfg_get_int(REPL_EXPORT_CFG_SLUG_MSAA, 1)
            ? CMD_ENABLE : CMD_DISABLE);
    write.command.args[0] = (float)GL_MULTISAMPLE;
    write.command.num_args = 1;
    out[count++] = write;

    write = generated_command_write(
        repl_cfg_get_int(REPL_EXPORT_CFG_SLUG_LINE_SMOOTH, 0)
            ? CMD_ENABLE : CMD_DISABLE);
    write.command.args[0] = (float)GL_LINE_SMOOTH;
    write.command.num_args = 1;
    out[count++] = write;

    count = generated_append_light_positions(out, count, 1);

    memset(&camera, 0, sizeof(camera));
    camera_bridge = repl_export_camera_bridge();
    if (camera_bridge && camera_bridge->fill_display_block) {
        camera_bridge->fill_display_block(&camera);
        for (i = 0; i < REPL_EXPORT_CAMERA_LINES; i++) {
            if (generated_parse_command_write(camera.lines[i], &write))
                out[count++] = write;
        }
    }

    count = generated_append_light_positions(out, count, 0);
    return count;
}

int repl_generated_init_state_write_count(void) {
    ReplGeneratedStateWrite writes[GENERATED_INIT_STATE_MAX];
    repl_ensure_init_bootstrap_ready();
    return generated_init_state_writes(writes);
}

int repl_generated_init_state_write_at(int state_write_idx,
                                       ReplGeneratedStateWrite *out_write) {
    ReplGeneratedStateWrite writes[GENERATED_INIT_STATE_MAX];
    int count;
    if (state_write_idx < 0 || !out_write)
        return 0;
    repl_ensure_init_bootstrap_ready();
    count = generated_init_state_writes(writes);
    if (state_write_idx >= count)
        return 0;
    *out_write = writes[state_write_idx];
    return 1;
}

int repl_generated_display_state_write_count(void) {
    ReplGeneratedStateWrite writes[GENERATED_DISPLAY_STATE_MAX];
    return generated_display_state_writes(writes);
}

int repl_generated_display_state_write_at(int state_write_idx,
                                          ReplGeneratedStateWrite *out_write) {
    ReplGeneratedStateWrite writes[GENERATED_DISPLAY_STATE_MAX];
    int count;
    if (state_write_idx < 0 || !out_write)
        return 0;
    count = generated_display_state_writes(writes);
    if (state_write_idx >= count)
        return 0;
    *out_write = writes[state_write_idx];
    return 1;
}
