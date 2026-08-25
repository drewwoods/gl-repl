/*
 * tests/test_camera_header_parity.c - The two loaders must agree.
 *
 * This is the test the whole plan is built around. For every `.glr` in the
 * corpus, load it *as a file* (src/repl/import.c) and *via the catalog path*
 * (src/repl/example_loader.c) and compare what each produced:
 *
 *   - the normalized source text of every document row, and the row count;
 *   - the command structure - CmdType sequence and per-command arg values;
 *   - the resolved camera pose, observed through the shared recording bridge;
 *   - predefined variable names and values;
 *   - the diagnostic list, as ordered (role, rule, line_no) triples plus the
 *     overflow count.
 *
 * Not compared: a document row's cached `args[]`. Those are a snapshot of the
 * last evaluation of an expression row, not part of the document - a row like
 * `glColor3f(1 - 0.85*s, ...)` carries whatever `s` happened to hold when the
 * row was parsed, and the flat program re-evaluates it every frame anyway. The
 * source text, CmdType and arity pin the structure; comparing the cache would
 * be comparing an artifact.
 *
 * Comparing row counts alone would pass a file whose camera lines became
 * geometry as long as the count matched, which is precisely the bug this
 * comes from: one loader ate two of three transforms and left the third as
 * geometry, and the tree rendered 2.5 units low on one path only.
 *
 * Both loads run against the shared recording stub bridge; without a bridge
 * installed there is no pose to compare.
 */
#include <dirent.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "app/glr_ctrl.h"
#include "repl/camera_header.h"
#include "repl/doc_order.h"
#include "repl/command.h"
#include "repl/example_loader.h"
#include "repl/export.h"
#include "repl/host_effects.h"
#include "repl/state.h"
#include "repl/text_helpers.h"
#include "source_document.h"
#include "support/camera_bridge_stub.h"
#include "support/scene_corpus.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond)      TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, want)  TEST_ASSERT_INT(&g_harness, label, got, want)

/* A load's whole observable result, so the comparison is a struct compare
 * rather than a list of ad-hoc assertions that can quietly stop covering
 * something. */
#define PARITY_MAX_ROWS  MAX_EDITOR_COMMANDS

typedef struct {
    int   ok;
    int   row_count;
    char  rows[PARITY_MAX_ROWS][MAX_LINE_LEN];
    int   types[PARITY_MAX_ROWS];
    int   num_args[PARITY_MAX_ROWS];

    int            pose_applied;
    ReplCameraPose pose;

    int   num_predefs;
    char  predef_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX];
    float predef_vals[MAX_PREDEF_VARS];
} ParityResult;

static void parity_capture(ParityResult *out) {
    SourceTextView text = source_document_view();
    const GLCmd *cmds = repl_state_document_cmds();
    int count = repl_state_document_count();
    int i;

    if (count > PARITY_MAX_ROWS)
        count = PARITY_MAX_ROWS;
    out->row_count = count;
    for (i = 0; i < count; i++) {
        const char *line = source_text_line(text, i);
        snprintf(out->rows[i], MAX_LINE_LEN, "%s", line ? line : "");
        out->types[i]    = (int)cmds[i].type;
        out->num_args[i] = cmds[i].num_args;
    }

    out->pose_applied = g_camera_bridge_stub.apply_count > 0;
    out->pose         = g_camera_bridge_stub.applied;

    {
        ReplVariableView vars = repl_state_variables();
        int n = vars.var_count > MAX_PREDEF_VARS ? MAX_PREDEF_VARS
                                                 : vars.var_count;
        out->num_predefs = n;
        for (i = 0; i < n; i++) {
            snprintf(out->predef_names[i], REPL_PREDEF_NAME_MAX, "%s",
                     vars.vars[i].name);
            out->predef_vals[i] = vars.vars[i].value;
        }
    }
}

/* Read a file into a NULL-terminated line array - the shape the catalog path
 * is handed. For a `.glr` that array *is* the file's lines, which is what
 * makes a 1-based array index and the importer's file line the same number,
 * and therefore makes line_no a parity key rather than an approximation. */
