#include "editor/state.h"
#include "app/glr_camera.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
// For linux mkdtemp
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "repl/example_loader.h"  /* repl_load_example_lines */
#include "repl/examples.h"
#include "repl/state_owners.h"
#include <math.h>
#include "repl/export.h"
#include "source_document.h"
#include "repl/text_helpers.h"
#include "repl/util.h"
#include "ui/app/layout.h"      /* CODE_PANEL_LAYOUT_* enum values */
#include "ui/app/state.h"
#include "render3d/render.h"
#include "render3d/themes.h"       /* GRID_THEME_*, AXES_THEME_*, RENDER3D_BACKDROP_* */
#include "app/glr_defaults.h"   /* CFG_DEFAULT_* */
#include "support/cpuprof.h"   /* prof_histogram_reset on example load */

#define g_accum_effect        (glr_state_render_mut()->accum_effect)
#define g_multisample_enabled (glr_state_render_mut()->multisample_enabled)
#define g_line_smooth_enabled (glr_state_render_mut()->line_smooth_enabled)

#include "support/test_harness.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;
static int g_verbose = 0;
static int g_use_color = 0;
static int g_show_mismatch = 0;
static int g_keep_temp = 0;

#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_RED   "\033[31m"
#define ANSI_RESET "\033[0m"

static const char *ansi_green(void) {
    return g_use_color ? ANSI_GREEN : "";
}

static const char *ansi_red(void) {
    return g_use_color ? ANSI_RED : "";
}

static const char *ansi_yellow(void) {
    return g_use_color ? ANSI_YELLOW : "";
}

static const char *ansi_reset(void) {
    return g_use_color ? ANSI_RESET : "";
}

static int env_truthy(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static void assert_true_impl(const char *label, int cond, int line) {
    g_harness.run++;
    if (cond) {
        g_harness.passed++;
        return;
    }
    printf("%sFAIL%s [%s] (line %d)\n", ansi_red(), ansi_reset(), label, line);
}

#define ASSERT_TRUE(label, cond) assert_true_impl(label, (cond), __LINE__)

static void log_example_step(int idx, const char *name,
                             const char *step, const char *detail) {
    if (!g_verbose)
        return;
    printf("example %02d [%s] %s", idx, name ? name : "(unnamed)", step);
    if (detail && detail[0])
        printf(": %s", detail);
    printf("\n");
    fflush(stdout);
}

static void log_example_result(int idx, const char *name, const char *step,
                               int pass, const char *detail) {
    if (!g_verbose && pass)
        return;

    printf("example %02d [%s] %s: %s%s%s",
           idx, name ? name : "(unnamed)", step,
           pass ? ansi_green() : ansi_red(),
           pass ? "PASS" : "FAIL",
           ansi_reset());
    if (detail && detail[0])
        printf(" (%s)", detail);
    printf("\n");
    fflush(stdout);
}

static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("y", err, sizeof(err));
    repl_eval_declare_predef_var("z", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
    repl_eval_declare_predef_var("j", err, sizeof(err));
    repl_eval_declare_predef_var("k", err, sizeof(err));
    repl_eval_declare_predef_var("a", err, sizeof(err));
    repl_eval_declare_predef_var("b", err, sizeof(err));
    repl_eval_declare_predef_var("c", err, sizeof(err));
    repl_eval_declare_predef_var("n", err, sizeof(err));
}

static void pin_code_panel_state(void) {
    glr_camera_set_orbit(18.0f, 32.0f);
    glr_camera_set_distance(5.5f);
    glr_camera_set_pan(0.0f, 0.0f, 0.0f);
    glr_state_presentation_mut()->axes_theme = CFG_DEFAULT_AXES_THEME;
    glr_state_presentation_mut()->backdrop_mode = CFG_DEFAULT_BACKDROP_MODE;
    glr_state_presentation_mut()->show_vertex_outlines = CFG_DEFAULT_VERTEX_OUTLINES;
    g_accum_effect = RENDER3D_ACCUM_EFFECT_AA;
    glr_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT; glr_ctrl_sync_ui_chrome();
    g_multisample_enabled = CFG_DEFAULT_MULTISAMPLE;
    g_line_smooth_enabled = CFG_DEFAULT_LINE_SMOOTH;
}

static void seed_nondefault_example_presentation_state(void) {
    glr_camera_set_orbit(-41.0f, 73.0f);
    glr_camera_set_distance(12.0f);
    glr_camera_set_pan(1.5f, -2.0f, 0.75f);
    glr_state_presentation_mut()->wireframe = 1;
    glr_state_presentation_mut()->grid_theme = 1;
    glr_state_presentation_mut()->grid_major_idx = GRID_MAJOR_10;
    glr_state_presentation_mut()->grid_extent_idx = GRID_EXTENT_CLOSE;
    glr_state_presentation_mut()->axes_theme = 5;
    glr_state_presentation_mut()->show_vertex_labels = 0;
    glr_ctrl_sync_ui_chrome();
    glr_state_presentation_mut()->show_normal_vectors = 1;
    glr_state_presentation_mut()->show_vertex_outlines = 0;
    glr_state_presentation_mut()->show_vertex_points = 0;
    glr_state_presentation_mut()->xform_guide_mode = RENDER3D_XFORM_GUIDE_OFF;
    glr_state_presentation_mut()->show_light_indicators = 0;
    glr_state_presentation_mut()->backdrop_mode = 1;
    glr_camera_mut()->auto_rotate = 1;
}

static int camera_pose_near(float rx, float ry, float dist,
                            float tx, float ty, float tz) {
    GlrCameraState cam = glr_camera();
    return fabsf(cam.rx - rx) < 1e-4f &&
           fabsf(cam.ry - ry) < 1e-4f &&
           fabsf(cam.dist - dist) < 1e-4f &&
           fabsf(cam.tx - tx) < 1e-4f &&
           fabsf(cam.ty - ty) < 1e-4f &&
           fabsf(cam.tz - tz) < 1e-4f;
}

static void settle_camera_transition_for_test(void) {
    for (int i = 0; i < 140; i++)
        glr_camera_tick();
}

static char *slurp_stream(FILE *f) {
    char *buf;
    long len;
    size_t nread;

    if (!f)
        return NULL;
    fflush(f);
    if (fseek(f, 0, SEEK_END) != 0)
        return NULL;
    len = ftell(f);
    if (len < 0)
        return NULL;
    if (fseek(f, 0, SEEK_SET) != 0)
        return NULL;

    buf = (char *)malloc((size_t)len + 1);
    if (!buf)
        return NULL;
    nread = fread(buf, 1, (size_t)len, f);
    buf[nread] = '\0';
    return buf;
}

static char *slurp_path(const char *path) {
    FILE *f = fopen(path, "r");
    char *buf = slurp_stream(f);
    if (f)
        fclose(f);
    return buf;
}

static int write_text_path(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    if (fputs(text, f) == EOF) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static char *dump_current_code_panel_text(void) {
    FILE *tmp = tmpfile();
    char *buf;
    if (!tmp)
        return NULL;
    repl_dump_code_panel_text(tmp, source_document_view());
    buf = slurp_stream(tmp);
    fclose(tmp);
    return buf;
}

static int compile_exported_source(int idx, const char *name,
                                   const char *src_path,
                                   char *detail, size_t detail_sz) {
    const char *cc = getenv("REPL_EXPORT_CC");
    const char *cflags = getenv("REPL_EXPORT_COMPILE_CFLAGS");
    char obj_path[512];
    char log_path[512];
    char cmd[4096];
    char *log;
    int rc;

    if (!cc || !cc[0])
        cc = "cc";
    if (!cflags || !cflags[0])
        cflags = "-std=c2x -DGL_SILENCE_DEPRECATION -Iinclude";

    snprintf(obj_path, sizeof(obj_path), "%s.o", src_path);
    snprintf(log_path, sizeof(log_path), "%s.log", src_path);
    snprintf(cmd, sizeof(cmd),
             "%s %s -c \"%s\" -o \"%s\" >\"%s\" 2>&1",
             cc, cflags, src_path, obj_path, log_path);
    log_example_step(idx, name, "compile cmd", cmd);

    rc = system(cmd);
    if (rc == 0) {
        log_example_result(idx, name, "compile result", 1, "ok");
        remove(obj_path);
        remove(log_path);
        return 1;
    }

    log = slurp_path(log_path);
    if (detail && detail_sz > 0) {
        if (log && log[0])
            snprintf(detail, detail_sz, "%s", log);
        else if (!repl_format_fits(detail, detail_sz, "command failed: %s", cmd))
            repl_format_fits(detail, detail_sz, "command failed");
    }
    log_example_result(idx, name, "compile result", 0, "see detail below");
    free(log);
    remove(obj_path);
    remove(log_path);
    return 0;
}

static void fixture_path_for_idx(int idx, char *out, int out_sz) {
    snprintf(out, (size_t)out_sz, "tests/testdata/repl_examples_ui/%02d.golden.txt", idx);
}

static int examples_have_no_invalid_cmds(void) {
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (!repl_state_document_cmds()[i].valid)
            return 0;
    }
    return 1;
}

/* Strip trailing `// comment` from `static float ...` decl lines. The
 * export path writes CMD_VAR_DECLARE as a fixed-grammar `// @declare ...`
 * marker that cannot carry an arbitrary trailing comment, so a decl line
 * like `static float foo = 1; // note` round-trips back as `static float
 * foo = 1;`. Stripping the comment off decl lines on both sides keeps the
 * roundtrip check honest about everything else without forcing examples
 * to drop pedagogically useful inline notes on their variable decls.
 * Other line types (assigns, calls, etc.) round-trip trailing comments
 * verbatim and are left untouched here.
 *
 * Returns a heap-allocated copy; caller must free. */
static char *strip_decl_trailing_comments(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    const char *p = src;
    char *w = out;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        size_t line_len = (size_t)(p - line_start);

        const char *scan = line_start;
        const char *line_end = line_start + line_len;
        while (scan < line_end && (*scan == ' ' || *scan == '\t')) scan++;
        int is_decl = 0;
        if ((size_t)(line_end - scan) >= 13 &&
            strncmp(scan, "static float ", 13) == 0) {
            is_decl = 1;
        }

        size_t emit_len = line_len;
        if (is_decl) {
            for (size_t i = 0; i + 1 < line_len; i++) {
                if (line_start[i] == '/' && line_start[i + 1] == '/') {
                    while (i > 0 && (line_start[i - 1] == ' ' ||
                                     line_start[i - 1] == '\t'))
                        i--;
                    emit_len = i;
                    break;
                }
            }
        }
        memcpy(w, line_start, emit_len);
        w += emit_len;
        if (*p == '\n') {
            *w++ = '\n';
            p++;
        }
    }
    *w = '\0';
    return out;
}

static int compare_exact_text(const char *expected, const char *actual, int *line_out) {
    int line = 1;
    const unsigned char *ep = (const unsigned char *)expected;
    const unsigned char *ap = (const unsigned char *)actual;

    while (*ep && *ap) {
        if (*ep != *ap) {
            if (line_out) *line_out = line;
            return 0;
        }
        if (*ep == '\n')
            line++;
        ep++;
        ap++;
    }

    if (*ep == *ap) {
        if (line_out) *line_out = line;
        return 1;
    }

    if (line_out) *line_out = line;
    return 0;
}

static int text_line_bounds(const char *text, int target_line,
                            const char **start_out, int *len_out) {
    const char *p = text;
    int line = 1;

    if (!text || target_line < 1)
        return 0;

    while (line < target_line && *p) {
        if (*p == '\n')
            line++;
        p++;
    }
    if (line != target_line || !*p)
        return 0;

    const char *start = p;
    while (*p && *p != '\n')
        p++;
    int len = (int)(p - start);
    while (len > 0 && start[len - 1] == '\r')
        len--;

    if (start_out) *start_out = start;
    if (len_out) *len_out = len;
    return 1;
}

static void print_text_line(const char *side, int line_no, const char *text,
                            int highlight) {
    const char *start = NULL;
    int len = 0;

    printf("%c %-8s %4d | ", highlight ? '>' : ' ', side, line_no);
    if (text_line_bounds(text, line_no, &start, &len))
        printf("%.*s", len, start);
    else
        printf("<missing>");
    printf("\n");
}

static void print_text_mismatch(const char *title,
                                const char *expected_label,
                                const char *actual_label,
                                const char *expected,
                                const char *actual,
                                int diff_line) {
    if (!g_show_mismatch || diff_line <= 0)
        return;

    int start = diff_line - 2;
    int end = diff_line + 2;
    if (start < 1)
        start = 1;

    printf("MISMATCH [%s] first differing line=%d\n", title, diff_line);
    printf("  expected: %s\n", expected_label ? expected_label : "(expected)");
    printf("  actual:   %s\n", actual_label ? actual_label : "(actual)");
    for (int line = start; line <= end; line++) {
        int highlight = line == diff_line;
        print_text_line("expected", line, expected, highlight);
        print_text_line("actual", line, actual, highlight);
    }
}

