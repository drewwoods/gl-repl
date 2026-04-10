#include "repl_core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s]\n", label); \
} while (0)

static void pin_code_panel_state(void) {
    g_cam_rx = 18.0f;
    g_cam_ry = 32.0f;
    g_cam_dist = 5.5f;
    g_cam_px = 0.0f;
    g_cam_py = 0.0f;
    g_multisample_enabled = 1;
    g_line_smooth_enabled = 0;
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
    repl_dump_code_panel_text(tmp);
    buf = slurp_stream(tmp);
    fclose(tmp);
    return buf;
}

static void fixture_path_for_idx(int idx, char *out, int out_sz) {
    snprintf(out, (size_t)out_sz, "testdata/repl_examples_ui/%02d.golden.txt", idx);
}

static int examples_have_no_invalid_cmds(void) {
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid)
            return 0;
    }
    return 1;
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

static void load_example_for_test(int idx) {
    repl_reset_state();
    pin_code_panel_state();
    repl_load_example(idx);
}

static int dump_single_example_to_stdout(int idx) {
    char *dump;

    if (idx < 0 || idx >= repl_example_count()) {
        fprintf(stderr, "invalid example index: %d\n", idx);
        return 1;
    }

    init_predef_vars();
    load_example_for_test(idx);
    dump = dump_current_code_panel_text();
    if (!dump)
        return 1;
    fputs(dump, stdout);
    free(dump);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--dump-index") == 0)
        return dump_single_example_to_stdout(atoi(argv[2]));

    init_predef_vars();

    for (int idx = 0; idx < repl_example_count(); idx++) {
        char fixture_path[256];
        char label[160];
        char *actual;
        char *expected;
        int diff_line = 0;
        int exact;

        load_example_for_test(idx);
        snprintf(label, sizeof(label), "example %02d loads cmds", idx);
        ASSERT_TRUE(label, g_num_cmds > 0);
        snprintf(label, sizeof(label), "example %02d has public name", idx);
        ASSERT_TRUE(label, repl_example_name(idx) != NULL);
        snprintf(label, sizeof(label), "example %02d has no invalid cmds", idx);
        ASSERT_TRUE(label, examples_have_no_invalid_cmds());

        actual = dump_current_code_panel_text();
        snprintf(label, sizeof(label), "example %02d dump alloc", idx);
        ASSERT_TRUE(label, actual != NULL);
        if (!actual)
            continue;

        fixture_path_for_idx(idx, fixture_path, sizeof(fixture_path));
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
        }
        snprintf(label, sizeof(label), "example %02d fixture matches", idx);
        ASSERT_TRUE(label, exact);

        free(expected);
        free(actual);
    }

    /*
     * The golden fixtures intentionally stay logical, not visual: they still
     * assert one logical row per header/source line. Wrapped-row rendering is
     * covered separately by the visual code-panel dump tests.
     */
    load_example_for_test(repl_example_count() - 1);
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

    printf("repl_core_examples: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
