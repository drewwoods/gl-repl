#include "repl/export_internal.h"
#include "repl/text_helpers.h"

/* Predef vars other than `t` carry their snapshot value forward into
 * the next frame. `t` is set per-frame from glutGet at the top of
 * display(), so its static initializer is irrelevant. */
static int export_predef_var_persists(int var_idx) {
    return strcmp(g_predef_vars[var_idx].name, "t") != 0;
}

int export_has_persistent_predef_vars(void) {
    for (int i = 0; i < g_num_predef_vars; i++)
        if (export_predef_var_persists(i))
            return 1;
    return 0;
}

void write_predef_var_globals(FILE *f) {
    if (g_num_predef_vars <= 0) return;
    fprintf(f, "\n/* Scene state variables.\n"
               " * Initializers are the live snapshot at export time, so the\n"
               " * program starts in the same state the REPL preview ended in.\n"
               " * All variables (including t) carry their snapshot. */\n");
    for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++) {
        const char *name = g_predef_vars[var_idx].name;
        char vbuf[32];
        export_format_decl_float(vbuf, sizeof(vbuf),
                                 g_predef_vars[var_idx].value);
        fprintf(f, "static float %s = %s;\n", name, vbuf);
    }
}

/* Multipass save/restore: capture the predef-var state once at the top
 * of each frame and restore it before every pass after the first, so
 * each pass sees the same starting state but the LAST pass's mutations
 * persist into the next frame. Mirrors the live REPL's per-frame
 * save/restore around the executor. `t` is not saved — every pass
 * sees the same per-frame `t` value set in display(). */
void write_save_restore_helpers(FILE *f) {
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

void write_rand_helper(FILE *f) {
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

void write_glfloat_vector_helpers(FILE *f) {
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
        REPL_EXPORT_GLFLOAT1_HELPER,
        REPL_EXPORT_GLFLOAT3_HELPER,
        REPL_EXPORT_GLFLOAT4_HELPER);
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
void write_label_helper(FILE *f) {
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

void write_render_helper_as_c(FILE *f, const char *name) {
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

void write_func_defs_as_c(FILE *f) {
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

void write_tess_preamble(FILE *f) {
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
