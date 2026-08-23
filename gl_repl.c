#include "app/glr_ctrl.h"
#include "app/glr_actions.h"
#include "app/boot/glr_capture_env.h"
#include "app/boot/glr_cli.h"
#include "app/boot/glr_boot_dumps.h"
#include "app/glr_audio.h"
#include "app/glr_extedit.h"
#include "app/boot/glr_frame_pacer.h"
#include "app/boot/glr_init_trace.h"
#include "app/glr_mesh_export.h"
#include "app/glr_paths.h"
#include "app/boot/splash.h"
#include "support/cpuprof.h"     /* the two callback stages that span the
                                  * boot/controller seam are bracketed here;
                                  * every other stage self-times */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Set by --export-ply <file>: when non-NULL, the first rendered frame
 * captures the scene to this path (PLY) and the process exits. */
static const char *g_export_ply_path = NULL;
/* Set by --export-ply-srgb: decode vertex colors sRGB -> linear on export
 * (for color-managed viewers that otherwise render them washed out). */
static int g_export_ply_srgb = 0;

static void display_func(void) {
    /* Open the profiled frame - first statement in the callback, because this
     * file owns that callback boundary, not the controller, which is only one
     * of the stages below. Everything from here to glr_frame_ended() at the
     * bottom is Frame Time. FPS uses callback-start cadence instead; begin()
     * attributes the prior callback-to-callback gap to Browser/Frame Wait. */
    glr_frame_begin();

    /* Every per-frame stage below is inside the frame's work span, so a stage
     * with no ProfSection of its own appears only as unattributed work. Add a
     * section when adding a stage: inside the callee if it can own its bracket,
     * or here when the stage spans the boot/controller seam that only this file
     * bridges. */
    prof_begin(PROF_SCRIPTED_INPUT);
    /* Headless-capture env hooks that need a live viewport / frame clock
     * (color picker, GL-state popup, help overlay, scheduled view toggle);
     * no-ops unless their GLR_* env var is set. */
    glr_capture_env_frame_hook();
    /* Scripted pointer/keyboard input (GLR_POINTER_SCRIPT): fire this
     * frame's events before the frame renders so it reflects them. The
     * synthesized keys/clicks dispatch through glr_ctrl_scripted_*, so a
     * scripted commit's parse/flatten cost lands in this section. */
    glr_ctrl_run_scripted_input_frame();
    prof_end(PROF_SCRIPTED_INPUT);

    /* `--watch`: has the external editor saved the bound scene file? Before
     * the controller's frame, so a reload renders this frame rather than the
     * next one. Its own section, not a child of Scripted Input - it is a peer
     * host-band stage, and the number is what stage 2.5's latency gate is
     * measured against. A bare stat() when nothing moved. */
    prof_begin(PROF_EXTERNAL_EDIT);
    glr_extedit_poll();
    prof_end(PROF_EXTERNAL_EDIT);
    /* Trace the first two frames separately (gated on the init-trace
     * detailed flag - see --detailed-prof / GLR_DETAILED_PROF). The first frame
     * pays one-shot costs (GLUT solid-shape display-list compile,
     * macOS first-drawable wait, GL stack lazy init / SW-fallback
     * chatter) that frame 2 reveals as gone. A side-by-side frame-1
     * vs. frame-2 gap proves the spend was first-frame-only and not
     * a steady-state regression. */
    static int frames_traced = 0;
    int trace_this = glr_init_trace_detailed() && (frames_traced < 2);
    int frame_num = frames_traced + 1;
    char buf[64];
    if (trace_this) {
        snprintf(buf, sizeof(buf), "frame %d display callback", frame_num);
        glr_init_trace(buf);
    }
    glr_ctrl_display_frame();
    if (trace_this) {
        snprintf(buf, sizeof(buf), "frame %d render done", frame_num);
        glr_init_trace(buf);
    }

    /* --export-ply: the scene has rendered one full frame, so it is loaded,
     * flattened, and the GL context is live. Capture geometry to the requested
     * file (post-context - feedback needs a context, unlike the pre-context
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

    /* Host-band overlays: the last two passes over the composited frame, both
     * owned by this callback rather than the controller (splash is boot-band,
     * and the tour overlay must land after the compositor to stay topmost).
     * The aggregate row is bracketed here because it is the only place that can
     * span both - a boot-band pass and a controller-band one. Its children are
     * self-timed where they can be (see the controller's scripted overlay
     * stage);
     * splash_render() is bracketed from here to keep the profiler out of
     * test_splash's deliberately minimal link. */
    prof_begin(PROF_HOST_OVERLAYS);
    /* Startup splash banner along the bottom; frame-counted, fades out.
     * Drawn over the composited frame so the scene/UI warm up underneath. */
    if (splash_active()) {
        prof_begin(PROF_HOST_SPLASH);
        splash_render(glutGet(GLUT_WINDOW_WIDTH), glutGet(GLUT_WINDOW_HEIGHT));
        prof_end(PROF_HOST_SPLASH);
    }

    /* Tour narration overlay (caption, cursor arrow, click ripple,
     * highlight ring) so a tour - or a recorded video - shows what is being
     * said and where the synthetic mouse is. Self-timed as PROF_TOUR_OVERLAY. */
    glr_ctrl_render_script_overlay(glutGet(GLUT_WINDOW_WIDTH),
                                   glutGet(GLUT_WINDOW_HEIGHT));

    /* Ambient tour presence (title card, breathing border, exit collapse) -
     * dead last, so nothing paints over the one piece of chrome that says the
     * app is in a mode, and so it survives a user's whole-frame Post FX.
     * Called unconditionally: it owns the clock its exit animation runs on,
     * which outlives the tour itself. Self-timed as PROF_TOUR_PRESENCE. */
    glr_ctrl_render_tour_presence(glutGet(GLUT_WINDOW_WIDTH),
                                  glutGet(GLUT_WINDOW_HEIGHT));
    prof_end(PROF_HOST_OVERLAYS);

    /* Close the frame's *work* span: everything the callback does to produce
     * this frame is now behind us. The callback stays open across finish,
     * snapshot, and swap, and Present is the derived remainder (nothing in
     * between can go unattributed that way). A native backend may wait here;
     * a browser may return quickly and delay its next rAF callback instead,
     * which the following frame records as Browser Wait. See prof_sections.h. */
    glr_frame_work_end();

    glFinish(); /* ensure all GL commands are done before we timestamp the swap */

    /* Vertex-label occlusion depth, for NEXT frame's label pass.
     *
     * THIS CALL'S POSITION IS LOAD-BEARING and the reason is not local: it must
     * sit AFTER the glFinish above and BEFORE the swap. glReadPixels of depth
     * is a synchronous whole-pipeline sync point, so issued anywhere earlier in
     * the frame it blocks until the driver's queued, vsync-throttled work
     * retires - ~14 ms on a render-ahead driver like NVIDIA's, which is the
     * entire frame budget. Here the glFinish has already drained the queue and
     * that wait is already attributed to Present, so the read costs only its
     * transfer (~2.5 ms at 4x MSAA) in a span that has slack.
     *
     * It follows that the glFinish above is not merely a timestamping nicety
     * any more - this depends on it. If it is ever removed as an optimization,
     * this capture silently becomes a mid-frame stall again.
     * `make check-depth-capture-after-finish` asserts the
     * glFinish -> capture -> swap order so that cannot happen
     * quietly. Full measurements: docs/plans/in-review/
     * vertex-label-depth-readback-stall.md, and `make bench-vertex-labels`.
     *
     * Deliberately outside the Frame Work span closed just above - and outside
     * PROF_HOST_OVERLAYS, which closed with it: this is work for the next frame,
     * paid out of this frame's slack. It has its own PROF_DEPTH_SNAPSHOT row,
     * and glr_frame_ended() subtracts that row out of Present rather than
     * leaving a pixel transfer inside the row drawn as headroom - so the
     * summary reads Frame Work + Depth Snapshot + Present = Frame Time. Nesting
     * it under any span that ends before this line would instead report a 2 ms
     * child of a microsecond parent. */
    glr_ctrl_capture_depth_snapshot();

    glutSwapBuffers();
    if (trace_this) {
        snprintf(buf, sizeof(buf), "frame %d swap done", frame_num);
        glr_init_trace(buf);
        frames_traced++;
    }

    /* Close the frame: records Frame Time and derives Present, and in
     * GLR_TICK_PER_FRAME mode advances the simulation now that the frame is on
     * screen, so the captured sequence is t0, t0+dt, ... . (The timer keeps
     * pacing redisplays but skips its own tick in that mode.) */
    glr_frame_ended();
}

