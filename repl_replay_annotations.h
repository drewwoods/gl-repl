/*
 * repl_replay_annotations.h - Replay-time source annotations and variable substitution.
 *
 * During replay, the code panel displays annotated versions of source lines showing
 * variable values substituted into expressions and the results of expression
 * evaluation. This module builds those annotations for display.
 *
 * Annotation types:
 *   1. Variable substitution: "x = 5 + y" displays as "x = 5 + 3" (y's current value
 *      substituted). Shows variable bindings at the time of execution.
 *   2. Expression evaluation: "sin(t * 0.5)" displays as "sin(t * 0.5) = 0.841" (the
 *      computed result). Helps users understand what each expression evaluates to.
 *   3. Extra rows: Some annotations (especially substitutions) expand to extra rows
 *      below the source line, keeping the code panel readable while showing full
 *      variable context.
 *
 * Preparation: repl_replay_annotations_prepare() is called once per frame during
 * replay to pre-compute which flat commands are executing at the current replay PC.
 * This allows O(1) lookups from source line index to corresponding flat command.
 *
 * Mapping: One source line may expand into multiple flat commands (if it contains
 * for-loops or function calls). repl_replay_annotation_flat_cmd_for_source() finds
 * the flat command index for a given source line. repl_replay_annotation_extra_rows_for_line()
 * counts how many extra annotation rows (if any) are needed below a source line.
 *
 * Display building: repl_replay_build_subst_annotation() and
 * repl_replay_build_eval_annotation() construct the annotated text for a command,
 * substituting variable values and computing expression results using the current
 * predefined variable state at execution time.
 *
 * Code panel display: repl_replay_code_panel_get_command_display_text() returns the
 * full display text for a source line, combining the source and any annotations,
 * used by the code-panel renderer during replay mode.
 */
#ifndef REPL_REPLAY_ANNOTATIONS_H
#define REPL_REPLAY_ANNOTATIONS_H

/* Prepare annotation lookups for the current frame. Scans the replay state and
 * pre-computes the mapping from source line indices to flat command indices at
 * the current replay PC. Called once per frame during active replay. */
void repl_replay_annotations_prepare(void);

/* Find the flat command index corresponding to a source line during replay. If the
 * source line expands into multiple flat commands (e.g., a for-loop), returns the
 * most recent matching flat command at or before the current replay PC. Returns -1
 * if no flat command is executing for this source line. Used by annotation builders
 * to look up runtime variable state. */
int  repl_replay_annotation_flat_cmd_for_source(int src_line);

/* Count extra annotation rows needed below a source line (used by the code-panel
 * renderer to adjust row heights). Returns 0 for lines with no annotations, 1+ for
 * lines with substitutions or evaluations that expand to extra rows. Used to
 * accurately compute code-panel layout during replay. */
int  repl_replay_annotation_extra_rows_for_line(int cmd_idx);

/* Build a variable substitution annotation. Scans the source command at cmd_idx,
 * finds variable references, substitutes their current values from the flat command's
 * local variable scope, and writes the substituted text to subst buffer. Also builds
 * a comment line (var_comment buffer) listing the variable bindings. Returns 1 if
 * an annotation was produced, 0 otherwise. Used by the code-panel renderer to show
 * variable context during replay. */
int  repl_replay_build_subst_annotation(int cmd_idx, int flat_idx,
                                        char *subst, int subst_size,
                                        char *var_comment, int comment_size);

/* Build an expression evaluation annotation. Re-evaluates the expression at cmd_idx
 * using the current predefined variable state from the flat command, and writes the
 * result to eval_buf (e.g., "sin(t * 0.5) = 0.841"). Returns 1 if an annotation was
 * produced, 0 otherwise (e.g., for non-expression commands like glBegin). Used by
 * the code-panel renderer to show computed results during replay. */
int  repl_replay_build_eval_annotation(int cmd_idx, int flat_idx,
                                       char *eval_buf, int eval_size);

/* Get the full display text for a source line during replay, combining source code
 * and all annotations. Writes to out buffer (up to out_size bytes). Used by the
 * code-panel renderer to display annotated commands. */
int  repl_replay_code_panel_get_command_display_text(int cmd_idx, char *out, int out_size);

#endif /* REPL_REPLAY_ANNOTATIONS_H */