static char **parity_read_lines(const char *path, int *out_count) {
    FILE *f = fopen(path, "r");
    char line[MAX_LINE_LEN];
    char **lines = NULL;
    int count = 0, cap = 0;

    if (!f)
        return NULL;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (count + 2 > cap) {
            cap = cap ? cap * 2 : 64;
            lines = (char **)realloc(lines, (size_t)cap * sizeof(*lines));
            if (!lines)
                break;
        }
        lines[count] = strdup(line);
        if (!lines[count])
            break;
        count++;
    }
    fclose(f);
    if (lines)
        lines[count] = NULL;
    if (out_count)
        *out_count = count;
    return lines;
}

static void parity_free_lines(char **lines) {
    int i;
    if (!lines)
        return;
    for (i = 0; lines[i]; i++)
        free(lines[i]);
    free(lines);
}

/* Read a text fixture whole so the stability comparison can normalize a
 * numeric token without touching comments or changing the comparison's
 * ordering/whitespace sensitivity. */
static char *parity_read_file_text(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    long size;
    char *text;

    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f) fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    text = (char *)malloc((size_t)size + 1);
    if (!text) {
        fclose(f);
        return NULL;
    }
    if (fread(text, 1, (size_t)size, f) != (size_t)size) {
        free(text);
        fclose(f);
        return NULL;
    }
    fclose(f);
    text[size] = '\0';
    if (out_len)
        *out_len = (size_t)size;
    return text;
}

/* The .glr writer emits the exact float32 spelling from parsed command
 * values, so `1.0` can become `1` without changing the program. Normalize
 * only decimal/exponent/suffixed numeric tokens in code; comments remain
 * byte-for-byte significant, as do integer spellings and all whitespace. */
static char *parity_canonicalize_numeric_literals(const char *src) {
    size_t len, i, o;
    size_t cap;
    int line_comment = 0;
    int block_comment = 0;
    char *out;

    if (!src)
        return NULL;
    len = strlen(src);
    cap = len * 4 + 64;
    out = (char *)malloc(cap);
    if (!out)
        return NULL;

    for (i = 0, o = 0; i < len; ) {
        char *end;
        char value_text[REPL_SOURCE_FLOAT_TEXT_MAX];
        float value;
        int consumed;
        int suffix_len = 0;
        int has_float_syntax = 0;
        size_t next;

        if (line_comment) {
            out[o++] = src[i];
            if (src[i++] == '\n')
                line_comment = 0;
            continue;
        }
        if (block_comment) {
            out[o++] = src[i];
            if (src[i] == '*' && i + 1 < len && src[i + 1] == '/') {
                out[o++] = src[++i];
                block_comment = 0;
            }
            i++;
            continue;
        }
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '/') {
            out[o++] = src[i++];
            out[o++] = src[i++];
            line_comment = 1;
            continue;
        }
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
            out[o++] = src[i++];
            out[o++] = src[i++];
            block_comment = 1;
            continue;
        }

        if (!isdigit((unsigned char)src[i]) &&
            !(src[i] == '.' && i + 1 < len &&
              isdigit((unsigned char)src[i + 1]))) {
            out[o++] = src[i++];
            continue;
        }
        if (i > 0 && (isalnum((unsigned char)src[i - 1]) ||
                      src[i - 1] == '_')) {
            out[o++] = src[i++];
            continue;
        }

        end = NULL;
        value = strtof(src + i, &end);
        consumed = (int)(end - (src + i));
        if (consumed <= 0) {
            out[o++] = src[i++];
            continue;
        }
        if (src[i + consumed] == 'f' || src[i + consumed] == 'F')
            suffix_len = 1;
        next = i + (size_t)consumed + (size_t)suffix_len;
        if (next < len && (isalnum((unsigned char)src[next]) ||
                           src[next] == '_')) {
            out[o++] = src[i++];
            continue;
        }
        for (size_t j = i; j < next; j++)
            if (src[j] == '.' || src[j] == 'e' || src[j] == 'E' ||
                src[j] == 'f' || src[j] == 'F')
                has_float_syntax = 1;
        if (!has_float_syntax) {
            out[o++] = src[i++];
            continue;
        }

        repl_format_source_float(value_text, sizeof(value_text), value);
        {
            size_t value_len = strlen(value_text);
            if (o + value_len >= cap) {
                free(out);
                return NULL;
            }
            memcpy(out + o, value_text, value_len);
            o += value_len;
        }
        i = next;
    }
    out[o] = '\0';
    return out;
}