static int is_definition_source_line(const char *line) {
    char rhs[MAX_LINE_LEN];
    const char *p = line;

    while (*p && isspace((unsigned char)*p))
        p++;
    /* Optional canonical `static ` prefix (see format_decl_text). */
    if (strncmp(p, "static", 6) == 0 && isspace((unsigned char)p[6])) {
        p += 6;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (strncmp(p, "float", 5) == 0 &&
        !isalnum((unsigned char)p[5]) && p[5] != '_')
        return 1;
    return repl_extract_assignment_parts(p, NULL, 0, rhs, sizeof(rhs));
}

static int cmp_string_ptrs(const void *lhs, const void *rhs) {
    const char *const *left = (const char *const *)lhs;
    const char *const *right = (const char *const *)rhs;
    return strcmp(*left, *right);
}

static void canonicalize_float_literals(const char *in, char *out, int out_sz) {
    int i = 0;
    int o = 0;
    int in_len = (int)strlen(in);

    while (i < in_len && o < out_sz - 1) {
        if (isdigit((unsigned char)in[i]) ||
            (in[i] == '.' && i + 1 < in_len && isdigit((unsigned char)in[i + 1]))) {
            /* Preceding token boundary check */
            if (i > 0 && (isalnum((unsigned char)in[i - 1]) || in[i - 1] == '_')) {
                out[o++] = in[i++];
                continue;
            }
            char *endptr;
            float val = strtof(in + i, &endptr);
            int consumed = (int)(endptr - (in + i));
            if (consumed > 0) {
                int suffix_len = 0;
                if (in[i + consumed] == 'f' || in[i + consumed] == 'F') {
                    suffix_len = 1;
                }
                /* Trailing token boundary check */
                int next_char_idx = i + consumed + suffix_len;
                if (next_char_idx < in_len && (isalnum((unsigned char)in[next_char_idx]) || in[next_char_idx] == '_')) {
                    out[o++] = in[i++];
                    continue;
                }

                int total_consumed = consumed + suffix_len;
                char buf[32];
                snprintf(buf, sizeof(buf), "%.6g", (double)val);
                int blen = (int)strlen(buf);
                if (o + blen < out_sz - 1) {
                    memcpy(out + o, buf, (size_t)blen);
                    o += blen;
                }
                i += total_consumed;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

static char *canonicalize_text_floats(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    size_t out_sz = len * 2 + 1;
    char *out = (char *)malloc(out_sz);
    if (!out) return NULL;
    canonicalize_float_literals(src, out, (int)out_sz);
    return out;
}

static char *canonicalize_definition_line(const char *line) {
    char repl_line[MAX_LINE_LEN];
    char canon_float_line[MAX_LINE_LEN];
    char *comment;
    char *out;

    repl_eval_c_expr_to_repl(line, repl_line, sizeof(repl_line));
    trim_in_place(repl_line);

    comment = strstr(repl_line, "//");
    if (comment) {
        while (comment > repl_line && isspace((unsigned char)comment[-1]))
            comment--;
        *comment = '\0';
        trim_in_place(repl_line);
    }

    canonicalize_float_literals(repl_line, canon_float_line, sizeof(canon_float_line));

    /* Strip initializers from float declarations, e.g. "static float x = 1, y = 2;" -> "static float x, y;" */
    /* This allows comparing the structure of the declarations without being sensitive to live snapshot values. */
    char stripped_line[MAX_LINE_LEN];
    const char *p = canon_float_line;
    char *dst = stripped_line;
    while (*p && isspace((unsigned char)*p)) {
        *dst++ = *p++;
    }
    int is_decl = 0;
    if (strncmp(p, "static", 6) == 0 && isspace((unsigned char)p[6])) {
        const char *q = p + 6;
        while (*q && isspace((unsigned char)*q)) q++;
        if (strncmp(q, "float", 5) == 0 && (q[5] == '\0' || isspace((unsigned char)q[5]))) {
            is_decl = 1;
        }
    } else if (strncmp(p, "float", 5) == 0 && (p[5] == '\0' || isspace((unsigned char)p[5]))) {
        is_decl = 1;
    }

    if (is_decl) {
        /* Copy "static float " or "float " prefix */
        if (strncmp(p, "static", 6) == 0) {
            memcpy(dst, "static float ", 13);
            dst += 13;
            p += 6;
            while (*p && isspace((unsigned char)*p)) p++;
            p += 5; /* skip "float" */
        } else {
            memcpy(dst, "float ", 6);
            dst += 6;
            p += 5;
        }
        /* Parse variable names, ignoring "= value" */
        while (*p) {
            while (*p && isspace((unsigned char)*p)) {
                *dst++ = *p++;
            }
            if (!*p) break;
            if (isalnum((unsigned char)*p) || *p == '_') {
                while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
                    *dst++ = *p++;
                }
            } else if (*p == '=') {
                p++;
                while (*p && *p != ',' && *p != ';') {
                    p++;
                }
            } else if (*p == ',') {
                while (dst > stripped_line && isspace((unsigned char)dst[-1])) dst--;
                *dst++ = ',';
                *dst++ = ' ';
                p++;
                while (*p && isspace((unsigned char)*p)) p++;
            } else if (*p == ';') {
                while (dst > stripped_line && isspace((unsigned char)dst[-1])) dst--;
                *dst++ = ';';
                p++;
                break;
            } else {
                *dst++ = *p++;
            }
        }
        *dst = '\0';
        out = (char *)malloc(strlen(stripped_line) + 1);
        if (!out) return NULL;
        strcpy(out, stripped_line);
        return out;
    }

    out = (char *)malloc(strlen(canon_float_line) + 1);
    if (!out)
        return NULL;
    strcpy(out, canon_float_line);
    return out;
}

static char *strip_declaration_initializers(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *out = (char *)malloc(len * 2 + 512);
    if (!out) return NULL;

    const char *p = src;
    char *dst = out;
    char line[MAX_LINE_LEN];

    while (*p) {
        int li = 0;
        while (*p && *p != '\n' && li < MAX_LINE_LEN - 1) {
            line[li++] = *p++;
        }
        line[li] = '\0';
        if (*p == '\n') p++;

        /* Strip initializers if it is a declaration line */
        const char *lp = line;
        while (*lp && isspace((unsigned char)*lp)) lp++;
        int is_decl = 0;
        if (strncmp(lp, "static", 6) == 0 && isspace((unsigned char)lp[6])) {
            const char *q = lp + 6;
            while (*q && isspace((unsigned char)*q)) q++;
            if (strncmp(q, "float", 5) == 0 && (q[5] == '\0' || isspace((unsigned char)q[5]))) {
                is_decl = 1;
            }
        } else if (strncmp(lp, "float", 5) == 0 && (lp[5] == '\0' || isspace((unsigned char)lp[5]))) {
            is_decl = 1;
        }

        if (is_decl) {
            /* Format without initializers */
            const char *sp = line;
            while (*sp && isspace((unsigned char)*sp)) {
                *dst++ = *sp++;
            }
            if (strncmp(sp, "static", 6) == 0) {
                memcpy(dst, "static float ", 13);
                dst += 13;
                sp += 6;
                while (*sp && isspace((unsigned char)*sp)) sp++;
                sp += 5;
            } else {
                memcpy(dst, "float ", 6);
                dst += 6;
                sp += 5;
            }
            while (*sp) {
                while (*sp && isspace((unsigned char)*sp)) {
                    *dst++ = *sp++;
                }
                if (!*sp) break;
                if (isalnum((unsigned char)*sp) || *sp == '_') {
                    while (*sp && (isalnum((unsigned char)*sp) || *sp == '_')) {
                        *dst++ = *sp++;
                    }
                } else if (*sp == '=') {
                    sp++;
                    while (*sp && *sp != ',' && *sp != ';') {
                        sp++;
                    }
                } else if (*sp == ',') {
                    while (dst > out && isspace((unsigned char)dst[-1])) dst--;
                    *dst++ = ',';
                    *dst++ = ' ';
                    sp++;
                    while (*sp && isspace((unsigned char)*sp)) sp++;
                } else if (*sp == ';') {
                    while (dst > out && isspace((unsigned char)dst[-1])) dst--;
                    *dst++ = ';';
                    sp++;
                    break;
                } else {
                    *dst++ = *sp++;
                }
            }
        } else {
            /* Copy line verbatim */
            memcpy(dst, line, (size_t)li);
            dst += li;
        }
        *dst++ = '\n';
    }
    *dst = '\0';
    return out;
}

static char *join_canonical_definition_lines(char **lines, int count) {
    size_t total = 1;
    size_t off = 0;
    char *buf;

    qsort(lines, (size_t)count, sizeof(*lines), cmp_string_ptrs);
    for (int i = 0; i < count; i++)
        total += strlen(lines[i]) + 1;

    buf = (char *)malloc(total);
    if (!buf)
        return NULL;
    buf[0] = '\0';

    for (int i = 0; i < count; i++) {
        size_t len = strlen(lines[i]);
        memcpy(buf + off, lines[i], len);
        off += len;
        buf[off++] = '\n';
    }
    buf[off] = '\0';
    return buf;
}

static char *collect_example_definition_lines(int idx) {
    const char *const *lines = repl_example_lines(idx);
    char **canon_lines;
    int count = 0;
    char *joined;

    if (!lines)
        return NULL;

    for (int i = 0; lines[i]; i++) {
        if (!is_definition_source_line(lines[i]))
            continue;
        count++;
    }

    canon_lines = (char **)malloc((size_t)(count > 0 ? count : 1) * sizeof(*canon_lines));
    if (!canon_lines)
        return NULL;

    count = 0;
    for (int i = 0; lines[i]; i++) {
        if (!is_definition_source_line(lines[i]))
            continue;
        canon_lines[count] = canonicalize_definition_line(lines[i]);
        if (!canon_lines[count]) {
            for (int j = 0; j < count; j++)
                free(canon_lines[j]);
            free(canon_lines);
            return NULL;
        }
        count++;
    }

    joined = join_canonical_definition_lines(canon_lines, count);
    for (int i = 0; i < count; i++)
        free(canon_lines[i]);
    free(canon_lines);
    return joined;
}

static char *collect_loaded_definition_lines(void) {
    char **canon_lines;
    int count = 0;
    char *joined;

    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds()[i].type != CMD_VAR_DECLARE && repl_state_document_cmds()[i].type != CMD_VAR_ASSIGN)
            continue;
        count++;
    }

    canon_lines = (char **)malloc((size_t)(count > 0 ? count : 1) * sizeof(*canon_lines));
    if (!canon_lines)
        return NULL;

    count = 0;
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds()[i].type != CMD_VAR_DECLARE && repl_state_document_cmds()[i].type != CMD_VAR_ASSIGN)
            continue;
        canon_lines[count] = canonicalize_definition_line(editor_buffer_line(i));
        if (!canon_lines[count]) {
            for (int j = 0; j < count; j++)
                free(canon_lines[j]);
            free(canon_lines);
            return NULL;
        }
        count++;
    }

    joined = join_canonical_definition_lines(canon_lines, count);
    for (int i = 0; i < count; i++)
        free(canon_lines[i]);
    free(canon_lines);
    return joined;
}

static int find_example_index_by_name(const char *name) {
    for (int idx = 0; idx < repl_example_count(); idx++) {
        const char *example_name = repl_example_name(idx);

        if (example_name && strcmp(example_name, name) == 0)
            return idx;
    }

    return -1;
}

static void load_example_for_test(int idx) {
    glr_ctrl_reset_all(); declare_test_vars();
    pin_code_panel_state();
    repl_load_example(idx);
    settle_camera_transition_for_test();
}

static void load_custom_example_lines_for_test(const char *const *lines) {
    glr_ctrl_reset_all(); declare_test_vars();
    pin_code_panel_state();
    repl_load_example_lines(lines);
    settle_camera_transition_for_test();
}

static void test_example_loader_body_import_limits(void) {
    {
        enum { OLD_EXAMPLE_BODY_LINES_MAX = 384 };
        enum { LINE_COUNT = OLD_EXAMPLE_BODY_LINES_MAX + 1 };
        const char **lines = (const char **)malloc((LINE_COUNT + 1) * sizeof(*lines));
        int loaded;

        ASSERT_TRUE("public example body cap is 512",
                    EXAMPLE_BODY_LINES_MAX == 512);
        ASSERT_TRUE("public example body cap clears old private cap",
                    EXAMPLE_BODY_LINES_MAX >= LINE_COUNT);
        ASSERT_TRUE("example body limit fixture alloc", lines != NULL);
        if (lines) {
            for (int i = 0; i < LINE_COUNT; i++)
                lines[i] = "// body capacity sentinel";
            lines[LINE_COUNT] = NULL;

            glr_ctrl_reset_all(); declare_test_vars();
            pin_code_panel_state();
            loaded = repl_load_example_lines(lines);
            ASSERT_TRUE("example body loads past old 384-line cap",
                        loaded == LINE_COUNT);
            ASSERT_TRUE("example body keeps every line past old cap",
                        repl_state_document_count() == LINE_COUNT);
            free(lines);
        }
    }

    {
        static const char *const bad_body[] = {
            "glBegin(GL_POINTS);",
            "notACommand(1, 2, 3);",
            "glEnd();",
            NULL
        };
        int loaded;
        UiStatusState status;

        glr_ctrl_reset_all(); declare_test_vars();
        pin_code_panel_state();
        loaded = repl_load_example_lines(bad_body);
        status = ui_state_status();
        ASSERT_TRUE("example body parse failure aborts load", loaded == 0);
        ASSERT_TRUE("example body parse failure clears partial document",
                    repl_state_document_count() == 0);
        ASSERT_TRUE("example body parse failure reports line",
                    strstr(status.text, "Example load failed at body line 2") != NULL);
        ASSERT_TRUE("example body parse failure is error status",
                    status.kind == UI_STATUS_ERROR);
    }
}

