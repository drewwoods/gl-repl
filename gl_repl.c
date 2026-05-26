#include "app/glr_ctrl.h"
#include "app/glr_actions.h"
#include "app/glr_debug.h"
#include "repl/executor.h"
#include "app/glr_audio.h"

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

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
 * this folder are picked up in filename order as a playlist. */
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

/* Scans AUDIO_ASSETS_DIR for *.mp3 files, sorts them by filename,
 * and writes "<dir>/<name>" into out_paths[]. Returns the number of
 * paths written, or 0 on error / empty / missing directory. */
static int scan_mp3_playlist(char out_paths[][AUDIO_MUSIC_MAX_LEN],
                              int max_paths) {
    DIR *d = opendir(AUDIO_ASSETS_DIR);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_paths) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        if (!has_mp3_ext(name)) continue;
        snprintf(out_paths[count], AUDIO_MUSIC_MAX_LEN, "%s/%s",
                 AUDIO_ASSETS_DIR, name);
        count++;
    }
    closedir(d);

    if (count > 1) {
        qsort(out_paths, (size_t)count, sizeof(out_paths[0]), cmp_mp3_path);
    }
    return count;
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
            "  --dump-code  Load the session and print the editor buffer\n"
            "  --dump-flat  Load the session and print flattened commands\n"
            "  --dump-state-layout  Print ReplRuntimeState field layout\n"
            "  --detailed-prof  Emit finer-grained startup init traces\n"
            "               (also via GLR_DETAILED_PROF env var)\n"
            "\n"
            "Arguments:\n"
            "  input.c      Optional saved session to load at startup\n"
            "  workspace/   Optional directory: load every *.c as a user scene\n",
            name);
}

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
        else if (!input_file)
            input_file = argv[i];
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
            glr_debug_dump_editor(stdout, source_document_view());
        if (dump_flat)
            glr_debug_dump_flat_commands(stdout, source_document_view());
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
    init_trace("GL init done");
    atexit(repl_executor_destroy_resources);
    glr_ctrl_bootstrap_repl(input_file);
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
        int n = scan_mp3_playlist(music_paths, AUDIO_MUSIC_MAX_PATHS);
        /* opendir/readdir on assets/. Cheap when the dir is local;
         * worth timing when the working directory lives on iCloud. */
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
        atexit(glr_audio_shutdown);
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
