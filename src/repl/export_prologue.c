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
 * save/restore around the executor. `t` is not saved - every pass
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

/* Must stay bit-identical to expr_rand01 in eval.c, seed offset included.
 * The `seed + 0.5f` is not cosmetic: it keeps seed == 0 off the sin() zero
 * crossing, and without it rand(0, 0) is exactly 0 and rand(0, iter)
 * collapses to a 1-D sweep. Dropping it here made every rand() scene render
 * differently once exported - silently, because the two hashes agree on
 * nothing but the call count. test_export_trace_parity --full compares the
 * values now, which is what surfaced it. */
void write_rand_helper(FILE *f) {
    fprintf(f,
        "\nstatic float repl_randf(float seed, float iter) {\n"
        "  float h = sinf((seed + 0.5f) * 12.9898f + iter * 78.233f) * 43758.5453f;\n"
        "  float frac = h - floorf(h);\n"
        "  if (frac < 0.0f) frac += 1.0f;\n"
        "  return frac;\n"
        "}\n"
        "\nstatic float repl_rand2f(float seed, float iter) {\n"
        "  return repl_randf(seed, iter) * 2.0f - 1.0f;\n"
        "}\n");
}

/* Shaping builtins with no libm twin. Bodies must stay identical to
 * builtin_clamp / builtin_lerp / builtin_smoothstep / builtin_sign in
 * src/repl/eval.c - the exported binary and the live REPL are supposed to
 * render the same frame, and these are pure math with no GL state to hide a
 * divergence. */
void write_shape_helpers(FILE *f, const ExportNeeds *needs) {
    if (!needs)
        return;
    /* One gate per helper rather than one for the group: an exported file
     * compiled with -Wall would warn on every static the scene doesn't
     * call. */
    if (needs->needs_clamp)
        fprintf(f,
            "\nstatic float repl_clampf(float x, float lo, float hi) {\n"
            "  if (x < lo) return lo;\n"
            "  if (x > hi) return hi;\n"
            "  return x;\n"
            "}\n");
    if (needs->needs_lerp)
        fprintf(f,
            "\nstatic float repl_lerpf(float a, float b, float s) {\n"
            "  return a + (b - a) * s;\n"
            "}\n");
    if (needs->needs_smoothstep)
        fprintf(f,
            "\nstatic float repl_smoothstepf(float e0, float e1, float x) {\n"
            "  float span = e1 - e0;\n"
            "  float u;\n"
            "  if (fabsf(span) < 1e-9f) return x < e0 ? 0.0f : 1.0f;\n"
            "  u = (x - e0) / span;\n"
            "  if (u < 0.0f) u = 0.0f;\n"
            "  if (u > 1.0f) u = 1.0f;\n"
            "  return u * u * (3.0f - 2.0f * u);\n"
            "}\n");
    if (needs->needs_sign)
        fprintf(f,
            "\nstatic float repl_signf(float x) {\n"
            "  if (x > 0.0f) return 1.0f;\n"
            "  if (x < 0.0f) return -1.0f;\n"
            "  return 0.0f;\n"
            "}\n");
}

static const char *const g_glfloat1_helper_lines[] = {
    "static GLfloat *" REPL_EXPORT_GLFLOAT1_HELPER "(GLfloat a) {",
    "  static GLfloat repl_glfloat1_buf[1];",
    "  repl_glfloat1_buf[0] = a;",
    "  return repl_glfloat1_buf;",
    "}",
    NULL
};

static const char *const g_glfloat3_helper_lines[] = {
    "static GLfloat *" REPL_EXPORT_GLFLOAT3_HELPER
        "(GLfloat a, GLfloat b, GLfloat c) {",
    "  static GLfloat repl_glfloat3_buf[3];",
    "  repl_glfloat3_buf[0] = a;",
    "  repl_glfloat3_buf[1] = b;",
    "  repl_glfloat3_buf[2] = c;",
    "  return repl_glfloat3_buf;",
    "}",
    NULL
};

static const char *const g_glfloat4_helper_lines[] = {
    "static GLfloat *" REPL_EXPORT_GLFLOAT4_HELPER
        "(GLfloat a, GLfloat b, GLfloat c, GLfloat d) {",
    "  static GLfloat repl_glfloat4_buf[4];",
    "  repl_glfloat4_buf[0] = a;",
    "  repl_glfloat4_buf[1] = b;",
    "  repl_glfloat4_buf[2] = c;",
    "  repl_glfloat4_buf[3] = d;",
    "  return repl_glfloat4_buf;",
    "}",
    NULL
};

static const char *const g_gldouble4_helper_lines[] = {
    "static GLdouble *" REPL_EXPORT_GLDOUBLE4_HELPER
        "(GLdouble a, GLdouble b, GLdouble c, GLdouble d) {",
    "  static GLdouble repl_gldouble4_buf[4];",
    "  repl_gldouble4_buf[0] = a;",
    "  repl_gldouble4_buf[1] = b;",
    "  repl_gldouble4_buf[2] = c;",
    "  repl_gldouble4_buf[3] = d;",
    "  return repl_gldouble4_buf;",
    "}",
    NULL
};

