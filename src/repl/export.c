#include "repl/export.h"
#include "c_compat.h"            /* STATIC_ASSERT — header-budget guard */
#include "source_document.h"     /* source_document_insert_line */
#include "repl/load.h"           /* repl_load_apply_line — step 5b */
/* glr_camera.h removed in step 4a: the export pipeline no longer
 * references glr_camera_*. Camera state flows through the
 * controller-installed ReplExportCameraBridge (see src/repl/export.h).
 * glr_config.h was already dropped in step 4 for the same reason. */
#include "config.h"             /* shared export/runtime constants */
#include "repl/command_store.h"
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/executor.h"        /* repl_apply_state_cmd */
#include "repl/parser.h"
#include "repl/pipeline.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"

#define IMPORT_EXPORT_VIEW     (repl_state_import_export())
#define IMPORT_EXPORT_WRITABLE (repl_state_import_export_writable())

#define g_workspace_header_lines      (IMPORT_EXPORT_VIEW.workspace_header_lines)
#define g_workspace_header_line_count (IMPORT_EXPORT_VIEW.workspace_header_line_count)
#define g_render_state_lines          (IMPORT_EXPORT_VIEW.render_state_lines)
#define g_cam_lines                   (IMPORT_EXPORT_VIEW.cam_lines)
#define g_export_scene_name_hint      (IMPORT_EXPORT_VIEW.export_scene_name_hint)
#define g_pending_scene_name          (IMPORT_EXPORT_VIEW.pending_scene_name)
#define g_pending_workspace_dir       (IMPORT_EXPORT_VIEW.pending_workspace_dir)

#define g_workspace_header_lines_writable      (IMPORT_EXPORT_WRITABLE->workspace_header_lines)
#define g_workspace_header_line_count_writable (IMPORT_EXPORT_WRITABLE->workspace_header_line_count)
#define g_render_state_lines_writable          (IMPORT_EXPORT_WRITABLE->render_state_lines)
#define g_cam_lines_writable                   (IMPORT_EXPORT_WRITABLE->cam_lines)
#define g_export_scene_name_hint_writable      (IMPORT_EXPORT_WRITABLE->export_scene_name_hint)
#define g_pending_scene_name_writable          (IMPORT_EXPORT_WRITABLE->pending_scene_name)
#define g_pending_workspace_dir_writable       (IMPORT_EXPORT_WRITABLE->pending_workspace_dir)

#include "repl/cfg_baseline.h"

#define g_export_cfg_bridge (repl_config_bridge())

static const char k_cfg_slug_point_attenuation[] = "point_attenuation";
static const char k_cfg_slug_msaa[] = "msaa";
static const char k_cfg_slug_line_smooth[] = "line_smooth";
static const char k_cfg_slug_vertex_outlines[] = "vertex_outlines";
static const char k_cfg_slug_vertex_points[] = "vertex_points";
/* Light bridge — the controller installs an adapter that copies the live
 * app-owned theme-seeded light data (positions/colors/eye-space) into the
 * neutral ReplExportLightInfo. This TU stays clean of scene/app includes
 * (check-repl-export-via-bridge / controller-boundary guards). NULL on the
 * demo and in tests, where lights export as zeroed + disabled. */
static const ReplExportLightBridge *g_export_light_bridge = NULL;

void repl_export_install_light_bridge(const ReplExportLightBridge *bridge) {
    g_export_light_bridge = bridge;
}

const ReplExportLightBridge *repl_export_light_bridge(void) {
    return g_export_light_bridge;
}

/* Resolve slot `slot`'s dimensional light data through the bridge, or zero
 * it when no bridge is installed. */
static void export_light_info(int slot, ReplExportLightInfo *out) {
    memset(out, 0, sizeof(*out));
    if (g_export_light_bridge && g_export_light_bridge->fill_slot)
        g_export_light_bridge->fill_slot(slot, out);
}

/* True when the slot's POSITION line must be emitted from init() (at
 * identity modelview) rather than from display() after the camera
 * transforms. The flag is set by the scene module's theme presets and
 * carried across the light bridge; the exporter just reads it, which keeps
 * this TU clean of scene includes (check-controller-boundaries
 * forbids them in src/repl/). */
static int export_slot_pos_is_eye_space(int slot) {
    ReplExportLightInfo info;
    export_light_info(slot, &info);
    return info.pos_is_eye_space;
}

/* `@declare` marker name emitted by write_canonical_cmd_as_c for each
 * CMD_VAR_DECLARE row. The reader half lives in src/repl/import.c and
 * declares its own copy so the two files stay independent. */
static const char k_snippet_directive_declare[] = "declare";
static const char k_export_c89_loop_scope_marker[] = "repl-export-c89-loop-scope";
static const char k_export_c89_loop_var_marker[] = "repl-export-c89-loop-var";
static const char k_export_glfloat1_helper[] = "repl_glfloat1";
static const char k_export_glfloat3_helper[] = "repl_glfloat3";
static const char k_export_glfloat4_helper[] = "repl_glfloat4";

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

/* Reshape-projection bridge — same install shape as the camera bridge.
 * NULL on the demo / in tests, where the perspective default is emitted.
 * Read internally via the g_export_projection_bridge static. */
static const ReplExportProjectionBridge *g_export_projection_bridge = NULL;

void repl_export_install_projection_bridge(const ReplExportProjectionBridge *bridge) {
    g_export_projection_bridge = bridge;
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

/* The pending @cfg accumulator and `repl_export_apply_pending_cfg`
 * live in src/repl/import.c — they only fire during file load. */

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

/* display() opening: shared by the code panel header and by
 * emit_export_display_begin. Keeping these as literal strings in one
 * array (rather than hardcoded fprintf calls in the exporter) is what
 * keeps panel and export byte-identical for the opening lines. */
const char *g_display_header[] = {
    REPL_EXPORT_DISPLAY_OPEN_LINE,
    "  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "  glLoadIdentity();",
    "  glPushAttrib(GL_ALL_ATTRIB_BITS);",
    NULL
};

/* Deferred @var values + parse_workspace_header_line readers moved to
 * src/repl/import.c — the writer side never needs them. The emit_*
 * helpers below are still file-static here and continue to populate
 * g_workspace_header_lines through the typed-state facade. */

/* workspace_slug_from_name moved out of src/repl/export.c. The bridge
 * implementation in glr_export.c owns slug derivation now; src/repl/export.c
 * just emits/parses the (slug, value) pairs the bridge produces
 * (implemented in step 4 of the decouple plan). */

static void workspace_format_float(char *buf, size_t n, float v) {
    /* Shortest exact-round-trip form (e.g. "0.8", not %.9g's
     * "0.800000012"); reload reproduces the identical float32. */
    repl_format_source_float(buf, (int)n, v);
}

static void export_format_decl_float(char *buf, size_t n, float v) {
    repl_format_source_float(buf, (int)n, v);
}

/* One formatted float uses REPL_SOURCE_FLOAT_TEXT_MAX (text_helpers.h);
 * exported GLfloat helper calls join up to 4 of them with ", " separators. */
#define EXPORT_FLOAT_TEXT_MAX REPL_SOURCE_FLOAT_TEXT_MAX
#define EXPORT_FLOAT_LIST_MAX (4 * EXPORT_FLOAT_TEXT_MAX)

/* Join `count` floats (count <= 4) as "a, b, c, d" using the shortest
 * exact-round-trip representation (repl_format_source_float) rather than
 * %.9g. Same bit-exact reload guarantee, but a value of 0.8f reads as
 * "0.8" instead of %.9g's "0.800000012". Used for the light
 * color/position vectors shared by the code panel and export. */
static void export_format_float_list(char *buf, size_t n,
                                     const float *v, int count) {
    if (!buf || n == 0) return;
    buf[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < count && off < n; i++) {
        char num[EXPORT_FLOAT_TEXT_MAX];
        repl_format_source_float(num, (int)sizeof(num), v[i]);
        int w = snprintf(buf + off, n - off, "%s%s", i ? ", " : "", num);
        if (w < 0) break;
        off += (size_t)w;
    }
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

static const char *export_line_comment_start(const char *s) {
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
            return s;
    }
    return NULL;
}

static void export_append_c89_comment_payload(char *out, size_t out_sz,
                                              size_t *off,
                                              const char *payload) {
    while (*payload && *off + 1 < out_sz) {
        if (payload[0] == '*' && payload[1] == '/') {
            out[(*off)++] = '*';
            if (*off + 1 < out_sz)
                out[(*off)++] = ' ';
            payload += 2;
            continue;
        }
        out[(*off)++] = *payload++;
    }
}

static void export_format_c89_comment_line(char *out, size_t out_sz,
                                           const char *line) {
    const char *comment;
    size_t off = 0;
    size_t prefix_len;

    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    if (!line) {
        return;
    }

    comment = export_line_comment_start(line);
    if (!comment) {
        snprintf(out, out_sz, "%s", line);
        return;
    }

    prefix_len = (size_t)(comment - line);
    if (prefix_len > out_sz - 1)
        prefix_len = out_sz - 1;
    memcpy(out, line, prefix_len);
    off = prefix_len;

    if (off + 2 < out_sz) {
        out[off++] = '/';
        out[off++] = '*';
    }
    export_append_c89_comment_payload(out, out_sz, &off, comment + 2);
    if (off + 3 < out_sz) {
        out[off++] = ' ';
        out[off++] = '*';
        out[off++] = '/';
    }
    out[off < out_sz ? off : out_sz - 1] = '\0';
}

static void export_write_c89_line(FILE *f, const char *line) {
    char out[MAX_LINE_LEN * 2];

    export_format_c89_comment_line(out, sizeof(out), line ? line : "");
    fprintf(f, "%s\n", out);
}

/* ========================================================================= */
/* Workspace header directive table — writer half.                            */
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

/* --- var ------------------------------------------------------------------- */

static void emit_vars(int *n) {
    for (int var_idx = 0; var_idx < g_num_predef_vars && *n < MAX_WORKSPACE_HEADER_LINES; var_idx++) {
        char vbuf[32];
        workspace_format_float(vbuf, sizeof(vbuf), g_predef_vars[var_idx].value);
        if (repl_format_fits(g_workspace_header_lines_writable[*n], WORKSPACE_HEADER_LINE_LEN,
                             "/* @var %s = %s */", g_predef_vars[var_idx].name, vbuf))
            (*n)++;
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
                "repl_export: workspace header full (%d lines) — dropped %d "
                "@cfg line(s) starting at '%s'; those scene settings will "
                "not round-trip. Raise MAX_WORKSPACE_HEADER_LINES.\n",
                MAX_WORKSPACE_HEADER_LINES, cfg.count - i, cfg.items[i].key);
    }
}