static void test_example_catalog_metadata(void) {
    int example_count = repl_example_count();

    ASSERT_TRUE("example catalog is non-empty", example_count > 0);
    ASSERT_TRUE("Rotating cube migrated from worktree catalog",
                find_example_index_by_name("Rotating cube") >= 0);

    for (int idx = 0; idx < example_count; idx++) {
        char label[192];
        const char *name = repl_example_name(idx);
        const char *const *lines = repl_example_lines(idx);
        const char *group = repl_example_subheading(idx);
        ReplExampleSourceFormat format = repl_example_source_format(idx);

        snprintf(label, sizeof(label), "example %d has name", idx);
        ASSERT_TRUE(label, name != NULL && name[0] != '\0');
        snprintf(label, sizeof(label), "example %d has source lines", idx);
        ASSERT_TRUE(label, lines != NULL && lines[0] != NULL);
        snprintf(label, sizeof(label), "example %d has group", idx);
        ASSERT_TRUE(label, group != NULL && group[0] != '\0');
        snprintf(label, sizeof(label), "example %d has supported source format", idx);
        ASSERT_TRUE(label, format == REPL_EXAMPLE_SOURCE_GLR ||
                           format == REPL_EXAMPLE_SOURCE_C);

        for (int other = idx + 1; other < example_count; other++) {
            const char *other_name = repl_example_name(other);
            snprintf(label, sizeof(label), "example name unique: %s",
                     name ? name : "(null)");
            ASSERT_TRUE(label,
                        !name || !other_name || strcmp(name, other_name) != 0);
        }

        if (format == REPL_EXAMPLE_SOURCE_GLR) {
            for (int li = 0; lines && lines[li]; li++) {
                snprintf(label, sizeof(label),
                         "example %d line %d does not carry catalog name metadata",
                         idx, li);
                ASSERT_TRUE(label, strstr(lines[li], "@scene-name") == NULL);
            }
        }
    }

    ASSERT_TRUE("out-of-range example name is NULL",
                repl_example_name(example_count) == NULL);
    ASSERT_TRUE("negative example name is NULL",
                repl_example_name(-1) == NULL);
    ASSERT_TRUE("out-of-range example lines are NULL",
                repl_example_lines(example_count) == NULL);
    ASSERT_TRUE("negative example lines are NULL",
                repl_example_lines(-1) == NULL);
    ASSERT_TRUE("out-of-range example format defaults to raw snippet",
                repl_example_source_format(example_count) == REPL_EXAMPLE_SOURCE_GLR);
}

