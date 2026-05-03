/*
 * repl_var_drag.h - Variable slider drag transaction management.
 *
 * Handles the interactive drag transaction for adjusting predefined variable
 * values via the floating variable slider panel. Owns the drag state (which
 * variable is being dragged, current mode, starting position/value) and
 * implements the motion-to-value mapping for linear and logarithmic drag modes.
 *
 * Drag lifecycle: repl_var_drag_begin() initializes a drag on a variable row
 * with a specified mode (linear or log) and starting mouse x-coordinate. The
 * starting value is captured from the current predefined variable value.
 * repl_var_drag_motion() updates the variable as the mouse moves, computing
 * new values via linear mapping (motion_dx -> delta_value) or log mapping
 * (exponential scaling for large value ranges). repl_var_drag_reset() clears
 * the drag state and releases the variable.
 *
 * State writeback: As motion computes new values, they are written back to
 * the live predefined-variable table immediately, so the geometry updates in
 * real time. If the source contains a literal CMD_VAR_ASSIGN line for the
 * dragged variable, that command text is rewritten in place to match the new
 * value. Expression-based assignments are left unchanged.
 *
 * Visual feedback: The variable panel renderer queries repl_var_drag_active_var()
 * and repl_var_drag_log_mode() to highlight the active drag row and adjust
 * visual styling (e.g., color/opacity) to indicate which variable is being
 * manipulated.
 */
#ifndef VARIABLE_PANEL_DRAG_H
#define VARIABLE_PANEL_DRAG_H

/* Query drag state. repl_var_drag_active() returns 1 if a drag is currently
 * in progress, 0 otherwise. repl_var_drag_active_var() returns the index of
 * the variable being dragged (into g_predef_vars[]), or -1 if no drag is
 * active. repl_var_drag_log_mode() returns 1 if the active drag is in
 * logarithmic mode (exponential scaling), 0 for linear mode. Used by the
 * variable panel renderer to highlight the active row. */
int  repl_var_drag_active(void);
int  repl_var_drag_active_var(void);
int  repl_var_drag_log_mode(void);

/* Begin a drag transaction on a variable. row is the index into g_predef_vars[];
 * log_mode is 1 for exponential scaling (useful for large value ranges like
 * rotation angles) or 0 for linear mapping. x is the initial mouse x-coordinate,
 * used as the baseline for motion deltas. The current value of g_predef_vars[row]
 * is captured as the starting value. Called by the editor's mouse handler when
 * the user presses on a variable row in the slider panel. */
void repl_var_drag_begin(int row, int log_mode, int x);

/* Update the drag with new mouse x-coordinate. Computes the motion delta from
 * the start x-coordinate, maps it to a new variable value (linear or log),
 * and writes the result back to the live predefined-variable table. Also
 * rewrites any matching literal CMD_VAR_ASSIGN source line to keep the
 * displayed code in sync with the dragged value. Called repeatedly as the
 * mouse moves during a drag. */
void repl_var_drag_motion(int x);

/* End a drag transaction and clear the drag state. Called by the editor's
 * mouse handler when the user releases the mouse button. Clears
 * repl_var_drag_active_var() back to -1. */
void repl_var_drag_reset(void);

#endif /* VARIABLE_PANEL_DRAG_H */