/* --- directive table (writer half) ----------------------------------------- */

#define WS_DIR(name, emit_fn) { name, emit_fn }

static const WorkspaceDirective WORKSPACE_DIRECTIVES[] = {
    /* Emit order matches this array. */
    WS_DIR("scene-name",    emit_scene_name),
    WS_DIR("workspace-dir", emit_workspace_dir),
    WS_DIR("var",           emit_vars),
    WS_DIR("func",          emit_func_aliases),
    WS_DIR("cfg",           emit_cfgs),
};
#define WORKSPACE_DIRECTIVE_COUNT \
    ((int)(sizeof(WORKSPACE_DIRECTIVES) / sizeof(WORKSPACE_DIRECTIVES[0])))

#undef WS_DIR

/* Pin the shared header budget against the worst case so adding cfg
 * items / predef-var slots / func slots can't silently reintroduce the
 * @cfg truncation. Worst case = 1 banner + 1 @scene-name + 1
 * @workspace-dir + every @var + every @func + every @cfg (capped by the
 * bag's own REPL_CFG_MAX_ITEMS). See export_state.h. */
STATIC_ASSERT(MAX_WORKSPACE_HEADER_LINES >=
                  3 + MAX_PREDEF_VARS + REPL_FUNC_SLOT_COUNT + REPL_CFG_MAX_ITEMS,
              "workspace header budget too small for worst-case directives");

void repl_state_refresh_workspace_header_lines(void) {
    int line_count = 0;
    if (line_count < MAX_WORKSPACE_HEADER_LINES) {
        snprintf(g_workspace_header_lines_writable[line_count++], WORKSPACE_HEADER_LINE_LEN,
                 "/* @workspace: REPL state (auto-saved) */");
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
     * unconditional. Looked up via the export config bridge —
     * src/repl/export.c does not call glr_config_get directly. */
    const char *toggle_slug;
} InitBootstrapEntry;

/* Bootstrap commands are REPL lines that run once at startup through
 * repl_apply_state_cmd. Each frame's glPushAttrib/glPopAttrib bracket then
 * preserves this state across user commands. Comment entries (// ...)
 * parse as CMD_COMMENT — repl_apply_state_cmd no-ops them, but the editor
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
        { "// Point attenuation: points hold a constant world-space footprint,", k_cfg_slug_point_attenuation },
        { "// shrinking as 1/distance like everything else under perspective.", k_cfg_slug_point_attenuation },
    /* Near-pure-quadratic attenuation: derived_size = size/sqrt(a + c*d^2)
     * with d the per-vertex eye distance. The quadratic term c = 1/REF_DIST^2
     * dominates at any normal viewing distance, so size scales as ~1/d ->
     * constant on-screen footprint, matching the software fallback
     * size*REF_DIST/cam_dist. Text formatted at startup from the shared
     * REPL_POINT_SIZE_REF_DIST (see g_point_atten_bootstrap_line). */
    { g_point_atten_bootstrap_line, k_cfg_slug_point_attenuation },
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

static void format_cmd_source_as_c(char *out, size_t out_sz,
                                   const char *source_text,
                                   int translate_exprs);
static int write_materialfv_as_c89(FILE *f, const char *source_text);
static int write_point_parameterfv_as_c89(FILE *f, const char *source_text);

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
    "void init(void) {",
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

/* Idea A clear-color hoist: scan the document for the LAST CMD_CLEAR_COLOR
 * and return its (literal, commit-time) args. The exported per-frame glClear
 * runs before render_repl_geometry() and inside the glPushAttrib(GL_ALL)
 * bracket — so a glClearColor in the body can never drive the visible clear
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

static void emit_export_init_section_to_file(FILE *f, int include_tess) {
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

typedef struct ExportNeeds {
    int needs_tess;
    int needs_rand;
    int needs_label;
    int needs_scratch_a;
    int needs_scratch_b;
    int needs_scratch_c;
    /* @tune knobs: names point into the live decl payloads (valid for the
     * duration of one export). tune_count is what we emit (capped at
     * REPL_TUNE_MAX_KNOBS); tune_total is the full tagged count for the
     * "capped at N" note. */
    int         tune_count;
    int         tune_total;
    const char *tune_names[REPL_TUNE_MAX_KNOBS];
} ExportNeeds;

static void emit_export_header_pre(FILE *f, const ExportNeeds *needs) {
    /* The label() and @tune helpers need <stdarg.h>/<stdio.h>. Emit them
     * here, grouped with the other system includes, rather than mid-file
     * at each helper's definition — a stray `#include` below file-scope
     * code reads as a sanitization bug even though it compiles. */
    int needs_stdio = needs && (needs->needs_label || needs->tune_count > 0);

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
            export_write_c89_line(f, block.lines[i]);
    }
}

void repl_refresh_render_state_strings(void) {
    /* Render-config toggles moved to glr_state. Read them
     * via the bridge's slug-keyed get_int — same opaque path the rest
     * of the export pipeline uses for cfg state. The demo doesn't
     * install a bridge, so the toggles fall back to "Enable" /
     * "Disable" defaults below; the demo never exports anyway
     * (implemented in step 7a). */
    int msaa_on = repl_cfg_get_int(k_cfg_slug_msaa, 1);
    int line_smooth_on = repl_cfg_get_int(k_cfg_slug_line_smooth, 0);
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
     * empty — the demo doesn't render a code panel. */
    if (!g_export_camera_bridge || !g_export_camera_bridge->fill_display_block) {
        for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++)
            g_cam_lines_writable[i][0] = '\0';
        return;
    }
    ReplExportCameraBlock block;
    memset(&block, 0, sizeof(block));
    g_export_camera_bridge->fill_display_block(&block);
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++) {
        snprintf(g_cam_lines_writable[i], sizeof(g_cam_lines_writable[i]), "%s", block.lines[i]);
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
 * Exported C uses repl_glfloatN(...) helpers instead of compound literals;
 * values go through export_format_float_list (repl_format_source_float) so
 * they keep float32 round-trip safety while reading as the shortest exact
 * form (0.8f -> "0.8", not the "0.800000012" a raw %.9g would emit). */

static const char *const k_light_names[REPL_LIGHT_SLOT_COUNT] = {
    "GL_LIGHT0", "GL_LIGHT1", "GL_LIGHT2", "GL_LIGHT3"
};

#define LIGHT_INIT_LINES_PER_LIGHT 4  /* DIFFUSE, AMBIENT, SPECULAR, glDisable */

/* Section-header comment lines emitted above the generated GL calls.
 * Pure text — the editor renders them as dimmed init/header lines and
 * the export writes them verbatim as C comments. */
static const char *const k_lights_init_header[] = {
    "  // Per-light colors. World-space POSITION is set in display() after the",
    "  // camera transforms (glLightfv(GL_POSITION) snapshots the active",
    "  // modelview); eye-space slots (headlight theme) push POSITION here at",
    "  // identity, before main() drives the first display callback.",
};
#define LIGHTS_INIT_HEADER_LINES \
    ((int)(sizeof(k_lights_init_header) / sizeof(k_lights_init_header[0])))

static const char *const k_lights_display_header[] = {
    "  // Light positions, set after the camera so they stay anchored in world space.",
};
#define LIGHTS_DISPLAY_HEADER_LINES \
    ((int)(sizeof(k_lights_display_header) / sizeof(k_lights_display_header[0])))

/* Lines slot `slot` contributes to the init section: 4 base lines plus
 * an extra POSITION line for eye-space slots (HEADLIGHT slot 0). */
static int lights_init_slot_line_count(int slot) {
    return LIGHT_INIT_LINES_PER_LIGHT
        + (export_slot_pos_is_eye_space(slot) ? 1 : 0);
}

/* Emit slot `slot`'s `sub` line into buf. The first 4 sub-indices are
 * always DIFFUSE / AMBIENT / SPECULAR / glDisable; the optional 5th
 * (only when export_slot_pos_is_eye_space(slot)) is the eye-space
 * POSITION push. */
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
                 ln, k_export_glfloat4_helper, body);
        return;
    case 1:
        export_format_float_list(body, sizeof(body), l.ambient, 4);
        snprintf(buf, n,
                 "  glLightfv(%s, GL_AMBIENT,  %s(%s));",
                 ln, k_export_glfloat4_helper, body);
        return;
    case 2:
        export_format_float_list(body, sizeof(body), l.specular, 4);
        snprintf(buf, n,
                 "  glLightfv(%s, GL_SPECULAR, %s(%s));",
                 ln, k_export_glfloat4_helper, body);
        return;
    case 3:
        snprintf(buf, n, "  glDisable(%s);", ln);
        return;
    case 4:
        /* Eye-space POSITION push. The modelview is still identity at
         * this point in init() so glLightfv snapshots eye coordinates;
         * the slot will then track the camera as the user orbits. */
        export_format_float_list(body, sizeof(body), l.pos, 4);
        snprintf(buf, n,
                 "  glLightfv(%s, GL_POSITION, %s(%s));",
                 ln, k_export_glfloat4_helper, body);
        return;
    }
}

