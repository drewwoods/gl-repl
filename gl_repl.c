#include "app/glr_ctrl.h"
#include "app/glr_actions.h"
#include "app/glr_debug.h"
#include "app/glr_audio.h"
#include "app/glr_mesh_export.h"
#include "app/glr_paths.h"

#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdint.h>
#endif

/* Startup stall diagnostic. Each phase logs wall-clock seconds elapsed
 * since main() started so a slow phase (e.g. ma_engine_init opening the
 * audio device) is visible in the terminal. gettimeofday keeps this
 * portable/C99 without the per-platform timebase branching in prof.c.
 *
 * Two granularity levels:
 *  - init_trace()          always emits — the baseline boot phases.
 *  - init_trace_detail()   emits only when --detailed-prof or the
 *                          GLR_DETAILED_PROF env var is set; covers
 *                          the finer-grained phases (glutInit split,
 *                          audio playlist sub-steps, first two frames). */
static double g_init_t0 = -1.0;
static int    g_detailed_prof = 0;

static double init_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static void init_trace(const char *phase) {
    if (g_init_t0 < 0.0) g_init_t0 = init_now();
    fprintf(stderr, "[init +%6.3fs] %s\n", init_now() - g_init_t0, phase);
}

static void init_trace_detail(const char *phase) {
    if (g_detailed_prof) init_trace(phase);
}

/* Music assets live here. Relative to gl-repl's working directory
 * on native, and to the Emscripten virtual FS mount point set up by
 * --preload-file in emscripten/build.sh. Any *.mp3 files dropped into
 * this folder are picked up in filename order as a playlist.
 *
 * The playlist is built from three sources, in order (see
 * build_mp3_playlist): the primary assets dir — this working-directory
 * "assets" by default, or the --assets <dir> / GLR_ASSETS_DIR override
 * — then the copy bundled beside the executable in a macOS .app
 * (<exe>/../Resources/assets — where `make app` puts sample.mp3), and
 * a per-user music folder (glr_paths_user_music_dir) the user can drop more
 * tracks into. */
#ifndef AUDIO_ASSETS_DIR
#define AUDIO_ASSETS_DIR "assets"
#endif

/* Single-file fallback used when the assets directory is empty or
 * missing, so existing single-track setups keep working. */
#ifndef AUDIO_DEFAULT_MUSIC
#define AUDIO_DEFAULT_MUSIC "assets/song.mp3"
#endif

/* Persists current track + playback offset so the session resumes where it
 * left off.  Managed entirely by repl_audio; not shown in any config UI.
 * Lives alongside output.c in the working directory. */
#ifndef AUDIO_STATE_FILE
#define AUDIO_STATE_FILE "audio_state.ini"
#endif

#define AUDIO_MUSIC_MAX_PATHS 64
#define AUDIO_MUSIC_MAX_LEN   512

static int has_mp3_ext(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 5) return 0;  /* need at least "x.mp3" */
    const char *ext = name + n - 4;
    return ext[0] == '.'
        && (ext[1] == 'm' || ext[1] == 'M')
        && (ext[2] == 'p' || ext[2] == 'P')
        && ext[3] == '3';
}

static int cmp_mp3_path(const void *a, const void *b) {
    /* Each element is a fixed-size char buffer containing a C string,
     * so strcmp on the base pointer sorts by filename text. */
    return strcmp((const char *)a, (const char *)b);
}

/* Scans one directory for *.mp3 files, appending "<dir>/<name>" into
 * out_paths[] starting at index `start`, and sorts just the slice it
 * added (so each music source stays in filename order). Returns the
 * new count. A missing/unreadable dir is a no-op (returns `start`). */
static int scan_dir_into(const char *dir,
                         char out_paths[][AUDIO_MUSIC_MAX_LEN],
                         int start, int max_paths) {
    DIR *d = opendir(dir);
    if (!d) return start;

    int count = start;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_paths) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        if (!has_mp3_ext(name)) continue;
        snprintf(out_paths[count], AUDIO_MUSIC_MAX_LEN, "%s/%s", dir, name);
        count++;
    }
    closedir(d);

    if (count - start > 1) {
        qsort(out_paths[start], (size_t)(count - start),
              sizeof(out_paths[0]), cmp_mp3_path);
    }
    return count;
}