static void reshape_func(int w, int h) {
    glr_ctrl_reshape(w, h);
}

static void keyboard_func(unsigned char key, int x, int y) {
    splash_skip(); /* any keypress dismisses the startup banner */
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

    /* A native title-bar close clears freeglut's current window before the
     * destroy callback and before glutMainLoop() returns. An already-queued
     * timer can still run in that interval. Emscripten's GLUT surface has no
     * glutGetWindow and browser teardown does not use this native close path. */
#if !defined(__EMSCRIPTEN__)
    if (glutGetWindow() == 0)
        return;
#endif

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
    GlrCliOptions opts;
    int exit_code = 0;

    glr_init_trace("start");
    glr_audio_set_hitch_log_elapsed_fn(glr_init_trace_elapsed_seconds);
    glr_ctrl_set_init_log_elapsed_fn(glr_init_trace_elapsed_seconds);
    glr_ctrl_set_program_name(argv[0]);

    /* GLR_DETAILED_PROF (any non-empty value) is the env-var twin of
     * --detailed-prof; either one promotes the optional fine-grained
     * glr_init_trace_detail() phases (glutInit split, audio playlist
     * sub-steps, first-two-frames triple) to stderr. Default off so
     * a normal start emits only the baseline boot-phase set. */
    {
        const char *env = getenv("GLR_DETAILED_PROF");
        if (env && *env) glr_init_trace_set_detailed(1);
    }

    /* Parse the command line: glr_cli handles -h/--list-* itself and resolves
     * --examples-dir / --example / --tutorial / --tour up front (fail-fast
     * before a window opens). A 0 return means "exit now with exit_code"
     * (help/list/error). */
    if (!glr_cli_parse(argc, argv, &opts, &exit_code))
        return exit_code;
    if (opts.detailed_prof)
        glr_init_trace_set_detailed(1);
    /* --export-ply is consumed on the first display callback (needs a live GL
     * context), so hand it to the file-static seen by display_func. */
    g_export_ply_path = opts.export_ply_path;
    g_export_ply_srgb = opts.export_ply_srgb;

    /* Dump-only CLI paths (--dump-* / --flat-histogram) run GL-free and exit
     * before any window opens. */
    if (glr_boot_run_dumps(&opts, stdout))
        return 0;
    /* --export-c / --export-glr: same GL-free bootstrap, but they write the
     * session out as a scene file instead of dumping to stdout. Unlike
     * --export-ply (which needs a live context and runs from the display
     * callback), both source exporters are pure text over the loaded
     * program, so they never open a window. */
    if (glr_boot_run_exports(&opts, &exit_code))
        return exit_code;

    glr_init_trace("glutInit begin");
    glutInit(&argc, argv);
    glr_init_trace_detail("glutInit done");
    {
        /* AUTO still asks for the accum visual - whether the feature is
         * armed is decided post-init from the renderer string
         * (glr_ctrl_set_accum), which needs a live context and so cannot
         * run this early. Only an explicit --no-accum drops the request.
         *
         * But the request can be unsatisfiable: Mesa on modern Intel
         * advertises no FBConfig that carries accum planes alongside
         * multisample + stencil, and freeglut answers an impossible mode
         * by failing fgOpenWindow outright ("FBConfig with necessary
         * capabilities not found") - the app never opens a window at all.
         * So probe the mode first and, under AUTO, silently fall back to
         * the accum-less visual, which is exactly what AUTO would have
         * settled on anyway once it saw a Mesa renderer string. An
         * explicit --accum keeps the request and is left to fail loudly
         * rather than being quietly downgraded.
         *
         * The probe is native-only: Emscripten's JS GLUT abort()s on any
         * glutGet it does not implement, and GLUT_DISPLAY_MODE_POSSIBLE is
         * one of them - the whole app dies before callMain returns. It has
         * nothing to answer anyway, since its glutInitDisplayMode only reads
         * MULTISAMPLE/DEPTH/STENCIL/ALPHA out of the mode and there is no
         * accumulation buffer in WebGL to request. */
        unsigned int base = GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH |
                            GLUT_STENCIL | GLUT_MULTISAMPLE;
        unsigned int mode = base |
            (opts.use_accum != GLR_CLI_ACCUM_OFF ? GLUT_ACCUM : 0);
        glutInitDisplayMode(mode);
#if !defined(__EMSCRIPTEN__)
        if (opts.use_accum == GLR_CLI_ACCUM_AUTO &&
            !glutGet(GLUT_DISPLAY_MODE_POSSIBLE)) {
            glr_init_trace_detail(
                "no accum-capable visual; retrying without GLUT_ACCUM");
            mode = base;
            glutInitDisplayMode(mode);
        }
#endif
        glutInitWindowSize(opts.window_w, opts.window_h);
        glutCreateWindow(GLR_WINDOW_TITLE_BASE);
    }
    glr_init_trace("window created");

    glr_ctrl_init_gl();
    atexit(glr_shutdown);
    glr_init_trace("GL init done");
    glr_ctrl_bootstrap_repl(opts.input_file);
    if (opts.example_index >= 0)
        glr_scene_load_example(opts.example_index);
    /* --tutorial <name|idx>: replace the loaded document with the interactive
     * lesson once bootstrap has installed its host bridges. Do this before
     * capture-env posing so hooks such as GLR_EDIT_LINE address the tutorial
     * document, just as they address a selected example. */
    /* --watch: arm BEFORE the tutorial / tour below. Those replace the document
     * with a slot-less transient, so from that point on there is no active
     * scene to resolve a file from - arming afterwards would leave the watcher
     * with nothing bound for the whole session. Seeded with the positional
     * argument outright, which is the file `--watch` names; the binding
     * follows scene switches from there. */
    if (opts.watch) {
        glr_extedit_set_enabled(1);
        glr_extedit_bind_path(opts.input_file);
    }
    if (opts.tutorial_index >= 0) {
        splash_skip();
        glr_ctrl_start_tutorial(opts.tutorial_index);
    }
    /* Headless-capture env hooks applied at bootstrap (after the file/example
     * or tutorial load, before the main loop): initial-time override, pointer
     * script, splash skip, tick-per-frame resolve, edit-line / type-keys pose,
     * accum passes. Ordering inside is load-bearing - see glr_capture_env.c. */
    glr_capture_env_apply(opts.time_arg);
    /* --tour <name|idx>: start a built-in guided tour on launch. Deferred to
     * here so it runs after every capture/env resolve above - in particular
     * the GLR_TICK_PER_FRAME resolve, which keys off glr_pointer_script_active()
     * and must not see the tour's controlled script (a launched tour plays on
     * the real 60 Hz clock with live transport, like one started from the
     * Tours menu). Index was validated up front; the events fire against the
     * live layout once the main loop is drawing. Any real key/click cancels. */
    if (opts.tour_index >= 0) {
        splash_skip();       /* start clean - no splash band over the tour */
        if (opts.tour_stop)
            glr_ctrl_start_tour_at_checkpoint(opts.tour_index,
                                               opts.tour_stop);
        else
            glr_ctrl_start_tour(opts.tour_index);
    }
    glr_init_trace("REPL bootstrap done");
    glr_ctrl_set_accum(opts.use_accum);

    if (!opts.no_audio)
        glr_audio_bootstrap(opts.assets_override, glr_init_trace, glr_init_trace_detail);
    glr_actions_apply_defaults();

    glr_init_trace("entering main loop");
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
    if (g_window_closed)
        glr_ctrl_save_quit_recovery();
#endif
    return 0;
}
