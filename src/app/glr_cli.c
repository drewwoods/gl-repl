/*
 * glr_cli.c - argv parsing + print-and-exit paths for the gl-repl binary.
 *
 * See glr_cli.h for the contract. All lookups here are pure reads over the
 * example/tour catalogs; nothing touches GL or controller state.
 */
#include "app/glr_cli.h"

#include "app/glr_actions.h"   /* glr_scene_example_count / _name */
#include "app/glr_tours.h"     /* glr_tours_count / _name */
#include "repl/examples.h"     /* repl_examples_load_dir */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog) {
    const char *name = (prog && prog[0]) ? prog : "gl-repl";

    fprintf(stdout,
            "Usage: %s [options] [input.c | workspace]\n"
            "\n"
            "Options:\n"
            "  -h, --help   Show this help text and exit\n"
            "  --no-accum   Disable accumulation buffer antialiasing\n"
            "  --no-audio   Start without audio (disables music entirely)\n"
            "  --assets <dir>  Music directory to scan for *.mp3 instead of\n"
            "               ./assets (also via GLR_ASSETS_DIR env var)\n"
            "  --dump-code  Load the session and print the editor buffer\n"
            "  --dump-flat  Load the session and print flattened commands\n"
            "  --flat-histogram  Load the session and print per-function /\n"
            "               per-line flat-command costs (budget breakdown)\n"
            "  --dump-state-layout  Print ReplRuntimeState field layout\n"
            "  --detailed-prof  Emit finer-grained startup init traces\n"
            "               (also via GLR_DETAILED_PROF env var)\n"
            "  --export-ply <file>  Render one frame, capture geometry to <file>\n"
            "               as a PLY mesh, then exit (needs a display)\n"
            "  --export-ply-srgb  Decode vertex colors sRGB -> linear on export\n"
            "               (for color-managed viewers; pair with --export-ply)\n"
            "  --example <name|idx>  Start on a built-in example (name is\n"
            "               case-insensitive; or a 1-based index)\n"
            "  --examples-dir <dir>  Load example catalog.ini + scenes/ from\n"
            "               <dir> at runtime instead of compiled-in examples\n"
            "  --list-examples  Print the built-in examples and exit\n"
            "  --tour <name|idx>  Start and play a built-in guided tour on\n"
            "               launch (name is case-insensitive; or a 1-based\n"
            "               index). Space play/pause, arrows step, Esc exit.\n"
            "  --list-tours  Print the built-in guided tours and exit\n"
            "  --time <secs>  Set the initial animation time t at startup\n"
            "               (else GLR_TIME; --time wins). Start animations later.\n"
            "  --window <WxH>  Initial window size (default 1200x800). Headless\n"
            "               captures render at 2x and downscale for 4x supersampling.\n"
            "\n"
            "Environment:\n"
            "  GLR_EDIT_LINE=<n>  Park the cursor on source line n (0-based)\n"
            "               after load, as if arrowed to. Poses cursor-bound\n"
            "               overlays (transform guides, vertex labels) for\n"
            "               headless captures.\n"
            "  GLR_ACCUM_PASSES=<n>  Accumulation AA sample count (1/2/4/8/12/16).\n"
            "               Captures use it to smooth 3D edges at full UI text\n"
            "               size (the 2D UI renders outside the accum loop).\n"
            "  GLR_TICK_PER_FRAME=1  Advance the fixed-dt simulation exactly\n"
            "               once per rendered frame instead of per timer tick.\n"
            "               Intended for deterministic offline recording.\n"
            "  GLR_VIEW_TOGGLE_AT=<t1,t2,...>  Toggle the 2D/3D view mode as the\n"
            "               rendered-frame clock crosses each listed second (t\n"
            "               advances 1/60 s per frame). Records the menu-bar\n"
            "               2D/3D swatch transition headlessly and implicitly\n"
            "               enables GLR_TICK_PER_FRAME.\n"
            "  GLR_TYPE_KEYS=<text>  Feed each character through the keyboard\n"
            "               dispatch after load, exactly as typing would.\n"
            "               Poses mid-typing states (partial-input guides,\n"
            "               autocomplete ghost) for headless captures.\n"
            "  GLR_OPEN_COLOR_PICKER=<n>  Open the floating color picker on\n"
            "               source line n (0-based; the line must be an\n"
            "               editable color command). Poses the picker for\n"
            "               captures - it otherwise needs a swatch click.\n"
            "  GLR_OPEN_GL_STATE=<n>  Open the floating OpenGL-state popup\n"
            "               anchored to source line n (0-based; the line\n"
            "               must be a visually blank editor row). Poses the\n"
            "               popup for captures - it otherwise needs a\n"
            "               right-click.\n"
            "  GLR_OPEN_HELP=<tab>  Open the F1 help overlay on tab index\n"
            "               tab (0=Overview 1=Commands 2=Keys 3=About) on\n"
            "               the first frame. Poses the overlay for captures\n"
            "               - it otherwise needs an F1 special-key press.\n"
            "  GLR_POINTER_SCRIPT=<file>  Drive scripted synthetic mouse +\n"
            "               keyboard input on the rendered-frame clock\n"
            "               (implies GLR_TICK_PER_FRAME) with a visible\n"
            "               cursor overlay. Video capture hook - records\n"
            "               menu navigation; see scripts/record-video.sh.\n"
            "  GLR_NO_SPLASH=1  Skip the startup splash banner (captures\n"
            "               that should not open on the splash band).\n"
            "\n"
            "Arguments:\n"
            "  input.c      Optional saved session to load at startup\n"
            "  workspace/   Optional directory: load every *.c as a user scene\n",
            name);
}