static int parity_files_equal(const char *a_path, const char *b_path) {
    char *a_text = parity_read_file_text(a_path, NULL);
    char *b_text = parity_read_file_text(b_path, NULL);
    char *a_norm;
    char *b_norm;
    int equal;

    if (!a_text || !b_text) {
        free(a_text);
        free(b_text);
        return 0;
    }
    a_norm = parity_canonicalize_numeric_literals(a_text);
    b_norm = parity_canonicalize_numeric_literals(b_text);
    equal = a_norm && b_norm && strcmp(a_norm, b_norm) == 0;
    free(a_text);
    free(b_text);
    free(a_norm);
    free(b_norm);
    return equal;
}

static int parity_has_explicit_display(const char *path) {
    FILE *f = fopen(path, "rb");
    char *text;
    long size;
    const char *display;
    const char *close;
    int ok = 0;

    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f) fclose(f);
        return 0;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    text = (char *)malloc((size_t)size + 1);
    if (!text) {
        fclose(f);
        return 0;
    }
    if (fread(text, 1, (size_t)size, f) == (size_t)size) {
        text[size] = '\0';
        display = strstr(text, "display() {");
        close = strrchr(text, '}');
        ok = display && close && display < close;
    }
    free(text);
    fclose(f);
    return ok;
}

static int parity_diags_equal(const ReplCameraDiag *a, int an, int a_overflow,
                              const ReplCameraDiag *b, int bn, int b_overflow) {
    int i;

    /* The overflow counter is part of parity, not bookkeeping: comparing only
     * the stored eight would let two paths agree on the first eight
     * diagnostics and differ on everything after. */
    if (an != bn || a_overflow != b_overflow)
        return 0;
    for (i = 0; i < an; i++) {
        if (a[i].role != b[i].role || a[i].rule != b[i].rule ||
            a[i].line_no != b[i].line_no)
            return 0;
    }
    return 1;
}

static int parity_compare(const char *name, const ParityResult *file,
                          const ParityResult *catalog) {
    char label[512];
    int i, j;
    int rows_equal = 1;

    snprintf(label, sizeof(label), "%s: row count agrees", name);
    TEST_ASSERT_INT(&g_harness, label, catalog->row_count, file->row_count);
    if (catalog->row_count != file->row_count)
        return 0;

    for (i = 0; i < file->row_count && rows_equal; i++) {
        if (strcmp(file->rows[i], catalog->rows[i]) != 0) {
            printf("  %s: row %d differs\n    file:    %s\n    catalog: %s\n",
                   name, i, file->rows[i], catalog->rows[i]);
            rows_equal = 0;
        }
        if (file->types[i] != catalog->types[i]) {
            printf("  %s: row %d CmdType differs (%d vs %d)\n",
                   name, i, file->types[i], catalog->types[i]);
            rows_equal = 0;
        }
        if (file->num_args[i] != catalog->num_args[i]) {
            printf("  %s: row %d arg count differs (%d vs %d): %s\n",
                   name, i, file->num_args[i], catalog->num_args[i],
                   file->rows[i]);
            rows_equal = 0;
        }
        (void)j;
    }
    snprintf(label, sizeof(label), "%s: document text and commands agree", name);
    TEST_ASSERT_TRUE(&g_harness, label, rows_equal);

    snprintf(label, sizeof(label), "%s: camera applied on both paths", name);
    TEST_ASSERT_INT(&g_harness, label, catalog->pose_applied,
                    file->pose_applied);

    snprintf(label, sizeof(label), "%s: resolved pose agrees", name);
    TEST_ASSERT_TRUE(&g_harness, label,
                     fabsf(file->pose.dist - catalog->pose.dist) < 1e-3f &&
                     fabsf(file->pose.rx   - catalog->pose.rx)   < 1e-3f &&
                     fabsf(file->pose.ry   - catalog->pose.ry)   < 1e-3f &&
                     fabsf(file->pose.tx   - catalog->pose.tx)   < 1e-3f &&
                     fabsf(file->pose.ty   - catalog->pose.ty)   < 1e-3f &&
                     fabsf(file->pose.tz   - catalog->pose.tz)   < 1e-3f);

    snprintf(label, sizeof(label), "%s: predef count agrees", name);
    TEST_ASSERT_INT(&g_harness, label, catalog->num_predefs,
                    file->num_predefs);
    if (file->num_predefs == catalog->num_predefs) {
        int vars_equal = 1;
        for (i = 0; i < file->num_predefs; i++)
            if (strcmp(file->predef_names[i], catalog->predef_names[i]) != 0 ||
                fabsf(file->predef_vals[i] - catalog->predef_vals[i]) > 1e-4f)
                vars_equal = 0;
        snprintf(label, sizeof(label), "%s: predef names and values agree", name);
        TEST_ASSERT_TRUE(&g_harness, label, vars_equal);
    }
    return rows_equal;
}