/* Sixteen named parameters rather than varargs: exported C remains C89 and
 * type-checked, and the glMultMatrixf call site always passes exactly 16. */
static const char *const g_glfloat16_helper_lines[] = {
    "static GLfloat *" REPL_EXPORT_GLFLOAT16_HELPER
        "(GLfloat m0, GLfloat m1, GLfloat m2, GLfloat m3,",
    "                   GLfloat m4, GLfloat m5, GLfloat m6, GLfloat m7,",
    "                   GLfloat m8, GLfloat m9, GLfloat m10, GLfloat m11,",
    "                   GLfloat m12, GLfloat m13, GLfloat m14, GLfloat m15) {",
    "  static GLfloat repl_glfloat16_buf[16];",
    "  repl_glfloat16_buf[0] = m0;   repl_glfloat16_buf[1] = m1;",
    "  repl_glfloat16_buf[2] = m2;   repl_glfloat16_buf[3] = m3;",
    "  repl_glfloat16_buf[4] = m4;   repl_glfloat16_buf[5] = m5;",
    "  repl_glfloat16_buf[6] = m6;   repl_glfloat16_buf[7] = m7;",
    "  repl_glfloat16_buf[8] = m8;   repl_glfloat16_buf[9] = m9;",
    "  repl_glfloat16_buf[10] = m10; repl_glfloat16_buf[11] = m11;",
    "  repl_glfloat16_buf[12] = m12; repl_glfloat16_buf[13] = m13;",
    "  repl_glfloat16_buf[14] = m14; repl_glfloat16_buf[15] = m15;",
    "  return repl_glfloat16_buf;",
    "}",
    NULL
};

typedef struct {
    unsigned bit;
    const char *const *lines;
} ExportGlVectorHelper;

static const ExportGlVectorHelper g_gl_vector_helpers[] = {
    /* bit,                           lines */
    { REPL_EXPORT_GL_VECTOR_FLOAT1,   g_glfloat1_helper_lines  },
    { REPL_EXPORT_GL_VECTOR_FLOAT3,   g_glfloat3_helper_lines  },
    { REPL_EXPORT_GL_VECTOR_FLOAT4,   g_glfloat4_helper_lines  },
    { REPL_EXPORT_GL_VECTOR_DOUBLE4,  g_gldouble4_helper_lines },
    { REPL_EXPORT_GL_VECTOR_FLOAT16,  g_glfloat16_helper_lines },
};

static unsigned export_gl_vector_mask_from_needs(const ExportNeeds *needs) {
    unsigned mask = 0;
    if (!needs) return 0;
    if (needs->needs_glfloat1)  mask |= REPL_EXPORT_GL_VECTOR_FLOAT1;
    if (needs->needs_glfloat3)  mask |= REPL_EXPORT_GL_VECTOR_FLOAT3;
    if (needs->needs_glfloat4)  mask |= REPL_EXPORT_GL_VECTOR_FLOAT4;
    if (needs->needs_gldouble4) mask |= REPL_EXPORT_GL_VECTOR_DOUBLE4;
    if (needs->needs_glfloat16) mask |= REPL_EXPORT_GL_VECTOR_FLOAT16;
    return mask;
}

unsigned repl_export_gl_vector_helper_mask(void) {
    ExportNeeds needs = export_collect_needs();
    return export_gl_vector_mask_from_needs(&needs);
}

int repl_export_gl_vector_helper_line_count(unsigned mask) {
    int count = mask ? 1 : 0; /* Section comment. */
    for (size_t i = 0; i < sizeof(g_gl_vector_helpers) /
                           sizeof(g_gl_vector_helpers[0]); i++) {
        if (!(mask & g_gl_vector_helpers[i].bit))
            continue;
        count++; /* Blank separator before each helper. */
        for (int j = 0; g_gl_vector_helpers[i].lines[j]; j++)
            count++;
    }
    return count;
}

int repl_export_gl_vector_helper_line(unsigned mask, int line_idx,
                                      char *out, size_t out_size) {
    const char *line = NULL;

    if (!out || out_size == 0 || line_idx < 0)
        return 0;
    if (mask && line_idx-- == 0)
        line = "/* C89 replacements for C99 compound GL literals. */";
    for (size_t i = 0; !line && i < sizeof(g_gl_vector_helpers) /
                                    sizeof(g_gl_vector_helpers[0]); i++) {
        if (!(mask & g_gl_vector_helpers[i].bit))
            continue;
        if (line_idx-- == 0) {
            line = "";
            break;
        }
        for (int j = 0; g_gl_vector_helpers[i].lines[j]; j++) {
            if (line_idx-- == 0) {
                line = g_gl_vector_helpers[i].lines[j];
                break;
            }
        }
    }
    if (!line) {
        out[0] = '\0';
        return 0;
    }
    snprintf(out, out_size, "%s", line);
    return 1;
}