/* Case-insensitive full compare / substring test (keeps this TU free of a
 * <strings.h> / strcasecmp dependency). Shared by --example and --tour name
 * resolution. */
static int arg_ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == '\0' && *b == '\0';
}
static int arg_ci_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 0;
    for (; *hay; hay++) {
        size_t k = 0;
        while (hay[k] && needle[k] &&
               tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k]))
            k++;
        if (k == nl) return 1;
    }
    return 0;
}

static void list_examples(FILE *out) {
    int n = glr_scene_example_count();
    fprintf(out, "Built-in examples (%d):\n", n);
    for (int i = 0; i < n; i++)
        fprintf(out, "  %2d  %s\n", i + 1, glr_scene_example_name(i));
}

/* Resolve --example <arg> to a built-in example index. `arg` is either an
 * index (all digits) or a name (case-insensitive: exact match preferred, else
 * the first substring match). Returns the index, or -1 if nothing matches. */
static int resolve_example_index(const char *arg) {
    int n = glr_scene_example_count();
    if (n <= 0 || !arg || !arg[0]) return -1;

    int all_digits = 1;
    for (const char *p = arg; *p; p++)
        if (!isdigit((unsigned char)*p)) { all_digits = 0; break; }
    if (all_digits) {
        int idx = atoi(arg) - 1;
        return (idx >= 0 && idx < n) ? idx : -1;
    }

    int substr = -1;
    for (int i = 0; i < n; i++) {
        const char *name = glr_scene_example_name(i);
        if (!name) continue;
        if (arg_ci_equal(name, arg)) return i;              /* exact wins */
        if (substr < 0 && arg_ci_contains(name, arg)) substr = i;
    }
    return substr;
}

static void list_tours(FILE *out) {
    int n = glr_tours_count();
    fprintf(out, "Built-in tours (%d):\n", n);
    for (int i = 0; i < n; i++)
        fprintf(out, "  %2d  %s\n", i + 1, glr_tours_name(i));
}

/* Resolve --tour <arg> to a built-in tour index, same rules as
 * resolve_example_index: all-digits is a 1-based index, otherwise a
 * case-insensitive name (exact match preferred, else first substring match).
 * Returns the index, or -1 if nothing matches. */
static int resolve_tour_index(const char *arg) {
    int n = glr_tours_count();
    if (n <= 0 || !arg || !arg[0]) return -1;

    int all_digits = 1;
    for (const char *p = arg; *p; p++)
        if (!isdigit((unsigned char)*p)) { all_digits = 0; break; }
    if (all_digits) {
        int idx = atoi(arg) - 1;
        return (idx >= 0 && idx < n) ? idx : -1;
    }

    int substr = -1;
    for (int i = 0; i < n; i++) {
        const char *name = glr_tours_name(i);
        if (!name) continue;
        if (arg_ci_equal(name, arg)) return i;              /* exact wins */
        if (substr < 0 && arg_ci_contains(name, arg)) substr = i;
    }
    return substr;
}

