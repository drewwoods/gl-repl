#include "app/glr_ctrl.h"
#include "app/glr_actions.h"
#include "app/glr_debug.h"
#include "app/glr_audio.h"
#include "app/glr_frame_pacer.h"
#include "app/glr_mesh_export.h"
#include "app/glr_paths.h"
#include "app/glr_pointer_script.h"
#include "app/splash.h"
#include "repl/examples.h"
#include "repl/host_effects.h"   /* repl_set_status (tour cancel notice) */

#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
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

static double init_elapsed_seconds(void) {
    if (g_init_t0 < 0.0) g_init_t0 = init_now();
    return init_now() - g_init_t0;
}

static void init_trace(const char *phase) {
    fprintf(stderr, "[init +%6.3fs] %s\n", init_elapsed_seconds(), phase);
}

static void init_trace_detail(const char *phase) {
    if (g_detailed_prof) init_trace(phase);
}

/* Native music assets live here, relative to gl-repl's working directory.
 * Any *.mp3 files dropped into this folder are picked up in filename order
 * as a playlist. The web build gets its playlist from assets/music.json and
 * streams those files directly via the browser.
 *
 * The playlist is built from three sources, in order (see
 * build_mp3_playlist): the primary assets dir — this working-directory
 * "assets" by default, or the --assets <dir> / GLR_ASSETS_DIR override
 * — then the copy bundled beside the executable in a macOS .app
 * (<exe>/../Resources/assets — where `make app` puts sample.mp3), and
 * a per-user music folder (glr_paths_user_music_dir) the user can drop more
 * tracks into. */

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

/* Capture affordance (doc GIFs), sibling of GLR_TIME / GLR_EDIT_LINE:
 * GLR_VIEW_TOGGLE_AT="t1,t2,..." toggles the 2D/3D view mode once as the
 * rendered-frame clock crosses each listed second (t advances 1/60 s per
 * frame), so the menu-bar swatch transition is recordable headlessly.
 * main() enables GLR_TICK_PER_FRAME semantics whenever this hook is active,
 * so the transition and the rest of the simulation advance exactly once per
 * captured frame. Unset => no-op; production behavior is unchanged. */
static void maybe_capture_view_toggle(void) {
    static int   inited = 0;
    static int   frame = 0;
    static int   fire_frame[8];
    static int   fired[8];
    static int   n = 0;
    if (!inited) {
        inited = 1;
        const char *s = getenv("GLR_VIEW_TOGGLE_AT");
        while (s && *s && n < 8) {
            char *end;
            float secs = strtof(s, &end);
            if (end == s) break;
            fire_frame[n] = (int)(secs * 60.0f + 0.5f);
            fired[n] = 0;
            n++;
            s = end;
            while (*s == ',' || *s == ' ') s++;
        }
    }
    if (n == 0) return;
    for (int i = 0; i < n; i++) {
        if (!fired[i] && frame >= fire_frame[i]) {
            fired[i] = 1;
            glr_action_toggle_view_mode();
        }
    }
    frame++;
}

/* Capture affordance, sibling of GLR_VIEW_TOGGLE_AT: GLR_OPEN_COLOR_PICKER=
 * <line> opens the floating color picker on that source line (0-based,
 * clamped; no-op unless the line is a picker-editable color command). The
 * picker only opens via a swatch click, which a capture run has no mouse to
 * deliver. Applied on the first display callback — not at bootstrap —
 * because the popup placement clamps against the live viewport, which
 * reshape has not populated until the main loop starts. */
static void maybe_capture_open_color_picker(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *s = getenv("GLR_OPEN_COLOR_PICKER");
    if (s && *s)
        glr_ctrl_open_color_picker(atoi(s));
}

/* Capture affordance, sibling of GLR_OPEN_COLOR_PICKER: GLR_OPEN_GL_STATE=
 * <line> opens the floating OpenGL-state popup anchored to that source line
 * (0-based; must be a visually blank committed or live editor row — the
 * controller's per-frame view builder closes the popup otherwise). The popup
 * only opens via a right-click, which a headless capture run has no mouse to
 * deliver. */
