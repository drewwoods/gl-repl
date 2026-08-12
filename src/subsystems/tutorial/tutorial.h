/*
 * tutorial.h - Tutorial runner, commit guards, and match helpers.
 *
 * Drives the built-in guided tutorials: start/stop lifecycle, per-step expected
 * command matching, locked-line protection, fade bookkeeping, and autocomplete-
 * style shadow text for the current step. The runner mutates
 * `TutorialRuntimeState` and coordinates with the load/commit path so tutorial
 * instruction comments and expected user edits stay aligned.
 */
#ifndef TUTORIAL_H
#define TUTORIAL_H

#include <stddef.h>  /* size_t */

#include "repl/tutorials.h"           /* TutorialEntry */
#include "subsystems/tutorial/tutorial_state.h"

/* Tunable constants for the per-character fade-in animation that
 * reveals tutorial instruction comments. Both are #ifndef-guarded so
 * config.h (or a -D flag) can override the defaults at build time
 * without touching this header. */

/* Rate at which the reveal animation writes characters, in chars per
 * second. Per-line total duration is computed at emit time as
 * (line_len + TUTORIAL_FADE_SETTLE_CHARS) / TUTORIAL_FADE_CHARS_PER_SEC,
 * so short instructions reveal quickly and long ones take longer at
 * the same readable pace - instead of every line racing through a
 * fixed wall-clock budget. */
#ifndef TUTORIAL_FADE_CHARS_PER_SEC
#define TUTORIAL_FADE_CHARS_PER_SEC 50.0f
#endif

/* Width of the white -> base-color "settle" wave that trails the fade-in
 * head. Each character becomes bright white the instant it is fully
 * revealed and eases back to the line's base color over this many
 * character-slots of time. Used by the renderer to size the per-char
 * gradient segments and by tutorial.c's timing math; kept comfortably
 * below UI_TEXT_PANEL_MAX_COLOR_SEGMENTS so the gradient always fits
 * in a row's segment budget. */
#ifndef TUTORIAL_FADE_SETTLE_CHARS
#define TUTORIAL_FADE_SETTLE_CHARS 6
#endif

/* Absolute-value tolerance used when a REQUIRE_VAR step compares a
 * predefined variable's live float value to its `var_target`. Tight
 * enough that the linear-drag granularity (1 px = 0.1 units) won't
 * cross the boundary without intent, but loose enough that float
 * round-trip from a typed `name = 5;` commit always matches. */
#ifndef TUTORIAL_VAR_EPS
#define TUTORIAL_VAR_EPS 1e-4f
#endif

/* Runtime validator: walk every entry-level `@cfg` line and every
 * SET / REQUIRE step in `entry`, and reject the tutorial if the
 * controller-installed config bridge doesn't recognise a slug, or if
 * a symbolic value name (cfg_value_name on a step, or the right-hand
 * side of an entry-level `@cfg` line) can't be resolved to an int.
 *
 * Called by tutorial_start before any state mutation; exposed here so
 * tests can exercise the rules against synthetic out-of-catalog
 * entries. Returns 1 on success; on failure returns 0 and writes a
 * diagnostic into `err`. */
int                  tutorial_validate_entry_against_bridge(const TutorialEntry *entry,
                                                            char *err, int err_size);

/* Lifecycle and commit-path integration. */
void                 tutorial_start(int idx);
void                 tutorial_stop(void);
/* Restore the cfg baseline captured at tutorial_start and reset the runtime
 * state. Called by tutorial_stop, tutorial completion, AND every external
 * teardown path that called tutorial_state_reset() directly
 * (workspace/scene/example load, glr_ctrl_reset_all) so the workspace-load
 * stash never enshrines tutorial-mutated cfg as the new baseline.
 * Idempotent - no-op when no tutorial is active. */
void                 tutorial_teardown(void);
int                  tutorial_handle_commit_attempt(const char *input,
                                                    TutorialMatchResult *out);
/* Called by the completion provider on every input-change update while
 * the cursor sits on the expected commit line: when `input` is a
 * complete match for the active COMMAND step's expected text (Tab
 * accepted the ghost or the user finished typing), refresh the status
 * bar with a "press Enter or ';' to commit" reminder. No-op for
 * inactive, SET / REQUIRE, or partial input - those let the prior
 * status fade naturally. */
void                 tutorial_refresh_input_hint(const char *input);

/* Per-frame controller hook: compute the COMMAND-step status hint that
 * should be visible right now. Returns 1 with the hint in `out` for
 * active COMMAND steps (commit-reminder variant when the input fully
 * matches the expected command on the expected commit line; otherwise
 * the "type or Tab" entry variant). Returns 0 with `out` cleared for
 * inactive, SET, or REQUIRE. Paired with tutorial_status_is_hint to
 * let the controller re-emit the hint each frame without trampling
 * non-tutorial status messages - see glr_ctrl_tick. */
int                  tutorial_status_hint(char *out, size_t out_size);

