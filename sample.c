#include "glr_ctrl.h"
#include "glr_actions.h"
#include "repl_debug.h"
#include "repl_executor.h"
#include "audio.h"

#include <dirent.h>
#include <stdlib.h>

/* Music assets live here. Relative to the sample's working directory
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
    const char *name = (prog && prog[0]) ? prog : "sample";

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
            "\n"
            "Arguments:\n"
            "  input.c      Optional saved session to load at startup\n"
            "  workspace/   Optional directory: load every *.c as a user scene\n",
            name);
}

static void display_func(void) {
    glr_ctrl_display_frame();
    glutSwapBuffers();
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

int main(int argc, char **argv) {
    const char *input_file = NULL;
    int dump_code = 0;
    int dump_flat = 0;
    int dump_state_layout = 0;
    int no_audio  = 0;
    int use_accum = 1;
    int window_w  = 1200;
    int window_h  = 800;
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
        else if (!input_file)
            input_file = argv[i];
    }

    if (dump_code || dump_flat || dump_state_layout) {
        if (dump_code || dump_flat)
            glr_ctrl_bootstrap_repl(input_file);
        if (dump_code)
            repl_debug_dump_editor(stdout, editor_buffer_view());
        if (dump_flat)
            repl_debug_dump_flat_commands(stdout, editor_buffer_view());
        if (dump_state_layout)
            repl_debug_dump_runtime_state_layout(stdout);
        return 0;
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH | GLUT_MULTISAMPLE |
                        (use_accum ? GLUT_ACCUM : 0));
    glutInitWindowSize(window_w, window_h);
    glutCreateWindow("OpenGL REPL - Display List Dynamic Rendering");

    glr_ctrl_init_gl();
    atexit(repl_executor_destroy_resources);
    glr_ctrl_bootstrap_repl(input_file);
    glr_ctrl_set_accum(use_accum);

    /* Audio: init once, scan assets/ *.mp3 for a playlist, play the
     * first track, shutdown on exit. Failures here are non-fatal: the
     * REPL keeps running without sound.
     * Skipped entirely when --no-audio was passed. */
    if (!no_audio && audio_init() == 0) {
        audio_set_state_file(AUDIO_STATE_FILE);
        static char music_paths[AUDIO_MUSIC_MAX_PATHS]
                               [AUDIO_MUSIC_MAX_LEN];
        int n = scan_mp3_playlist(music_paths, AUDIO_MUSIC_MAX_PATHS);
        if (n > 0) {
            const char *ptrs[AUDIO_MUSIC_MAX_PATHS];
            for (int i = 0; i < n; i++) ptrs[i] = music_paths[i];
            audio_set_playlist(ptrs, n);
            audio_play_playlist();
        } else {
            /* Back-compat: no .mp3 files found, fall back to the
             * legacy single-file default so existing setups that use
             * assets/song.mp3 keep working. */
            audio_play_music(AUDIO_DEFAULT_MUSIC);
        }
        /* Apply saved audio cfg after play_playlist() so load_state() has
         * already populated g_cfg_mode. The action layer maps that UI config
         * value back onto the audio engine before the first frame. */
        repl_actions_apply_defaults();
        atexit(audio_shutdown);
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
