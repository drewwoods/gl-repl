/*
 * test_glr_cli.c - command-line parsing and dump-dispatch contract.
 *
 * Two halves of the same subject, both keyed off GlrCliOptions: glr_cli_parse
 * builds the bag from argv, and glr_debug_run_dumps consumes it to decide
 * whether the run is a GL-free --dump-* path that exits before any window.
 *
 * glr_cli_parse is the whole argv surface of the binary: it fills a
 * GlrCliOptions bag, handles the print-and-exit flags itself, and runs the
 * fail-fast resolvers (--example / --tour name->index) before any window
 * opens. The return value is the run/exit decision main() keys off, so every
 * case here asserts (return, exit_code, opts) together.
 *
 * Parses are run with stdout/stderr redirected to /dev/null: the help and
 * list paths print by design, and the unknown-name paths dump the catalog to
 * stderr. Silencing keeps the suite output readable without weakening the
 * assertions.
 *
 * Links CORE_TEST_OBJS — the resolvers read the compiled-in example and tour
 * catalogs (glr_scene_example_*, glr_tours_*). Nothing here opens a window or
 * calls GL.
 */
#include "app/glr_cli.h"
#include "app/glr_actions.h"   /* glr_scene_example_count / _name */
#include "app/glr_debug.h"     /* glr_debug_run_dumps */
#include "app/glr_tours.h"     /* glr_tours_count / _name */

#include "support/test_harness.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, g, e)  TEST_ASSERT_INT(&g_harness, label, g, e)
#define ASSERT_STR(label, g, e)  TEST_ASSERT_STR(&g_harness, label, g, e)

/* Run glr_cli_parse with stdout/stderr silenced, restoring both afterwards
 * so the harness can still report. argv must be NULL-terminated. */
static int parse_v(GlrCliOptions *out, int *exit_code, char **argv) {
    int argc = 0;
    while (argv[argc]) argc++;

    fflush(stdout);
    fflush(stderr);
    int saved_out = dup(fileno(stdout));
    int saved_err = dup(fileno(stderr));
    FILE *r1 = freopen("/dev/null", "w", stdout);
    FILE *r2 = freopen("/dev/null", "w", stderr);
    (void)r1;
    (void)r2;

    *exit_code = -999;   /* poisoned: proves the parse wrote it when it exits */
    int rc = glr_cli_parse(argc, argv, out, exit_code);

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, fileno(stdout));
    close(saved_out);
    dup2(saved_err, fileno(stderr));
    close(saved_err);
    clearerr(stdout);
    clearerr(stderr);
    return rc;
}

static void test_defaults(void) {
    GlrCliOptions o;
    int code = 0;
    char *av[] = { "gl-repl", NULL };

    ASSERT_INT("bare argv proceeds", parse_v(&o, &code, av), 1);
    ASSERT_TRUE("no input file", o.input_file == NULL);
    ASSERT_TRUE("no examples dir", o.examples_dir == NULL);
    ASSERT_TRUE("no assets override", o.assets_override == NULL);
    ASSERT_TRUE("no time arg", o.time_arg == NULL);
    ASSERT_TRUE("no export-ply path", o.export_ply_path == NULL);
    ASSERT_INT("export-ply srgb off", o.export_ply_srgb, 0);
    ASSERT_INT("dump-code off", o.dump_code, 0);
    ASSERT_INT("dump-flat off", o.dump_flat, 0);
    ASSERT_INT("flat-histogram off", o.dump_flat_histogram, 0);
    ASSERT_INT("dump-state-layout off", o.dump_state_layout, 0);
    ASSERT_INT("audio on by default", o.no_audio, 0);
    ASSERT_INT("accum on by default", o.use_accum, 1);
    ASSERT_INT("detailed-prof off", o.detailed_prof, 0);
    ASSERT_INT("default window width", o.window_w, 1200);
    ASSERT_INT("default window height", o.window_h, 800);
    ASSERT_INT("no example selected", o.example_index, -1);
    ASSERT_INT("no tour selected", o.tour_index, -1);
}

static void test_positional_input_file(void) {
    GlrCliOptions o;
    int code = 0;
    char *av[] = { "gl-repl", "scene.c", NULL };

    ASSERT_INT("positional proceeds", parse_v(&o, &code, av), 1);
    ASSERT_STR("input file captured", o.input_file, "scene.c");

    /* Only the first positional wins; later ones are ignored. */
    char *av2[] = { "gl-repl", "first.c", "second.c", NULL };
    ASSERT_INT("two positionals proceed", parse_v(&o, &code, av2), 1);
    ASSERT_STR("first positional wins", o.input_file, "first.c");
}