void write_glfloat_vector_helpers(FILE *f, const ExportNeeds *needs) {
    unsigned mask = export_gl_vector_mask_from_needs(needs);
    int line_count;

    if (!f || !mask)
        return;
    line_count = repl_export_gl_vector_helper_line_count(mask);
    fprintf(f, "\n");
    for (int i = 0; i < line_count; i++) {
        char line[MAX_LINE_LEN];
        repl_export_gl_vector_helper_line(mask, i, line, sizeof(line));
        fprintf(f, "%s\n", line);
    }
}
/* Wrapper for the REPL `label("fmt", ...)` primitive. Walks the format
 * string and substitutes each `%f` with `%g`-formatted output (matching
 * the REPL's CMD_LABEL executor case in src/repl/executor.c) so the live
 * REPL and exported binary render identical text. Using vsnprintf with
 * the raw format would print `1.000000` for `%f` while the REPL prints
 * `1` - that divergence breaks visual round-trips.
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
    write_render_body_range_as_c(f, 0, repl_state_document_count(), 1, 0);
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

static void export_func_c_signature(int cmd_idx,
                                    char *fn_name, size_t fn_name_sz,
                                    char param_names[][REPL_PREDEF_NAME_MAX],
                                    int *param_count) {
    const GLCmd *cmd = &repl_state_document_cmds()[cmd_idx];
    int fn = (int)cmd->args[0];
    int parsed_fn = fn;
    int count = 0;

    if (!parse_repl_func_signature(export_document_text(cmd_idx), &parsed_fn,
                                   param_names, MAX_EXPR_VARS, &count)) {
        parsed_fn = fn;
        count = 0;
    }

    {
        const char *alias = repl_func_alias_get(parsed_fn);
        if (alias)
            snprintf(fn_name, fn_name_sz, "%s", alias);
        else
            snprintf(fn_name, fn_name_sz, "func%d", parsed_fn);
    }
    *param_count = count;
}

static void write_func_signature(FILE *f, const char *fn_name,
                                 char param_names[][REPL_PREDEF_NAME_MAX],
                                 int param_count, int prototype) {
    fprintf(f, "static void %s(", fn_name);
    if (param_count <= 0) {
        fputs("void", f);
    } else {
        for (int param_idx = 0; param_idx < param_count; param_idx++)
            fprintf(f, "%sfloat %s", param_idx == 0 ? "" : ", ",
                    param_names[param_idx]);
    }
    fprintf(f, ")%s\n", prototype ? "; /* " REPL_EXPORT_FORWARD_DECL_MARKER " */"
                                    : " {");
}

void write_func_defs_as_c(FILE *f) {
    /* Keep authored function order for the export/import round trip, but
     * declare every function first so forward and mutual calls are valid C. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds()[cmd_idx].valid ||
            repl_state_document_cmds()[cmd_idx].type != CMD_FUNC_DEF)
            continue;

        char fn_name[REPL_FUNC_NAME_MAX + 8];
        char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
        int param_count;
        export_func_c_signature(cmd_idx, fn_name, sizeof(fn_name),
                                param_names, &param_count);
        write_func_signature(f, fn_name, param_names, param_count, 1);
    }

    /* Iterate through all document commands looking for function definitions. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds()[cmd_idx].valid || repl_state_document_cmds()[cmd_idx].type != CMD_FUNC_DEF) continue;
        int comment_start = cmd_idx;
        while (comment_start > 0 &&
               repl_state_document_cmds()[comment_start - 1].valid &&
               (repl_state_document_cmds()[comment_start - 1].type == CMD_COMMENT ||
                repl_state_document_cmds()[comment_start - 1].type == CMD_EMPTY))
            comment_start--;
        /* Emit any preceding comment and empty lines. */
        for (int comment_idx = comment_start; comment_idx < cmd_idx; ) {
            fputc('\n', f);
            if (!repl_state_document_cmds()[comment_idx].valid) {
                comment_idx++;
            } else if (repl_state_document_cmds()[comment_idx].type == CMD_EMPTY) {
                fputc('\n', f);
                comment_idx++;
            } else if (repl_state_document_cmds()[comment_idx].type == CMD_COMMENT) {
                int block_end = comment_idx + 1;
                while (block_end < cmd_idx &&
                       block_end < repl_state_document_count()) {
                    if (!repl_state_document_cmds()[block_end].valid) {
                        block_end++;
                        continue;
                    }
                    if (repl_state_document_cmds()[block_end].type != CMD_COMMENT)
                        break;
                    block_end++;
                }
                export_write_comment_run_as_c(f, comment_idx, block_end);
                comment_idx = block_end;
            } else {
                comment_idx++;
            }
        }

        char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
        int fe = find_export_block_end(cmd_idx);
        /* Emit the C function under the user's alias when one is
         * registered (round-tripped via the `// @func N = name`
         * directive), falling back to the bare `funcN` slot name. The
         * call sites keep their canonical alias text, so defining the
         * body under the same name is what keeps the standalone .c
         * self-consistent and compilable. */
        char fn_name[REPL_FUNC_NAME_MAX + 8];
        int param_count;
        export_func_c_signature(cmd_idx, fn_name, sizeof(fn_name),
                                param_names, &param_count);
        fputc('\n', f);
        write_func_signature(f, fn_name, param_names, param_count, 0);
        write_render_body_range_as_c(f, cmd_idx + 1, fe, 0, 1);
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