static void test_runtime_examples_dir_catalog(const char *temp_dir) {
    int builtin_count = repl_example_count();
    char root[512];
    char scenes_dir[512];
    char catalog_path[512];
    char glr_path[512];
    char c_path[512];
    char bad_path[512];
    char err[512];
    char *dump;

    snprintf(root, sizeof(root), "%s/runtime_examples", temp_dir);
    snprintf(scenes_dir, sizeof(scenes_dir), "%s/scenes", root);
    snprintf(catalog_path, sizeof(catalog_path), "%s/catalog.ini", root);
    snprintf(glr_path, sizeof(glr_path), "%s/raw-points.glr", scenes_dir);
    snprintf(c_path, sizeof(c_path), "%s/exported-lines.c", scenes_dir);
    snprintf(bad_path, sizeof(bad_path), "%s/bad.txt", scenes_dir);

    ASSERT_TRUE("runtime examples root mkdir", mkdir(root, 0700) == 0);
    ASSERT_TRUE("runtime examples scenes mkdir", mkdir(scenes_dir, 0700) == 0);
    ASSERT_TRUE("runtime .glr scene written",
                write_text_path(glr_path,
                                "// @cfg view_mode = 1\n"
                                "glBegin(GL_POINTS);\n"
                                "glVertex3f(0, 0, 0);\n"
                                "glEnd();\n"));
    ASSERT_TRUE("runtime .c scene written",
                write_text_path(c_path,
                                "#include <GL/glut.h>\n"
                                "\n"
                                "void display(void) {\n"
                                "  // Snippet start\n"
                                "  glBegin(GL_LINES);\n"
                                "  glVertex3f(0, 0, 0);\n"
                                "  glVertex3f(1, 0, 0);\n"
                                "  glEnd();\n"
                                "  // Snippet end\n"
                                "}\n"));
    ASSERT_TRUE("runtime catalog written",
                write_text_path(catalog_path,
                                "[raw-points]\n"
                                "file = scenes/raw-points.glr\n"
                                "name = Runtime raw points\n"
                                "tags = 2D, Lines\n"
                                "group = Runtime\n"
                                "\n"
                                "[exported-lines]\n"
                                "file = scenes/exported-lines.c\n"
                                "name = Runtime exported lines\n"
                                "tags = 3D, Lines\n"
                                "group = Runtime\n"));

    err[0] = '\0';
    ASSERT_TRUE("runtime examples dir loads",
                repl_examples_load_dir(root, err, sizeof(err)));
    ASSERT_TRUE("runtime examples replace compiled count",
                repl_example_count() == 2 && builtin_count != 2);
    ASSERT_TRUE("runtime first name from catalog",
                strcmp(repl_example_name(0), "Runtime raw points") == 0);
    ASSERT_TRUE("runtime first format is glr",
                repl_example_source_format(0) == REPL_EXAMPLE_SOURCE_GLR);
    ASSERT_TRUE("runtime second format is c",
                repl_example_source_format(1) == REPL_EXAMPLE_SOURCE_C);
    ASSERT_TRUE("runtime group from catalog",
                strcmp(repl_example_subheading(1), "Runtime") == 0);
    ASSERT_TRUE("runtime raw tags include 2D",
                repl_example_has_tag(0, REPL_EXAMPLE_TAG_2D));
    ASSERT_TRUE("runtime c tags include 3D",
                repl_example_has_tag(1, REPL_EXAMPLE_TAG_3D));

    load_example_for_test(0);
    ASSERT_TRUE("runtime .glr example loads",
                repl_state_document_count() == 3);
    ASSERT_TRUE("runtime .glr cfg applies",
                glr_state_presentation().ortho_mode == RENDER3D_VIEW_2D);

    load_example_for_test(1);
    ASSERT_TRUE("runtime .c example imports",
                repl_state_document_count() == 4);
    ASSERT_TRUE("runtime .c example keeps cmds valid",
                examples_have_no_invalid_cmds());
    dump = dump_current_code_panel_text();
    ASSERT_TRUE("runtime .c dump alloc", dump != NULL);
    if (dump) {
        ASSERT_TRUE("runtime .c import loaded snippet body",
                    strstr(dump, "glVertex3f(1, 0, 0);") != NULL);
        ASSERT_TRUE("runtime .c import hid snippet markers",
                    strstr(dump, "Snippet start") == NULL);
        free(dump);
    }

    ASSERT_TRUE("runtime bad extension scene written",
                write_text_path(bad_path, "glBegin(GL_POINTS);\n"));
    ASSERT_TRUE("runtime bad catalog written",
                write_text_path(catalog_path,
                                "[bad]\n"
                                "file = scenes/bad.txt\n"
                                "name = Bad extension\n"
                                "tags = 2D\n"
                                "group = Runtime\n"));
    err[0] = '\0';
    ASSERT_TRUE("runtime loader rejects unsupported extension",
                !repl_examples_load_dir(root, err, sizeof(err)) &&
                strstr(err, ".glr or .c") != NULL);
    ASSERT_TRUE("failed runtime load leaves previous catalog active",
                repl_example_count() == 2 &&
                strcmp(repl_example_name(0), "Runtime raw points") == 0);

    /* Additional catalog parsing/validation error test cases to cover examples.c paths */

    /* Empty/NULL directory paths */
    err[0] = '\0';
    ASSERT_TRUE("reject empty dir", !repl_examples_load_dir("", err, sizeof(err)));
    ASSERT_TRUE("reject NULL dir", !repl_examples_load_dir(NULL, err, sizeof(err)));

    /* Non-existent directory */
    err[0] = '\0';
    ASSERT_TRUE("reject non-existent dir", !repl_examples_load_dir("/tmp/nonexistent_catalog_path_xyz", err, sizeof(err)));

    /* Missing scenes subdirectory */
    char no_scenes_root[512];
    char no_scenes_cat[512];
    snprintf(no_scenes_root, sizeof(no_scenes_root), "%s/no_scenes_root", temp_dir);
    snprintf(no_scenes_cat, sizeof(no_scenes_cat), "%s/catalog.ini", no_scenes_root);
    if (mkdir(no_scenes_root, 0700) == 0) {
        write_text_path(no_scenes_cat, "[sec]\nfile = scenes/raw-points.glr\nname = Raw\ntags = 2D\ngroup = G\n");
        err[0] = '\0';
        ASSERT_TRUE("reject catalog when scenes dir missing", !repl_examples_load_dir(no_scenes_root, err, sizeof(err)));
        remove(no_scenes_cat);
        rmdir(no_scenes_root);
    }

    /* Missing catalog.ini */
    char no_cat_root[512];
    char no_cat_scenes[512];
    snprintf(no_cat_root, sizeof(no_cat_root), "%s/no_cat_root", temp_dir);
    snprintf(no_cat_scenes, sizeof(no_cat_scenes), "%s/scenes", no_cat_root);
    if (mkdir(no_cat_root, 0700) == 0) {
        if (mkdir(no_cat_scenes, 0700) == 0) {
            err[0] = '\0';
            ASSERT_TRUE("reject when catalog.ini missing", !repl_examples_load_dir(no_cat_root, err, sizeof(err)));
            rmdir(no_cat_scenes);
        }
        rmdir(no_cat_root);
    }

    /* Empty catalog.ini */
    write_text_path(catalog_path, "\n");
    err[0] = '\0';
    ASSERT_TRUE("empty catalog.ini fails", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Key outside section */
    write_text_path(catalog_path, "file = scenes/raw-points.glr\n");
    err[0] = '\0';
    ASSERT_TRUE("reject key outside section", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Missing '=' in key-value */
    write_text_path(catalog_path, "[sec]\nfile scenes/raw-points.glr\n");
    err[0] = '\0';
    ASSERT_TRUE("reject missing eq", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Unknown key */
    write_text_path(catalog_path, "[sec]\nunknown_key_name = value\n");
    err[0] = '\0';
    ASSERT_TRUE("reject unknown key", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Duplicate key */
    write_text_path(catalog_path, "[sec]\nfile = scenes/raw-points.glr\nfile = scenes/raw-points.glr\n");
    err[0] = '\0';
    ASSERT_TRUE("reject duplicate key", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Duplicate section */
    write_text_path(catalog_path, "[sec]\nfile = scenes/raw-points.glr\n[sec]\nfile = scenes/raw-points.glr\n");
    err[0] = '\0';
    ASSERT_TRUE("reject duplicate section", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Missing closing ']' in section header */
    write_text_path(catalog_path, "[sec\nfile = scenes/raw-points.glr\n");
    err[0] = '\0';
    ASSERT_TRUE("reject missing closing brace", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Extra chars in section header */
    write_text_path(catalog_path, "[sec] extra_chars\nfile = scenes/raw-points.glr\n");
    err[0] = '\0';
    ASSERT_TRUE("reject extra characters in section header", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Empty section name */
    write_text_path(catalog_path, "[]\nfile = scenes/raw-points.glr\n");
    err[0] = '\0';
    ASSERT_TRUE("reject empty section name", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Missing required keys */
    write_text_path(catalog_path, "[sec]\nfile = scenes/raw-points.glr\n");
    err[0] = '\0';
    ASSERT_TRUE("reject missing required keys", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Empty values in required keys */
    write_text_path(catalog_path, "[sec]\nfile = \nname = Raw\ntags = 2D\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject empty values in required keys", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Duplicate example name */
    write_text_path(catalog_path,
                    "[sec1]\nfile = scenes/raw-points.glr\nname = SameName\ntags = 2D\ngroup = G\n"
                    "[sec2]\nfile = scenes/exported-lines.c\nname = SameName\ntags = 2D\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject duplicate example name", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Absolute path in file */
    write_text_path(catalog_path, "[sec]\nfile = /tmp/scenes/raw-points.glr\nname = Raw\ntags = 2D\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject absolute path", !repl_examples_load_dir(root, err, sizeof(err)));

    /* File not under scenes */
    write_text_path(catalog_path, "[sec]\nfile = ../raw-points.glr\nname = Raw\ntags = 2D\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject file not under scenes", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Duplicate scene file */
    write_text_path(catalog_path,
                    "[sec1]\nfile = scenes/raw-points.glr\nname = Raw1\ntags = 2D\ngroup = G\n"
                    "[sec2]\nfile = scenes/raw-points.glr\nname = Raw2\ntags = 2D\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject duplicate scene file", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Nonexistent scene file */
    write_text_path(catalog_path, "[sec]\nfile = scenes/nonexistent.glr\nname = Raw\ntags = 2D\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject nonexistent scene file", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Tag validation errors */
    write_text_path(catalog_path, "[sec]\nfile = scenes/raw-points.glr\nname = Raw\ntags = All\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject synthetic tag All", !repl_examples_load_dir(root, err, sizeof(err)));

    write_text_path(catalog_path, "[sec]\nfile = scenes/raw-points.glr\nname = Raw\ntags = 2D,UnknownTag\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject unknown tag", !repl_examples_load_dir(root, err, sizeof(err)));

    write_text_path(catalog_path, "[sec]\nfile = scenes/raw-points.glr\nname = Raw\ntags = 2D,2D\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject duplicate tags", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Empty scene file */
    char empty_scene[512];
    snprintf(empty_scene, sizeof(empty_scene), "%s/empty.glr", scenes_dir);
    write_text_path(empty_scene, "");
    write_text_path(catalog_path, "[sec]\nfile = scenes/empty.glr\nname = Raw\ntags = 2D\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject empty scene file", !repl_examples_load_dir(root, err, sizeof(err)));
    remove(empty_scene);

    /* Scene line exceeds MAX_LINE_LEN */
    char long_line[300];
    memset(long_line, 'a', 258);
    long_line[258] = '\n';
    long_line[259] = '\0';
    char long_glr_path[512];
    snprintf(long_glr_path, sizeof(long_glr_path), "%s/long.glr", scenes_dir);
    write_text_path(long_glr_path, long_line);
    write_text_path(catalog_path, "[sec]\nfile = scenes/long.glr\nname = Raw\ntags = 2D\ngroup = G\n");
    err[0] = '\0';
    ASSERT_TRUE("reject scene line exceeding MAX_LINE_LEN", !repl_examples_load_dir(root, err, sizeof(err)));
    remove(long_glr_path);

    /* Catalog line exceeds MAX_LINE_LEN */
    char long_cat_line[300];
    memset(long_cat_line, 'a', 258);
    long_cat_line[258] = '\n';
    long_cat_line[259] = '\0';
    write_text_path(catalog_path, long_cat_line);
    err[0] = '\0';
    ASSERT_TRUE("reject catalog line exceeding MAX_LINE_LEN", !repl_examples_load_dir(root, err, sizeof(err)));

    /* Boundary bounds checks for active examples getters */
    int current_example_count = repl_example_count();
    ASSERT_TRUE("out of bounds example name is NULL", repl_example_name(current_example_count) == NULL);
    ASSERT_TRUE("negative example name is NULL", repl_example_name(-1) == NULL);
    ASSERT_TRUE("out of bounds example lines is NULL", repl_example_lines(current_example_count) == NULL);
    ASSERT_TRUE("negative example lines is NULL", repl_example_lines(-1) == NULL);
    ASSERT_TRUE("out of bounds example subheading is NULL", repl_example_subheading(current_example_count) == NULL);
    ASSERT_TRUE("negative example subheading is NULL", repl_example_subheading(-1) == NULL);
    ASSERT_TRUE("out of bounds example tag mask is 0", repl_example_tag_mask(current_example_count) == 0u);
    ASSERT_TRUE("negative example tag mask is 0", repl_example_tag_mask(-1) == 0u);

    repl_examples_clear_runtime_catalog();
    ASSERT_TRUE("runtime examples clear restores compiled catalog",
                repl_example_count() == builtin_count);

    if (!g_keep_temp) {
        remove(bad_path);
        remove(c_path);
        remove(glr_path);
        remove(catalog_path);
        rmdir(scenes_dir);
        rmdir(root);
    }
}

static void test_example_tag_metadata(void) {
    int tag_count = repl_example_tag_count();
    int example_count = repl_example_count();
    unsigned int known_tag_bits = 0u;
    int stress_idx;
    int stress_tag_hits = 0;

    ASSERT_TRUE("example tag count positive", tag_count > 0);
    for (int tag_idx = 0; tag_idx < tag_count; tag_idx++) {
        char label[128];
        const char *tag_label = repl_example_tag_label(tag_idx);
        int count;

        snprintf(label, sizeof(label), "example tag %d label", tag_idx);
        ASSERT_TRUE(label, tag_label != NULL && tag_label[0] != '\0');
        known_tag_bits |= repl_example_tag_bit(tag_idx);

        count = repl_example_count_for_tag(tag_idx);
        for (int ordinal = 0; ordinal < count; ordinal++) {
            int example_idx = repl_example_index_for_tag(tag_idx, ordinal);
            snprintf(label, sizeof(label), "tag %d ordinal %d maps valid",
                     tag_idx, ordinal);
            ASSERT_TRUE(label, example_idx >= 0 && example_idx < example_count);
            snprintf(label, sizeof(label), "tag %d ordinal %d has tag",
                     tag_idx, ordinal);
            ASSERT_TRUE(label, repl_example_has_tag(example_idx, tag_idx));
        }
    }

    ASSERT_TRUE("invalid negative tag bit", repl_example_tag_bit(-1) == 0u);
    ASSERT_TRUE("invalid high tag bit",
                repl_example_tag_bit(repl_example_tag_count()) == 0u);
    ASSERT_TRUE("visible tag count within tag count",
                repl_example_visible_tag_count() <= tag_count);
    for (int dense_idx = 0; dense_idx < repl_example_visible_tag_count(); dense_idx++) {
        char label[128];
        int tag_idx = repl_example_visible_tag_at(dense_idx);
        snprintf(label, sizeof(label), "visible tag %d maps valid", dense_idx);
        ASSERT_TRUE(label, tag_idx >= 0 && tag_idx < tag_count);
        snprintf(label, sizeof(label), "visible tag %d has examples", dense_idx);
        ASSERT_TRUE(label, repl_example_count_for_tag(tag_idx) > 0);
    }

    for (int idx = 0; idx < example_count; idx++) {
        char label[160];
        unsigned int mask = repl_example_tag_mask(idx);

        snprintf(label, sizeof(label), "example %d tag mask nonzero", idx);
        ASSERT_TRUE(label, mask != 0u);
        snprintf(label, sizeof(label), "example %d tag mask known bits", idx);
        ASSERT_TRUE(label, (mask & ~known_tag_bits) == 0u);
        for (int tag_idx = 0; tag_idx < tag_count; tag_idx++) {
            int expected = (mask & repl_example_tag_bit(tag_idx)) != 0u;
            snprintf(label, sizeof(label), "example %d tag %d agreement",
                     idx, tag_idx);
            ASSERT_TRUE(label, repl_example_has_tag(idx, tag_idx) == expected);
        }
    }

    stress_idx = find_example_index_by_name("Dusk lighthouse atoll (stress test)");
    ASSERT_TRUE("known multi-tag example found", stress_idx >= 0);
    if (stress_idx >= 0) {
        for (int tag_idx = 0; tag_idx < tag_count; tag_idx++) {
            int found_under_tag = 0;
            if (!repl_example_has_tag(stress_idx, tag_idx))
                continue;
            stress_tag_hits++;
            for (int ordinal = 0;
                 ordinal < repl_example_count_for_tag(tag_idx);
                 ordinal++) {
                if (repl_example_index_for_tag(tag_idx, ordinal) == stress_idx) {
                    found_under_tag = 1;
                    break;
                }
            }
            ASSERT_TRUE("known multi-tag discoverable under assigned tag",
                        found_under_tag);
        }
        ASSERT_TRUE("known example has multiple tags", stress_tag_hits > 1);
    }
}

/* Mirror of test_catalog_subheading_metadata in tests/test_tutorial_runner.c.
 * Each example declares an optional free-form section label
 * (`ReplExampleEntry.subheading`); the Scene menu groups consecutive
 * examples sharing a subheading under a `### subheading` chrome row in the
 * per-tag flyout. Invariants:
 *   - Every subheading is either NULL or a non-empty string (the menu
 *     stripping logic would render an empty string as zero-width chrome).
 *   - The getter returns NULL for out-of-range indices.
 *   - Per tag, every non-NULL subheading appears in a single contiguous
 *     run of examples (matches the menu walker's emit rule - one header
 *     per group; interleaving would render duplicate headers).
 *   - At least one shipped example has a non-NULL subheading so the
 *     menu walker's HEADER path is exercised in production. */
static void test_example_subheading_metadata(void) {
    int tag_count = repl_example_tag_count();
    int example_count = repl_example_count();

    for (int idx = 0; idx < example_count; idx++) {
        char label[128];
        const char *sub = repl_example_subheading(idx);
        snprintf(label, sizeof(label),
                 "example %d subheading is NULL or non-empty", idx);
        ASSERT_TRUE(label, !sub || sub[0] != '\0');
    }
    ASSERT_TRUE("out-of-range example subheading is NULL",
                repl_example_subheading(example_count) == NULL);
    ASSERT_TRUE("negative example subheading is NULL",
                repl_example_subheading(-1) == NULL);

    int has_any_subheading = 0;
    for (int idx = 0; idx < example_count; idx++) {
        if (repl_example_subheading(idx)) { has_any_subheading = 1; break; }
    }
    ASSERT_TRUE("catalog ships at least one subheading", has_any_subheading);

    /* Per tag: contiguity check - count distinct non-NULL subheadings
     * (set semantics) vs the number of subheading-change transitions
     * the menu walker would emit. Equality means each distinct
     * subheading appears in a single contiguous run; a mismatch means
     * an interleaved subheading would render its header twice. */
    for (int t = 0; t < tag_count; t++) {
        char label[128];
        const char *seen[16];
        int seen_count = 0;
        const char *prev = NULL;
        int transitions = 0;
        int n = repl_example_count_for_tag(t);
        for (int o = 0; o < n; o++) {
            int ex_idx = repl_example_index_for_tag(t, o);
            const char *sub = repl_example_subheading(ex_idx);
            if (!sub)
                continue;
            int already_seen = 0;
            for (int s = 0; s < seen_count; s++) {
                if (strcmp(seen[s], sub) == 0) { already_seen = 1; break; }
            }
            if (!already_seen &&
                seen_count < (int)(sizeof(seen) / sizeof(seen[0]))) {
                seen[seen_count++] = sub;
            }
            int header_here = !prev || strcmp(prev, sub) != 0;
            if (header_here)
                transitions++;
            prev = sub;
        }
        snprintf(label, sizeof(label),
                 "tag %d subheadings are contiguous (no interleaving)", t);
        ASSERT_TRUE(label, transitions == seen_count);
    }

    /* Known multi-tag entry: Stress test. Its subheading must be
     * non-NULL and the same value must surface under every tag it
     * carries (catalog authoring rule: a single entry cannot rename
     * itself per tag). */
    int stress_idx = find_example_index_by_name("Dusk lighthouse atoll (stress test)");
    ASSERT_TRUE("Stress test in catalog", stress_idx >= 0);
    if (stress_idx >= 0) {
        const char *expected_sub = repl_example_subheading(stress_idx);
        ASSERT_TRUE("Stress test has a subheading", expected_sub != NULL);
        for (int t = 0; t < tag_count; t++) {
            if (!repl_example_has_tag(stress_idx, t))
                continue;
            int n = repl_example_count_for_tag(t);
            int found = 0;
            for (int o = 0; o < n; o++) {
                int ex_idx = repl_example_index_for_tag(t, o);
                if (ex_idx != stress_idx)
                    continue;
                const char *sub = repl_example_subheading(ex_idx);
                if (sub && expected_sub && strcmp(sub, expected_sub) == 0)
                    found = 1;
                break;
            }
            ASSERT_TRUE("multi-tag entry has same subheading in every tag",
                        found);
        }
    }
}

/* Tag-based example default overrides. The 2D tag default sets the
 * grid theme to GRID_THEME_PLANES. Verify that:
 *
 *   1. A 2D example with no explicit grid `@cfg` lands at
 *      GRID_THEME_PLANES.
 *   2. A multi-tag example that includes 2D also lands at
 *      GRID_THEME_PLANES when it doesn't override grid.
 *   3. An example outside the 2D tag (3D-only) lands at the global
 *      grid default (CFG_DEFAULT_GRID_THEME) - no tag default applies.
 *   4. An example tagged 2D but with its own `@cfg grid = N` keeps N
 *      (example `@cfg` wins over the tag default).
 *
 * Precedence chain under test: example `@cfg` > tag default > global
 * default. */
/* Audit #41: example catalogs must use symbolic value names for
 * enum-valued slugs so reordering the matching enum can't silently shift
 * an example to a different choice. Scan every example's leading metadata block; any
 * `@cfg <slug> = <val>` that targets an enum-valued slug must have a
 * non-digit value (i.e., a symbolic name, not a raw integer). */
static void test_example_cfg_uses_symbolic_names(void) {
    static const char *const enum_slugs[] = {
        "grid", "axes", "backdrop", "grid_brightness", "vertex_labels"
    };
    int n = repl_example_count();
    for (int i = 0; i < n; i++) {
        const char *const *lines = repl_example_lines(i);
        if (!lines) continue;
        for (int li = 0; lines[li]; li++) {
            const char *line = lines[li];
            if (strncmp(line, "// @cfg ", 8) != 0) break;  /* end of metadata */
            const char *body = line + 8;
            for (int s = 0; s < (int)(sizeof(enum_slugs) / sizeof(*enum_slugs)); s++) {
                size_t slen = strlen(enum_slugs[s]);
                if (strncmp(body, enum_slugs[s], slen) != 0) continue;
                const char *p = body + slen;
                while (*p == ' ') p++;
                if (*p != '=') continue;
                p++;
                while (*p == ' ') p++;
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "example '%s' line '%s' uses symbolic value (not a digit)",
                         repl_example_name(i), line);
                ASSERT_TRUE(buf, !isdigit((unsigned char)*p));
            }
        }
    }
}

/* Every glClearColor an example sets must be followed by a glClear, or it
 * paints nothing. Execution is source-ordered (see repl_flat_resolve_clear_
 * color) and the frame's glPushAttrib/glPopAttrib bracket reverts it before
 * the next frame, so a trailing clear color is silently dead: the scene just
 * renders on the bootstrap background instead of the authored one. This is
 * easy to reintroduce - the author sees the color in the source, so nothing
 * looks wrong - and it once shipped in ten catalog scenes at once. */
static void test_example_clear_color_precedes_clear(void) {
    int n = repl_example_count();
    for (int i = 0; i < n; i++) {
        const char *const *lines = repl_example_lines(i);
        int last_clear_color = -1;
        int last_clear = -1;
        if (!lines) continue;
        for (int li = 0; lines[li]; li++) {
            const char *p = lines[li];
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "glClearColor(", 13) == 0)
                last_clear_color = li;
            else if (strncmp(p, "glClear(", 8) == 0)
                last_clear = li;
        }
        if (last_clear_color >= 0) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "example '%s': glClearColor (line %d) precedes a glClear",
                     repl_example_name(i), last_clear_color + 1);
            ASSERT_TRUE(buf, last_clear > last_clear_color);
        }
    }
}

/* Switching examples replaces the measured workload, so the cumulative timing
 * histograms must not carry the previous example's distribution (or its
 * startup outliers) into the new one. repl_load_example is the chokepoint
 * every path funnels through - menu, F12 cycle, --example, bootstrap. */
static void test_example_load_resets_histograms(void) {
    HistogramBin bins[HISTOGRAM_BIN_COUNT];
    int total;

    ASSERT_TRUE("catalog has at least one example", repl_example_count() > 0);

    prof_test_reset();
    prof_test_set_now_us(1000.0);
    prof_begin(PROF_FLATTEN);
    prof_test_set_now_us(31000.0);
    prof_end(PROF_FLATTEN);
    prof_test_clear_now_us();

    prof_section_histogram(PROF_FLATTEN, bins, HISTOGRAM_BIN_COUNT);
    total = 0;
    for (int i = 0; i < HISTOGRAM_BIN_COUNT; i++) total += (int)bins[i];
    ASSERT_TRUE("histogram holds the pre-switch sample", total == 1);

    ASSERT_TRUE("example loads", repl_load_example(0) > 0);

    prof_section_histogram(PROF_FLATTEN, bins, HISTOGRAM_BIN_COUNT);
    total = 0;
    for (int i = 0; i < HISTOGRAM_BIN_COUNT; i++) total += (int)bins[i];
    ASSERT_TRUE("example load clears the stale histogram", total == 0);

    prof_test_reset();
}

static void test_example_tag_default_cfg(void) {
    int bezier_idx     = find_example_index_by_name("Bezier curve with guides");
    int cube_idx       = find_example_index_by_name("Conditional colors (if + t)");
    int stress_idx     = find_example_index_by_name("Dusk lighthouse atoll (stress test)");
    int whale_idx      = find_example_index_by_name("Whale (particle system + lit model)");
    int spirograph_idx =
        find_example_index_by_name("Animated spirograph curve");

    ASSERT_TRUE("bezier example index found", bezier_idx >= 0);
    ASSERT_TRUE("cube example index found", cube_idx >= 0);
    ASSERT_TRUE("stress example index found", stress_idx >= 0);
    ASSERT_TRUE("whale example index found", whale_idx >= 0);
    ASSERT_TRUE("spirograph example index found", spirograph_idx >= 0);

    /* Sanity-check tag membership so the test isn't quietly invalidated
     * if an example's tags are edited later. */
    if (bezier_idx >= 0) {
        ASSERT_TRUE("bezier is in 2D bucket",
                    repl_example_has_tag(bezier_idx, REPL_EXAMPLE_TAG_2D));
        ASSERT_TRUE("bezier is not in 3D bucket",
                    !repl_example_has_tag(bezier_idx, REPL_EXAMPLE_TAG_3D));
    }
    if (cube_idx >= 0) {
        ASSERT_TRUE("cube is not in 2D bucket",
                    !repl_example_has_tag(cube_idx, REPL_EXAMPLE_TAG_2D));
        ASSERT_TRUE("cube is in 3D bucket",
                    repl_example_has_tag(cube_idx, REPL_EXAMPLE_TAG_3D));
    }
    if (stress_idx >= 0) {
        ASSERT_TRUE("stress is in 3D bucket",
                    repl_example_has_tag(stress_idx, REPL_EXAMPLE_TAG_3D));
        ASSERT_TRUE("stress is not in 2D bucket",
                    !repl_example_has_tag(stress_idx, REPL_EXAMPLE_TAG_2D));
    }
    if (whale_idx >= 0) {
        ASSERT_TRUE("whale is in 3D bucket",
                    repl_example_has_tag(whale_idx, REPL_EXAMPLE_TAG_3D));
        ASSERT_TRUE("whale is not in 2D bucket",
                    !repl_example_has_tag(whale_idx, REPL_EXAMPLE_TAG_2D));
    }
    if (spirograph_idx >= 0) {
        ASSERT_TRUE("spirograph is in 2D bucket",
                    repl_example_has_tag(spirograph_idx, REPL_EXAMPLE_TAG_2D));
    }

    /* (1) 2D-only-bucket example (in 2D, not in 3D), no own grid @cfg
     * -> GRID_THEME_PLANES. The bezier example has @cfg lines for
     * other slugs but no `@cfg grid`. */
    if (bezier_idx >= 0) {
        load_example_for_test(bezier_idx);
        ASSERT_TRUE("2D tag default applies GRID_THEME_PLANES (2D-only)",
                    glr_state_presentation().grid_theme == GRID_THEME_PLANES);
    }

    /* (2) Multi-tag example including 2D, no own grid @cfg ->
     * GRID_THEME_PLANES. The spirograph example is 2D|LINES with no
     * explicit grid override. */
    if (spirograph_idx >= 0) {
        load_example_for_test(spirograph_idx);
        ASSERT_TRUE("2D tag default applies GRID_THEME_PLANES (multi tag)",
                    glr_state_presentation().grid_theme == GRID_THEME_PLANES);
    }

    /* (3) 3D-only example -> global default, no tag override. */
    if (cube_idx >= 0) {
        load_example_for_test(cube_idx);
        ASSERT_TRUE("non-2D example uses global grid default",
                    glr_state_presentation().grid_theme ==
                    CFG_DEFAULT_GRID_THEME);
    }

    /* (4) Example with its own @cfg grid -> the explicit value wins over the
     * tag / global default. Whale is 3D-only and sets grid =
     * GRID_THEME_OCEAN. */
    if (whale_idx >= 0) {
        load_example_for_test(whale_idx);
        ASSERT_TRUE("example @cfg grid overrides default",
                    glr_state_presentation().grid_theme == GRID_THEME_OCEAN);
    }
}

/* Exercise the tag-defaults dispatch with synthetic policies so the
 * shipped table can stay a single conflict-free row while we still
 * lock in the iteration semantics:
 *
 *   - Multiple entries targeting DIFFERENT keys all apply (independent
 *     stacking).
 *   - Multiple entries targeting the SAME key resolve last-wins, and
 *     the function returns a non-zero collision count so the warning
 *     surfaces a misconfigured policy.
 *   - Entries whose tag bit is not in the mask are skipped.
 *
 * The test calls glr_ctrl_apply_tag_defaults directly with a synthetic
 * table - no example loaded, no @cfg in play - so the only thing
 * mutating state here is the helper itself. glr_ctrl_reset_all
 * normalizes presentation to global defaults before each subcase. */
static void test_example_tag_default_dispatch(void) {
    /* (A) Two entries, distinct keys, both tags present -> both apply. */
    {
        static const GlrExampleTagDefault table[] = {
            { .tag_idx = REPL_EXAMPLE_TAG_2D,
              .key     = GLR_CONFIG_GRID_THEME,
              .value   = GRID_THEME_PLANES },
            { .tag_idx = REPL_EXAMPLE_TAG_LINES,
              .key     = GLR_CONFIG_LINE_SMOOTH,
              .value   = 1 },
        };
        unsigned int mask = repl_example_tag_bit(REPL_EXAMPLE_TAG_2D) |
                            repl_example_tag_bit(REPL_EXAMPLE_TAG_LINES);

        glr_ctrl_reset_all(); declare_test_vars();
        int collisions = glr_ctrl_apply_tag_defaults(mask, table, 2);
        ASSERT_TRUE("distinct-key stack: no collision",
                    collisions == 0);
        ASSERT_TRUE("distinct-key stack: first entry applied",
                    glr_state_presentation().grid_theme == GRID_THEME_PLANES);
        ASSERT_TRUE("distinct-key stack: second entry applied",
                    glr_state_render().line_smooth_enabled == 1);
    }

    /* (B) Two entries colliding on the same key for the same tag ->
     * later entry wins, collision count == 1. */
    {
        static const GlrExampleTagDefault table[] = {
            { .tag_idx = REPL_EXAMPLE_TAG_2D,
              .key     = GLR_CONFIG_GRID_THEME,
              .value   = GRID_THEME_TRON },
            { .tag_idx = REPL_EXAMPLE_TAG_2D,
              .key     = GLR_CONFIG_GRID_THEME,
              .value   = GRID_THEME_OCEAN },
        };
        unsigned int mask = repl_example_tag_bit(REPL_EXAMPLE_TAG_2D);

        glr_ctrl_reset_all(); declare_test_vars();
        int collisions = glr_ctrl_apply_tag_defaults(mask, table, 2);
        ASSERT_TRUE("same-key collision counted",
                    collisions == 1);
        ASSERT_TRUE("same-key collision: later entry wins",
                    glr_state_presentation().grid_theme == GRID_THEME_OCEAN);
    }

    /* (C) Two entries colliding on the same key but for DIFFERENT tags
     * - both tag bits set in the mask -> still a collision (the mask
     * picks both up). Same-key second-write wins. */
    {
        static const GlrExampleTagDefault table[] = {
            { .tag_idx = REPL_EXAMPLE_TAG_2D,
              .key     = GLR_CONFIG_GRID_THEME,
              .value   = GRID_THEME_PLANES },
            { .tag_idx = REPL_EXAMPLE_TAG_LINES,
              .key     = GLR_CONFIG_GRID_THEME,
              .value   = GRID_THEME_EMBER },
        };
        unsigned int mask = repl_example_tag_bit(REPL_EXAMPLE_TAG_2D) |
                            repl_example_tag_bit(REPL_EXAMPLE_TAG_LINES);

        glr_ctrl_reset_all(); declare_test_vars();
        int collisions = glr_ctrl_apply_tag_defaults(mask, table, 2);
        ASSERT_TRUE("cross-tag same-key collision counted",
                    collisions == 1);
        ASSERT_TRUE("cross-tag same-key collision: later wins",
                    glr_state_presentation().grid_theme == GRID_THEME_EMBER);
    }

    /* (D) Two entries colliding on the same key but the mask matches
     * only ONE of them -> no collision, the matching entry applies. */
    {
        static const GlrExampleTagDefault table[] = {
            { .tag_idx = REPL_EXAMPLE_TAG_2D,
              .key     = GLR_CONFIG_GRID_THEME,
              .value   = GRID_THEME_PLANES },
            { .tag_idx = REPL_EXAMPLE_TAG_3D,
              .key     = GLR_CONFIG_GRID_THEME,
              .value   = GRID_THEME_EMBER },
        };
        unsigned int mask = repl_example_tag_bit(REPL_EXAMPLE_TAG_3D);

        glr_ctrl_reset_all(); declare_test_vars();
        int collisions = glr_ctrl_apply_tag_defaults(mask, table, 2);
        ASSERT_TRUE("mask filters non-matching: no collision",
                    collisions == 0);
        ASSERT_TRUE("mask filters non-matching: only matching applied",
                    glr_state_presentation().grid_theme == GRID_THEME_EMBER);
    }

    /* (E) Empty mask -> nothing applied, no collisions. */
    {
        static const GlrExampleTagDefault table[] = {
            { .tag_idx = REPL_EXAMPLE_TAG_2D,
              .key     = GLR_CONFIG_GRID_THEME,
              .value   = GRID_THEME_PLANES },
        };
        int prev_grid;

        glr_ctrl_reset_all(); declare_test_vars();
        prev_grid = glr_state_presentation().grid_theme;
        int collisions = glr_ctrl_apply_tag_defaults(0u, table, 1);
        ASSERT_TRUE("empty mask: no collision", collisions == 0);
        ASSERT_TRUE("empty mask: state unchanged",
                    glr_state_presentation().grid_theme == prev_grid);
    }
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  --help, -h             Show this help message\n");
    printf("  --dump-index N         Dump code-panel text for example N to stdout\n");
    printf("                         (used to regenerate golden fixture files)\n\n");
    printf("  --update-golden        Regenerate/update all golden fixture files\n\n");
    printf("  --show-mismatch        Print expected/actual context around exact-text mismatches\n");
    printf("                         (alias: --diff)\n\n");
    printf("  --keep-temp            Do not delete the temp dir / export files on exit\n");
    printf("                         (leaves artifacts under /tmp for inspection)\n\n");
    printf("Environment:\n");
    printf("  REPL_EXPORT_VERBOSE=1  Print per-example step details\n");
    printf("  REPL_EXPORT_KEEP_TEMP=1  Same as --keep-temp\n");
    printf("  REPL_EXPORT_CC         C compiler to use (default: cc)\n");
    printf("  REPL_EXPORT_COMPILE_CFLAGS  Extra compiler flags\n");
    printf("  NO_COLOR               Disable ANSI color output\n\n");
    printf("Golden fixture files: tests/testdata/repl_examples_ui/NN.golden.txt\n\n");
    printf("To regenerate all golden fixtures after intentional changes:\n");
    printf("  %s --update-golden\n\n", prog);
    printf("Or to regenerate a single fixture for example N:\n");
    printf("  %s --dump-index N > tests/testdata/repl_examples_ui/NN.golden.txt\n\n",
           prog);
}

static int dump_single_example_to_stdout(int idx) {
    char *dump;

    if (idx < 0 || idx >= repl_example_count()) {
        fprintf(stderr, "invalid example index: %d\n", idx);
        return 1;
    }

    repl_eval_init_predef_vars();
    load_example_for_test(idx);
    dump = dump_current_code_panel_text();
    if (!dump)
        return 1;
    fputs(dump, stdout);
    free(dump);
    return 0;
}

static int update_all_golden_fixtures(void) {
    int count = repl_example_count();
    printf("Updating %d golden fixture files...\n", count);
    for (int idx = 0; idx < count; idx++) {
        char fixture_path[512];
        fixture_path_for_idx(idx, fixture_path, sizeof(fixture_path));

        repl_eval_init_predef_vars();
        load_example_for_test(idx);
        char *dump = dump_current_code_panel_text();
        if (!dump) {
            fprintf(stderr, "error: failed to dump example %d\n", idx);
            return 1;
        }
        if (!write_text_path(fixture_path, dump)) {
            fprintf(stderr, "error: failed to write fixture file: %s\n", fixture_path);
            free(dump);
            return 1;
        }
        free(dump);
    }
    printf("Successfully updated all golden fixture files.\n");
    return 0;
}

int main(int argc, char **argv) {
    char temp_dir[] = "/tmp/repl_examples_export.XXXXXX";
    const char *verbose_env = getenv("REPL_EXPORT_VERBOSE");
    int dump_idx = -1;
    int update_golden = 0;

    g_verbose = verbose_env && verbose_env[0] && strcmp(verbose_env, "0") != 0;
    g_keep_temp = env_truthy("REPL_EXPORT_KEEP_TEMP");
    g_use_color = getenv("NO_COLOR") == NULL &&
                  (isatty(STDOUT_FILENO) ||
                   env_truthy("FORCE_COLOR") ||
                   env_truthy("CLICOLOR_FORCE"));

    for (int argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--help") == 0 ||
            strcmp(argv[argi], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[argi], "--show-mismatch") == 0 ||
            strcmp(argv[argi], "--diff") == 0) {
            g_show_mismatch = 1;
            continue;
        }
        if (strcmp(argv[argi], "--keep-temp") == 0) {
            g_keep_temp = 1;
            continue;
        }
        if (strcmp(argv[argi], "--update-golden") == 0) {
            update_golden = 1;
            continue;
        }
        if (strcmp(argv[argi], "--dump-index") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "--dump-index requires an example index\n");
                return 2;
            }
            dump_idx = atoi(argv[++argi]);
            continue;
        }

        fprintf(stderr, "unknown option: %s\n", argv[argi]);
        print_usage(argv[0]);
        return 2;
    }

    if (update_golden)
        return update_all_golden_fixtures();

    if (dump_idx >= 0)
        return dump_single_example_to_stdout(dump_idx);

    if (!mkdtemp(temp_dir)) {
        perror("mkdtemp");
        return 1;
    }

    repl_eval_init_predef_vars();
    test_runtime_examples_dir_catalog(temp_dir);
    test_example_loader_body_import_limits();
    test_example_catalog_metadata();
    test_example_tag_metadata();
    test_example_subheading_metadata();
    test_example_load_resets_histograms();
    test_example_tag_default_cfg();
    test_example_tag_default_dispatch();
    test_example_cfg_uses_symbolic_names();
    test_example_clear_color_precedes_clear();

    {
        static const char *const no_cfg_reset_example[] = {
            "glBegin(GL_POINTS);",
            "glVertex3f(0, 0, 0);",
            "glEnd();",
            NULL
        };

        glr_ctrl_reset_all(); declare_test_vars();
        seed_nondefault_example_presentation_state();
        repl_load_example_lines(no_cfg_reset_example);

        ASSERT_TRUE("no cfg reset wireframe default",
                    glr_state_presentation().wireframe == CFG_DEFAULT_WIREFRAME);
        ASSERT_TRUE("no cfg reset grid default",
                    glr_state_presentation().grid_theme == CFG_DEFAULT_GRID_THEME);
        ASSERT_TRUE("no cfg reset grid major default",
                    glr_state_presentation().grid_major_idx == CFG_DEFAULT_GRID_MAJOR_IDX);
        ASSERT_TRUE("no cfg reset grid extent default",
                    glr_state_presentation().grid_extent_idx == CFG_DEFAULT_GRID_EXTENT_IDX);
        ASSERT_TRUE("no cfg reset axes default",
                    glr_state_presentation().axes_theme == CFG_DEFAULT_AXES_THEME);
        ASSERT_TRUE("no cfg reset labels default",
                    glr_state_presentation().show_vertex_labels == CFG_DEFAULT_VERTEX_LABELS);
        ASSERT_TRUE("no cfg reset normals default",
                    glr_state_presentation().show_normal_vectors == CFG_DEFAULT_NORMAL_VECTORS);
        ASSERT_TRUE("no cfg reset outlines default",
                    glr_state_presentation().show_vertex_outlines == CFG_DEFAULT_VERTEX_OUTLINES);
        ASSERT_TRUE("no cfg reset points default",
                    glr_state_presentation().show_vertex_points == CFG_DEFAULT_VERTEX_POINTS);
        ASSERT_TRUE("no cfg reset guides default",
                    glr_state_presentation().xform_guide_mode == CFG_DEFAULT_XFORM_GUIDE_MODE);
        ASSERT_TRUE("no cfg reset lights default",
                    glr_state_presentation().show_light_indicators == CFG_DEFAULT_LIGHT_INDICATORS);
        ASSERT_TRUE("no cfg reset backdrop default",
                    glr_state_presentation().backdrop_mode == CFG_DEFAULT_BACKDROP_MODE);
        ASSERT_TRUE("no cfg reset camera rotate default",
                    glr_camera().auto_rotate == CFG_DEFAULT_CAMERA_ROTATE);
        ASSERT_TRUE("no cfg keeps camera rx",
                    fabsf(glr_camera().rx - (-41.0f)) < 1e-4f);
        ASSERT_TRUE("no cfg keeps camera ry",
                    fabsf(glr_camera().ry - 73.0f) < 1e-4f);
        ASSERT_TRUE("no cfg keeps camera dist",
                    fabsf(glr_camera().dist - 12.0f) < 1e-4f);
        ASSERT_TRUE("no cfg keeps camera tx",
                    fabsf(glr_camera().tx - 1.5f) < 1e-4f);
        ASSERT_TRUE("no cfg keeps camera ty",
                    fabsf(glr_camera().ty - (-2.0f)) < 1e-4f);
        ASSERT_TRUE("no cfg keeps camera tz",
                    fabsf(glr_camera().tz - 0.75f) < 1e-4f);
    }

    {
        static const char *const partial_cfg_reset_example[] = {
            "// @cfg wireframe = 1",
            "// @cfg vertex_labels = 0",
            "// @cfg backdrop = 1",
            "glBegin(GL_POINTS);",
            "glVertex3f(0, 0, 0);",
            "glEnd();",
            NULL
        };

        glr_ctrl_reset_all(); declare_test_vars();
        seed_nondefault_example_presentation_state();
        repl_load_example_lines(partial_cfg_reset_example);

        ASSERT_TRUE("partial cfg wireframe applied",
                    glr_state_presentation().wireframe == 1);
        ASSERT_TRUE("partial cfg labels applied",
                    glr_state_presentation().show_vertex_labels == 0);
        ASSERT_TRUE("partial cfg backdrop applied",
                    glr_state_presentation().backdrop_mode == 1);
        ASSERT_TRUE("partial cfg grid reset default",
                    glr_state_presentation().grid_theme == CFG_DEFAULT_GRID_THEME);
        ASSERT_TRUE("partial cfg grid major reset default",
                    glr_state_presentation().grid_major_idx == CFG_DEFAULT_GRID_MAJOR_IDX);
        ASSERT_TRUE("partial cfg grid extent reset default",
                    glr_state_presentation().grid_extent_idx == CFG_DEFAULT_GRID_EXTENT_IDX);
        ASSERT_TRUE("partial cfg axes reset default",
                    glr_state_presentation().axes_theme == CFG_DEFAULT_AXES_THEME);
        ASSERT_TRUE("partial cfg normals reset default",
                    glr_state_presentation().show_normal_vectors == CFG_DEFAULT_NORMAL_VECTORS);
        ASSERT_TRUE("partial cfg outlines reset default",
                    glr_state_presentation().show_vertex_outlines == CFG_DEFAULT_VERTEX_OUTLINES);
        ASSERT_TRUE("partial cfg points reset default",
                    glr_state_presentation().show_vertex_points == CFG_DEFAULT_VERTEX_POINTS);
        ASSERT_TRUE("partial cfg guides reset default",
                    glr_state_presentation().xform_guide_mode == CFG_DEFAULT_XFORM_GUIDE_MODE);
        ASSERT_TRUE("partial cfg lights reset default",
                    glr_state_presentation().show_light_indicators == CFG_DEFAULT_LIGHT_INDICATORS);
        ASSERT_TRUE("partial cfg camera rotate reset default",
                    glr_camera().auto_rotate == CFG_DEFAULT_CAMERA_ROTATE);
        ASSERT_TRUE("partial cfg keeps camera rx",
                    fabsf(glr_camera().rx - (-41.0f)) < 1e-4f);
    }

    {
        /* view_mode is example-settable via @cfg: the slug maps to
         * GLR_CONFIG_ORTHO_MODE through the cfg bridge, and like other
         * scene-presentation toggles it is reset to its default per
         * example load before any leading @cfg is applied - so a 3D
         * example never silently renders in 2D just because the prior
         * example set ortho. */
        static const char *const view_mode_2d_example[] = {
            "// @cfg view_mode = 1",
            "glBegin(GL_LINE_STRIP);",
            "glVertex3f(0, 0, 0);",
            "glVertex3f(1, 1, 0);",
            "glEnd();",
            NULL
        };
        static const char *const no_view_mode_example[] = {
            "glBegin(GL_POINTS);",
            "glVertex3f(0, 0, 0);",
            "glEnd();",
            NULL
        };
        static const char *const view_mode_3d_example[] = {
            "// @cfg view_mode = 0",
            "glBegin(GL_POINTS);",
            "glVertex3f(0, 0, 0);",
            "glEnd();",
            NULL
        };

        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("view_mode starts at 3D default after reset",
                    glr_state_presentation().ortho_mode == CFG_DEFAULT_ORTHO_MODE);

        repl_load_example_lines(view_mode_2d_example);
        ASSERT_TRUE("@cfg view_mode = 1 applies 2D (ortho)",
                    glr_state_presentation().ortho_mode == RENDER3D_VIEW_2D);

        /* The reset is the load-bearing assertion: a later example with
         * no @cfg view_mode reverts to default 3D, NOT inherited 2D. */
        repl_load_example_lines(no_view_mode_example);
        ASSERT_TRUE("view_mode resets to default on example load",
                    glr_state_presentation().ortho_mode == CFG_DEFAULT_ORTHO_MODE);

        /* @cfg still wins over the per-load reset. */
        repl_load_example_lines(view_mode_2d_example);
        ASSERT_TRUE("@cfg view_mode = 1 still applies after a reset",
                    glr_state_presentation().ortho_mode == RENDER3D_VIEW_2D);
        repl_load_example_lines(view_mode_3d_example);
        ASSERT_TRUE("@cfg view_mode = 0 overrides any prior 2D",
                    glr_state_presentation().ortho_mode == RENDER3D_VIEW_3D);
    }

    {
        static const char *const easing_camera_example[] = {
            "// camera",
            "glTranslatef(0.0f, 0.0f, -10.0f);",
            "glRotatef(40.0f, 1.0f, 0.0f, 0.0f);",
            "glRotatef(64.0f, 0.0f, 1.0f, 0.0f);",
            "glTranslatef(-1.0f, 0.5f, -2.0f);",
            "glBegin(GL_POINTS);",
            "glVertex3f(0, 0, 0);",
            "glEnd();",
            NULL
        };
        GlrCameraState before;
        GlrCameraState after_load;
        GlrCameraState after_tick;

        glr_ctrl_reset_all(); declare_test_vars();
        pin_code_panel_state();
        before = glr_camera();
        repl_load_example_lines(easing_camera_example);
        after_load = glr_camera();
        ASSERT_TRUE("example camera preset does not teleport",
                    fabsf(after_load.rx - before.rx) < 1e-4f &&
                    fabsf(after_load.ry - before.ry) < 1e-4f &&
                    fabsf(after_load.dist - before.dist) < 1e-4f &&
                    fabsf(after_load.tx - before.tx) < 1e-4f &&
                    fabsf(after_load.ty - before.ty) < 1e-4f &&
                    fabsf(after_load.tz - before.tz) < 1e-4f);

        glr_camera_tick();
        after_tick = glr_camera();
        ASSERT_TRUE("example camera preset eases toward target",
                    after_tick.rx > after_load.rx &&
                    after_tick.rx < 40.0f &&
                    after_tick.ry > after_load.ry &&
                    after_tick.ry < 64.0f &&
                    after_tick.dist > after_load.dist &&
                    after_tick.dist < 10.0f);
        settle_camera_transition_for_test();
        ASSERT_TRUE("example camera preset settles on target",
                    camera_pose_near(40.0f, 64.0f, 10.0f,
                                     1.0f, -0.5f, 2.0f));
    }

    {
        int idx = find_example_index_by_name("Dusk lighthouse atoll (stress test)");
        char *dump;

        ASSERT_TRUE("stress example index found", idx >= 0);
        if (idx >= 0) {
            load_example_for_test(idx);
            ASSERT_TRUE("stress example axes preset",
                        glr_state_presentation().axes_theme == AXES_THEME_OFF);
            ASSERT_TRUE("stress example outlines preset",
                        glr_state_presentation().show_vertex_outlines == 0);
            ASSERT_TRUE("stress example backdrop preset",
                        glr_state_presentation().backdrop_mode ==
                        RENDER3D_BACKDROP_STARS);
            ASSERT_TRUE("stress example camera rx preset",
                        fabsf(glr_camera().rx - 26.0f) < 1e-4f);
            ASSERT_TRUE("stress example camera ry preset",
                        fabsf(glr_camera().ry - (-20.0f)) < 1e-4f);
            ASSERT_TRUE("stress example camera dist preset",
                        fabsf(glr_camera().dist - 15.0f) < 1e-4f);
            ASSERT_TRUE("stress example camera tx preset",
                        fabsf(glr_camera().tx - 0.0f) < 1e-4f);
            ASSERT_TRUE("stress example camera ty preset",
                        fabsf(glr_camera().ty - 0.0f) < 1e-4f);
            ASSERT_TRUE("stress example camera tz preset",
                        fabsf(glr_camera().tz - 0.0f) < 1e-4f);

            dump = dump_current_code_panel_text();
            ASSERT_TRUE("stress example camera dump alloc", dump != NULL);
            if (dump) {
                  ASSERT_TRUE("stress example cfg axes hidden",
                        strstr(dump, "// @cfg axes = 0") == NULL);
                  ASSERT_TRUE("stress example cfg outlines hidden",
                        strstr(dump,
                            "// @cfg vertex_outlines = 0") == NULL);
                  ASSERT_TRUE("stress example cfg backdrop hidden",
                        strstr(dump, "// @cfg backdrop = 6") == NULL);
                ASSERT_TRUE("stress example camera marker restored in expanded panel",
                            strstr(dump, "  // camera") != NULL);
                ASSERT_TRUE("stress example camera rotate hidden",
                            strstr(dump,
                                   "glRotatef(26.0f, 1.0f, 0.0f, 0.0f);") == NULL);
                free(dump);
            }
        }
    }

    {
        static const char *const mixed_cfg_camera_example[] = {
            "// @cfg axes = 5",
            "// @cfg accum_effect = 0",
            "// @cfg top_code_panel = 1",
            "// @cfg code_panel = 3",
            "// camera",
            "glTranslatef(0.0f, 0.0f, -9.0f);",
            "glRotatef(11.0f, 1.0f, 0.0f, 0.0f);",
            "glRotatef(-17.0f, 0.0f, 1.0f, 0.0f);",
            "glTranslatef(-0.2f, -0.3f, 0.4f);",
            "glBegin(GL_POINTS);",
            "glVertex3f(0, 0, 0);",
            "glEnd();",
            NULL
        };
        char *dump;

        load_custom_example_lines_for_test(mixed_cfg_camera_example);
        ASSERT_TRUE("mixed cfg camera allowed axes applied",
                    glr_state_presentation().axes_theme == 5);
        ASSERT_TRUE("mixed cfg camera disallowed accum effect ignored",
                    g_accum_effect == RENDER3D_ACCUM_EFFECT_AA);
        ASSERT_TRUE("mixed cfg camera disallowed layout ignored",
                    glr_state_presentation().code_panel_layout == CFG_DEFAULT_CODE_PANEL_LAYOUT);
        ASSERT_TRUE("mixed cfg camera rx preset",
                    fabsf(glr_camera().rx - 11.0f) < 1e-4f);
        ASSERT_TRUE("mixed cfg camera ry preset",
                    fabsf(glr_camera().ry - (-17.0f)) < 1e-4f);
        ASSERT_TRUE("mixed cfg camera dist preset",
                    fabsf(glr_camera().dist - 9.0f) < 1e-4f);
        ASSERT_TRUE("mixed cfg camera tx preset",
                    fabsf(glr_camera().tx - 0.2f) < 1e-4f);
        ASSERT_TRUE("mixed cfg camera ty preset",
                    fabsf(glr_camera().ty - 0.3f) < 1e-4f);
        ASSERT_TRUE("mixed cfg camera tz preset",
                    fabsf(glr_camera().tz - (-0.4f)) < 1e-4f);
        ASSERT_TRUE("mixed cfg camera body cmds loaded", repl_state_document_count() == 3);

        dump = dump_current_code_panel_text();
        ASSERT_TRUE("mixed cfg camera dump alloc", dump != NULL);
        if (dump) {
            ASSERT_TRUE("mixed cfg camera allowed cfg hidden",
                        strstr(dump, "// @cfg axes = 5") == NULL);
            ASSERT_TRUE("mixed cfg camera disallowed cfg hidden",
                        strstr(dump, "// @cfg accum_effect = 0") == NULL);
            ASSERT_TRUE("mixed cfg camera layout cfg hidden",
                        strstr(dump, "// @cfg code_panel = 3") == NULL);
            ASSERT_TRUE("mixed cfg camera marker restored in expanded panel",
                        strstr(dump, "  // camera") != NULL);
            ASSERT_TRUE("mixed cfg camera body kept",
                        strstr(dump, "glVertex3f(0, 0, 0);") != NULL);
            free(dump);
        }
    }

    {
        static const char *const nonleading_cfg_example[] = {
            "// plain comment",
            "// @cfg axes = 4",
            "glBegin(GL_POINTS);",
            "glVertex3f(1, 0, 0);",
            "glEnd();",
            NULL
        };
        char *dump;

        load_custom_example_lines_for_test(nonleading_cfg_example);
        ASSERT_TRUE("nonleading cfg leaves axes unchanged",
                    glr_state_presentation().axes_theme == CFG_DEFAULT_AXES_THEME);
        ASSERT_TRUE("nonleading cfg comments preserved",
                repl_state_document_count() == 5);

        dump = dump_current_code_panel_text();
        ASSERT_TRUE("nonleading cfg dump alloc", dump != NULL);
        if (dump) {
            ASSERT_TRUE("nonleading cfg comment remains visible",
                        strstr(dump, "// @cfg axes = 4") != NULL);
            free(dump);
        }
    }

    {
        static const char *const spaced_cfg_camera_example[] = {
            "// @cfg axes = 4",
            "",
            "// --- Camera -------------------------",
            "glTranslatef(0.0f, 0.0f, -8.0f);",
            "glRotatef(14.0f, 1.0f, 0.0f, 0.0f);",
            "glRotatef(27.0f, 0.0f, 1.0f, 0.0f);",
            "glTranslatef(-0.25f, 0.5f, -0.75f);",
            "glBegin(GL_POINTS);",
            "glVertex3f(0, 0, 0);",
            "glEnd();",
            NULL
        };
        char *dump;

        glr_ctrl_reset_all(); declare_test_vars();
        pin_code_panel_state();
        repl_load_example_lines(spaced_cfg_camera_example);
        ASSERT_TRUE("spaced cfg camera allowed cfg applied",
                    glr_state_presentation().axes_theme == 4);
        ASSERT_TRUE("decorated camera marker preserved for expanded panel",
                    strcmp(repl_state_import_export().camera_comment_line,
                           "  // --- Camera -------------------------") == 0);

        dump = dump_current_code_panel_text();
        ASSERT_TRUE("spaced cfg camera immediate dump alloc", dump != NULL);
        if (dump) {
            ASSERT_TRUE("spaced cfg camera immediate dump uses target",
                        strstr(dump,
                               "glTranslatef(0.0000f, 0.0000f, -8.0000f);") != NULL);
            ASSERT_TRUE("decorated camera marker restored in expanded panel",
                        strstr(dump, "  // --- Camera") != NULL);
            ASSERT_TRUE("spaced cfg camera rotate hidden",
                        strstr(dump,
                               "glRotatef(14.0f, 1.0f, 0.0f, 0.0f);") == NULL);
            ASSERT_TRUE("spaced cfg camera body kept",
                        strstr(dump, "glVertex3f(0, 0, 0);") != NULL);
            free(dump);
        }

        settle_camera_transition_for_test();
        ASSERT_TRUE("spaced cfg camera rx preset",
                    fabsf(glr_camera().rx - 14.0f) < 1e-4f);
        ASSERT_TRUE("spaced cfg camera ry preset",
                    fabsf(glr_camera().ry - 27.0f) < 1e-4f);
        ASSERT_TRUE("spaced cfg camera dist preset",
                    fabsf(glr_camera().dist - 8.0f) < 1e-4f);
        ASSERT_TRUE("spaced cfg camera tx preset",
                    fabsf(glr_camera().tx - 0.25f) < 1e-4f);
        ASSERT_TRUE("spaced cfg camera ty preset",
                    fabsf(glr_camera().ty - (-0.5f)) < 1e-4f);
        ASSERT_TRUE("spaced cfg camera tz preset",
                    fabsf(glr_camera().tz - 0.75f) < 1e-4f);
        ASSERT_TRUE("spaced cfg camera body cmds loaded", repl_state_document_count() == 3);
    }

    {
        /* Normalization is exact after punctuation is removed. A prose
         * comment containing "camera" must remain ordinary scene source,
         * along with the transforms that follow it. */
        static const char *const prose_camera_example[] = {
            "// The camera starts here.",
            "glTranslatef(0.0f, 0.0f, -8.0f);",
            "glRotatef(14.0f, 1.0f, 0.0f, 0.0f);",
            "glRotatef(27.0f, 0.0f, 1.0f, 0.0f);",
            "glTranslatef(-0.25f, 0.5f, -0.75f);",
            "glBegin(GL_POINTS);",
            "glVertex3f(0, 0, 0);",
            "glEnd();",
            NULL
        };
        char *dump;

        load_custom_example_lines_for_test(prose_camera_example);
        ASSERT_TRUE("camera prose keeps every source line",
                    repl_state_document_count() == 8);
        ASSERT_TRUE("camera prose does not become expanded-panel marker",
                    repl_state_import_export().camera_comment_line[0] == '\0');

        dump = dump_current_code_panel_text();
        ASSERT_TRUE("camera prose dump alloc", dump != NULL);
        if (dump) {
            ASSERT_TRUE("camera prose comment remains visible",
                        strstr(dump, "// The camera starts here.") != NULL);
            ASSERT_TRUE("camera prose transforms remain visible",
                        strstr(dump, "glRotatef(14") != NULL);
            free(dump);
        }
    }

    {
        /* Shape-check fails (dist_x = 1.0f > 1e-4f) so the camera bridge
         * is NOT applied. Pre-#12 the loader still skipped the 5 lines
         * after `// camera` regardless, eating the marker + 4 would-be
         * camera transforms before geometry parsing resumed. Post-#12
         * the lines are left for ordinary parsing so the user sees
         * what they typed instead of silent loss. Camera state is
         * still untouched (the bridge rejected the block). */
        static const char *const invalid_camera_example[] = {
            "// camera",
            "glTranslatef(1.0f, 0.0f, -9.0f);",
            "glRotatef(20.0f, 1.0f, 0.0f, 0.0f);",
            "glRotatef(91.0f, 0.0f, 1.0f, 0.0f);",
            "glTranslatef(-0.5f, 0.0f, 0.0f);",
            "glBegin(GL_POINTS);",
            "glVertex3f(1, 2, 3);",
            "glEnd();",
            NULL
        };
        char *dump;

        load_custom_example_lines_for_test(invalid_camera_example);
        ASSERT_TRUE("invalid camera header keeps all body cmds (no data loss)",
                    repl_state_document_count() == 8);
        ASSERT_TRUE("invalid camera header keeps cmds valid",
                    examples_have_no_invalid_cmds());
        ASSERT_TRUE("invalid camera header preserves rx",
                    fabsf(glr_camera().rx - 18.0f) < 1e-4f);
        ASSERT_TRUE("invalid camera header preserves ry",
                    fabsf(glr_camera().ry - 32.0f) < 1e-4f);
        ASSERT_TRUE("invalid camera header preserves dist",
                    fabsf(glr_camera().dist - 5.5f) < 1e-4f);
        ASSERT_TRUE("invalid camera header preserves tx",
                    fabsf(glr_camera().tx - 0.0f) < 1e-4f);
        ASSERT_TRUE("invalid camera header preserves ty",
                    fabsf(glr_camera().ty - 0.0f) < 1e-4f);
        ASSERT_TRUE("invalid camera header preserves tz",
                    fabsf(glr_camera().tz - 0.0f) < 1e-4f);

        dump = dump_current_code_panel_text();
        ASSERT_TRUE("invalid camera header dump alloc", dump != NULL);
        if (dump) {
            ASSERT_TRUE("invalid camera marker kept",
                        strstr(dump, "// camera") != NULL);
            ASSERT_TRUE("invalid camera rotate kept",
                        strstr(dump, "glRotatef(91") != NULL);
            ASSERT_TRUE("invalid camera translate kept",
                        strstr(dump, "glTranslatef(1") != NULL);
            ASSERT_TRUE("invalid camera body kept",
                        strstr(dump, "glVertex3f(1, 2, 3);") != NULL);
            free(dump);
        }
    }

    {
        /* Regression for #12 - a truncated camera header (`// camera`
         * + 2 transforms then geometry) used to eat the marker, both
         * transforms, AND the first 2 geometry lines (5-line skip
         * regardless of validation). Post-fix every line survives;
         * camera state is left untouched because the bridge rejected
         * the malformed 2-line block. */
        static const char *const truncated_camera_example[] = {
            "// camera",
            "glTranslatef(0.0f, 0.0f, -9.0f);",
            "glRotatef(20.0f, 1.0f, 0.0f, 0.0f);",
            "glBegin(GL_POINTS);",
            "glVertex3f(7, 8, 9);",
            "glEnd();",
            NULL
        };
        char *dump;

        load_custom_example_lines_for_test(truncated_camera_example);
        ASSERT_TRUE("truncated camera header keeps all body cmds (no data loss)",
                    repl_state_document_count() == 6);
        ASSERT_TRUE("truncated camera header keeps cmds valid",
                    examples_have_no_invalid_cmds());
        ASSERT_TRUE("truncated camera header preserves rx",
                    fabsf(glr_camera().rx - 18.0f) < 1e-4f);
        ASSERT_TRUE("truncated camera header preserves ry",
                    fabsf(glr_camera().ry - 32.0f) < 1e-4f);
        ASSERT_TRUE("truncated camera header preserves dist",
                    fabsf(glr_camera().dist - 5.5f) < 1e-4f);

        dump = dump_current_code_panel_text();
        ASSERT_TRUE("truncated camera header dump alloc", dump != NULL);
        if (dump) {
            ASSERT_TRUE("truncated camera geometry kept",
                        strstr(dump, "glVertex3f(7, 8, 9);") != NULL);
            free(dump);
        }
    }

    for (int idx = 0; idx < repl_example_count(); idx++) {
        char fixture_path[256];
        char label[160];
        const char *name;
        char *actual;
        char *expected;
        char *imported;
        char *expected_defs;
        char *loaded_defs;
        char *imported_defs;
        char export_path[512];
        char reexport_path[512];
        char compile_detail[4096];
        char reexport_detail[4096];
        int diff_line = 0;
        int exact;
        int compiled;
        int roundtrip_loaded;
        int roundtrip_exact;
        int roundtrip_compiled;
        int original_cmd_count;
        int defs_exact;
        int imported_defs_exact;

        load_example_for_test(idx);
        name = repl_example_name(idx);
        log_example_step(idx, name, "verify", "loaded example into REPL state");
        snprintf(label, sizeof(label), "example %02d loads cmds", idx);
        ASSERT_TRUE(label, repl_state_document_count() > 0);
        original_cmd_count = repl_state_document_count();
        snprintf(label, sizeof(label), "example %02d has public name", idx);
        ASSERT_TRUE(label, name != NULL);
        snprintf(label, sizeof(label), "example %02d has no invalid cmds", idx);
        ASSERT_TRUE(label, examples_have_no_invalid_cmds());

        expected_defs = collect_example_definition_lines(idx);
        snprintf(label, sizeof(label), "example %02d expected defs alloc", idx);
        ASSERT_TRUE(label, expected_defs != NULL);
        loaded_defs = collect_loaded_definition_lines();
        snprintf(label, sizeof(label), "example %02d loaded defs alloc", idx);
        ASSERT_TRUE(label, loaded_defs != NULL);
        defs_exact = 0;
        if (expected_defs && loaded_defs) {
            defs_exact = compare_exact_text(expected_defs, loaded_defs, &diff_line);
            if (!defs_exact) {
                printf("DETAIL [example %02d definition mismatch] name=%s line=%d\nEXPECTED DEFS:\n%sACTUAL DEFS:\n%s\n",
                       idx, name, diff_line, expected_defs, loaded_defs);
                print_text_mismatch("example definition mismatch",
                                    "source example definitions",
                                    "loaded command definitions",
                                    expected_defs, loaded_defs, diff_line);
            }
        }
        snprintf(label, sizeof(label), "example %02d definitions match source arrays", idx);
        ASSERT_TRUE(label, defs_exact);
        free(loaded_defs);
        loaded_defs = NULL;

        actual = dump_current_code_panel_text();
        snprintf(label, sizeof(label), "example %02d dump alloc", idx);
        ASSERT_TRUE(label, actual != NULL);
        if (!actual)
        {
            free(expected_defs);
            continue;
        }

        fixture_path_for_idx(idx, fixture_path, sizeof(fixture_path));
        log_example_step(idx, name, "fixture", fixture_path);
        expected = slurp_path(fixture_path);
        snprintf(label, sizeof(label), "example %02d fixture exists", idx);
        ASSERT_TRUE(label, expected != NULL);
        if (!expected) {
            free(actual);
            continue;
        }

        exact = compare_exact_text(expected, actual, &diff_line);
        if (!exact) {
            printf("DETAIL [example %02d fixture mismatch] name=%s line=%d fixture=%s\n",
                   idx, repl_example_name(idx), diff_line, fixture_path);
            print_text_mismatch("example fixture mismatch",
                                fixture_path, "current code panel dump",
                                expected, actual, diff_line);
            printf("NOTE: if this change is intentional, regenerate the golden file:\n"
                   "  %s --dump-index %d > %s\n",
                   argv[0], idx, fixture_path);
        }
        snprintf(label, sizeof(label), "example %02d fixture matches", idx);
        ASSERT_TRUE(label, exact);

        snprintf(export_path, sizeof(export_path), "%s/example_%02d.c",
                 temp_dir, idx);
        log_example_step(idx, name, "export", export_path);
        repl_export_save_output(export_path, source_document_view(), NULL);
        compile_detail[0] = '\0';
        compiled = compile_exported_source(idx, name, export_path,
                                           compile_detail,
                                           sizeof(compile_detail));
        if (!compiled) {
            printf("DETAIL [example %02d export compile failed] name=%s file=%s\n%s\n",
                   idx, name, export_path, compile_detail);
        }
        snprintf(label, sizeof(label), "example %02d export compiles", idx);
        ASSERT_TRUE(label, compiled);

        glr_ctrl_reset_all();
        pin_code_panel_state();
        roundtrip_loaded = repl_export_load_from_file(export_path, NULL);
        snprintf(label, sizeof(label), "example %02d export imports", idx);
        ASSERT_TRUE(label, roundtrip_loaded == 1);

        roundtrip_exact = 0;
        imported = NULL;
        imported_defs = NULL;
        roundtrip_compiled = 0;
        imported_defs_exact = 0;
        if (roundtrip_loaded == 1) {
            snprintf(label, sizeof(label), "example %02d import cmd count", idx);
            ASSERT_TRUE(label, repl_state_document_count() == original_cmd_count);
            snprintf(label, sizeof(label), "example %02d import has no invalid cmds", idx);
            ASSERT_TRUE(label, examples_have_no_invalid_cmds());

            imported_defs = collect_loaded_definition_lines();
            snprintf(label, sizeof(label), "example %02d imported defs alloc", idx);
            ASSERT_TRUE(label, imported_defs != NULL);
            if (expected_defs && imported_defs) {
                imported_defs_exact = compare_exact_text(expected_defs, imported_defs,
                                                         &diff_line);
                if (!imported_defs_exact) {
                    printf("DETAIL [example %02d definition roundtrip mismatch] name=%s line=%d export=%s\nEXPECTED DEFS:\n%sIMPORTED DEFS:\n%s\n",
                           idx, name, diff_line, export_path, expected_defs, imported_defs);
                    print_text_mismatch("definition roundtrip mismatch",
                                        "source example definitions",
                                        "imported definitions",
                                        expected_defs, imported_defs, diff_line);
                }
            }
            snprintf(label, sizeof(label), "example %02d definitions roundtrip", idx);
            ASSERT_TRUE(label, imported_defs_exact);
            free(imported_defs);
            imported_defs = NULL;

            imported = dump_current_code_panel_text();
            snprintf(label, sizeof(label), "example %02d import dump alloc", idx);
            ASSERT_TRUE(label, imported != NULL);
            if (imported) {
                char *actual_cmp_raw = strip_decl_trailing_comments(actual);
                char *imported_cmp_raw = strip_decl_trailing_comments(imported);
                char *actual_cmp_floats = canonicalize_text_floats(actual_cmp_raw);
                char *imported_cmp_floats = canonicalize_text_floats(imported_cmp_raw);
                char *actual_cmp = strip_declaration_initializers(actual_cmp_floats);
                char *imported_cmp = strip_declaration_initializers(imported_cmp_floats);
                roundtrip_exact = compare_exact_text(actual_cmp, imported_cmp,
                                                     &diff_line);
                if (!roundtrip_exact) {
                    printf("%sDETAIL [example %02d export/import mismatch] name=%s line=%d export=%s%s\n",
                           ansi_yellow(), idx, name, diff_line, export_path,
                           ansi_reset());
                    print_text_mismatch("export/import mismatch",
                                        "pre-export code panel",
                                        "imported code panel",
                                        actual_cmp, imported_cmp, diff_line);
                }
                free(actual_cmp);
                free(imported_cmp);
                free(actual_cmp_floats);
                free(imported_cmp_floats);
                free(actual_cmp_raw);
                free(imported_cmp_raw);
                snprintf(label, sizeof(label),
                         "example %02d exact export/import roundtrip", idx);
                ASSERT_TRUE(label, roundtrip_exact);

                snprintf(reexport_path, sizeof(reexport_path),
                         "%s/example_%02d_roundtrip.c", temp_dir, idx);
                log_example_step(idx, name, "re-export", reexport_path);
                repl_export_save_output(reexport_path, source_document_view(), NULL);
                reexport_detail[0] = '\0';
                roundtrip_compiled = compile_exported_source(idx, name, reexport_path,
                                                             reexport_detail,
                                                             sizeof(reexport_detail));
                if (!roundtrip_compiled) {
                    printf("DETAIL [example %02d re-export compile failed] name=%s file=%s\n%s\n",
                           idx, name, reexport_path, reexport_detail);
                }
                snprintf(label, sizeof(label), "example %02d re-export compiles", idx);
                ASSERT_TRUE(label, roundtrip_compiled);
                if (!g_keep_temp)
                    remove(reexport_path);
                free(imported);
            }
        }

        if (!g_keep_temp)
            remove(export_path);

        free(expected_defs);
        free(expected);
        free(actual);
    }

    /*
     * The golden fixtures intentionally stay logical, not visual: they still
     * assert one logical row per header/source line. Wrapped-row rendering is
     * covered separately by the visual code-panel dump tests.
     */
    {
        /*
         * The anchor is Mars' JPL Keplerian element list in the orrery: at
         * ~165 columns it is the longest source line in the catalog, and it
         * is long for a reason no refactor can shorten (twelve orbital
         * elements, one per argument). Earlier revisions of this test pinned
         * whichever dense expression happened to be longest, and every pass
         * that factored one out had to repoint the fixture.
         */
        int wrap_idx = find_example_index_by_name("Orrery (labels track 3D orbits)");

        ASSERT_TRUE("wrap placeholder example found", wrap_idx >= 0);
        if (wrap_idx >= 0) {
            load_example_for_test(wrap_idx);
            {
                char *dump = dump_current_code_panel_text();
                ASSERT_TRUE("wrap placeholder dump alloc", dump != NULL);
                if (dump) {
                    ASSERT_TRUE("logical dump keeps long line unwrapped",
                                strstr(dump,
                                       "  planetKepler(1.52371034, 0.00001847, 0.09339410, 0.00007882, 1.84969142, -0.00813131, -4.55343205, 19140.30268499, -23.94362959, 0.44441088, 49.55953891, -0.29257343);")
                                != NULL);
                    free(dump);
                }
            }
        }
    }

    if (g_keep_temp)
        fprintf(stderr, "repl_core_examples: keeping temp dir %s\n", temp_dir);
    else
        rmdir(temp_dir);

    printf("%srepl_core_examples: %d/%d passed%s\n",
           (g_harness.run == g_harness.passed) ? ansi_green() : ansi_red(),
           g_harness.passed, g_harness.run, ansi_reset());
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