int repl_export_lights_init_line_count(void) {
    int n = LIGHTS_INIT_HEADER_LINES;
    for (int s = 0; s < REPL_LIGHT_SLOT_COUNT; s++)
        n += lights_init_slot_line_count(s);
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
        int slot_lines = lights_init_slot_line_count(slot);
        if (i < slot_lines) {
            lights_init_emit_slot_line(slot, i, buf, n);
            return;
        }
        i -= slot_lines;
    }
    buf[0] = '\0';  /* unreachable: index bounds already enforced */
}

/* True if slot `slot`'s POSITION line should appear in the display
 * section. Eye-space slots emit POSITION from init() instead. */
static int lights_display_slot_visible(int slot) {
    return !export_slot_pos_is_eye_space(slot);
}

int repl_export_lights_display_line_count(void) {
    int n = LIGHTS_DISPLAY_HEADER_LINES;
    for (int s = 0; s < REPL_LIGHT_SLOT_COUNT; s++)
        if (lights_display_slot_visible(s)) n++;
    return n;
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
    for (int slot = 0; slot < REPL_LIGHT_SLOT_COUNT; slot++) {
        if (!lights_display_slot_visible(slot)) continue;
        if (i == 0) {
            ReplExportLightInfo l;
            export_light_info(slot, &l);
            const char *ln = k_light_names[slot];
            char body[EXPORT_FLOAT_LIST_MAX];
            export_format_float_list(body, sizeof(body), l.pos, 4);
            snprintf(buf, n,
                     "  glLightfv(%s, GL_POSITION, %s(%s));",
                     ln, k_export_glfloat4_helper, body);
            return;
        }
        i--;
    }
    buf[0] = '\0';  /* unreachable */
}

/* Emit a CMD_FOR_BEGIN as C89: a marker-tagged scope brace + hoisted
 * `float i;` decl (C89 has no for-init declarations; the markers let
 * import_is_c89_loop_marker_line drop the scaffolding on re-import),
 * then the `for (...)` header. Two paths: a has_vars loop re-extracts
 * the start/end/step expression text from the source line and runs it
 * through the expression-to-C translator (so `t`-animated bounds export
 * live); a constant loop re-parses the header to floats and emits
 * literals. Half-open REPL semantics map to `<` / `>` by step sign. */
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

        if (var_name[0] == '\0') {
            export_write_c89_line(f, source_text);
            return;
        }

        fprintf(f, "%s{ /* %s */\n", ind, k_export_c89_loop_scope_marker);
        fprintf(f, "%sfloat %s; /* %s */\n",
                ind, var_name, k_export_c89_loop_var_marker);
        float step_v = cmd->args[2];
        if (step_v >= 0) {
            fprintf(f, "%sfor (%s = %s; %s < %s; %s += %s) {\n",
                    ind, var_name, c_start, var_name, c_end, var_name, c_step);
        } else {
            fprintf(f, "%sfor (%s = %s; %s > %s; %s += %s) {\n",
                    ind, var_name, c_start, var_name, c_end, var_name, c_step);
        }
        return;
    }

    float start_v, end_v, step_v;
    const char *body;
    if (repl_eval_parse_for_header(p, var_name, sizeof(var_name),
                         &start_v, &end_v, &step_v, &body)) {
        char start_s[EXPORT_FLOAT_TEXT_MAX], end_s[EXPORT_FLOAT_TEXT_MAX];
        repl_format_source_float(start_s, sizeof(start_s), start_v);
        repl_format_source_float(end_s, sizeof(end_s), end_v);
        fprintf(f, "%s{ /* %s */\n", ind, k_export_c89_loop_scope_marker);
        fprintf(f, "%sfloat %s; /* %s */\n",
                ind, var_name, k_export_c89_loop_var_marker);
        if (step_v == 1.0f) {
            fprintf(f, "%sfor (%s = %s; %s < %s; %s += 1.0f) {\n",
                    ind, var_name, start_s, var_name, end_s, var_name);
        } else if (step_v == -1.0f) {
            fprintf(f, "%sfor (%s = %s; %s > %s; %s -= 1.0f) {\n",
                    ind, var_name, start_s, var_name, end_s, var_name);
        } else {
            char step_s[EXPORT_FLOAT_TEXT_MAX];
            repl_format_source_float(step_s, sizeof(step_s), step_v);
            fprintf(f, "%sfor (%s = %s; %s %s %s; %s += %sf) {\n",
                    ind, var_name, start_s, var_name,
                    step_v > 0 ? "<" : ">", end_s, var_name, step_s);
        }
    } else {
        export_write_c89_line(f, source_text);
    }
}