/* The two loads' diagnostics, captured straight off each reader's
 * accumulator rather than through a sink - a fire-and-forget sink cannot be
 * asserted on. */
static ReplCameraDiag g_file_diags[REPL_CAMERA_MAX_DIAGS];
static int g_file_diag_count, g_file_diag_overflow;
static ReplCameraDiag g_catalog_diags[REPL_CAMERA_MAX_DIAGS];
static int g_catalog_diag_count, g_catalog_diag_overflow;

enum { PARITY_STATUS_MAX = 32 };
static char g_status_log[PARITY_STATUS_MAX][REPL_STATUS_TEXT_MAX];
static int g_status_count;

static void parity_status_capture(const char *msg) {
    if (!msg || !msg[0] || g_status_count >= PARITY_STATUS_MAX)
        return;
    snprintf(g_status_log[g_status_count], REPL_STATUS_TEXT_MAX, "%s", msg);
    g_status_count++;
}

static const ReplHostEffects g_parity_status_host = {
    .status       = parity_status_capture,
    .status_error = parity_status_capture,
};

static const char *parity_camera_status(void) {
    for (int i = 0; i < g_status_count; i++)
        if (strstr(g_status_log[i], "camera header:"))
            return g_status_log[i];
    return NULL;
}

/* Re-run the reader over the same lines to capture what each loader's own
 * reader would have accumulated. Both loaders offer every line from the top
 * in order, so this reproduces their accumulators exactly - and if it ever
 * stops doing so, that *is* the divergence the test is looking for. */
static void parity_collect_diags(char *const *lines, ReplCameraDiag *out,
                                 int *out_count, int *out_overflow) {
    ReplCameraHeader hdr;
    int i;

    repl_camera_header_init(&hdr);
    for (i = 0; lines && lines[i]; i++) {
        if (repl_doc_order_line_is_display_open(lines[i]))
            repl_camera_header_set_region(&hdr, REPL_CAMERA_REGION_DISPLAY);
        (void)repl_camera_header_offer(&hdr, lines[i], i + 1);
    }
    memcpy(out, hdr.diags, sizeof(hdr.diags));
    *out_count    = hdr.diag_count;
    *out_overflow = hdr.diag_overflow;
}

static void parity_check_file(const char *path, const char *name) {
    ParityResult *as_file = (ParityResult *)calloc(1, sizeof(ParityResult));
    ParityResult *as_catalog = (ParityResult *)calloc(1, sizeof(ParityResult));
    char **lines;
    int line_count = 0;
    char label[512];

    if (!as_file || !as_catalog) {
        free(as_file);
        free(as_catalog);
        return;
    }

    lines = parity_read_lines(path, &line_count);
    snprintf(label, sizeof(label), "%s: fixture readable", name);
    TEST_ASSERT_TRUE(&g_harness, label, lines != NULL);
    if (!lines) {
        free(as_file);
        free(as_catalog);
        return;
    }

    /* Catalog path: the same line array a compiled-in example carries. A full
     * reset between loads, because the command store is a global and the
     * corpus would otherwise hit its 1024 cap partway through. reset_all
     * installs the app's camera bridge, so the stub goes in after it. */
    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);
    (void)repl_load_example_lines((const char *const *)lines);
    parity_capture(as_catalog);
    parity_collect_diags(lines, g_catalog_diags, &g_catalog_diag_count,
                         &g_catalog_diag_overflow);

    /* File path: the same bytes, through src/repl/import.c. */
    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);
    (void)repl_export_load_from_file(path, NULL);
    parity_capture(as_file);
    parity_collect_diags(lines, g_file_diags, &g_file_diag_count,
                         &g_file_diag_overflow);

    (void)parity_compare(name, as_file, as_catalog);

    if (strncmp(path, "examples/scenes/", 16) == 0 &&
        strstr(path, ".glr") != NULL) {
        const char *roundtrip = "/tmp/test_camera_parity_roundtrip.glr";
        snprintf(label, sizeof(label), "%s: .glr re-save succeeds", name);
        TEST_ASSERT_TRUE(&g_harness, label,
                         repl_export_save_glr(roundtrip,
                                              source_document_view()) != 0);
        snprintf(label, sizeof(label),
                 "%s: .glr re-save is stable modulo numeric spelling", name);
        TEST_ASSERT_TRUE(&g_harness, label,
                         parity_files_equal(path, roundtrip));
        snprintf(label, sizeof(label), "%s: .glr re-save has display frame",
                 name);
        TEST_ASSERT_TRUE(&g_harness, label,
                         parity_has_explicit_display(roundtrip));
        remove(roundtrip);
    }

    snprintf(label, sizeof(label), "%s: diagnostics agree", name);
    TEST_ASSERT_TRUE(&g_harness, label,
                     parity_diags_equal(g_file_diags, g_file_diag_count,
                                        g_file_diag_overflow,
                                        g_catalog_diags, g_catalog_diag_count,
                                        g_catalog_diag_overflow));

    parity_free_lines(lines);
    free(as_file);
    free(as_catalog);
}

