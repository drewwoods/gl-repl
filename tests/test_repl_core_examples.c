#include "editor/state.h"
#include "app/glr_camera.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
// For linux mkdtemp
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "repl/example_loader.h"  /* repl_load_example_lines_for_test */
#include "repl/examples.h"
#include "repl/state_owners.h"
#include <math.h>
#include "repl/export.h"
#include "source_document.h"
#include "repl/text_helpers.h"
#include "repl/util.h"
#include "ui/app/layout.h"      /* CODE_PANEL_LAYOUT_* enum values */
#include "ui/app/state.h"
#include "scene/render.h"
#include "scene/themes.h"       /* GRID_THEME_*, AXES_THEME_*, SCENE_BACKDROP_* */
#include "app/glr_defaults.h"   /* CFG_DEFAULT_* */

#define g_accum_effect        (glr_state_render_mut()->accum_effect)
#define g_multisample_enabled (glr_state_render_mut()->multisample_enabled)
#define g_line_smooth_enabled (glr_state_render_mut()->line_smooth_enabled)

#include "support/test_harness.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    g_accum_effect = SCENE_ACCUM_EFFECT_AA;
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
    glr_state_presentation_mut()->show_vertex_indices = 0; glr_ctrl_sync_ui_chrome();
    glr_state_presentation_mut()->show_normal_vectors = 1;
    glr_state_presentation_mut()->show_vertex_outlines = 0;
    glr_state_presentation_mut()->show_vertex_points = 0;
    glr_state_presentation_mut()->xform_guide_mode = SCENE_XFORM_GUIDE_OFF;
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
        if (!repl_state_document_cmds_mut()[i].valid)
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

    out = (char *)malloc(strlen(canon_float_line) + 1);
    if (!out)
        return NULL;
    strcpy(out, canon_float_line);
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
        if (repl_state_document_cmds_mut()[i].type != CMD_VAR_DECLARE && repl_state_document_cmds_mut()[i].type != CMD_VAR_ASSIGN)
            continue;
        count++;
    }

    canon_lines = (char **)malloc((size_t)(count > 0 ? count : 1) * sizeof(*canon_lines));
    if (!canon_lines)
        return NULL;

    count = 0;
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds_mut()[i].type != CMD_VAR_DECLARE && repl_state_document_cmds_mut()[i].type != CMD_VAR_ASSIGN)
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
    repl_load_example_lines_for_test(lines);
    settle_camera_transition_for_test();
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

    stress_idx = find_example_index_by_name("Stress test (all features)");
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
 *     run of examples (matches the menu walker's emit rule — one header
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

    /* Per tag: contiguity check — count distinct non-NULL subheadings
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
    int stress_idx = find_example_index_by_name("Stress test (all features)");
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
 *      grid default (CFG_DEFAULT_GRID_THEME) — no tag default applies.
 *   4. An example tagged 2D but with its own `@cfg grid = N` keeps N
 *      (example `@cfg` wins over the tag default).
 *
 * Precedence chain under test: example `@cfg` > tag default > global
 * default. */
/* Audit #41: example catalogs must use symbolic value names for
 * scene-enum slugs (grid, axes, backdrop) so reordering the matching
 * enum in src/scene/themes.h can't silently shift an example to a
 * different theme. Scan every example's leading metadata block; any
 * `@cfg <slug> = <val>` that targets a scene-enum slug must have a
 * non-digit value (i.e., a symbolic name, not a raw integer). */
static void test_example_cfg_uses_symbolic_names(void) {
    static const char *const enum_slugs[] = { "grid", "axes", "backdrop" };
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

static void test_example_tag_default_cfg(void) {
    int bezier_idx     = find_example_index_by_name("Bezier curve with guides");
    int cube_idx       = find_example_index_by_name("Lit cube");
    int stress_idx     = find_example_index_by_name("Stress test (all features)");
    int spirograph_idx =
        find_example_index_by_name("Animated spirograph curve");

    ASSERT_TRUE("bezier example index found", bezier_idx >= 0);
    ASSERT_TRUE("cube example index found", cube_idx >= 0);
    ASSERT_TRUE("stress example index found", stress_idx >= 0);
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
    if (spirograph_idx >= 0) {
        ASSERT_TRUE("spirograph is in 2D bucket",
                    repl_example_has_tag(spirograph_idx, REPL_EXAMPLE_TAG_2D));
    }

    /* (1) 2D-only-bucket example (in 2D, not in 3D), no own grid @cfg
     * → GRID_THEME_PLANES. The bezier example has @cfg lines for
     * other slugs but no `@cfg grid`. */
    if (bezier_idx >= 0) {
        load_example_for_test(bezier_idx);
        ASSERT_TRUE("2D tag default applies GRID_THEME_PLANES (2D-only)",
                    glr_state_presentation().grid_theme == GRID_THEME_PLANES);
    }

    /* (2) Multi-tag example including 2D, no own grid @cfg →
     * GRID_THEME_PLANES. The spirograph example is 2D|LINES with no
     * explicit grid override. */
    if (spirograph_idx >= 0) {
        load_example_for_test(spirograph_idx);
        ASSERT_TRUE("2D tag default applies GRID_THEME_PLANES (multi tag)",
                    glr_state_presentation().grid_theme == GRID_THEME_PLANES);
    }

    /* (3) 3D-only example → global default, no tag override. */
    if (cube_idx >= 0) {
        load_example_for_test(cube_idx);
        ASSERT_TRUE("non-2D example uses global grid default",
                    glr_state_presentation().grid_theme ==
                    CFG_DEFAULT_GRID_THEME);
    }

    /* (4) Example with its own @cfg grid → the explicit value wins over the
     * tag / global default. Stress is 3D-only and sets grid =
     * GRID_THEME_AURORA. */
    if (stress_idx >= 0) {
        load_example_for_test(stress_idx);
        ASSERT_TRUE("example @cfg grid overrides default",
                    glr_state_presentation().grid_theme == GRID_THEME_AURORA);
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
 * table — no example loaded, no @cfg in play — so the only thing
 * mutating state here is the helper itself. glr_ctrl_reset_all
 * normalizes presentation to global defaults before each subcase. */
static void test_example_tag_default_dispatch(void) {
    /* (A) Two entries, distinct keys, both tags present → both apply. */
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

    /* (B) Two entries colliding on the same key for the same tag →
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
     * — both tag bits set in the mask → still a collision (the mask
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
     * only ONE of them → no collision, the matching entry applies. */
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

    /* (E) Empty mask → nothing applied, no collisions. */
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
    printf("  for i in $(seq -f '%%02g' 0 N); do\n");
    printf("    %s --dump-index $i > tests/testdata/repl_examples_ui/$i.golden.txt\n",
           prog);
    printf("  done\n");
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

int main(int argc, char **argv) {
    char temp_dir[] = "/tmp/repl_examples_export.XXXXXX";
    const char *verbose_env = getenv("REPL_EXPORT_VERBOSE");
    int dump_idx = -1;

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

    if (dump_idx >= 0)
        return dump_single_example_to_stdout(dump_idx);

    if (!mkdtemp(temp_dir)) {
        perror("mkdtemp");
        return 1;
    }

    repl_eval_init_predef_vars();
    test_example_tag_metadata();
    test_example_subheading_metadata();
    test_example_tag_default_cfg();
    test_example_tag_default_dispatch();
    test_example_cfg_uses_symbolic_names();

    {
        static const char *const no_cfg_reset_example[] = {
            "glBegin(GL_POINTS);",
            "glVertex3f(0, 0, 0);",
            "glEnd();",
            NULL
        };

        glr_ctrl_reset_all(); declare_test_vars();
        seed_nondefault_example_presentation_state();
        repl_load_example_lines_for_test(no_cfg_reset_example);

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
        ASSERT_TRUE("no cfg reset indices default",
                    glr_state_presentation().show_vertex_indices == CFG_DEFAULT_VERTEX_INDICES);
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
        repl_load_example_lines_for_test(partial_cfg_reset_example);

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
         * example load before any leading @cfg is applied — so a 3D
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

        repl_load_example_lines_for_test(view_mode_2d_example);
        ASSERT_TRUE("@cfg view_mode = 1 applies 2D (ortho)",
                    glr_state_presentation().ortho_mode == SCENE_VIEW_2D);

        /* The reset is the load-bearing assertion: a later example with
         * no @cfg view_mode reverts to default 3D, NOT inherited 2D. */
        repl_load_example_lines_for_test(no_view_mode_example);
        ASSERT_TRUE("view_mode resets to default on example load",
                    glr_state_presentation().ortho_mode == CFG_DEFAULT_ORTHO_MODE);

        /* @cfg still wins over the per-load reset. */
        repl_load_example_lines_for_test(view_mode_2d_example);
        ASSERT_TRUE("@cfg view_mode = 1 still applies after a reset",
                    glr_state_presentation().ortho_mode == SCENE_VIEW_2D);
        repl_load_example_lines_for_test(view_mode_3d_example);
        ASSERT_TRUE("@cfg view_mode = 0 overrides any prior 2D",
                    glr_state_presentation().ortho_mode == SCENE_VIEW_3D);
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
        repl_load_example_lines_for_test(easing_camera_example);
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
        int idx = find_example_index_by_name("Stress test (all features)");
        char *dump;

        ASSERT_TRUE("stress example index found", idx >= 0);
        if (idx >= 0) {
            load_example_for_test(idx);
            ASSERT_TRUE("stress example axes preset",
                        glr_state_presentation().axes_theme == 4);
            ASSERT_TRUE("stress example outlines preset",
                        glr_state_presentation().show_vertex_outlines == 0);
            ASSERT_TRUE("stress example backdrop preset",
                        glr_state_presentation().backdrop_mode ==
                        SCENE_BACKDROP_AURORA);
            ASSERT_TRUE("stress example camera rx preset",
                        fabsf(glr_camera().rx - 27.5f) < 1e-4f);
            ASSERT_TRUE("stress example camera ry preset",
                        fabsf(glr_camera().ry - (-24.0f)) < 1e-4f);
            ASSERT_TRUE("stress example camera dist preset",
                        fabsf(glr_camera().dist - 12.5f) < 1e-4f);
            ASSERT_TRUE("stress example camera tx preset",
                        fabsf(glr_camera().tx - 0.6f) < 1e-4f);
            ASSERT_TRUE("stress example camera ty preset",
                        fabsf(glr_camera().ty - 0.1f) < 1e-4f);
            ASSERT_TRUE("stress example camera tz preset",
                        fabsf(glr_camera().tz - 0.4f) < 1e-4f);

            dump = dump_current_code_panel_text();
            ASSERT_TRUE("stress example camera dump alloc", dump != NULL);
            if (dump) {
                  ASSERT_TRUE("stress example cfg axes hidden",
                        strstr(dump, "// @cfg axes = 4") == NULL);
                  ASSERT_TRUE("stress example cfg outlines hidden",
                        strstr(dump,
                            "// @cfg vertex_outlines = 0") == NULL);
                  ASSERT_TRUE("stress example cfg backdrop hidden",
                        strstr(dump, "// @cfg backdrop = 1") == NULL);
                ASSERT_TRUE("stress example camera marker hidden",
                            strstr(dump, "// camera") == NULL);
                ASSERT_TRUE("stress example camera rotate hidden",
                            strstr(dump,
                                   "glRotatef(27.5f, 1.0f, 0.0f, 0.0f);") == NULL);
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
                    g_accum_effect == SCENE_ACCUM_EFFECT_AA);
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
            ASSERT_TRUE("mixed cfg camera marker hidden",
                        strstr(dump, "// camera") == NULL);
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
        /* Regression for #12 — a truncated camera header (`// camera`
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
                char *actual_cmp = canonicalize_text_floats(actual_cmp_raw);
                char *imported_cmp = canonicalize_text_floats(imported_cmp_raw);
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
        int stress_idx = find_example_index_by_name("Stress test (all features)");

        ASSERT_TRUE("wrap placeholder stress example found", stress_idx >= 0);
        if (stress_idx >= 0) {
            load_example_for_test(stress_idx);
            {
                char *dump = dump_current_code_panel_text();
                ASSERT_TRUE("wrap placeholder dump alloc", dump != NULL);
                if (dump) {
                         ASSERT_TRUE("logical dump keeps stress line unwrapped",
                                strstr(dump,
                                       "      n = 1.0/sqrt(1 + amp*amp*6.25*(cos(x*2.5 + phase)*cos(x*2.5 + phase)*cos(z*2.5 + phase*0.7)*cos(z*2.5 + phase*0.7) + sin(x*2.5 + phase)*sin(x*2.5 + phase)*sin(z*2.5 + phase*0.7)*sin(z*2.5 + phase*0.7)));")
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
