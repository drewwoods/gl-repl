/*
 * glr_cli.h - Command-line surface for the gl-repl binary.
 *
 * Parses argv into a plain GlrCliOptions bag and handles the self-contained
 * exit paths (-h/--help, --list-examples, --list-tutorials, --list-tours)
 * plus the fail-fast resolvers (--examples-dir load, --example / --tutorial /
 * --tour name->index) that must validate before a window opens. Everything
 * here is policy-free plumbing:
 * no GL, no window, no controller state.
 *
 * main() owns the run sequence; glr_cli only tells it whether to proceed and,
 * if not, with what exit code.
 */
#ifndef GLR_CLI_H
#define GLR_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

/* use_accum tri-state. AUTO is the default: main() still asks GLUT for an
 * accumulation visual, but the controller decides at GL-init time whether to
 * arm the feature, from the renderer string (Mesa emulates glAccum on the
 * CPU, so a non-zero GL_ACCUM_RED_BITS there buys nothing but frame time).
 * ON / OFF are the explicit --accum / --no-accum overrides. */
#define GLR_CLI_ACCUM_OFF   0
#define GLR_CLI_ACCUM_ON    1
#define GLR_CLI_ACCUM_AUTO  (-1)

/* Parsed command-line state. Pointer fields alias into argv (valid for the
 * lifetime of main); the resolved *_index fields are -1 when the matching
 * argument was absent. */
typedef struct GlrCliOptions {
    const char *input_file;        /* positional file/workspace, or "-" stdin */
    const char *examples_dir;      /* --examples-dir DIR (loaded during parse)  */
    const char *assets_override;   /* --assets DIR (else GLR_ASSETS_DIR)        */
    const char *time_arg;          /* --time SECS (else GLR_TIME)               */
    const char *export_ply_path;   /* --export-ply FILE, else NULL              */
    int         export_ply_srgb;   /* --export-ply-srgb                         */
    const char *export_c_path;     /* --export-c FILE, else NULL                */
    const char *export_glr_path;   /* --export-glr FILE, else NULL              */

    int dump_code;                 /* --dump-code                               */
    int dump_flat;                 /* --dump-flat                               */
    int dump_flat_histogram;       /* --flat-histogram                          */
    int dump_state_layout;         /* --dump-state-layout                       */

    int no_audio;                  /* --no-audio                                */
    int use_accum;                 /* --accum / --no-accum / AUTO (default)     */
    int detailed_prof;             /* --detailed-prof (also GLR_DETAILED_PROF)  */

    int window_w;                  /* --window WxH (default 1200)               */
    int window_h;                  /* --window WxH (default 800)                */

    int example_index;             /* resolved --example, else -1               */
    int tutorial_index;            /* resolved --tutorial, else -1              */
    int tour_index;                /* resolved --tour, else -1                  */
} GlrCliOptions;

/* Parse argv into *out (zeroed then populated with defaults). Handles the
 * print-and-exit flags itself and resolves --examples-dir/--example/
 * --tutorial/--tour up front so a bad argument fails before any window opens.
 *
 * Returns 1 if the caller should proceed with *out; returns 0 if the caller
 * should exit immediately with *exit_code (help/list printed, or an error
 * already reported to stderr). */
int glr_cli_parse(int argc, char **argv, GlrCliOptions *out, int *exit_code);

#ifdef __cplusplus
}
#endif

#endif /* GLR_CLI_H */