static void test_boolean_flags(void) {
    GlrCliOptions o;
    int code = 0;

    char *av1[] = { "gl-repl", "--no-accum", NULL };
    ASSERT_INT("--no-accum proceeds", parse_v(&o, &code, av1), 1);
    ASSERT_INT("--no-accum clears accum", o.use_accum, 0);

    char *av2[] = { "gl-repl", "--no-audio", NULL };
    ASSERT_INT("--no-audio proceeds", parse_v(&o, &code, av2), 1);
    ASSERT_INT("--no-audio sets flag", o.no_audio, 1);

    char *av3[] = { "gl-repl", "--detailed-prof", NULL };
    ASSERT_INT("--detailed-prof proceeds", parse_v(&o, &code, av3), 1);
    ASSERT_INT("--detailed-prof sets flag", o.detailed_prof, 1);

    char *av4[] = { "gl-repl", "--export-ply-srgb", NULL };
    ASSERT_INT("--export-ply-srgb proceeds", parse_v(&o, &code, av4), 1);
    ASSERT_INT("--export-ply-srgb sets flag", o.export_ply_srgb, 1);
}

static void test_dump_flags(void) {
    GlrCliOptions o;
    int code = 0;

    char *av1[] = { "gl-repl", "--dump-code", NULL };
    ASSERT_INT("--dump-code proceeds", parse_v(&o, &code, av1), 1);
    ASSERT_INT("--dump-code sets flag", o.dump_code, 1);

    char *av2[] = { "gl-repl", "--dump-flat", NULL };
    ASSERT_INT("--dump-flat proceeds", parse_v(&o, &code, av2), 1);
    ASSERT_INT("--dump-flat sets flag", o.dump_flat, 1);

    char *av3[] = { "gl-repl", "--flat-histogram", NULL };
    ASSERT_INT("--flat-histogram proceeds", parse_v(&o, &code, av3), 1);
    ASSERT_INT("--flat-histogram sets flag", o.dump_flat_histogram, 1);

    char *av4[] = { "gl-repl", "--dump-state-layout", NULL };
    ASSERT_INT("--dump-state-layout proceeds", parse_v(&o, &code, av4), 1);
    ASSERT_INT("--dump-state-layout sets flag", o.dump_state_layout, 1);
}

static void test_value_flags(void) {
    GlrCliOptions o;
    int code = 0;

    char *av1[] = { "gl-repl", "--assets", "/tmp/music", NULL };
    ASSERT_INT("--assets proceeds", parse_v(&o, &code, av1), 1);
    ASSERT_STR("--assets captured", o.assets_override, "/tmp/music");

    char *av2[] = { "gl-repl", "--time", "5.5", NULL };
    ASSERT_INT("--time proceeds", parse_v(&o, &code, av2), 1);
    ASSERT_STR("--time captured verbatim", o.time_arg, "5.5");

    char *av3[] = { "gl-repl", "--export-ply", "out.ply", NULL };
    ASSERT_INT("--export-ply proceeds", parse_v(&o, &code, av3), 1);
    ASSERT_STR("--export-ply captured", o.export_ply_path, "out.ply");
}

static void test_window_size(void) {
    GlrCliOptions o;
    int code = 0;

    char *av1[] = { "gl-repl", "--window", "640x480", NULL };
    ASSERT_INT("valid --window proceeds", parse_v(&o, &code, av1), 1);
    ASSERT_INT("window width parsed", o.window_w, 640);
    ASSERT_INT("window height parsed", o.window_h, 480);

    /* Malformed geometry is a hard error, not a silent fallback to default. */
    char *av2[] = { "gl-repl", "--window", "notageometry", NULL };
    ASSERT_INT("garbage --window exits", parse_v(&o, &code, av2), 0);
    ASSERT_INT("garbage --window exit code", code, 1);

    /* Zero/negative extents are rejected the same way. */
    char *av3[] = { "gl-repl", "--window", "0x480", NULL };
    ASSERT_INT("zero-width --window exits", parse_v(&o, &code, av3), 0);
    ASSERT_INT("zero-width --window exit code", code, 1);

    char *av4[] = { "gl-repl", "--window", "-4x-4", NULL };
    ASSERT_INT("negative --window exits", parse_v(&o, &code, av4), 0);
    ASSERT_INT("negative --window exit code", code, 1);
}

