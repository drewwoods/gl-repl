/*
 * tests/export_trace_driver.c - test-only entry point that #includes a
 * freshly-exported REPL program and runs only its user-geometry body.
 *
 * Why #include and not link? The exporter emits the user-geometry as
 * static helpers (draw_scene / reset_repl_vars) inside the
 * exported translation unit. Including the file pulls those statics
 * into this TU so the driver can call them directly, skipping the
 * camera transform, outline-pass, vertex-point-pass, and lighting
 * setup that display() would otherwise emit between init() and the
 * user code - none of which the REPL executor replays.
 *
 * Compile invocation supplies:
 *   -Dmain=app_main                      so the exported file's GLUT
 *                                        main is renamed (it's still
 *                                        defined, just unreferenced).
 *   -DEXPORTED_C='"/tmp/...c"'           path to the freshly-exported
 *                                        REPL program.
 *
 * After running the body the driver writes "<symbol> <count>\n" lines
 * for every non-zero stub counter to argv[1], which the parent test
 * (test_export_trace_parity) reads back and compares against the
 * counts it captured around repl_execute_program().
 *
 * When requested, the driver also writes a per-call trace through the
 * gl_stub_trace_fp hook. The parent test uses that trace to localize a
 * count mismatch after comparing the scalar counters.
 */
#include <GL/gl_stub_counts.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef EXPORTED_C
#  error "compile with -DEXPORTED_C='\"/path/to/exported.c\"'"
#endif

/* The exporter emits an int main(...) that runs glutInit + glutMainLoop;
 * rename it just for the duration of the #include so the driver's own
 * main() (below) is the binary's real entry point. */
#define main app_main
#include EXPORTED_C
#undef main

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <counts-file> [<trace-file> [<t>]]\n",
                argc > 0 ? argv[0] : "export_trace_driver");
        return 2;
    }

    /* Reset counters AFTER the include so any file-scope initializers
     * the exporter emits (none today, but cheap insurance) don't leak
     * into the trace. */
    gl_stub_counts_reset();

    /* Optional per-call trace. The parent test passes a per-leg path
     * here so it can diff the resulting file against the REPL leg's
     * trace whenever counts disagree. */
    if (argc >= 3) gl_stub_trace_open(argv[2]);

    /* draw_scene is static-in-translation-unit by the exporter;
     * the #include above makes it visible. We deliberately skip the
     * companion reset_repl_vars() - it's only emitted when the program
     * actually uses a predefined REPL var, so calling it would be a
     * compile error for predef-free programs. File-scope `static float
     * t = 0.0f;` etc. emitted by the exporter already gives the same
     * starting state the REPL side has after glr_ctrl_reset_all().
     *
     * argv[3..] are t values, one frame each, drawn in sequence within
     * this one process. Successive frames rather than repeated first
     * frames is the whole point: the REPL leg re-uses its flat program
     * across frames and only re-bakes the values, so a stale bake shows
     * up on frame 2 and never on frame 1. Sequencing them here also keeps
     * predef-var mutation carrying forward on both legs alike. `t` is
     * always emitted by the exporter (t is always predefined), so this
     * assignment always compiles. */
    if (argc >= 4) {
        for (int frame = 0; frame + 3 < argc; frame++) {
            t = (float)atof(argv[frame + 3]);
            gl_stub_trace_mark(frame, (double)t);
            draw_scene();
        }
    } else {
        gl_stub_trace_mark(0, (double)t);
        draw_scene();
    }
    gl_stub_trace_close();

    FILE *f = fopen(argv[1], "w");
    if (!f) {
        fprintf(stderr, "open %s: failed\n", argv[1]);
        return 3;
    }
    for (int i = 0; i < GL_STUB_COUNT_MAX; i++) {
        if (gl_stub_counts[i] != 0)
            fprintf(f, "%s %llu\n",
                    gl_stub_count_name(i), gl_stub_counts[i]);
    }
    fclose(f);
    return 0;
}