/* Returns 1 when `text` is one of the COMMAND-step status hints
 * tutorial_status_hint emits (any "Tutorial: step ..." prefix), letting
 * the controller distinguish "this status is mine - refresh it" from
 * "another subsystem owns the slot - let its TTL run out." */
int                  tutorial_status_is_hint(const char *text);

void                 tutorial_advance_after_successful_commit(void);
const char          *tutorial_current_expected_text(void);

/* Kind of the current step (TUTORIAL_STEP_KIND_COMMAND when inactive - a
 * safe default that lets callers branch on "is the document writable?"
 * without first checking active). */
TutorialStepKind     tutorial_current_step_kind(void);

/* REQUIRE-step hook: every cfg write should notify so the runner can
 * advance when the watched slug reaches its target value. Slug-scoped
 * and inactive-checked, so calling it after every glr_config_set is
 * cheap and safe. */
void                 tutorial_notify_state_changed(void);

/* Showcase-step ack: if the current step is SET or NOTE and `key` is
 * Enter / Tab / Space, advance to the next step and return 1
 * (consumed); else return 0 (not consumed - caller continues its
 * dispatch chain). */
int                  tutorial_handle_ack_key(unsigned char key);

/* Editor precheck helper: when the current step is SET, NOTE, or
 * REQUIRE, set a kind-appropriate status hint ("Press Enter / Tab /
 * Space ..." for SET/NOTE; "Set <slug> = <value> ..." for REQUIRE) and
 * return 1 to tell the editor to reject the commit. Returns 0 for
 * COMMAND / inactive (let the normal commit path run). Lives in
 * tutorial.c so input.c gains zero new direct repl_* calls -
 * check-editor-repl-surface stays at its baseline of 21 unique
 * repl_* symbols. */
int                  tutorial_reject_noncommand_commit_with_hint(void);

/* Pure predicate twin of the above: 1 when the current step is waiting on
 * something other than a typed command (SET / NOTE / REQUIRE), 0 for
 * COMMAND / REQUIRE_VAR / inactive. Sets no status.
 *
 * Render and snapshot paths must use THIS one. The _with_hint variant is an
 * editor precheck that writes a status message as a side effect, so calling
 * it once per frame overwrites whatever the current step put there. A
 * label-placed NOTE is what exposed the difference: it is the one park that
 * leaves a row loaded in the input buffer with insert mode off, which is
 * what those guards test for before they fire. */
int                  tutorial_step_rejects_commit(void);

/* The source row the next user commit should land on. -1 when no
 * step is currently waiting for a user commit. */
int                  tutorial_expected_commit_line(void);

/* Stamp a pending-commit record after the precheck matcher passes
 * but before the editor's commit dispatch runs. Pairs 1:1 with
 * exactly one of tutorial_note_expected_commit_applied (on
 * COMMIT_OK) or tutorial_cancel_pending (every other outcome).
 * `step_idx` should be the current step at the moment of begin. */
void                 tutorial_begin_expected_commit_attempt(void);

/* Bookkeeping after a matched expected commit succeeded: shift any
 * existing tracked tutorial lines at-or-after pending.commit_line by
 * the row delta the commit produced (the catalog rule constrains
 * delta to 1, but the math stays general), then record the source
 * row for the just-committed step so a later label-targeted step
 * can resolve a target_label pointing at it. Clears the pending
 * record.
 *
 * Returns 1 if a pending COMMAND expected-command attempt was in
 * flight (i.e. this commit was the matched COMMAND commit), 0 if not
 * (a free-form REQUIRE_VAR commit, which sets no pending record). The
 * commit-side advance is gated on this: a REQUIRE_VAR commit advances
 * via the predef-writeback notify hook, never the commit, so a 0 here
 * keeps the commit path from double-advancing past the notify. */
int                  tutorial_note_expected_commit_applied(void);

/* Idempotent: no-op when no pending record is in flight. Call from
 * any commit-rejection path that bypasses
 * tutorial_note_expected_commit_applied. */
void                 tutorial_cancel_pending(void);

/* Lock/source guards used by the editor/commit path. The pure fade math
 * over TutorialFadeView lives in tutorial_animation.{c,h}. */
int                  tutorial_line_is_locked(int line_idx);
int                  tutorial_guard_source_change(int pos, int delete_count,
                                                  int insert_count);
TutorialMatchResult  tutorial_match(const char *expected, const char *got);

/* Autocomplete-style helper for showing the untyped suffix of the current
 * expected command. */
/* Compute the shadow-text suffix for the current step: the portion of the
 * expected command the user has not yet typed. Returns 1 and writes the
 * suffix into `out` when the tutorial is active and `input` is a strict
 * prefix of the expected text (empty input counts as the empty prefix and
 * yields the full expected text). Returns 0 and clears `out[0]` otherwise.
 * Used by the autocomplete provider to populate ghost text so the user
 * passively sees what they need to type. */
int                  tutorial_shadow_suffix(const char *input,
                                            char *out, size_t out_size);

#endif /* TUTORIAL_H */
