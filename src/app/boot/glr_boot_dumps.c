/*
 * glr_boot_dumps.c - CLI dump-and-exit boot path (see glr_boot_dumps.h).
 *
 * Keeps the GL-free bootstrap + dump sequence out of main(): the same
 * @cfg/status services install as the windowed path, but nothing renders,
 * so status messages are silent.
 */
#include <stdio.h>

#include "app/boot/glr_boot_dumps.h"
#include "app/glr_debug.h"    /* glr_debug_dump_current_* (downward call) */
#include "app/glr_ctrl.h"     /* glr_ctrl_bootstrap_repl */
#include "app/glr_actions.h"  /* glr_scene_load_example */

int glr_boot_run_dumps(const GlrCliOptions *opts, FILE *out) {
    FILE *dst = out ? out : stdout;

    if (!(opts->dump_code || opts->dump_flat || opts->dump_flat_histogram ||
          opts->dump_state_layout))
        return 0;

    /* Dump-only paths skip glr_ctrl_init_gl (no GL context, no window).
     * glr_ctrl_bootstrap_repl installs the app services (status sink +
     * export-config bridge) at its top so @cfg in imported files is applied
     * even on the dump path. */
    if (opts->dump_code || opts->dump_flat || opts->dump_flat_histogram) {
        glr_ctrl_bootstrap_repl(opts->input_file);
        /* --example works on the dump paths too: the loader chain (reset
         * transients, undo note, repl_load_example) is GL-free, so built-ins
         * can be inspected without a window. */
        if (opts->example_index >= 0)
            glr_scene_load_example(opts->example_index);
    }
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