/* Directory holding the running executable, into buf. Lets us find
 * assets bundled beside it in a macOS .app (<exe>/../Resources/...).
 * Returns 1 on success, 0 if the path can't be resolved. */
static int executable_dir(char *buf, size_t buflen) {
#if defined(__APPLE__)
    uint32_t sz = (uint32_t)buflen;
    if (_NSGetExecutablePath(buf, &sz) != 0) return 0;
#elif defined(__linux__)
    ssize_t k = readlink("/proc/self/exe", buf, buflen - 1);
    if (k <= 0) return 0;
    buf[k] = '\0';
#else
    (void)buf; (void)buflen;
    return 0;
#endif
    char *slash = strrchr(buf, '/');
    if (!slash) return 0;
    *slash = '\0';                 /* strip the executable filename */
    return 1;
}

/* Builds the playlist from every candidate music source in priority
 * order: `assets_dir` (the working-directory "assets" by default, or
 * the --assets / GLR_ASSETS_DIR override), the bundled assets beside
 * the executable (macOS .app), then the per-user music folder (created
 * on first run so the user has somewhere to drop tracks). Returns the
 * total number of paths written. */
static int build_mp3_playlist(const char *assets_dir,
                              char out_paths[][AUDIO_MUSIC_MAX_LEN],
                              int max_paths) {
    int n = scan_dir_into(assets_dir, out_paths, 0, max_paths);

    char exedir[AUDIO_MUSIC_MAX_LEN];
    if (executable_dir(exedir, sizeof(exedir))) {
        /* Path can be exedir + "/../Resources/" + AUDIO_ASSETS_DIR + null. */
        char bundled[AUDIO_MUSIC_MAX_LEN + 64];
        snprintf(bundled, sizeof(bundled), "%s/../Resources/%s",
                 exedir, AUDIO_ASSETS_DIR);
        n = scan_dir_into(bundled, out_paths, n, max_paths);
    }

    char udir[AUDIO_MUSIC_MAX_LEN];
    if (glr_paths_user_music_dir(udir, sizeof(udir))) {
        int created = 0;
        if (glr_paths_ensure_dir(udir, &created) && created) {
            fprintf(stderr, "repl_audio: add more music in %s\n", udir);
        }
        n = scan_dir_into(udir, out_paths, n, max_paths);
    }
    return n;
}

static void print_usage(const char *prog) {
    const char *name = (prog && prog[0]) ? prog : "gl-repl";

    fprintf(stdout,
            "Usage: %s [options] [input.c | workspace]\n"
            "\n"
            "Options:\n"
            "  -h, --help   Show this help text and exit\n"
            "  --noaccum    Disable accumulation buffer antialiasing\n"
            "  --no-audio   Start without audio (disables music entirely)\n"
            "  --assets <dir>  Music directory to scan for *.mp3 instead of\n"
            "               ./assets (also via GLR_ASSETS_DIR env var)\n"
            "  --dump-code  Load the session and print the editor buffer\n"
            "  --dump-flat  Load the session and print flattened commands\n"
            "  --dump-state-layout  Print ReplRuntimeState field layout\n"
            "  --detailed-prof  Emit finer-grained startup init traces\n"
            "               (also via GLR_DETAILED_PROF env var)\n"
            "  --export-ply <file>  Render one frame, capture geometry to <file>\n"
            "               as a PLY mesh, then exit (needs a display)\n"
            "  --export-ply-srgb  Decode vertex colors sRGB -> linear on export\n"
            "               (for color-managed viewers; pair with --export-ply)\n"
            "  --example <name|idx>  Start on a built-in example (name is\n"
            "               case-insensitive; or a 0-based index)\n"
            "  --list-examples  Print the built-in examples and exit\n"
            "  --time <secs>  Set the initial animation time t at startup\n"
            "               (else GLR_TIME; --time wins). Start animations later.\n"
            "\n"
            "Arguments:\n"
            "  input.c      Optional saved session to load at startup\n"
            "  workspace/   Optional directory: load every *.c as a user scene\n",
            name);
}