static void write_for_end_as_c(FILE *f, const char *source_text) {
    int indent = 0;

    while (source_text[indent] && isspace((unsigned char)source_text[indent]))
        indent++;

    export_write_c89_line(f, source_text);
    fprintf(f, "%.*s} /* %s */\n",
            indent, source_text, k_export_c89_loop_scope_marker);
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
        if (!repl_state_document_cmds()[cmd_idx].valid)
            continue;
        if (cmd_type_is_tess(repl_state_document_cmds()[cmd_idx].type))
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
        fprintf(f, "      { _tn[0] = %s; _tn[1] = %s; _tn[2] = %s; }\n",
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
        fprintf(f,
                "      { _tc[0] = %s; _tc[1] = %s; _tc[2] = %s; _tc[3] = %s; }\n",
                c_args[0], c_args[1], c_args[2], c_args[3]);
        return 1;
    case CMD_TESS_VERTEX:
        if (arg_count != 3)
            return 0;
        fprintf(f,
                "      {TessVertex*_v=&_tv[_tv_n++];"
                "_v->pos[0]=%s;_v->pos[1]=%s;_v->pos[2]=%s;"
                "memcpy(_v->normal,_tn,sizeof _v->normal);"
                "memcpy(_v->color,_tc,sizeof _v->color);"
                "gluTessVertex(g_tess,_v->pos,_v);}\n",
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
    export_write_c89_line(f, out);
}

static int find_export_block_end(int begin_idx) {
    int depth = 1;

    for (int cmd_idx = begin_idx + 1; cmd_idx < repl_state_document_count(); cmd_idx++) {
        CmdType t = repl_state_document_cmds()[cmd_idx].type;
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
           repl_state_document_cmds()[cmd_idx].valid &&
           (repl_state_document_cmds()[cmd_idx].type == CMD_COMMENT ||
            repl_state_document_cmds()[cmd_idx].type == CMD_EMPTY))
        cmd_idx++;
    if (cmd_idx > start && cmd_idx < end_idx && cmd_idx < repl_state_document_count() &&
        repl_state_document_cmds()[cmd_idx].valid && repl_state_document_cmds()[cmd_idx].type == CMD_FUNC_DEF)
        return cmd_idx;
    return -1;
}

/* The per-command writer for the exported display() body: emit one
 * source command as standalone C. One independent case per command
 * family — most canonical REPL text is already valid C and passes
 * through the default arm (expression-to-C translated when the command
 * carries vars); the exceptions each own a case: var declares become
 * `// @declare` markers (locals would shadow the file-scope globals),
 * tess commands expand to the gluTess* call sequences, and label()
 * keeps its format string byte-exact. Block structure (for/func) is the
 * caller's job. The import translators in src/repl/import.c are the
 * inverses of these arms — change them in pairs to keep export/import
 * round-trips lossless. */
static void write_canonical_cmd_as_c(FILE *f, const GLCmd *cmd, int cmd_idx,
                                     int for_depth, int *tess_depth) {
    const char *source_text = export_document_text(cmd_idx);

    switch (cmd->type) {
    case CMD_EMPTY:
        fputc('\n', f);
        break;
    case CMD_COMMENT:
        export_write_c89_line(f, source_text);
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
                while (*p && *p != ';' && idx < cmd->payload.decl.count) {
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
        fprintf(f, "  /* @%s", k_snippet_directive_declare);
        for (int di = 0; di < cmd->payload.decl.count; di++) {
            if (has_init[di]) {
                char vbuf[32];
                export_format_decl_float(vbuf, sizeof(vbuf), inits[di]);
                fprintf(f, " %s=%s", cmd->payload.decl.names[di], vbuf);
            } else
                fprintf(f, " %s", cmd->payload.decl.names[di]);
        }
        /* Round-trip the @tune knob tag: import re-attaches `// @tune` to the
         * reconstructed decl line. Import's name loop stops at `@`, so the
         * trailing token detaches cleanly. */
        if (repl_eval_line_has_tune_tag(source_text))
            fprintf(f, " @tune");
        fprintf(f, " */\n");
        break;
    }
    case CMD_VAR_ASSIGN: {
        char c_src[MAX_LINE_LEN];
        repl_eval_expr_to_c(source_text, c_src, sizeof(c_src));
        export_write_c89_line(f, c_src);
        break;
    }
    case CMD_SCRATCH_ASSIGN: {
        char c_src[MAX_LINE_LEN];
        repl_eval_expr_to_c(source_text, c_src, sizeof(c_src));
        export_write_c89_line(f, c_src);
        break;
    }
    case CMD_CALL:
        write_cmd_source_as_c(f, source_text, 1);
        break;
    case CMD_MATERIALFV:
        if (!write_materialfv_as_c89(f, source_text))
            write_cmd_source_as_c(f, source_text, cmd->has_vars);
        break;
    case CMD_POINT_PARAMETER_FV:
        if (!write_point_parameterfv_as_c89(f, source_text))
            write_cmd_source_as_c(f, source_text, cmd->has_vars);
        break;
    case CMD_TESS_BEGIN_POLYGON:
        fprintf(f, "  { _tv_n = 0; gluTessBeginPolygon(g_tess, NULL); }\n");
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
            char x[EXPORT_FLOAT_TEXT_MAX], y[EXPORT_FLOAT_TEXT_MAX], z[EXPORT_FLOAT_TEXT_MAX];
            repl_format_source_float(x, sizeof(x), cmd->args[0]);
            repl_format_source_float(y, sizeof(y), cmd->args[1]);
            repl_format_source_float(z, sizeof(z), cmd->args[2]);
            fprintf(f, "      { _tn[0] = %s; _tn[1] = %s; _tn[2] = %s; }\n",
                    x, y, z);
        }
        break;
    case CMD_TESS_COLOR:
        if (!write_tess_source_as_c(f, cmd, source_text)) {
            char r[EXPORT_FLOAT_TEXT_MAX], g[EXPORT_FLOAT_TEXT_MAX],
                 b[EXPORT_FLOAT_TEXT_MAX], a[EXPORT_FLOAT_TEXT_MAX];
            repl_format_source_float(r, sizeof(r), cmd->args[0]);
            repl_format_source_float(g, sizeof(g), cmd->args[1]);
            repl_format_source_float(b, sizeof(b), cmd->args[2]);
            repl_format_source_float(a, sizeof(a), cmd->args[3]);
            fprintf(f,
                    "      { _tc[0] = %s; _tc[1] = %s; _tc[2] = %s; _tc[3] = %s; }\n",
                    r, g, b, a);
        }
        break;
    case CMD_TESS_VERTEX:
        if (!write_tess_source_as_c(f, cmd, source_text)) {
            char x[EXPORT_FLOAT_TEXT_MAX], y[EXPORT_FLOAT_TEXT_MAX], z[EXPORT_FLOAT_TEXT_MAX];
            repl_format_source_float(x, sizeof(x), cmd->args[0]);
            repl_format_source_float(y, sizeof(y), cmd->args[1]);
            repl_format_source_float(z, sizeof(z), cmd->args[2]);
            fprintf(f,
                    "      {TessVertex*_v=&_tv[_tv_n++];"
                    "_v->pos[0]=%s;_v->pos[1]=%s;_v->pos[2]=%s;"
                    "memcpy(_v->normal,_tn,sizeof _v->normal);"
                    "memcpy(_v->color,_tc,sizeof _v->color);"
                    "gluTessVertex(g_tess,_v->pos,_v);}\n",
                    x, y, z);
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
                {
                    char label_line[MAX_LINE_LEN];
                    int take = prefix_len;
                    if (take >= (int)sizeof(label_line))
                        take = (int)sizeof(label_line) - 1;
                    memcpy(label_line, source_text, (size_t)take);
                    label_line[take] = '\0';
                    snprintf(label_line + take, sizeof(label_line) - (size_t)take,
                             "\"%s\"%s%s);",
                             fmt, post_c[0] ? ", " : "", post_c);
                    export_write_c89_line(f, label_line);
                }
                break;
            }
        }
        /* Fallback: emit raw text — the importer handles it via the
         * default repl_eval_c_expr_to_repl path. */
        export_write_c89_line(f, source_text);
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
        if (!repl_state_document_cmds()[cmd_idx].valid) continue;
        if (skip_func_defs &&
            (repl_state_document_cmds()[cmd_idx].type == CMD_COMMENT ||
             repl_state_document_cmds()[cmd_idx].type == CMD_EMPTY)) {
            int attached_func = comment_run_attached_func_idx(cmd_idx, end_idx);
            if (attached_func >= 0) {
                cmd_idx = find_export_block_end(attached_func);
                continue;
            }
        }
        switch (repl_state_document_cmds()[cmd_idx].type) {
        case CMD_FOR_BEGIN:
            write_for_begin_as_c(f, &repl_state_document_cmds()[cmd_idx],
                                 export_document_text(cmd_idx));
            for_depth++;
            break;
        case CMD_FOR_END:
            for_depth--;
            write_for_end_as_c(f, export_document_text(cmd_idx));
            break;
        case CMD_FUNC_DEF:
            if (skip_func_defs)
                cmd_idx = find_export_block_end(cmd_idx);
            break;
        case CMD_FUNC_END:
            break;
        default:
            write_canonical_cmd_as_c(f, &repl_state_document_cmds()[cmd_idx],
                                     cmd_idx, for_depth, &tess_depth);
            break;
        }
    }
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
    fprintf(f, "\n/* Scene state variables.\n"
               " * Initializers are the live snapshot at export time, so the\n"
               " * program starts in the same state the REPL preview ended in.\n"
               " * Variables other than t keep mutations from frame to frame. */\n");
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

static void write_glfloat_vector_helpers(FILE *f) {
    fprintf(f,
        "\n/* C89 replacement for C99 compound GLfloat literals. */\n"
        "static GLfloat repl_glfloat1_buf[1];\n"
        "static GLfloat repl_glfloat3_buf[3];\n"
        "static GLfloat repl_glfloat4_buf[4];\n"
        "\n"
        "static GLfloat *%s(GLfloat a) {\n"
        "  repl_glfloat1_buf[0] = a;\n"
        "  return repl_glfloat1_buf;\n"
        "}\n"
        "\n"
        "static GLfloat *%s(GLfloat a, GLfloat b, GLfloat c) {\n"
        "  repl_glfloat3_buf[0] = a;\n"
        "  repl_glfloat3_buf[1] = b;\n"
        "  repl_glfloat3_buf[2] = c;\n"
        "  return repl_glfloat3_buf;\n"
        "}\n"
        "\n"
        "static GLfloat *%s(GLfloat a, GLfloat b, GLfloat c, GLfloat d) {\n"
        "  repl_glfloat4_buf[0] = a;\n"
        "  repl_glfloat4_buf[1] = b;\n"
        "  repl_glfloat4_buf[2] = c;\n"
        "  repl_glfloat4_buf[3] = d;\n"
        "  return repl_glfloat4_buf;\n"
        "}\n",
        k_export_glfloat1_helper,
        k_export_glfloat3_helper,
        k_export_glfloat4_helper);
}

static int export_copy_first_arg(const char **p_inout,
                                 char *out, size_t out_sz) {
    const char *p = *p_inout;
    const char *start;
    const char *delim;
    int len;

    while (*p && isspace((unsigned char)*p))
        p++;
    start = p;
    delim = repl_scan_next_arg_delim(p);
    if (*delim != ',')
        return 0;
    len = (int)(delim - start);
    while (len > 0 && isspace((unsigned char)start[len - 1]))
        len--;
    if (len <= 0 || len >= (int)out_sz)
        return 0;
    memcpy(out, start, (size_t)len);
    out[len] = '\0';
    *p_inout = delim + 1;
    return 1;
}

static int export_extract_vector_payload(const char *arg,
                                         char *out, size_t out_sz) {
    const char *open;
    const char *close;
    int len;

    while (*arg && isspace((unsigned char)*arg))
        arg++;
    open = strchr(arg, '{');
    close = strrchr(arg, '}');
    if (open && close && close > open) {
        len = (int)(close - open - 1);
        if (len <= 0 || len >= (int)out_sz)
            return 0;
        memcpy(out, open + 1, (size_t)len);
        out[len] = '\0';
        return 1;
    }

    len = (int)strlen(arg);
    while (len > 0 && isspace((unsigned char)arg[len - 1]))
        len--;
    if (len <= 0 || len >= (int)out_sz)
        return 0;
    memcpy(out, arg, (size_t)len);
    out[len] = '\0';
    return 1;
}

static int export_translate_vector_args(const char *payload,
                                        char out[][MAX_LINE_LEN],
                                        int expected_count) {
    char raw[4][MAX_LINE_LEN];
    int count = split_top_level_args(payload, raw, 4);
    int arg_idx;

    if (count != expected_count)
        return 0;
    for (arg_idx = 0; arg_idx < count; arg_idx++)
        repl_eval_expr_to_c(raw[arg_idx], out[arg_idx], sizeof(out[arg_idx]));
    return 1;
}

static int write_materialfv_as_c89(FILE *f, const char *source_text) {
    const char *p = source_text;
    const char *payload_start;
    const char *payload_end;
    char face[MAX_LINE_LEN];
    char pname[MAX_LINE_LEN];
    char vector_payload[MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int indent = 0;
    int payload_len;
    char payload[MAX_LINE_LEN];
    const char *payload_p;
    int count;

    while (p[indent] && isspace((unsigned char)p[indent]))
        indent++;
    p += indent;
    if (strncmp(p, "glMaterialfv", 12) != 0)
        return 0;
    payload_start = strchr(p, '(');
    payload_end = strrchr(p, ')');
    if (!payload_start || !payload_end || payload_end <= payload_start)
        return 0;
    payload_len = (int)(payload_end - payload_start - 1);
    if (payload_len <= 0 || payload_len >= (int)sizeof(payload))
        return 0;
    memcpy(payload, payload_start + 1, (size_t)payload_len);
    payload[payload_len] = '\0';

    payload_p = payload;
    if (!export_copy_first_arg(&payload_p, face, sizeof(face)))
        return 0;
    if (!export_copy_first_arg(&payload_p, pname, sizeof(pname)))
        return 0;
    if (!export_extract_vector_payload(payload_p, vector_payload, sizeof(vector_payload)))
        return 0;

    count = split_top_level_args(vector_payload, c_args, 4);
    if (count != 1 && count != 4)
        return 0;
    if (!export_translate_vector_args(vector_payload, c_args, count))
        return 0;

    if (count == 1) {
        fprintf(f, "%.*sglMaterialfv(%s, %s, %s(%s));\n",
                indent, source_text, face, pname,
                k_export_glfloat1_helper, c_args[0]);
    } else {
        fprintf(f, "%.*sglMaterialfv(%s, %s, %s(%s, %s, %s, %s));\n",
                indent, source_text, face, pname,
                k_export_glfloat4_helper,
                c_args[0], c_args[1], c_args[2], c_args[3]);
    }
    return 1;
}

static int write_point_parameterfv_as_c89(FILE *f, const char *source_text) {
    const char *p = source_text;
    const char *payload_start;
    const char *payload_end;
    char pname[MAX_LINE_LEN];
    char vector_payload[MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int indent = 0;
    int payload_len;
    char payload[MAX_LINE_LEN];
    const char *payload_p;

    while (p[indent] && isspace((unsigned char)p[indent]))
        indent++;
    p += indent;
    if (strncmp(p, "glPointParameterfv", 18) != 0)
        return 0;
    payload_start = strchr(p, '(');
    payload_end = strrchr(p, ')');
    if (!payload_start || !payload_end || payload_end <= payload_start)
        return 0;
    payload_len = (int)(payload_end - payload_start - 1);
    if (payload_len <= 0 || payload_len >= (int)sizeof(payload))
        return 0;
    memcpy(payload, payload_start + 1, (size_t)payload_len);
    payload[payload_len] = '\0';

    payload_p = payload;
    if (!export_copy_first_arg(&payload_p, pname, sizeof(pname)))
        return 0;
    if (!export_extract_vector_payload(payload_p, vector_payload, sizeof(vector_payload)))
        return 0;
    if (!export_translate_vector_args(vector_payload, c_args, 3))
        return 0;

    fprintf(f, "%.*sglPointParameterfv(%s, %s(%s, %s, %s));\n",
            indent, source_text, pname, k_export_glfloat3_helper,
            c_args[0], c_args[1], c_args[2]);
    return 1;
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
 * The <stdarg.h>/<stdio.h> this needs are emitted up in the system-include
 * group by emit_export_header_pre (gated on needs_label || tune_count),
 * not mid-file here. */
static void write_label_helper(FILE *f) {
    /* fprintf format escaping: every literal `%` in the emitted C source
     * must be doubled (`%%`) so fprintf doesn't treat it as a conversion
     * specifier. The `%%g` below emits literal `%g` into the C output. */
    fprintf(f,
        "\n/* Draw bitmap text at the current raster position. */\n"
        "\nstatic void label(const char *fmt, ...) {\n"
        "  const char *ch;\n"
        "  char text[128];\n"
        "  int offset = 0;\n"
        "  va_list args;\n"
        "\n"
        "  va_start(args, fmt);\n"
        "  while (*fmt && offset < (int)sizeof(text) - 1) {\n"
        "    if (fmt[0] == '%%' && fmt[1] == 'f') {\n"
        "      double value = va_arg(args, double);\n"
        "      offset += snprintf(text + offset, sizeof(text) - (size_t)offset,\n"
        "                         \"%%g\", value);\n"
        "      if (offset >= (int)sizeof(text))\n"
        "        offset = (int)sizeof(text) - 1;\n"
        "      fmt += 2;\n"
        "    } else if (fmt[0] == '%%' && fmt[1] == '%%') {\n"
        "      text[offset++] = '%%';\n"
        "      fmt += 2;\n"
        "    } else {\n"
        "      text[offset++] = *fmt++;\n"
        "    }\n"
        "  }\n"
        "  text[offset] = '\\0';\n"
        "  va_end(args);\n"
        "\n"
        "  for (ch = text; *ch; ch++)\n"
        "    glutBitmapCharacter(GLUT_BITMAP_9_BY_15, (unsigned char)*ch);\n"
        "}\n");
}

static void write_render_helper_as_c(FILE *f, const char *name) {
    fprintf(f, "\n/* User scene commands captured from gl-repl. */\n");
    fprintf(f, "static void %s(void) {\n", name);
    fprintf(f, "  /* Snippet start */\n");
    write_render_body_range_as_c(f, 0, repl_state_document_count(), 1);
    int bb = 0;
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (repl_state_document_cmds()[cmd_idx].valid && repl_state_document_cmds()[cmd_idx].type == CMD_BEGIN) bb++;
        else if (repl_state_document_cmds()[cmd_idx].valid && repl_state_document_cmds()[cmd_idx].type == CMD_END) bb--;
    }
    if (bb > 0)
        fprintf(f, "  glEnd();\n");
    fprintf(f, "  /* Snippet end */\n");
    fprintf(f, "}\n");
}

static void write_func_defs_as_c(FILE *f) {
    /* Iterate through all document commands looking for function definitions. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds()[cmd_idx].valid || repl_state_document_cmds()[cmd_idx].type != CMD_FUNC_DEF) continue;
        int comment_start = cmd_idx;
        while (comment_start > 0 &&
               repl_state_document_cmds()[comment_start - 1].valid &&
               (repl_state_document_cmds()[comment_start - 1].type == CMD_COMMENT ||
                repl_state_document_cmds()[comment_start - 1].type == CMD_EMPTY))
            comment_start--;
        /* Emit any preceding comment lines. */
        for (int comment_idx = comment_start; comment_idx < cmd_idx; comment_idx++) {
            fputc('\n', f);
            export_write_c89_line(f, export_document_text(comment_idx));
        }

        int fn = (int)repl_state_document_cmds()[cmd_idx].args[0];
        int parsed_fn = fn;
        int param_count = 0;
        char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
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
        "\n#include <string.h>\n"
        "\n/* GLU tessellation support for concave polygons. */\n"
        "typedef struct {\n"
        "  GLdouble pos[3];\n"
        "  GLdouble normal[3];\n"
        "  GLdouble color[4];\n"
        "} TessVertex;\n"
        "\n"
        "static TessVertex _tv[256];\n"
        "static int _tv_n = 0;\n"
        "static GLdouble _tn[3] = {0.0, 0.0, 1.0};\n"
        "static GLdouble _tc[4] = {1.0, 1.0, 1.0, 1.0};\n"
        "static GLUtesselator *g_tess = NULL;\n"
        "\n"
        "typedef void (*_GluCb)(void);\n"
        "\n"
        "static void _tess_vtx_begin_cb(GLenum mode) {\n"
        "  glBegin(mode);\n"
        "}\n"
        "\n"
        "static void _tess_vtx_end_cb(void) {\n"
        "  glEnd();\n"
        "}\n"
        "\n"
        "static void _tess_vtx_cb(void *vd) {\n"
        "  TessVertex *v = (TessVertex *)vd;\n"
        "  glNormal3dv(v->normal);\n"
        "  glColor4dv(v->color);\n"
        "  glVertex3dv(v->pos);\n"
        "}\n"
        "\n"
        "static void _tess_comb_cb(GLdouble coords[3], void *vd[4],\n"
        "                          GLfloat w[4], void **out) {\n"
        "  TessVertex *v;\n"
        "  TessVertex *src;\n"
        "  double len;\n"
        "  int c;\n"
        "  int j;\n"
        "\n"
        "  if (_tv_n >= 256) {\n"
        "    *out = NULL;\n"
        "    return;\n"
        "  }\n"
        "\n"
        "  v = &_tv[_tv_n++];\n"
        "  v->pos[0] = coords[0];\n"
        "  v->pos[1] = coords[1];\n"
        "  v->pos[2] = coords[2];\n"
        "\n"
        "  for (c = 0; c < 3; c++)\n"
        "    v->normal[c] = 0.0;\n"
        "  for (c = 0; c < 4; c++)\n"
        "    v->color[c] = 0.0;\n"
        "\n"
        "  for (j = 0; j < 4; j++) {\n"
        "    if (!vd[j])\n"
        "      continue;\n"
        "\n"
        "    src = (TessVertex *)vd[j];\n"
        "    for (c = 0; c < 3; c++)\n"
        "      v->normal[c] += w[j] * src->normal[c];\n"
        "    for (c = 0; c < 4; c++)\n"
        "      v->color[c] += w[j] * src->color[c];\n"
        "  }\n"
        "\n"
        "  len = sqrt(v->normal[0] * v->normal[0] +\n"
        "             v->normal[1] * v->normal[1] +\n"
        "             v->normal[2] * v->normal[2]);\n"
        "  if (len > 1e-9) {\n"
        "    v->normal[0] /= len;\n"
        "    v->normal[1] /= len;\n"
        "    v->normal[2] /= len;\n"
        "  }\n"
        "\n"
        "  *out = v;\n"
        "}\n"
        "\n"
        "static void _tess_err_cb(GLenum err) {\n"
        "  (void)err;\n"
        "}\n"
    );
}

enum {
    EXPORT_NAME_MAX = 64,
    EXPORT_NAME_SET_MAX = 160
};

typedef struct {
    char names[EXPORT_NAME_SET_MAX][EXPORT_NAME_MAX];
    int  count;
} ExportNameSet;

typedef struct {
    char draw_scene[EXPORT_NAME_MAX];
    char draw_tuning_overlay[EXPORT_NAME_MAX];
    char tuning_step[EXPORT_NAME_MAX];
    char hud_text[EXPORT_NAME_MAX];
    char tuning_window_width[EXPORT_NAME_MAX];
    char tuning_window_height[EXPORT_NAME_MAX];
    char keyboard_key[EXPORT_NAME_MAX];
    char keyboard_mouse_x[EXPORT_NAME_MAX];
    char keyboard_mouse_y[EXPORT_NAME_MAX];
    char tune_modifiers[EXPORT_NAME_MAX];
    char tune_normalized_key[EXPORT_NAME_MAX];
    char tune_step_scale[EXPORT_NAME_MAX];
    char overlay_width[EXPORT_NAME_MAX];
    char overlay_height[EXPORT_NAME_MAX];
    char overlay_text_y[EXPORT_NAME_MAX];
} ExportGeneratedNames;

typedef struct {
    ExportNeeds              needs;
    const ReplExportLayout  *layout;
    ExportGeneratedNames     names;
} ExportScaffoldContext;

typedef void (*ExportDisplayPassSetupFn)(FILE *f);

typedef struct {
    const char                 *label;
    int                         enabled;
    ExportDisplayPassSetupFn    emit_setup;
} ExportDisplayPassSpec;

enum {
    EXPORT_DISPLAY_PASS_COUNT = 3
};

static int export_name_set_has(const ExportNameSet *set, const char *name) {
    if (!set || !name || !name[0])
        return 0;
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->names[i], name) == 0)
            return 1;
    }
    return 0;
}