/* The corpus has complete pose headers, so its reader-level diagnostic lists
 * are empty. Exercise the real loader sinks with a partial header too: the
 * catalog and file paths must both surface the same missing-role payload even
 * though their source labels differ. */
static void test_partial_header_diagnostic_parity(void) {
    static const char *const lines[] = {
        "display() {",
        "glTranslatef(0.0f, 0.0f, -4.0f);   // @camera dist",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "}",
        NULL
    };
    const char *path = "/tmp/test_camera_partial_header.glr";
    const char *catalog_msg;
    const char *file_msg;
    char catalog_payload[REPL_STATUS_TEXT_MAX];
    char file_payload[REPL_STATUS_TEXT_MAX];
    FILE *f = fopen(path, "w");

    printf("--- actual loader diagnostics agree on a partial header ---\n");
    TEST_ASSERT_TRUE(&g_harness, "partial parity fixture written", f != NULL);
    if (!f)
        return;
    for (int i = 0; lines[i]; i++)
        fprintf(f, "%s\n", lines[i]);
    fclose(f);

    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);
    repl_install_host_effects(&g_parity_status_host);
    g_status_count = 0;
    TEST_ASSERT_TRUE(&g_harness, "catalog partial header loads",
                     repl_load_example_lines(lines) > 0);
    catalog_msg = parity_camera_status();
    TEST_ASSERT_TRUE(&g_harness, "catalog reports missing camera roles",
                     catalog_msg != NULL);
    snprintf(catalog_payload, sizeof(catalog_payload), "%s",
             catalog_msg ? strstr(catalog_msg, "camera header:") : "");

    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);
    repl_install_host_effects(&g_parity_status_host);
    g_status_count = 0;
    TEST_ASSERT_INT(&g_harness, "file partial header loads",
                    repl_export_load_from_file(path, NULL), 1);
    file_msg = parity_camera_status();
    TEST_ASSERT_TRUE(&g_harness, "file reports missing camera roles",
                     file_msg != NULL);
    snprintf(file_payload, sizeof(file_payload), "%s",
             file_msg ? strstr(file_msg, "camera header:") : "");
    TEST_ASSERT_STR(&g_harness, "loader diagnostic payloads agree",
                    file_payload, catalog_payload);
    remove(path);
}

static int parity_name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int parity_walk_dir(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *entry;
    char **names = NULL;
    int count = 0, cap = 0, i;

    if (!d) {
        printf("  (skipping %s - not found)\n", dir);
        return 0;
    }
    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len <= 4 || strcmp(entry->d_name + len - 4, ".glr") != 0)
            continue;
        if (count + 1 > cap) {
            cap = cap ? cap * 2 : 64;
            names = (char **)realloc(names, (size_t)cap * sizeof(*names));
            if (!names)
                break;
        }
        names[count++] = strdup(entry->d_name);
    }
    closedir(d);
    qsort(names, (size_t)count, sizeof(*names), parity_name_cmp);

    for (i = 0; i < count; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        parity_check_file(path, names[i]);
        free(names[i]);
    }
    free(names);
    return count;
}