/* Case-insensitive full compare / substring test (keeps gl_repl.c free of a
 * <strings.h> / strcasecmp dependency). */
static int example_ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == '\0' && *b == '\0';
}
static int example_ci_contains(const char *hay, const char *needle) {
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
        fprintf(out, "  %2d  %s\n", i, glr_scene_example_name(i));
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
        int idx = atoi(arg);
        return (idx >= 0 && idx < n) ? idx : -1;
    }

    int substr = -1;
    for (int i = 0; i < n; i++) {
        const char *name = glr_scene_example_name(i);
        if (!name) continue;
        if (example_ci_equal(name, arg)) return i;          /* exact wins */
        if (substr < 0 && example_ci_contains(name, arg)) substr = i;
    }
    return substr;
}

/* Set by --export-ply <file>: when non-NULL, the first rendered frame
 * captures the scene to this path (PLY) and the process exits. */
static const char *g_export_ply_path = NULL;
/* Set by --export-ply-srgb: decode vertex colors sRGB -> linear on export
 * (for color-managed viewers that otherwise render them washed out). */
static int g_export_ply_srgb = 0;

static void display_func(void) {
    /* Trace the first two frames separately (gated on g_detailed_prof
     * — see --detailed-prof / GLR_DETAILED_PROF). The first frame
     * pays one-shot costs (GLUT solid-shape display-list compile,
     * macOS first-drawable wait, GL stack lazy init / SW-fallback
     * chatter) that frame 2 reveals as gone. A side-by-side frame-1
     * vs. frame-2 gap proves the spend was first-frame-only and not
     * a steady-state regression. */
    static int frames_traced = 0;
    int trace_this = g_detailed_prof && (frames_traced < 2);
    int frame_num = frames_traced + 1;
    char buf[64];
    if (trace_this) {
        snprintf(buf, sizeof(buf), "frame %d display callback", frame_num);
        init_trace(buf);
    }
    glr_ctrl_display_frame();
    if (trace_this) {
        snprintf(buf, sizeof(buf), "frame %d render done", frame_num);
        init_trace(buf);
    }

    /* --export-ply: the scene has rendered one full frame, so it is loaded,
     * flattened, and the GL context is live. Capture geometry to the requested
     * file (post-context — feedback needs a context, unlike the pre-context
     * --dump-* paths) and exit. Done BEFORE glutSwapBuffers() so the feedback
     * pass never re-enters the GL pipeline after a buffer swap (which can race
     * the display on some drivers); the swap is cosmetic here since we exit. */
    if (g_export_ply_path) {
        int n = glr_export_mesh_ply(g_export_ply_path, g_export_ply_srgb);
        if (n >= 0)
            fprintf(stderr, "[export-ply] wrote %d triangle(s) to %s\n",
                    n, g_export_ply_path);
        else
            fprintf(stderr, "[export-ply] failed to export %s "
                    "(empty scene, unwritable path, or capture overflow)\n",
                    g_export_ply_path);
        exit(n < 0 ? 1 : 0);
    }

    glFinish(); /* ensure all GL commands are done before we timestamp the swap */
    glutSwapBuffers();
    if (trace_this) {
        snprintf(buf, sizeof(buf), "frame %d swap done", frame_num);
        init_trace(buf);
        frames_traced++;
    }
}

static void reshape_func(int w, int h) {
    glr_ctrl_reshape(w, h);
}

static void keyboard_func(unsigned char key, int x, int y) {
    glr_ctrl_keyboard(key, x, y);
}

static void special_func(int key, int x, int y) {
    glr_ctrl_special(key, x, y);
}

static void mouse_func(int button, int state, int x, int y) {
    glr_ctrl_mouse(button, state, x, y);
}

static void motion_func(int x, int y) {
    glr_ctrl_motion(x, y);
}

static void passive_motion_func(int x, int y) {
    glr_ctrl_passive_motion(x, y);
}

