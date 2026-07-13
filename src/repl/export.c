#include "repl/export_internal.h"
#include "repl/text_helpers.h"

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
 * render passes (which straddle render3d_draw_scene) agree. See
 * docs/ARCHITECTURE.md, "Rule — where a per-frame dynamic value is resolved". */
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

void export_format_decl_float(char *buf, size_t n, float v) {
    repl_format_source_float(buf, (int)n, v);
}

/* Join `count` floats (count <= 4) as "a, b, c, d" using the shortest
 * exact-round-trip representation (repl_format_source_float) rather than
 * %.9g. Same bit-exact reload guarantee, but a value of 0.8f reads as
 * "0.8" instead of %.9g's "0.800000012". Used for the light
 * color/position vectors shared by the code panel and export. */
void export_format_float_list(char *buf, size_t n,
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

void export_set_source_text_view(SourceTextView text) {
    s_export_text_view = text;
}

SourceTextView export_source_text_view(void) {
    return s_export_text_view;
}

const char *export_document_text(int cmd_idx) {
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

void export_write_c89_line(FILE *f, const char *line) {
    char out[MAX_LINE_LEN * 2];

    export_format_c89_comment_line(out, sizeof(out), line ? line : "");
    fprintf(f, "%s\n", out);
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

    export_set_source_text_view(text);

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

void repl_save_default_output(const ReplExportLayout *layout) {
    (void)repl_export_save_output("output.c", source_document_view(), layout);
}

void repl_dump_code_panel_text(FILE *out, SourceTextView text) {
    FILE *dst = out ? out : stdout;

    export_set_source_text_view(text);

    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();

    /* Print config settings as block comments at the top of the dump,
     * mirroring the saved C file's workspace header but omitting the
     * volatile runtime state. */
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
    fprintf(dst, "--- render_state ---\n");
    /* Dump render state configuration. */
    for (int state_line_idx = 0; state_line_idx < RENDER_STATE_LINE_COUNT; state_line_idx++)
        fprintf(dst, "%s\n", g_render_state_lines[state_line_idx]);

    fprintf(dst, "--- lights_pre_camera ---\n");
    {
        int n = repl_export_lights_pre_camera_line_count();
        for (int i = 0; i < n; i++) {
            char line[MAX_LINE_LEN];
            repl_export_lights_pre_camera_line(i, line, sizeof(line));
            fprintf(dst, "%s\n", line);
        }
    }

    fprintf(dst, "--- camera ---\n");
    /* Dump the optional authored heading plus camera transformation lines. */
    if (g_camera_comment_line[0])
        fprintf(dst, "%s\n", g_camera_comment_line);
    for (int cam_line_idx = 0; cam_line_idx < REPL_EXPORT_CAMERA_LINES; cam_line_idx++)
        fprintf(dst, "%s\n", g_cam_lines[cam_line_idx]);

    fprintf(dst, "--- header_post ---\n");
    /* Dump post-header lines (light setup, etc). */
    for (int line_idx = 0; g_header_post[line_idx]; line_idx++)
        fprintf(dst, "%s\n", g_header_post[line_idx]);
    /* Panel-only scratch declaration, adjacent to the source that uses it. */
    fprintf(dst, "%s\n", REPL_CODE_PANEL_SCRATCH_DECL_LINE);

    fprintf(dst, "--- source ---\n");
    /* Dump all valid user commands. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds()[cmd_idx].valid) continue;
        fprintf(dst, "%s\n", export_document_text(cmd_idx));
    }

    fflush(dst);
}
