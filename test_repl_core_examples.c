// For linux mkdtemp
#define _DEFAULT_SOURCE

#include "repl_core_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_run = 0;
static int g_pass = 0;
static int g_verbose = 0;
static int g_use_color = 0;

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

static void assert_true_impl(const char *label, int cond) {
    g_run++;
    if (cond) {
        g_pass++;
        return;
    }
    printf("%sFAIL%s [%s]\n", ansi_red(), ansi_reset(), label);
}

#define ASSERT_TRUE(label, cond) assert_true_impl(label, (cond))

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

static void pin_code_panel_state(void) {
    g_cam_rx = 18.0f;
    g_cam_ry = 32.0f;
    g_cam_dist = 5.5f;
    g_cam_tx = 0.0f;
    g_cam_ty = 0.0f;
    g_cam_tz = 0.0f;
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
        cflags = "-std=c2x -DGL_SILENCE_DEPRECATION -I../../../include";

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
        else
            snprintf(detail, detail_sz, "command failed: %s", cmd);
    }
    log_example_result(idx, name, "compile result", 0, "see detail below");
    free(log);
    remove(obj_path);
    remove(log_path);
    return 0;
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
    char temp_dir[] = "/tmp/repl_examples_export.XXXXXX";
    const char *verbose_env = getenv("REPL_EXPORT_VERBOSE");

    g_verbose = verbose_env && verbose_env[0] && strcmp(verbose_env, "0") != 0;
    g_use_color = (isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL);

    if (argc == 3 && strcmp(argv[1], "--dump-index") == 0)
        return dump_single_example_to_stdout(atoi(argv[2]));

    if (!mkdtemp(temp_dir)) {
        perror("mkdtemp");
        return 1;
    }

    init_predef_vars();

    for (int idx = 0; idx < repl_example_count(); idx++) {
        char fixture_path[256];
        char label[160];
        const char *name;
        char *actual;
        char *expected;
        char *imported;
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

        load_example_for_test(idx);
        name = repl_example_name(idx);
        log_example_step(idx, name, "verify", "loaded example into REPL state");
        snprintf(label, sizeof(label), "example %02d loads cmds", idx);
        ASSERT_TRUE(label, g_num_cmds > 0);
        original_cmd_count = g_num_cmds;
        snprintf(label, sizeof(label), "example %02d has public name", idx);
        ASSERT_TRUE(label, name != NULL);
        snprintf(label, sizeof(label), "example %02d has no invalid cmds", idx);
        ASSERT_TRUE(label, examples_have_no_invalid_cmds());

        actual = dump_current_code_panel_text();
        snprintf(label, sizeof(label), "example %02d dump alloc", idx);
        ASSERT_TRUE(label, actual != NULL);
        if (!actual)
            continue;

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
        }
        snprintf(label, sizeof(label), "example %02d fixture matches", idx);
        ASSERT_TRUE(label, exact);

        snprintf(export_path, sizeof(export_path), "%s/example_%02d.c",
                 temp_dir, idx);
        log_example_step(idx, name, "export", export_path);
        repl_save_output(export_path);
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

        repl_reset_state();
        pin_code_panel_state();
        roundtrip_loaded = repl_load_from_file(export_path);
        snprintf(label, sizeof(label), "example %02d export imports", idx);
        ASSERT_TRUE(label, roundtrip_loaded == 1);

        roundtrip_exact = 0;
        imported = NULL;
        roundtrip_compiled = 0;
        if (roundtrip_loaded == 1) {
            snprintf(label, sizeof(label), "example %02d import cmd count", idx);
            ASSERT_TRUE(label, g_num_cmds == original_cmd_count);
            snprintf(label, sizeof(label), "example %02d import has no invalid cmds", idx);
            ASSERT_TRUE(label, examples_have_no_invalid_cmds());

            imported = dump_current_code_panel_text();
            snprintf(label, sizeof(label), "example %02d import dump alloc", idx);
            ASSERT_TRUE(label, imported != NULL);
            if (imported) {
                roundtrip_exact = compare_exact_text(actual, imported, &diff_line);
                if (!roundtrip_exact) {
                    printf("%sDETAIL [example %02d export/import mismatch] name=%s line=%d export=%s%s\n",
                           ansi_yellow(), idx, name, diff_line, export_path,
                           ansi_reset());
                }

                snprintf(reexport_path, sizeof(reexport_path),
                         "%s/example_%02d_roundtrip.c", temp_dir, idx);
                log_example_step(idx, name, "re-export", reexport_path);
                repl_save_output(reexport_path);
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
                snprintf(label, sizeof(label), "example %02d exact or re-export compiles", idx);
                ASSERT_TRUE(label, roundtrip_exact || roundtrip_compiled);
                remove(reexport_path);
                free(imported);
            }
        }

        remove(export_path);

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

    rmdir(temp_dir);

    printf("%srepl_core_examples: %d/%d passed%s\n",
           (g_run == g_pass) ? ansi_green() : ansi_red(),
           g_pass, g_run, ansi_reset());
    return (g_run == g_pass) ? 0 : 1;
}