/* The frozen pre-migration fixtures: the old shape must fail *loudly*.
 * Without this the only record of what it looked like is git history, and a
 * rejection message could silently degrade to "parse error" with no test
 * noticing. This is the rejection half of the two comparisons - the A/B half
 * (does the new form still render the old scene?) is the corpus walk above,
 * which loads the migrated files on both paths. */
static void test_frozen_rejections(void) {
    static const struct {
        const char *file;
        const char *why;
    } k_frozen[] = {
        { "matrix-stack-recursion-stress.glr",
          "func0 below the body" },
        { "function-local-shadowing-stress.glr",
          "markerless camera-shaped transforms, functions below the body" },
        { "torus-knot-animated.glr",
          "every violation at once" }
    };
    size_t i;

    printf("--- frozen pre-migration fixtures still fail loudly ---\n");
    for (i = 0; i < sizeof(k_frozen) / sizeof(k_frozen[0]); i++) {
        char path[512], label[512];
        char **lines;
        ReplDocOrder ord;
        ReplCameraHeader hdr;
        int j;

        snprintf(path, sizeof(path), "tests/testdata/camera-order/%s",
                 k_frozen[i].file);
        lines = parity_read_lines(path, NULL);
        snprintf(label, sizeof(label), "frozen %s readable", k_frozen[i].file);
        TEST_ASSERT_TRUE(&g_harness, label, lines != NULL);
        if (!lines)
            continue;

        repl_camera_header_init(&hdr);
        repl_doc_order_init(&ord);
        for (j = 0; lines[j]; j++) {
            ReplCameraLineResult r =
                repl_camera_header_offer(&hdr, lines[j], j + 1);
            (void)repl_doc_order_offer(&ord, lines[j], j + 1,
                                       r != REPL_CAMERA_LINE_NOT_CAMERA);
        }

        snprintf(label, sizeof(label), "frozen %s is rejected (%s)",
                 k_frozen[i].file, k_frozen[i].why);
        TEST_ASSERT_TRUE(&g_harness, label, ord.violations > 0);

        /* And rejected by *both real loaders*, not just by the checker they
         * are supposed to share. Running the fixture through doc_order alone
         * proves the rule exists; it says nothing about whether every entry
         * point applies it, which is exactly the gap that let the ordering
         * contract be enforced on one loader out of three. */
        glr_ctrl_reset_all();
        camera_bridge_stub_install(NULL);
        snprintf(label, sizeof(label), "frozen %s rejected by the catalog path",
                 k_frozen[i].file);
        TEST_ASSERT_INT(&g_harness, label,
                        repl_load_example_lines((const char *const *)lines), 0);

        glr_ctrl_reset_all();
        camera_bridge_stub_install(NULL);
        snprintf(label, sizeof(label), "frozen %s rejected by the file path",
                 k_frozen[i].file);
        TEST_ASSERT_INT(&g_harness, label,
                        repl_export_load_from_file(path, NULL), 0);

        /* The old form carried no tags at all, so its transforms are plain
         * geometry to the new reader - which is exactly why the ordering
         * check, not the camera reader, is what catches these files. */
        snprintf(label, sizeof(label), "frozen %s supplies no camera pose",
                 k_frozen[i].file);
        TEST_ASSERT_INT(&g_harness, label, (int)hdr.seen_mask, 0);

        parity_free_lines(lines);
    }
}

/* The shape no .glr can express: an exported .c with the spin row, in C89
 * block-comment spelling. A reader that handled only `//` would silently drop
 * the camera from every exported file - this bug, with new syntax. */
/* An exported `.c` is generated output whose layout the exporter fixes, and
 * that layout does not satisfy the phases - reshape() and main() follow
 * display(). The ordering contract is a property of the *authored* format, so
 * the same bytes that would be rejected as a `.glr` must still import as a
 * `.c`. This is the other half of the P1 fix: enforcing everywhere would be
 * just as wrong as enforcing in one place. */
