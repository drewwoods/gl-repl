/*
 * repl_replay_annotations.h - Replay-time source annotations and variable substitution.
 *
 * During replay, the code panel shows annotated versions of source lines:
 * variable values substituted into expressions and the result of evaluating
 * the line's expressions. The annotation rows live in the editor virtual-line
 * snapshot pushed each frame; this module builds that list and exposes the
 * minimum surface UI / layout actually consume.
 *
 * Public surface:
 *   - repl_replay_annotations_prepare() is called once per frame from the
 *     controller (and defensively from layout for legacy code paths). It
 *     refreshes the per-PC mapping cache and refills
 *     editor_state_virtual_lines() so layout / render can read a
 *     single source of truth.
 *   - repl_replay_annotation_extra_rows_for_line() counts virtual rows
 *     pushed below a source line so layout can compute row heights.
 *   - repl_replay_code_panel_get_command_display_text() returns the
 *     in-line annotated source text for a command (no extra rows).
 *
 * The substitution / evaluation builders and the per-source flat-cmd
 * lookup are file-private; they are only invoked from
 * repl_replay_annotations_refresh_virtual_lines() and the on-line
 * display-text helper.
 */
#ifndef REPL_REPLAY_ANNOTATIONS_H
#define REPL_REPLAY_ANNOTATIONS_H

#include "editor_state.h"  /* EditorBufferView */

/* Prepare annotation lookups for the current frame. Refreshes the per-PC
 * cache and refills editor_state_virtual_lines(). Idempotent within
 * a frame; safe to call from both the controller and the layout build.
 *
 * `text` is the editor buffer view the controller built for this
 * frame. The module caches the view internally so static helpers can
 * read source text without reaching back into editor globals. */
void repl_replay_annotations_prepare(EditorBufferView text);

/* Count virtual annotation rows attached to a source line. Reads the
 * controller-pushed editor_virtual_lines list, so this stays in sync with
 * what render draws. */
int  repl_replay_annotation_extra_rows_for_line(int cmd_idx);

/* Get the inline annotated display text for a source line during replay
 * (the source body, without the extra annotation rows). Writes up to
 * out_size bytes into out. `text` is the editor buffer view the caller
 * built for this frame. */
int  repl_replay_code_panel_get_command_display_text(EditorBufferView text,
                                                     int cmd_idx,
                                                     char *out, int out_size);

#endif /* REPL_REPLAY_ANNOTATIONS_H */
