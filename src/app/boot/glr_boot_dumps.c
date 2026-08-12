/*
 * glr_boot_dumps.c - CLI dump-and-exit boot path (see glr_boot_dumps.h).
 *
 * Keeps the GL-free bootstrap + dump sequence out of main(): the same
 * @cfg/status services install as the windowed path, but nothing renders,
 * so status messages are silent.
 */
#include <stdio.h>
#include <stdlib.h>

#include "app/boot/glr_boot_dumps.h"
#include "app/glr_debug.h"    /* glr_debug_dump_current_* (downward call) */
#include "app/glr_ctrl.h"     /* glr_ctrl_bootstrap_repl / glr_ctrl_set_time */
#include "app/glr_ctrl_export.h" /* glr_ctrl_fill_export_layout */
#include "app/glr_actions.h"  /* glr_scene_load_example */
#include "repl/export.h"      /* repl_export_save_output / _save_glr */
#include "ui/app/state.h"     /* ui_state_viewport_set_size */

/* Load the session the way the windowed path does, minus GL.
 *
 * Dump / export paths skip glr_ctrl_init_gl (no GL context, no window).
 * glr_ctrl_bootstrap_repl installs the app services (status sink +
 * export-config bridge) at its top so @cfg in imported files is applied
 * here too. --example works the same way: the loader chain (reset
 * transients, undo note, repl_load_example) is GL-free, so built-ins can be
 * inspected - or exported - without a window.
 *
 * --time / GLR_TIME are applied here rather than only in
 * glr_capture_env_apply: the dump/export path never reaches that hook, and
 * example/file load resets t to 0, so the override has to land after the
 * load. Same precedence as capture_env (--time wins over GLR_TIME). */
static void glr_boot_load_session(const GlrCliOptions *opts) {
    const char *t_src;

    glr_ctrl_bootstrap_repl(opts->input_file);
    if (opts->example_index >= 0)
        glr_scene_load_example(opts->example_index);

    t_src = opts->time_arg ? opts->time_arg : getenv("GLR_TIME");
    if (t_src && *t_src)
        glr_ctrl_set_time((float)atof(t_src));
}

int glr_boot_run_dumps(const GlrCliOptions *opts, FILE *out) {
    FILE *dst = out ? out : stdout;

    if (!(opts->dump_code || opts->dump_flat || opts->dump_flat_histogram ||
          opts->dump_state_layout))
        return 0;

    if (opts->dump_code || opts->dump_flat || opts->dump_flat_histogram)
        glr_boot_load_session(opts);
    if (opts->dump_code)
        glr_debug_dump_current_editor(dst);
    if (opts->dump_flat)
        glr_debug_dump_current_flat_commands_sync(dst);
    if (opts->dump_flat_histogram)
        glr_debug_dump_current_flat_histogram(dst);
    if (opts->dump_state_layout)
        glr_debug_dump_runtime_state_layout(dst);
    return 1;
}

/* Report one export's outcome. Status messages go to the (headless, silent)
 * UI sink, so the result has to reach the terminal from here. */
static int glr_boot_report_export(const char *tag, const char *path, int ok) {
    if (ok)
        fprintf(stderr, "[%s] wrote %s\n", tag, path);
    else
        fprintf(stderr, "[%s] failed to write %s\n", tag, path);
    return ok;
}

int glr_boot_run_exports(const GlrCliOptions *opts, int *exit_code) {
    int ok = 1;

    if (!opts->export_c_path && !opts->export_glr_path)
        return 0;

    glr_boot_load_session(opts);

    if (opts->export_c_path) {
        /* No reshape ever ran here, so the UI viewport is still zero-sized
         * and the exported window would fall back to the 800x600 default.
         * Seed it with the requested --window size so the scene rect - and
         * with it the exported program's window - keeps the aspect ratio a
         * windowed save would have written. */
        ReplExportLayout layout;
        ui_state_viewport_set_size(opts->window_w, opts->window_h);
        glr_ctrl_fill_export_layout(&layout);
        ok &= glr_boot_report_export(
            "export-c", opts->export_c_path,
            repl_export_save_output(opts->export_c_path,
                                    source_document_view(), &layout));
    }
    /* No layout for the .glr side: the authoring format carries no
     * viewport-dependent scaffold. */
    if (opts->export_glr_path)
        ok &= glr_boot_report_export(
            "export-glr", opts->export_glr_path,
            repl_export_save_glr(opts->export_glr_path,
                                 source_document_view()));

    if (exit_code) *exit_code = ok ? 0 : 1;
    return 1;
}