static void export_name_set_add(ExportNameSet *set, const char *name) {
    if (!set || !name || !name[0] || export_name_set_has(set, name))
        return;
    if (set->count >= EXPORT_NAME_SET_MAX)
        return;
    snprintf(set->names[set->count++], EXPORT_NAME_MAX, "%s", name);
}

static void export_choose_name(ExportNameSet *set, char out[EXPORT_NAME_MAX],
                               const char *preferred, const char *fallback) {
    const char *base = preferred && preferred[0] ? preferred : "generated_name";
    if (!export_name_set_has(set, base)) {
        snprintf(out, EXPORT_NAME_MAX, "%s", base);
        export_name_set_add(set, out);
        return;
    }
    if (fallback && fallback[0] && !export_name_set_has(set, fallback)) {
        snprintf(out, EXPORT_NAME_MAX, "%s", fallback);
        export_name_set_add(set, out);
        return;
    }
    for (int suffix = 2; suffix < 10000; suffix++) {
        char candidate[EXPORT_NAME_MAX];
        snprintf(candidate, sizeof(candidate), "%s_%d", base, suffix);
        if (!export_name_set_has(set, candidate)) {
            snprintf(out, EXPORT_NAME_MAX, "%s", candidate);
            export_name_set_add(set, out);
            return;
        }
    }
    snprintf(out, EXPORT_NAME_MAX, "%s_generated", base);
    export_name_set_add(set, out);
}