static void test_exported_c_is_exempt_from_ordering(void) {
    const char *path = "/tmp/test_camera_parity_exported.c";
    FILE *f = fopen(path, "w");

    printf("--- exported .c is exempt from the ordering contract ---\n");
    TEST_ASSERT_TRUE(&g_harness, "exempt fixture written", f != NULL);
    if (!f)
        return;
    /* Body code, then a function definition: a phase violation in a .glr. */
    fprintf(f,
        "void display(void) {\n"
        "  glTranslatef(0.0000f, 0.0000f, -4.0000f);   /* @camera dist */\n"
        "  // Snippet start\n"
        "  glVertex3f(0, 0, 0);\n"
        "  // Snippet end\n"
        "}\n"
        "void reshape(int w, int h) {\n"
        "  glViewport(0, 0, w, h);\n"
        "}\n");
    fclose(f);

    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);
    TEST_ASSERT_INT(&g_harness, "a .c whose functions follow the body imports",
                    repl_export_load_from_file(path, NULL), 1);
    TEST_ASSERT_TRUE(&g_harness, "and its camera still applies",
                     fabsf(g_camera_bridge_stub.applied.dist - 4.0f) < 1e-4f);
    remove(path);
}

static void test_exported_c_fixture(void) {
    char **lines = parity_read_lines("tests/testdata/camera-order/"
                                     "exported-with-spin.c", NULL);
    ReplCameraHeader hdr;
    ReplCameraFinish fin;
    int i;

    printf("--- exported-C fixture with the spin row ---\n");
    TEST_ASSERT_TRUE(&g_harness, "exported-C fixture readable", lines != NULL);
    if (!lines)
        return;

    camera_bridge_stub_install(NULL);
    repl_camera_header_init(&hdr);
    for (i = 0; lines[i]; i++) {
        const char *p = lines[i];
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "void display(void)", 18) == 0)
            repl_camera_header_set_region(&hdr, REPL_CAMERA_REGION_DISPLAY);
        else if (strncmp(p, "/* Snippet start", 16) == 0)
            repl_camera_header_set_region(&hdr, REPL_CAMERA_REGION_SNIPPET);
        (void)repl_camera_header_offer(&hdr, lines[i], i + 1);
    }
    fin = repl_camera_header_finish(&hdr, REPL_CAMERA_APPLY_IMPORT);

    TEST_ASSERT_INT(&g_harness, "every pose role read from block comments",
                    (int)fin.seen_mask, (int)REPL_CAMERA_MASK_POSE);
    TEST_ASSERT_TRUE(&g_harness, "the pose is the file's, not the default",
                     fabsf(fin.pose.dist - 12.0f) < 1e-4f &&
                     fabsf(fin.pose.rx - 18.0f) < 1e-4f &&
                     fabsf(fin.pose.ry - 42.0f) < 1e-4f &&
                     fabsf(fin.pose.tx - 0.5f) < 1e-4f);
    TEST_ASSERT_INT(&g_harness, "the spin row is accepted and discarded",
                    hdr.spin_seen, 1);
    TEST_ASSERT_INT(&g_harness, "no diagnostics on a canonical exported file",
                    hdr.diag_count, 0);
    parity_free_lines(lines);
}

/* Walk `dir` and assert it contributed something. parity_walk_dir returns 0
 * for a directory that isn't there, and a bare `total > 0` at the end is
 * satisfied by any one corpus - so a mistyped or missing path would silently
 * contribute no coverage while the test still passed. Bind the count to the
 * directory that produced it instead. */
static int parity_walk_dir_checked(const char *dir, const char *label) {
    int n = parity_walk_dir(dir);
    TEST_ASSERT_TRUE(&g_harness, label, n > 0);
    return n;
}

int main(void) {
    int n = 0;

    printf("=== camera header loader parity ===\n");
    n += parity_walk_dir_checked("examples/scenes",
                                 "examples/scenes corpus was found");
    /* The tests/scenes corpora are opt-in; see support/scene_corpus.h. */
    if (repl_test_scene_corpus_enabled()) {
        n += parity_walk_dir_checked("tests/scenes/stress",
                                     "stress corpus was found");
        n += parity_walk_dir_checked("tests/scenes/general",
                                     "general corpus was found");
    }
    printf("--- compared %d scenes on both load paths ---\n", n);
    TEST_ASSERT_TRUE(&g_harness, "the corpus was actually found", n > 0);
    test_frozen_rejections();
    test_partial_header_diagnostic_parity();
    test_exported_c_is_exempt_from_ordering();
    test_exported_c_fixture();

    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "camera_header_parity");
}