#ifndef USE_GLUT
static void mousewheel_func(int wheel, int direction, int x, int y) {
    glr_ctrl_mousewheel(wheel, direction, x, y);
}
#endif

static void timer_func(int value) {
    glr_ctrl_timer(value);
}

/* Ctrl+C / terminal SIGINT: route to the same save-and-quit safeguard
 * as Ctrl+Q. The handler only flips a flag (async-signal-safe); the
 * recovery save + exit happen on the normal path in glr_ctrl_tick(). */
static void on_sigint(int sig) {
    (void)sig;
    glr_ctrl_request_quit();
}

int main(int argc, char **argv) {
    const char *input_file = NULL;
    const char *example_arg = NULL;
    const char *assets_override = NULL;   /* --assets DIR (else GLR_ASSETS_DIR) */
    const char *time_arg = NULL;          /* --time SECS (else GLR_TIME) */
    int dump_code = 0;
    int dump_flat = 0;
    int dump_state_layout = 0;
    int no_audio  = 0;
    int use_accum = 1;
    int window_w  = 1200;
    int window_h  = 800;

    init_trace("start");
    glr_ctrl_set_program_name(argv[0]);

    /* GLR_DETAILED_PROF (any non-empty value) is the env-var twin of
     * --detailed-prof; either one promotes the optional fine-grained
     * init_trace_detail() phases (glutInit split, audio playlist
     * sub-steps, first-two-frames triple) to stderr. Default off so
     * a normal start emits only the baseline boot-phase set. */
    {
        const char *env = getenv("GLR_DETAILED_PROF");
        if (env && *env) g_detailed_prof = 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--noaccum") == 0)
            use_accum = 0;
        else if (strcmp(argv[i], "--no-audio") == 0)
            no_audio = 1;
        else if (strcmp(argv[i], "--dump-code") == 0)
            dump_code = 1;
        else if (strcmp(argv[i], "--dump-flat") == 0)
            dump_flat = 1;
        else if (strcmp(argv[i], "--dump-state-layout") == 0)
            dump_state_layout = 1;
        else if (strcmp(argv[i], "--detailed-prof") == 0)
            g_detailed_prof = 1;
        else if (strcmp(argv[i], "--export-ply") == 0 && i + 1 < argc)
            g_export_ply_path = argv[++i];
        else if (strcmp(argv[i], "--export-ply-srgb") == 0)
            g_export_ply_srgb = 1;
        else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc)
            assets_override = argv[++i];
        else if (strcmp(argv[i], "--example") == 0 && i + 1 < argc)
            example_arg = argv[++i];
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc)
            time_arg = argv[++i];
        else if (strcmp(argv[i], "--list-examples") == 0) {
            list_examples(stdout);
            return 0;
        }
        else if (!input_file)
            input_file = argv[i];
    }

    /* Resolve --example up front: a bad name fails fast (before opening a
     * window) and the error lists what is available. */
    int example_index = -1;
    if (example_arg) {
        example_index = resolve_example_index(example_arg);
        if (example_index < 0) {
            fprintf(stderr, "gl-repl: unknown example \"%s\"\n", example_arg);
            list_examples(stderr);
            return 1;
        }
    }

    if (dump_code || dump_flat || dump_state_layout) {
        /* Dump-only paths skip glr_ctrl_init_gl (no GL context, no
         * window). glr_ctrl_bootstrap_repl now installs the app
         * services (status sink + export-config bridge) at its top so
         * @cfg in imported files is applied even on the dump path.
         * Status messages still surface to UiState, but UiState is
         * never rendered here, so they're effectively silent. */
        if (dump_code || dump_flat)
            glr_ctrl_bootstrap_repl(input_file);
        if (dump_code)
            glr_debug_dump_current_editor(stdout);
        if (dump_flat)
            glr_debug_dump_current_flat_commands_sync(stdout);
        if (dump_state_layout)
            glr_debug_dump_runtime_state_layout(stdout);
        return 0;
    }

    init_trace("glutInit begin");
    glutInit(&argc, argv);
    init_trace_detail("glutInit done");
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH | GLUT_MULTISAMPLE |
                        (use_accum ? GLUT_ACCUM : 0));
    glutInitWindowSize(window_w, window_h);
    glutCreateWindow("OpenGL REPL - Display List Dynamic Rendering");
    init_trace("window created");

    glr_ctrl_init_gl();
    atexit(glr_shutdown);
    init_trace("GL init done");
    glr_ctrl_bootstrap_repl(input_file);
    if (example_index >= 0)
        glr_scene_load_example(example_index);
    /* Initial animation time: --time SECS wins over GLR_TIME. Applied after
     * any example load (which resets t to 0) so the override sticks. Lets a
     * headless capture start the animation from a later point in its
     * timeline (e.g. skip a long intro before recording). */
    {
        const char *t_src = time_arg ? time_arg : getenv("GLR_TIME");
        if (t_src && *t_src)
            glr_ctrl_set_time((float)atof(t_src));
    }
    init_trace("REPL bootstrap done");
    glr_ctrl_set_accum(use_accum);

    /* Audio: init once, scan assets/ *.mp3 for a playlist, play the
     * first track, shutdown on exit. Failures here are non-fatal: the
     * REPL keeps running without sound.
     * Skipped entirely when --no-audio was passed. */
    if (!no_audio) init_trace("glr_audio_init begin");
    if (!no_audio && glr_audio_init() == 0) {
        init_trace("glr_audio_init done");
        glr_audio_set_state_file(AUDIO_STATE_FILE);
        static char music_paths[AUDIO_MUSIC_MAX_PATHS]
                               [AUDIO_MUSIC_MAX_LEN];
        /* --assets wins over GLR_ASSETS_DIR wins over the default. */
        const char *assets_dir = assets_override;
        if (!assets_dir) {
            const char *env = getenv("GLR_ASSETS_DIR");
            if (env && env[0]) assets_dir = env;
        }
        if (!assets_dir) assets_dir = AUDIO_ASSETS_DIR;
        int n = build_mp3_playlist(assets_dir, music_paths, AUDIO_MUSIC_MAX_PATHS);
        /* opendir/readdir over the candidate music dirs. Cheap when
         * local; worth timing when the working directory lives on
         * iCloud. */
        if (g_detailed_prof) {
            char buf[64];
            snprintf(buf, sizeof(buf), "playlist scan done (%d tracks)", n);
            init_trace(buf);
        }
        if (n > 0) {
            const char *ptrs[AUDIO_MUSIC_MAX_PATHS];
            for (int i = 0; i < n; i++) ptrs[i] = music_paths[i];
            glr_audio_set_playlist(ptrs, n);
            glr_audio_play_playlist();
        } else {
            /* Back-compat: no .mp3 files found, fall back to the
             * legacy single-file default so existing setups that use
             * assets/song.mp3 keep working. */
            glr_audio_play_music(AUDIO_DEFAULT_MUSIC);
        }
        /* play_playlist() reads audio_state.ini synchronously on the
         * caller (see glr_audio.c header comment) before posting the
         * slow media-open request to the worker. The actual sound load
         * runs off-thread so it never lands here. */
        init_trace_detail("playlist start requested");
        /* Apply saved audio cfg after play_playlist() so load_state() has
         * already populated g_cfg_mode. The action layer maps that UI config
         * value back onto the audio engine before the first frame. */
        glr_actions_apply_defaults();
        init_trace("audio playlist started");
    }

    init_trace("entering main loop");
    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutKeyboardFunc(keyboard_func);
    glutSpecialFunc(special_func);
    glutMouseFunc(mouse_func);
    glutMotionFunc(motion_func);
    glutPassiveMotionFunc(passive_motion_func);
#ifndef USE_GLUT
    glutMouseWheelFunc(mousewheel_func);
#endif
    glutTimerFunc(16, timer_func, 0);
    signal(SIGINT, on_sigint);   /* Ctrl+C -> save-and-quit safeguard */

    glutMainLoop();
    return 0;
}