int glr_cli_parse(int argc, char **argv, GlrCliOptions *out, int *exit_code) {
    const char *prog = (argc > 0) ? argv[0] : NULL;
    const char *example_arg = NULL;       /* --example NAME|IDX (unresolved) */
    const char *tour_arg = NULL;          /* --tour NAME|IDX (unresolved)    */
    int list_examples_flag = 0;
    int list_tours_flag = 0;

    memset(out, 0, sizeof(*out));
    out->use_accum = 1;
    out->window_w = 1200;
    out->window_h = 800;
    out->example_index = -1;
    out->tour_index = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(prog);
            *exit_code = 0;
            return 0;
        } else if (strcmp(argv[i], "--no-accum") == 0)
            out->use_accum = 0;
        else if (strcmp(argv[i], "--no-audio") == 0)
            out->no_audio = 1;
        else if (strcmp(argv[i], "--dump-code") == 0)
            out->dump_code = 1;
        else if (strcmp(argv[i], "--dump-flat") == 0)
            out->dump_flat = 1;
        else if (strcmp(argv[i], "--flat-histogram") == 0)
            out->dump_flat_histogram = 1;
        else if (strcmp(argv[i], "--dump-state-layout") == 0)
            out->dump_state_layout = 1;
        else if (strcmp(argv[i], "--detailed-prof") == 0)
            out->detailed_prof = 1;
        else if (strcmp(argv[i], "--export-ply") == 0 && i + 1 < argc)
            out->export_ply_path = argv[++i];
        else if (strcmp(argv[i], "--export-ply-srgb") == 0)
            out->export_ply_srgb = 1;
        else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc)
            out->assets_override = argv[++i];
        else if (strcmp(argv[i], "--example") == 0 && i + 1 < argc)
            example_arg = argv[++i];
        else if (strcmp(argv[i], "--examples-dir") == 0 && i + 1 < argc)
            out->examples_dir = argv[++i];
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc)
            out->time_arg = argv[++i];
        else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            /* --window WxH: initial window size. Headless captures use
             * it to render at 2x and downscale (4x supersampling) since
             * the software rasterizer has no MSAA. */
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                out->window_w = w;
                out->window_h = h;
            } else {
                fprintf(stderr, "gl-repl: bad --window \"%s\" (want WxH)\n",
                        argv[i]);
                *exit_code = 1;
                return 0;
            }
        }
        else if (strcmp(argv[i], "--list-examples") == 0) {
            list_examples_flag = 1;
        }
        else if (strcmp(argv[i], "--tour") == 0 && i + 1 < argc)
            tour_arg = argv[++i];
        else if (strcmp(argv[i], "--list-tours") == 0) {
            list_tours_flag = 1;
        }
        else if (!out->input_file)
            out->input_file = argv[i];
    }

    if (out->examples_dir) {
        char err[512];
        if (!repl_examples_load_dir(out->examples_dir, err, sizeof(err))) {
            fprintf(stderr, "gl-repl: could not load examples from %s: %s\n",
                    out->examples_dir, err[0] ? err : "unknown error");
            *exit_code = 1;
            return 0;
        }
    }

    if (list_examples_flag) {
        list_examples(stdout);
        *exit_code = 0;
        return 0;
    }
    if (list_tours_flag) {
        list_tours(stdout);
        *exit_code = 0;
        return 0;
    }

    /* Resolve --example up front: a bad name fails fast (before opening a
     * window) and the error lists what is available. */
    if (example_arg) {
        out->example_index = resolve_example_index(example_arg);
        if (out->example_index < 0) {
            fprintf(stderr, "gl-repl: unknown example \"%s\"\n", example_arg);
            list_examples(stderr);
            *exit_code = 1;
            return 0;
        }
    }

    /* Resolve --tour up front too (same fail-fast contract). The tour itself
     * needs a live window/layout, so it is started after bootstrap. */
    if (tour_arg) {
        out->tour_index = resolve_tour_index(tour_arg);
        if (out->tour_index < 0) {
            fprintf(stderr, "gl-repl: unknown tour \"%s\"\n", tour_arg);
            list_tours(stderr);
            *exit_code = 1;
            return 0;
        }
    }

    return 1;
}
