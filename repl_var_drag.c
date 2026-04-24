/*
 * repl_var_drag.c -- Variable slider drag state and value writeback.
 *
 * Storage for g_drag_var / g_drag_log_mode / g_drag_start_val /
 * g_drag_start_x lives here.  The externs are still listed in
 * repl_state.h and registered in repl_state.c's ReplUiState catalog,
 * so the existing state-access contract is unchanged.
 *
 * Linear drag: 1 pixel = 0.05 units.
 * Log drag:    200 pixels = one decade (x10 / ÷10), sign preserved.
 *              Near-zero start value falls back to a linear bootstrap
 *              so the slider can walk off zero.
 */
#include "sample.h"
#include "repl_var_drag.h"

int   g_drag_var = -1;
int   g_drag_log_mode = 0;  /* 0=linear (LMB drag), 1=logarithmic (RMB drag) */
float g_drag_start_val = 0.0f;
int   g_drag_start_x = 0;

int repl_var_drag_active(void)     { return g_drag_var >= 0; }
int repl_var_drag_active_var(void) { return g_drag_var; }
int repl_var_drag_log_mode(void)   { return g_drag_log_mode; }

void repl_var_drag_begin(int row, int log_mode, int x) {
    if (row < 0 || row >= g_num_predef_vars) return;
    g_drag_var       = row;
    g_drag_log_mode  = log_mode ? 1 : 0;
    g_drag_start_val = g_predef_vars[row].value;
    g_drag_start_x   = x;
}

void repl_var_drag_reset(void) {
    g_drag_var      = -1;
    g_drag_log_mode = 0;
}

void repl_var_drag_motion(int x) {
    if (g_drag_var < 0) return;

    float new_val;
    if (g_drag_log_mode) {
        /* Logarithmic drag: ×10 / ÷10 per 200 pixels.
         * Preserves sign; near-zero start falls back to linear bootstrap. */
        float dx_total = (float)(x - g_drag_start_x);
        float mag = fabsf(g_drag_start_val);
        if (mag < 1e-6f) {
            /* Bootstrap from zero: treat first pixels as linear, then log. */
            new_val = dx_total * 0.001f;
        } else {
            float sign = (g_drag_start_val >= 0.0f) ? 1.0f : -1.0f;
            new_val = sign * mag * expf(dx_total * (logf(10.0f) / 200.0f));
        }
    } else {
        float delta = (float)(x - g_drag_start_x) * 0.05f;
        new_val = g_drag_start_val + delta;
    }

    g_predef_vars[g_drag_var].value = new_val;

    /* Sync any literal CMD_VAR_ASSIGN for this var so the source line
     * on screen matches the new value (constant assignments only —
     * expressions that reference other vars are left alone). */
    const char *vname = g_predef_vars[g_drag_var].name;
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds_mut()[i].valid && repl_state_document_cmds_mut()[i].type == CMD_VAR_ASSIGN &&
            repl_state_document_cmds_mut()[i].num_args == g_drag_var &&
            !repl_state_document_cmds_mut()[i].has_vars) {
            repl_state_document_cmds_mut()[i].args[0] = new_val;
            snprintf(repl_state_document_cmds_mut()[i].source, sizeof(repl_state_document_cmds_mut()[i].source),
                     "  %s = %g;", vname, (double)new_val);
        }
    }
    repl_state_mark_flat_dirty();
}