static void test_help_and_list_exit_paths(void) {
    GlrCliOptions o;
    int code = 0;

    char *av1[] = { "gl-repl", "-h", NULL };
    ASSERT_INT("-h exits", parse_v(&o, &code, av1), 0);
    ASSERT_INT("-h exit code is success", code, 0);

    char *av2[] = { "gl-repl", "--help", NULL };
    ASSERT_INT("--help exits", parse_v(&o, &code, av2), 0);
    ASSERT_INT("--help exit code is success", code, 0);

    char *av3[] = { "gl-repl", "--list-examples", NULL };
    ASSERT_INT("--list-examples exits", parse_v(&o, &code, av3), 0);
    ASSERT_INT("--list-examples exit code is success", code, 0);

    char *av4[] = { "gl-repl", "--list-tours", NULL };
    ASSERT_INT("--list-tours exits", parse_v(&o, &code, av4), 0);
    ASSERT_INT("--list-tours exit code is success", code, 0);

    /* --help wins over anything that would otherwise fail the parse. */
    char *av5[] = { "gl-repl", "--window", "bogus", "--help", NULL };
    ASSERT_INT("--help after bad flag still exits", parse_v(&o, &code, av5), 0);
}

static void test_example_resolution(void) {
    GlrCliOptions o;
    int code = 0;
    int n = glr_scene_example_count();

    ASSERT_TRUE("example catalog is non-empty", n > 0);
    if (n <= 0) return;

    /* All-digit argument is a 1-based index. */
    char *av1[] = { "gl-repl", "--example", "1", NULL };
    ASSERT_INT("--example 1 proceeds", parse_v(&o, &code, av1), 1);
    ASSERT_INT("--example 1 resolves to index 0", o.example_index, 0);

    /* Out-of-range index fails fast rather than clamping. */
    char *av2[] = { "gl-repl", "--example", "9999", NULL };
    ASSERT_INT("out-of-range --example exits", parse_v(&o, &code, av2), 0);
    ASSERT_INT("out-of-range --example exit code", code, 1);

    char *av3[] = { "gl-repl", "--example", "0", NULL };
    ASSERT_INT("--example 0 exits (1-based)", parse_v(&o, &code, av3), 0);
    ASSERT_INT("--example 0 exit code", code, 1);

    /* Exact name match, and the same name case-folded, resolve identically. */
    char name[160];
    const char *first = glr_scene_example_name(0);
    ASSERT_TRUE("first example has a name", first != NULL && first[0] != '\0');
    if (!first || !first[0]) return;
    snprintf(name, sizeof(name), "%s", first);

    char *av4[] = { "gl-repl", "--example", name, NULL };
    ASSERT_INT("--example by name proceeds", parse_v(&o, &code, av4), 1);
    ASSERT_INT("--example by name resolves", o.example_index, 0);

    char upper[160];
    snprintf(upper, sizeof(upper), "%s", first);
    for (char *p = upper; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    char *av5[] = { "gl-repl", "--example", upper, NULL };
    ASSERT_INT("--example name is case-insensitive", parse_v(&o, &code, av5), 1);
    ASSERT_INT("case-folded name resolves the same", o.example_index, 0);

    /* Unknown name fails fast (and prints the catalog to stderr). */
    char *av6[] = { "gl-repl", "--example", "zzz-no-such-example", NULL };
    ASSERT_INT("unknown --example exits", parse_v(&o, &code, av6), 0);
    ASSERT_INT("unknown --example exit code", code, 1);
}

static void test_tour_resolution(void) {
    GlrCliOptions o;
    int code = 0;
    int n = glr_tours_count();

    ASSERT_TRUE("tour catalog is non-empty", n > 0);
    if (n <= 0) return;

    char *av1[] = { "gl-repl", "--tour", "1", NULL };
    ASSERT_INT("--tour 1 proceeds", parse_v(&o, &code, av1), 1);
    ASSERT_INT("--tour 1 resolves to index 0", o.tour_index, 0);

    char *av2[] = { "gl-repl", "--tour", "9999", NULL };
    ASSERT_INT("out-of-range --tour exits", parse_v(&o, &code, av2), 0);
    ASSERT_INT("out-of-range --tour exit code", code, 1);

    char name[160];
    const char *first = glr_tours_name(0);
    ASSERT_TRUE("first tour has a name", first != NULL && first[0] != '\0');
    if (!first || !first[0]) return;
    snprintf(name, sizeof(name), "%s", first);

    char *av3[] = { "gl-repl", "--tour", name, NULL };
    ASSERT_INT("--tour by name proceeds", parse_v(&o, &code, av3), 1);
    ASSERT_INT("--tour by name resolves", o.tour_index, 0);

    char *av4[] = { "gl-repl", "--tour", "zzz-no-such-tour", NULL };
    ASSERT_INT("unknown --tour exits", parse_v(&o, &code, av4), 0);
    ASSERT_INT("unknown --tour exit code", code, 1);
}

static void test_combined_arguments(void) {
    GlrCliOptions o;
    int code = 0;
    char *av[] = { "gl-repl", "--no-audio", "--example", "2", "scene.c",
                   "--window", "800x600", "--time", "3", NULL };

    ASSERT_INT("combined argv proceeds", parse_v(&o, &code, av), 1);
    ASSERT_INT("combined: no-audio", o.no_audio, 1);
    ASSERT_INT("combined: example index", o.example_index, 1);
    ASSERT_STR("combined: input file", o.input_file, "scene.c");
    ASSERT_INT("combined: window width", o.window_w, 800);
    ASSERT_INT("combined: window height", o.window_h, 600);
    ASSERT_STR("combined: time", o.time_arg, "3");
    /* Untouched fields keep their defaults. */
    ASSERT_INT("combined: accum still default", o.use_accum, 1);
    ASSERT_INT("combined: no tour", o.tour_index, -1);
}

/* Runs last: a failed --examples-dir load may leave the example catalog in a
 * replaced state, which would perturb the resolver tests above. */
static void test_bad_examples_dir(void) {
    GlrCliOptions o;
    int code = 0;
    char *av[] = { "gl-repl", "--examples-dir",
                   "/nonexistent/gl-repl-test-examples", NULL };

    ASSERT_INT("bad --examples-dir exits", parse_v(&o, &code, av), 0);
    ASSERT_INT("bad --examples-dir exit code", code, 1);
}

/* Run the dump dispatcher into a temp stream and capture what it wrote.
 * Returns the dispatcher's verdict (1 = dumps ran, caller exits). */
static int run_dumps_capture(const GlrCliOptions *opts, char *buf, size_t buf_sz) {
    buf[0] = '\0';
    FILE *f = tmpfile();
    if (!f) return -1;

    int rc = glr_debug_run_dumps(opts, f);
    fflush(f);
    rewind(f);
    size_t n = fread(buf, 1, buf_sz - 1, f);
    buf[n] = '\0';
    fclose(f);
    return rc;
}

/* Runs last: the --dump-code leg bootstraps the REPL, which mutates global
 * document state the parse cases above do not want perturbed. */
static void test_dump_dispatch(void) {
    GlrCliOptions o;
    static char buf[16384];

    memset(&o, 0, sizeof(o));
    o.example_index = -1;
    o.tour_index = -1;

    /* No dump flag: the caller must be told to proceed to the windowed run,
     * and nothing may be written. */
    ASSERT_INT("no dump flag proceeds", run_dumps_capture(&o, buf, sizeof(buf)), 0);
    ASSERT_INT("no dump flag writes nothing", (int)strlen(buf), 0);

    /* --dump-state-layout is the one leg that needs no REPL bootstrap. */
    o.dump_state_layout = 1;
    ASSERT_INT("--dump-state-layout dispatches",
               run_dumps_capture(&o, buf, sizeof(buf)), 1);
    ASSERT_TRUE("state layout written",
                strstr(buf, "ReplRuntimeState") != NULL);
    o.dump_state_layout = 0;

    /* --dump-code takes the bootstrap leg (GL-free) and dumps the editor. */
    o.dump_code = 1;
    ASSERT_INT("--dump-code dispatches",
               run_dumps_capture(&o, buf, sizeof(buf)), 1);
    ASSERT_TRUE("editor dump written",
                strstr(buf, "REPL Editor Dump") != NULL);
}

int main(void) {
    test_defaults();
    test_positional_input_file();
    test_boolean_flags();
    test_dump_flags();
    test_value_flags();
    test_window_size();
    test_help_and_list_exit_paths();
    test_example_resolution();
    test_tour_resolution();
    test_combined_arguments();
    test_bad_examples_dir();
    test_dump_dispatch();

    return test_harness_report(&g_harness, "glr_cli");
}