static void export_name_set_add_user_identifiers(ExportNameSet *set) {
    for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++)
        export_name_set_add(set, g_predef_vars[var_idx].name);

    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        const char *alias = repl_func_alias_get(slot);
        if (alias)
            export_name_set_add(set, alias);
    }

    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        const GLCmd *cmd = &repl_state_document_cmds()[cmd_idx];
        if (!cmd->valid || cmd->type != CMD_FUNC_DEF)
            continue;
        int fn = (int)cmd->args[0];
        const char *alias = repl_func_alias_get(fn);
        char fallback_name[REPL_FUNC_NAME_MAX + 8];
        if (alias) {
            export_name_set_add(set, alias);
        } else {
            snprintf(fallback_name, sizeof(fallback_name), "func%d", fn);
            export_name_set_add(set, fallback_name);
        }
    }
}

static void export_generated_names_init(ExportGeneratedNames *names) {
    ExportNameSet used;

    memset(&used, 0, sizeof(used));
    memset(names, 0, sizeof(*names));
    export_name_set_add_user_identifiers(&used);

    export_choose_name(&used, names->draw_scene,
                       "draw_scene", "draw_repl_scene");
    export_choose_name(&used, names->draw_tuning_overlay,
                       "draw_tuning_overlay", "draw_repl_tuning_overlay");
    export_choose_name(&used, names->tuning_step,
                       "tuning_step", "tune_compute_step");
    export_choose_name(&used, names->hud_text,
                       "hud_text", "repl_hud_text");
    export_choose_name(&used, names->tuning_window_width,
                       "window_width", "tuning_window_width");
    export_choose_name(&used, names->tuning_window_height,
                       "window_height", "tuning_window_height");
    export_choose_name(&used, names->keyboard_key,
                       "key", "pressed_key");
    export_choose_name(&used, names->keyboard_mouse_x,
                       "mouse_x", "keyboard_mouse_x");
    export_choose_name(&used, names->keyboard_mouse_y,
                       "mouse_y", "keyboard_mouse_y");
    export_choose_name(&used, names->tune_modifiers,
                       "modifiers", "tuning_modifiers");
    export_choose_name(&used, names->tune_normalized_key,
                       "normalized_key", "tuning_key");
    export_choose_name(&used, names->tune_step_scale,
                       "step_scale", "tuning_step_scale");
    export_choose_name(&used, names->overlay_width,
                       "overlay_width", "tuning_overlay_width");
    export_choose_name(&used, names->overlay_height,
                       "overlay_height", "tuning_overlay_height");
    export_choose_name(&used, names->overlay_text_y,
                       "text_y", "overlay_text_y");
}

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
        const GLCmd *cmd = &repl_state_document_cmds()[cmd_idx];
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

    needs.tune_count = repl_collect_tuned_vars(
        repl_state_document_cmds(), repl_state_document_count(),
        s_export_text_view, needs.tune_names, REPL_TUNE_MAX_KNOBS,
        &needs.tune_total);

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

/* Build the actual display() pass plan. Multipass scaffolding must count
 * these enabled flags, not raw cfg toggles, because cfg-backed overlay
 * passes may be intentionally disabled in export. */
static size_t export_build_display_passes(
    ExportDisplayPassSpec passes[EXPORT_DISPLAY_PASS_COUNT]) {
    int outlines_on = repl_cfg_get_int(k_cfg_slug_vertex_outlines, 0);
    int vpoints_on = repl_cfg_get_int(k_cfg_slug_vertex_points, 0);

    if (!passes)
        return 0;

    passes[0].label = "Vertex Fill Pass";
    passes[0].enabled = 1;
    passes[0].emit_setup = NULL;

    /* Disable the outlines for now, they complicate the exported code
     * and are not one for one with the live REPL's outline pass.
     *
     * TODO: adapt both the REPL and export outline passes to use a
     * shared codegen path so they stay in sync and the export can
     * emit a matching outline pass setup.  Possibly using a stencil
     * buffer approach of drawing without color buffer with
     * polygonmode line and points and then fill the stencil in a
     * second pass, which would be more robust and simpler than the
     * current approach of using LIGHTING and setting lights to black
     * for the outline pass. */
    passes[1].label = "Vertex Outline Pass";
    passes[1].enabled = 0;
    passes[1].emit_setup = emit_export_outline_pass_setup;

    passes[2].label = "Vertex Point Pass";
    passes[2].enabled = 0;
    passes[2].emit_setup = emit_export_point_pass_setup;

    (void)outlines_on;
    (void)vpoints_on;
    return EXPORT_DISPLAY_PASS_COUNT;
}

static int export_count_enabled_pass_specs(
    const ExportDisplayPassSpec *passes, size_t pass_count) {
    int count = 0;
    size_t pass_idx;

    if (!passes)
        return 0;

    for (pass_idx = 0; pass_idx < pass_count; pass_idx++) {
        if (passes[pass_idx].enabled)
            count++;
    }
    return count;
}