static void maybe_capture_open_gl_state(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *s = getenv("GLR_OPEN_GL_STATE");
    if (s && *s)
        glr_ctrl_open_gl_state_popup(atoi(s));
}

/* Capture affordance, sibling of GLR_OPEN_COLOR_PICKER: GLR_OPEN_HELP=<tab>
 * opens the F1 help overlay on the given tab index (0-based, clamped by
 * the tab-advance action) on the first displayed frame. The overlay
 * otherwise needs an F1 special-key press, which a headless capture run
 * has no way to deliver (GLR_TYPE_KEYS only feeds ASCII bytes). */
static void maybe_capture_open_help(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *s = getenv("GLR_OPEN_HELP");
    if (!s) return;
    glr_ctrl_toggle_help();
    int tab = *s ? atoi(s) : 0;
    for (int i = 0; i < tab; i++)
        glr_action_help_tab_next();
}

static void display_func(void) {
    maybe_capture_view_toggle();
    maybe_capture_open_color_picker();
    maybe_capture_open_gl_state();
    maybe_capture_open_help();
    /* Scripted pointer/keyboard input (GLR_POINTER_SCRIPT): fire this
     * frame's events before the frame renders so it reflects them. */
    glr_pointer_script_frame();
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

    /* Startup splash banner along the bottom; frame-counted, fades out.
     * Drawn over the composited frame so the scene/UI warm up underneath. */
    if (splash_active())
        splash_render(glutGet(GLUT_WINDOW_WIDTH), glutGet(GLUT_WINDOW_HEIGHT));

    /* Scripted-pointer overlay (cursor arrow, click ripple, highlight
     * ring) so recorded video shows where the synthetic mouse is. */
    if (glr_pointer_script_active())
        glr_pointer_script_render_overlay(glutGet(GLUT_WINDOW_WIDTH),
                                          glutGet(GLUT_WINDOW_HEIGHT));

    glFinish(); /* ensure all GL commands are done before we timestamp the swap */
    glutSwapBuffers();
    if (trace_this) {
        snprintf(buf, sizeof(buf), "frame %d swap done", frame_num);
        init_trace(buf);
        frames_traced++;
    }

    /* In GLR_TICK_PER_FRAME mode, advance only after the completed frame so
     * the captured sequence is t0, t0+dt, ... . The timer continues to pace
     * redisplays but deliberately skips its own tick in this mode. */
    glr_ctrl_frame_presented();
}

static void reshape_func(int w, int h) {
    glr_ctrl_reshape(w, h);
}

/* Real user input during a menu-started guided tour hands control back:
 * the canceling event is swallowed (an accidental click shouldn't also
 * click through whatever the tour left under the pointer). Only genuine
 * input can trip this — scripted events dispatch straight into the
 * glr_ctrl_* entry points and never pass through these GLUT callbacks.
 * Pointer motion deliberately does NOT cancel (a nudged mouse shouldn't
 * kill the tour), and env-driven capture runs are never canceled. */
static int g_tour_swallow_release = 0;

static int tour_cancel_intercept(void) {
    if (!glr_pointer_script_tour_active())
        return 0;
    glr_pointer_script_stop();
    repl_set_status("Tour stopped");
    return 1;
}

static void keyboard_func(unsigned char key, int x, int y) {
    splash_skip(); /* any keypress dismisses the startup banner */
    if (tour_cancel_intercept()) return;
    glr_ctrl_keyboard(key, x, y);
}

static void special_func(int key, int x, int y) {
    if (tour_cancel_intercept()) return;
    glr_ctrl_special(key, x, y);
}

static void mouse_func(int button, int state, int x, int y) {
    if (state == GLUT_DOWN && tour_cancel_intercept()) {
        g_tour_swallow_release = 1; /* eat the paired release too */
        return;
    }
    if (state == GLUT_UP && g_tour_swallow_release) {
        g_tour_swallow_release = 0;
        return;
    }
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
    if (tour_cancel_intercept()) return;
    glr_ctrl_mousewheel(wheel, direction, x, y);
}

/* freeglut clears the current window before it runs the close callback.
 * Remember that this was a real window-manager close so main() can write
 * the same recovery copy as the explicit quit paths after the loop returns. */
