#include "sample.h"
#include "repl_core.h"
#include "repl_audio.h"

#include <dirent.h>
#include <stdlib.h>

/* Music assets live here. Relative to the sample's working directory
 * on native, and to the Emscripten virtual FS mount point set up by
 * --preload-file in emscripten/build.sh. Any *.mp3 files dropped into
 * this folder are picked up in filename order as a playlist. */
#ifndef REPL_AUDIO_ASSETS_DIR
#define REPL_AUDIO_ASSETS_DIR "assets"
#endif

/* Single-file fallback used when the assets directory is empty or
 * missing, so existing single-track setups keep working. */
#ifndef REPL_AUDIO_DEFAULT_MUSIC
#define REPL_AUDIO_DEFAULT_MUSIC "assets/song.mp3"
#endif

#define REPL_AUDIO_MUSIC_MAX_PATHS 64
#define REPL_AUDIO_MUSIC_MAX_LEN   512

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

/* Scans REPL_AUDIO_ASSETS_DIR for *.mp3 files, sorts them by filename,
 * and writes "<dir>/<name>" into out_paths[]. Returns the number of
 * paths written, or 0 on error / empty / missing directory. */
static int scan_mp3_playlist(char out_paths[][REPL_AUDIO_MUSIC_MAX_LEN],
                              int max_paths) {
    DIR *d = opendir(REPL_AUDIO_ASSETS_DIR);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_paths) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        if (!has_mp3_ext(name)) continue;
        snprintf(out_paths[count], REPL_AUDIO_MUSIC_MAX_LEN, "%s/%s",
                 REPL_AUDIO_ASSETS_DIR, name);
        count++;
    }
    closedir(d);

    if (count > 1) {
        qsort(out_paths, (size_t)count, sizeof(out_paths[0]), cmp_mp3_path);
    }
    return count;
}

static void print_usage(const char *prog) {
    const char *name = (prog && prog[0]) ? prog : "sample";

    fprintf(stdout,
            "Usage: %s [options] [input.c]\n"
            "\n"
            "Options:\n"
            "  -h, --help   Show this help text and exit\n"
            "  --noaccum    Disable accumulation buffer antialiasing\n"
            "  --dump-code  Load the session and print the editor buffer\n"
            "\n"
            "Arguments:\n"
            "  input.c      Optional saved session to load at startup\n",
            name);
}

static void display_func(void) {
    repl_display_func();
}

static void reshape_func(int w, int h) {
    repl_reshape_func(w, h);
}

static void keyboard_func(unsigned char key, int x, int y) {
    repl_keyboard_func(key, x, y);
}

static void special_func(int key, int x, int y) {
    repl_special_func(key, x, y);
}

static void mouse_func(int button, int state, int x, int y) {
    repl_mouse_func(button, state, x, y);
}

static void motion_func(int x, int y) {
    repl_motion_func(x, y);
}

static void passive_motion_func(int x, int y) {
    repl_passive_motion_func(x, y);
}

#ifndef USE_GLUT
static void mousewheel_func(int wheel, int direction, int x, int y) {
    repl_mousewheel_func(wheel, direction, x, y);
}
#endif

static void timer_func(int value) {
    repl_timer_func(value);
}

int main(int argc, char **argv) {
    const char *input_file = NULL;
    int dump_code = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--noaccum") == 0)
            g_use_accum = 0;
        else if (strcmp(argv[i], "--dump-code") == 0)
            dump_code = 1;
        else if (!input_file)
            input_file = argv[i];
    }

    if (dump_code) {
        init_predef_vars();
        for (int i = 0; i < g_num_predef_vars; i++)
            if (strcmp(g_predef_vars[i].name, "t") == 0) { g_t_var_idx = i; break; }
        repl_load_initial_commands(input_file);
        repl_debug_dump_editor(stdout);
        return 0;
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH | GLUT_MULTISAMPLE |
                        (g_use_accum ? GLUT_ACCUM : 0));
    glutInitWindowSize(g_win_w, g_win_h);
    glutCreateWindow("OpenGL REPL - Display List Dynamic Rendering");

    repl_init_gl();
    init_predef_vars();
    for (int i = 0; i < g_num_predef_vars; i++)
        if (strcmp(g_predef_vars[i].name, "t") == 0) { g_t_var_idx = i; break; }
    repl_load_initial_commands(input_file);

    /* Audio: init once, scan assets/*.mp3 for a playlist, play the
     * first track, shutdown on exit. Failures here are non-fatal: the
     * REPL keeps running without sound. */
    if (repl_audio_init() == 0) {
        static char music_paths[REPL_AUDIO_MUSIC_MAX_PATHS]
                               [REPL_AUDIO_MUSIC_MAX_LEN];
        int n = scan_mp3_playlist(music_paths, REPL_AUDIO_MUSIC_MAX_PATHS);
        if (n > 0) {
            const char *ptrs[REPL_AUDIO_MUSIC_MAX_PATHS];
            for (int i = 0; i < n; i++) ptrs[i] = music_paths[i];
            repl_audio_set_playlist(ptrs, n);
            repl_audio_play_playlist();
        } else {
            /* Back-compat: no .mp3 files found, fall back to the
             * legacy single-file default so existing setups that use
             * assets/song.mp3 keep working. */
            repl_audio_play_music(REPL_AUDIO_DEFAULT_MUSIC);
        }
        atexit(repl_audio_shutdown);
    }

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

    glutMainLoop();
    return 0;
}