static int export_display_has_multiple_enabled_passes(void) {
    ExportDisplayPassSpec passes[EXPORT_DISPLAY_PASS_COUNT];
    size_t pass_count = export_build_display_passes(passes);
    return export_count_enabled_pass_specs(passes, pass_count) > 1;
}

static void emit_export_geometry_pass(FILE *f,
                                      const ExportDisplayPassSpec *pass,
                                      int needs_restore,
                                      const ExportGeneratedNames *names) {
    if (!pass || !pass->enabled)
        return;

    fprintf(f, "\n  /* %s */\n", pass->label);
    fprintf(f, "  glPushAttrib(GL_ALL_ATTRIB_BITS);\n");
    if (pass->emit_setup)
        pass->emit_setup(f);
    fprintf(f, "  glPushMatrix();\n");
    if (needs_restore)
        fprintf(f, "  restore_repl_vars();\n");
    fprintf(f, "  %s();\n", names->draw_scene);
    fprintf(f, "  glPopMatrix();\n");
    fprintf(f, "  glPopAttrib();\n");
}

static void emit_export_display_begin(FILE *f) {
    /* display() opening lines come from g_display_header so the panel
     * (which renders the same array) and the exported file stay
     * byte-identical here. */
    fprintf(f, "\n/* Draw one frame. */\n");
    for (int line_idx = 0; g_display_header[line_idx]; line_idx++)
        fprintf(f, "%s\n", g_display_header[line_idx]);
    /* Emit render state configuration lines (lighting, depth, etc). */
    for (int state_line_idx = 0; state_line_idx < RENDER_STATE_LINE_COUNT; state_line_idx++)
        export_write_c89_line(f, g_render_state_lines[state_line_idx]);
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
            export_write_c89_line(f, line);
        }
    }
    /* g_header_post: additional setup after the dynamic state lines. */
    for (int line_idx = 0; g_header_post[line_idx]; line_idx++)
        export_write_c89_line(f, g_header_post[line_idx]);
}

static void emit_export_display_geometry(FILE *f,
                                         const ExportGeneratedNames *names) {
    ExportDisplayPassSpec passes[EXPORT_DISPLAY_PASS_COUNT];
    size_t pass_count = export_build_display_passes(passes);

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
    int multipass = export_count_enabled_pass_specs(passes, pass_count) > 1 &&
                    export_has_persistent_predef_vars();
    if (multipass)
        fprintf(f, "  save_repl_vars();\n");

    int rendered_passes = 0;
    for (size_t i = 0; i < pass_count; i++) {
        if (!passes[i].enabled) continue;
        int needs_restore = multipass && rendered_passes > 0;
        emit_export_geometry_pass(f, &passes[i], needs_restore, names);
        rendered_passes++;
    }
}

/* QWERTY column pairs: knob i raises with k_tune_up_keys[i], lowers with
 * k_tune_down_keys[i]. Index-aligned; length REPL_TUNE_MAX_KNOBS. */
static const char k_tune_up_keys[]   = "qwertyuio";
static const char k_tune_down_keys[] = "asdfghjkl";

STATIC_ASSERT(sizeof(k_tune_up_keys) - 1 == REPL_TUNE_MAX_KNOBS,
              tune_up_key_count);
STATIC_ASSERT(sizeof(k_tune_down_keys) - 1 == REPL_TUNE_MAX_KNOBS,
              tune_down_key_count);

/* Prologue: window-size globals (captured in reshape, read by the HUD),
 * the swatch-step mirror, a formatted HUD-text helper, and the HUD draw pass.
 * Emitted only when at least one variable is @tune-tagged. */
static void write_tune_helpers(FILE *f, const ExportNeeds *needs,
                               const ExportGeneratedNames *names) {
    if (needs->tune_total > needs->tune_count)
        fprintf(f,
            "\n/* @tune: %d variables tagged; capped at %d keyboard knobs. */\n",
            needs->tune_total, REPL_TUNE_MAX_KNOBS);
    fprintf(f,
        "\n/* @tune knobs: keyboard-adjustable variables + overlay, generated because\n"
        " * one or more `float` decls carried a `// @tune` tag. */\n"
        "static int %s = 800;\n"
        "static int %s = 600;\n"
        "\n/* Mirror of repl_eval_swatch_step() (src/repl/eval.c); pinned by the\n"
        " * swatch-parity test in tests/test_repl_tune.c so it cannot drift. */\n"
        "static float %s(float value) {\n"
        "  float magnitude = fabsf(value);\n"
        "  float exponent = (magnitude < 10.0f) ? 0.0f : floorf(log10f(magnitude));\n"
        "  return 0.05f * powf(10.0f, exponent);\n"
        "}\n"
        "\n/* Draw formatted bitmap text in screen-space HUD coordinates. */\n"
        "static void %s(float x, float y, const char *fmt, ...) {\n"
        "  const char *ch;\n"
        "  char line_text[96];\n"
        "  va_list args;\n"
        "\n"
        "  va_start(args, fmt);\n"
        "  vsnprintf(line_text, sizeof line_text, fmt, args);\n"
        "  va_end(args);\n"
        "\n"
        "  glRasterPos2f(x, y);\n"
        "  for (ch = line_text; *ch; ch++)\n"
        "    glutBitmapCharacter(GLUT_BITMAP_9_BY_15, (unsigned char)*ch);\n"
        "}\n"
        "\nstatic void %s(void) {\n"
        "  int %s = %s;\n"
        "  int %s = %s;\n"
        "  float %s = (float)%s - 18.0f;\n"
        "\n"
        "  glMatrixMode(GL_PROJECTION);\n"
        "  glPushMatrix();\n"
        "  glLoadIdentity();\n"
        "  glOrtho(0, %s, 0, %s, -1, 1);\n"
        "\n"
        "  glMatrixMode(GL_MODELVIEW);\n"
        "  glPushMatrix();\n"
        "  glLoadIdentity();\n"
        "\n"
        "  glPushAttrib(GL_ALL_ATTRIB_BITS);\n"
        "  glDisable(GL_LIGHTING);\n"
        "  glDisable(GL_DEPTH_TEST);\n"
        "  glColor3f(1.0f, 1.0f, 1.0f);\n"
        "\n",
        names->tuning_window_width,
        names->tuning_window_height,
        names->tuning_step,
        names->hud_text,
        names->draw_tuning_overlay,
        names->overlay_width,
        names->tuning_window_width,
        names->overlay_height,
        names->tuning_window_height,
        names->overlay_text_y,
        names->overlay_height,
        names->overlay_width,
        names->overlay_height);
    for (int i = 0; i < needs->tune_count; i++) {
        fprintf(f,
            "\n"
            "  %s(8.0f, %s, \"%c/%c  %s = %%.4g\", (double)%s);\n"
            "  %s -= 16.0f;\n",
            names->hud_text,
            names->overlay_text_y,
            k_tune_up_keys[i], k_tune_down_keys[i],
            needs->tune_names[i], needs->tune_names[i],
            names->overlay_text_y);
    }
    fprintf(f,
        "  glPopAttrib();\n"
        "\n"
        "  glMatrixMode(GL_PROJECTION);\n"
        "  glPopMatrix();\n"
        "  glMatrixMode(GL_MODELVIEW);\n"
        "  glPopMatrix();\n"
        "}\n");
}

/* Injected into the exported keyboard() body: decode the key (folding Shift
 * uppercase and Ctrl control-codes back to the base letter) and apply the
 * swatch step, Shift = fine and Ctrl = coarse — mirroring the in-app numeric
 * swatch and variable-panel adjustment multipliers. */
static void emit_tune_keyboard_decls(FILE *f,
                                     const ExportGeneratedNames *names) {
    fprintf(f,
        "  int %s = glutGetModifiers();\n"
        "  unsigned char %s = %s;\n"
        "  float %s = 1.0f;\n"
        "\n",
        names->tune_modifiers,
        names->tune_normalized_key,
        names->keyboard_key,
        names->tune_step_scale);
}

static void emit_tune_keyboard_handlers(FILE *f, const ExportNeeds *needs,
                                        const ExportGeneratedNames *names) {
    fprintf(f,
        "  if ((%s & GLUT_ACTIVE_CTRL) && %s >= 1 && %s <= 26) {\n"
        "    %s = (unsigned char)(%s - 1 + 'a');\n"
        "  } else if (%s >= 'A' && %s <= 'Z') {\n"
        "    %s = (unsigned char)(%s + ('a' - 'A'));\n"
        "  }\n"
        "\n"
        "  if (%s & GLUT_ACTIVE_SHIFT)\n"
        "    %s *= "
            REPL_EXPORT_STRINGIFY(GLR_ADJUST_FINE_SCALE) ";\n"
        "  if (%s & GLUT_ACTIVE_CTRL)\n"
        "    %s *= "
            REPL_EXPORT_STRINGIFY(GLR_ADJUST_COARSE_SCALE) ";\n"
        "\n",
        names->tune_modifiers,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_modifiers,
        names->tune_step_scale,
        names->tune_modifiers,
        names->tune_step_scale);
    for (int i = 0; i < needs->tune_count; i++) {
        const char *v = needs->tune_names[i];
        fprintf(f,
            "  if (%s == '%c') %s += %s(%s) * %s;\n"
            "  if (%s == '%c') %s -= %s(%s) * %s;\n",
            names->tune_normalized_key, k_tune_up_keys[i],
            v, names->tuning_step, v, names->tune_step_scale,
            names->tune_normalized_key, k_tune_down_keys[i],
            v, names->tuning_step, v, names->tune_step_scale);
    }
}