static int g_window_closed = 0;

static void window_close_func(void) {
    g_window_closed = 1;
}
#endif

static GlrFramePacer g_frame_pacer = GLR_FRAME_PACER_INIT;

static double frame_timer_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* The GLUT host owns window validity, redisplay, and timer registration.
 * Pace against an absolute 60 Hz deadline: the rounded delays average to
 * 16.667 ms without accumulating integer-millisecond drift. */
static void timer_func(int value) {
    int delay;
    (void)value;

    /* A title-bar close clears freeglut's current window before the destroy
     * callback and before glutMainLoop() returns. An already-queued timer can
     * still run in that interval. */
    if (glutGetWindow() == 0)
        return;

    glr_ctrl_on_frame_timer();
    glutPostRedisplay();

    delay = glr_frame_pacer_next_delay_ms(&g_frame_pacer,
                                          frame_timer_now_ms());
    glutTimerFunc((unsigned int)delay, timer_func, 0);
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
    const char *examples_dir = NULL;
    const char *assets_override = NULL;   /* --assets DIR (else GLR_ASSETS_DIR) */
    const char *time_arg = NULL;          /* --time SECS (else GLR_TIME) */
    int dump_code = 0;
    int dump_flat = 0;
    int dump_flat_histogram = 0;
    int dump_state_layout = 0;
    int no_audio  = 0;
    int list_examples_flag = 0;
    int use_accum = 1;
    int window_w  = 1200;
    int window_h  = 800;

    init_trace("start");
    glr_audio_set_hitch_log_elapsed_fn(init_elapsed_seconds);
    glr_ctrl_set_init_log_elapsed_fn(init_elapsed_seconds);
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
        } else if (strcmp(argv[i], "--no-accum") == 0)
            use_accum = 0;
        else if (strcmp(argv[i], "--no-audio") == 0)
            no_audio = 1;
        else if (strcmp(argv[i], "--dump-code") == 0)
            dump_code = 1;
        else if (strcmp(argv[i], "--dump-flat") == 0)
            dump_flat = 1;
        else if (strcmp(argv[i], "--flat-histogram") == 0)
            dump_flat_histogram = 1;
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
        else if (strcmp(argv[i], "--examples-dir") == 0 && i + 1 < argc)
            examples_dir = argv[++i];
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc)
            time_arg = argv[++i];
        else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            /* --window WxH: initial window size. Headless captures use
             * it to render at 2x and downscale (4x supersampling) since
             * the software rasterizer has no MSAA. */
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                window_w = w;
                window_h = h;
            } else {
                fprintf(stderr, "gl-repl: bad --window \"%s\" (want WxH)\n",
                        argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--list-examples") == 0) {
            list_examples_flag = 1;
        }
        else if (!input_file)
            input_file = argv[i];
    }

    if (examples_dir) {
        char err[512];
        if (!repl_examples_load_dir(examples_dir, err, sizeof(err))) {
            fprintf(stderr, "gl-repl: could not load examples from %s: %s\n",
                    examples_dir, err[0] ? err : "unknown error");
            return 1;
        }
    }

    if (list_examples_flag) {
        list_examples(stdout);
        return 0;
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

    if (dump_code || dump_flat || dump_flat_histogram || dump_state_layout) {
        /* Dump-only paths skip glr_ctrl_init_gl (no GL context, no
         * window). glr_ctrl_bootstrap_repl now installs the app
         * services (status sink + export-config bridge) at its top so
         * @cfg in imported files is applied even on the dump path.
         * Status messages still surface to UiState, but UiState is
         * never rendered here, so they're effectively silent. */
        if (dump_code || dump_flat || dump_flat_histogram) {
            glr_ctrl_bootstrap_repl(input_file);
            /* --example works on the dump paths too: the loader chain
             * (reset transients, undo note, repl_load_example) is
             * GL-free, so built-ins can be inspected without a window. */
            if (example_index >= 0)
                glr_scene_load_example(example_index);
        }
        if (dump_code)
            glr_debug_dump_current_editor(stdout);
        if (dump_flat)
            glr_debug_dump_current_flat_commands_sync(stdout);
        if (dump_flat_histogram)
            glr_debug_dump_current_flat_histogram(stdout);
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
    /* Scripted pointer/keyboard input: GLR_POINTER_SCRIPT=<file> drives
     * menu navigation & co. on the rendered-frame clock (video capture
     * hook — see src/app/glr_pointer_script.h for the grammar). Loaded
     * before the tick-per-frame resolve below because an active script
     * implies the mode. GLR_NO_SPLASH skips the startup banner (any
     * capture that shouldn't open on the splash band). */
    glr_pointer_script_load_env();
    {
        const char *no_splash = getenv("GLR_NO_SPLASH");
        if (no_splash && *no_splash)
            splash_skip();
    }
    /* Offline/capture clock: move the complete fixed-dt controller tick from
     * the wall-clock timer to completed frames. GLR_VIEW_TOGGLE_AT implies the
     * mode for backward-compatible deterministic swatch captures. */
    {
        const char *frame_tick = getenv("GLR_TICK_PER_FRAME");
        const char *view_toggle = getenv("GLR_VIEW_TOGGLE_AT");
        glr_ctrl_set_tick_per_frame((frame_tick && *frame_tick) ||
                                    (view_toggle && *view_toggle) ||
                                    glr_pointer_script_active());
    }
    /* Cursor line override: GLR_EDIT_LINE=<line> (0-based, clamped)
     * parks the cursor on a committed source line and loads it into the
     * input buffer, exactly as arrowing to it would. Headless-capture
     * hook: cursor-bound overlays (transform guides, vertex labels)
     * can't otherwise be posed without live keyboard input. Applied
     * after the file/example load so the line exists. */
    {
        const char *l_src = getenv("GLR_EDIT_LINE");
        if (l_src && *l_src)
            glr_ctrl_set_edit_line(atoi(l_src));
    }
    /* Typed-input override: GLR_TYPE_KEYS=<text> feeds each character
     * through the keyboard dispatch exactly as typing would (guides,
     * autocomplete ghost, and all). Headless-capture hook for
     * mid-typing states — e.g. a partially-entered glVertex3f( whose
     * 2-DOF plane / 1-DOF line guide can't be posed from a committed
     * line. Applied after GLR_EDIT_LINE so it can extend a loaded line
     * or (on a fresh row) start a new one. */
    {
        const char *k_src = getenv("GLR_TYPE_KEYS");
        if (k_src)
            for (const char *k = k_src; *k; k++)
                glr_ctrl_keyboard((unsigned char)*k, 0, 0);
    }
    /* GLR_OPEN_COLOR_PICKER is applied on the first display callback
     * (maybe_capture_open_color_picker), not here: the picker clamps its
     * placement against the live viewport, which reshape has not
     * populated yet at bootstrap time. */
    /* Accumulation-AA boost: GLR_ACCUM_PASSES=<count> (1/2/4/8/12/16)
     * raises the accumulation sample count. Capture hook: the 2D UI
     * renders outside the accumulation loop, so this antialiases the
     * 3D scene while text keeps its full size — unlike 2x supersampling,
     * which halves the apparent UI text. Applied after the file/example
     * load so it wins over any @cfg accum_passes header. */
    {
        const char *p_src = getenv("GLR_ACCUM_PASSES");
        if (p_src && *p_src)
            glr_ctrl_set_accum_passes(atoi(p_src));
    }
    init_trace("REPL bootstrap done");
    glr_ctrl_set_accum(use_accum);

    if (!no_audio)
        glr_audio_bootstrap(assets_override, init_trace, init_trace_detail);
    glr_actions_apply_defaults();

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
    glutCloseFunc(window_close_func);
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_CONTINUE_EXECUTION);
#endif
    glutTimerFunc(16, timer_func, 0);
    signal(SIGINT, on_sigint);   /* Ctrl+C -> save-and-quit safeguard */

    glutMainLoop();
#ifndef USE_GLUT
    if (g_window_closed && glr_ctrl_save_recovery_file()) {
        printf("Saved recovery copy to %s (reload: ./%s %s)\n",
               QUIT_RECOVERY_FILE, glr_ctrl_program_name(),
               QUIT_RECOVERY_FILE);
    }
#endif
    return 0;
}