static void emit_export_display_tail(FILE *f, const ExportNeeds *needs,
                                     const ReplExportLayout *layout,
                                     const ExportGeneratedNames *names) {
    int include_tess = needs ? needs->needs_tess : 0;
    int knobs = needs ? needs->tune_count : 0;

    if (knobs > 0)
        fprintf(f, "  %s();\n", names->draw_tuning_overlay);
    fprintf(f,
        "  glPopAttrib();\n"
        "  glutSwapBuffers();\n"
        "}\n"
        "\n"
        "/* Keep the projection matched to the current window size. */\n"
        "void reshape(int w, int h) {\n");
    if (knobs > 0)
        fprintf(f, "  %s = w;\n  %s = h;\n",
                names->tuning_window_width,
                names->tuning_window_height);
    fprintf(f,
        "  glViewport(0, 0, w, h);\n"
        "  glMatrixMode(GL_PROJECTION);\n"
        "  glLoadIdentity();\n");
    {
        const char *proj[REPL_EXPORT_PROJ_LINES];
        int pn = repl_export_reshape_projection_lines(proj);
        for (int j = 0; j < pn; j++)
            fprintf(f, "%s\n", proj[j]);
    }
    fprintf(f,
        "  glMatrixMode(GL_MODELVIEW);\n"
        "}\n"
        "\n"
        "/* Keyboard controls: Space toggles rotation; Esc exits. */\n"
        "void keyboard(unsigned char %s, int %s, int %s) {\n",
        names->keyboard_key,
        names->keyboard_mouse_x,
        names->keyboard_mouse_y);
    if (knobs > 0)
        emit_tune_keyboard_decls(f, names);
    fprintf(f,
        "  (void)%s;\n"
        "  (void)%s;\n",
        names->keyboard_mouse_x,
        names->keyboard_mouse_y);
    if (knobs > 0)
        fprintf(f, "\n");
    if (knobs > 0)
        emit_tune_keyboard_handlers(f, needs, names);
    fprintf(f,
        "  if (%s == ' ')\n"
        "    g_rotating = !g_rotating;\n"
        "  if (%s == 27)\n"
        "    exit(0);\n"
        "}\n"
        "\n"
        "/* Advance animation at roughly 60 frames per second. */\n"
        "void tick(int value) {\n"
        "  (void)value;\n"
        "  /* Fixed-step time advance, matching the live REPL's\n"
        "   * repl_state_time_advance(0.016) timer. Keeps tDelta = (t -\n"
        "   * tLast) * 10 constant across frames, independent of how long\n"
        "   * each render actually takes. */\n"
        "  t += 0.016f;\n"
        "  if (g_rotating)\n"
        "    g_angle += 0.5f;\n"
        "  glutPostRedisplay();\n"
        "  glutTimerFunc(16, tick, 0);\n"
        "}\n"
        "\n"
        "/* One-time OpenGL state setup. */\n"
        "void init(void) {\n"
        "  glLineWidth(1.5f);\n",
        names->keyboard_key,
        names->keyboard_key);
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
                                const ReplExportLayout *layout,
                                const ExportGeneratedNames *names) {
    emit_export_display_begin(f);
    emit_export_display_geometry(f, names);
    emit_export_display_tail(f, needs, layout, names);
}

typedef void (*ExportScaffoldSectionEmitFn)(FILE *f,
                                            const ExportScaffoldContext *ctx);

typedef struct {
    ExportScaffoldSectionEmitFn emit;
} ExportScaffoldSectionSpec;

static void emit_export_banner_section(FILE *f,
                                       const ExportScaffoldContext *ctx) {
    (void)ctx;
    fprintf(f,
        "/* Generated by gl-repl. */\n"
        "/* */\n"
        "/* This is a standalone GLUT/OpenGL C file. You can edit it by hand, */\n"
        "/* or load it back into gl-repl to keep working on the scene. */\n"
        "/* */\n"
        "/* Linux: */\n"
        "/*   cc -std=c89 -Wall -Wextra -o scene scene.c -lglut -lGL -lGLU -lm */\n"
        "/* */\n"
        "/* macOS: */\n"
        "/*   cc -std=c89 -Wall -Wextra -o scene scene.c -framework OpenGL -framework GLUT -lm */\n"
        "/* */\n"
        "/* SPDX-License-Identifier: MIT */\n"
        "/* See the gl-repl repository LICENSE file for the full text. */\n"
        "\n");
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
    emit_export_header_pre(f, ctx ? &ctx->needs : NULL);
}

static void emit_export_glfloat_helpers_section(FILE *f,
                                                const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_glfloat_vector_helpers(f);
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
    if (!ctx || !ctx->needs.needs_rand)
        return;
    (void)ctx;
    write_rand_helper(f);
}

static void emit_export_label_helper_section(FILE *f,
                                             const ExportScaffoldContext *ctx) {
    if (!ctx || !ctx->needs.needs_label)
        return;
    (void)ctx;
    write_label_helper(f);
}

static void emit_export_tess_preamble_section(FILE *f,
                                              const ExportScaffoldContext *ctx) {
    if (!ctx || !ctx->needs.needs_tess)
        return;
    (void)ctx;
    write_tess_preamble(f);
}

static void emit_export_save_restore_section(FILE *f,
                                             const ExportScaffoldContext *ctx) {
    (void)ctx;
    if (!export_display_has_multiple_enabled_passes() ||
        !export_has_persistent_predef_vars())
        return;
    write_save_restore_helpers(f);
}

static void emit_export_functions_section(FILE *f,
                                          const ExportScaffoldContext *ctx) {
    (void)ctx;
    write_func_defs_as_c(f);
}

static void emit_export_render_helper_section(FILE *f,
                                              const ExportScaffoldContext *ctx) {
    if (!ctx) return;
    write_render_helper_as_c(f, ctx->names.draw_scene);
}

static void emit_export_tune_section(FILE *f,
                                     const ExportScaffoldContext *ctx) {
    if (!ctx || ctx->needs.tune_count <= 0)
        return;
    write_tune_helpers(f, &ctx->needs, &ctx->names);
}

static void emit_export_display_section(FILE *f,
                                        const ExportScaffoldContext *ctx) {
    emit_export_display(f, &ctx->needs, ctx->layout, &ctx->names);
}

/* Section order is the exported C ABI: imports and compile tests assume it.
 * The tune helpers must precede the display section so display()/keyboard()/
 * reshape() can reference tune_compute_step/draw_tunable_overlay/the globals. */
static const ExportScaffoldSectionSpec EXPORT_SCAFFOLD_SECTIONS[] = {
    { emit_export_banner_section },
    { emit_export_workspace_metadata_section },
    { emit_export_header_section },
    { emit_export_glfloat_helpers_section },
    { emit_export_predef_globals_section },
    { emit_export_scratch_globals_section },
    { emit_export_rand_helper_section },
    { emit_export_label_helper_section },
    { emit_export_tess_preamble_section },
    { emit_export_save_restore_section },
    { emit_export_functions_section },
    { emit_export_render_helper_section },
    { emit_export_tune_section },
    { emit_export_display_section },
};

static void emit_export_scaffold(FILE *f, const ExportScaffoldContext *ctx) {
    for (size_t i = 0; i < sizeof(EXPORT_SCAFFOLD_SECTIONS) /
                           sizeof(EXPORT_SCAFFOLD_SECTIONS[0]); i++) {
        EXPORT_SCAFFOLD_SECTIONS[i].emit(f, ctx);
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
    export_generated_names_init(&scaffold.names);

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

void repl_dump_code_panel_text(FILE *out, SourceTextView text) {
    FILE *dst = out ? out : stdout;

    s_export_text_view = text;

    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();

    /* Print config settings as block comments at the top of the dump,
     * mirroring the saved C file's workspace header but omitting the
     * volatile runtime state like @var. */
    if (g_export_cfg_bridge && g_export_cfg_bridge->fill_all) {
        ReplConfigBag cfg;
        repl_config_bag_clear(&cfg);
        g_export_cfg_bridge->fill_all(&cfg);
        for (int i = 0; i < cfg.count; i++) {
            fprintf(dst, "/* @cfg %s = %s */\n", cfg.items[i].key, cfg.items[i].value);
        }
    }

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
    for (int cam_line_idx = 0; cam_line_idx < REPL_EXPORT_CAMERA_LINES; cam_line_idx++)
        fprintf(dst, "%s\n", g_cam_lines[cam_line_idx]);

    fprintf(dst, "--- header_post ---\n");
    /* Dump post-header lines (light setup, etc). */
    for (int line_idx = 0; g_header_post[line_idx]; line_idx++)
        fprintf(dst, "%s\n", g_header_post[line_idx]);

    fprintf(dst, "--- source ---\n");
    /* Dump all valid user commands. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds()[cmd_idx].valid) continue;
        fprintf(dst, "%s\n", export_document_text(cmd_idx));
    }

    fflush(dst);
}
