/*
 * tutorial.h - Tutorial runner, commit guards, and match helpers.
 *
 * Drives the built-in guided tutorials: start/exit lifecycle, per-step expected
 * command matching, locked-line protection, fade bookkeeping, and autocomplete-
 * style shadow text for the current step. The runner mutates
 * `TutorialRuntimeState` and coordinates with the load/commit path so tutorial
 * instruction comments and expected user edits stay aligned.
 */
#ifndef TUTORIAL_H
#define TUTORIAL_H

#include <stddef.h>  /* size_t */

#include "subsystems/tutorial/tutorial_state.h"

/* Tunable constants for the per-character fade-in animation that
 * reveals tutorial instruction comments. Both are #ifndef-guarded so
 * config.h (or a -D flag) can override the defaults at build time
 * without touching this header. */

/* Rate at which the reveal animation writes characters, in chars per
 * second. Per-line total duration is computed at emit time as
 * (line_len + TUTORIAL_FADE_SETTLE_CHARS) / TUTORIAL_FADE_CHARS_PER_SEC,
 * so short instructions reveal quickly and long ones take longer at
 * the same readable pace — instead of every line racing through a
 * fixed wall-clock budget (previously TUTORIAL_FADE_DURATION_SECS,
 * which made 80-char lines appear ~4x faster than 20-char ones). */
#ifndef TUTORIAL_FADE_CHARS_PER_SEC
#define TUTORIAL_FADE_CHARS_PER_SEC 50.0f
#endif

/* Width of the white→base-color "settle" wave that trails the fade-in
 * head. Each character becomes bright white the instant it is fully
 * revealed and eases back to the line's base color over this many
 * character-slots of time. Used by the renderer to size the per-char
 * gradient segments and by tutorial.c's timing math; kept comfortably
 * below UI_TEXT_PANEL_MAX_COLOR_SEGMENTS so the gradient always fits
 * in a row's segment budget. */
#ifndef TUTORIAL_FADE_SETTLE_CHARS
#define TUTORIAL_FADE_SETTLE_CHARS 6
#endif

/* Lifecycle and commit-path integration. */
void                 tutorial_start(int idx);
void                 tutorial_exit(void);
/* Restore the cfg baseline captured at tutorial_start and reset the runtime
 * state. Called by tutorial_exit, tutorial completion, AND every external
 * teardown path that previously called tutorial_state_reset() directly
 * (workspace/scene/example load, glr_app_reset_all) so the workspace-load
 * stash never enshrines tutorial-mutated cfg as the new baseline.
 * Idempotent — no-op when no tutorial is active. */
void                 tutorial_teardown(void);
int                  tutorial_handle_commit_attempt(const char *input,
                                                    TutorialMatchResult *out);
void                 tutorial_advance_after_successful_commit(void);
const char          *tutorial_current_expected_text(void);

/* Kind of the current step (TUTORIAL_STEP_KIND_COMMAND when inactive — a
 * safe default that lets callers branch on "is the document writable?"
 * without first checking active). */
TutorialStepKind     tutorial_current_step_kind(void);

/* REQUIRE-step hook: every cfg write should notify so the runner can
 * advance when the watched slug reaches its target value. Slug-scoped
 * and inactive-checked, so calling it after every glr_config_set is
 * cheap and safe. */
void                 tutorial_notify_state_changed(void);

/* SET-step ack: if the current step is SET and `key` is Enter / Tab /
 * Space, advance to the next step and return 1 (consumed); else return
 * 0 (not consumed — caller continues its dispatch chain). */
int                  tutorial_handle_ack_key(unsigned char key);

/* Editor precheck helper: when the current step is SET or REQUIRE,
 * set a kind-appropriate status hint ("Press Enter / Tab / Space …" for
 * SET; "Set <slug> = <value> …" for REQUIRE) and return 1 to tell the
 * editor to reject the commit. Returns 0 for COMMAND / inactive (let the
 * normal commit path run). Lives in tutorial.c so input.c gains zero
 * new direct repl_* calls — check-editor-repl-surface stays at its
 * baseline of 21 unique repl_* symbols. */
int                  tutorial_block_noncommand_commit(void);

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
 * the row delta the commit produced (v1 catalog rule constrains
 * delta to 1, but the math stays general), then record the source
 * row for the just-committed step so a later label-targeted step
 * can resolve a target_label pointing at it. Clears the pending
 * record. */
void                 tutorial_note_expected_commit_applied(void);

/* Idempotent: no-op when no pending record is in flight. Call from
 * any commit-rejection path that bypasses
 * tutorial_note_expected_commit_applied. */
void                 tutorial_cancel_pending(void);

/* Per-line fade/lock queries used by render and edit guards. */
int                  tutorial_step_fade_front(int line_idx, int line_len,
                                              float now);
float                tutorial_step_fade_alpha(int line_idx, int char_idx,
                                              int line_len, float now);
/* Returns 0..1 describing how far a character has eased from its
 * just-revealed bright-white highlight back to the line's base
 * (comment) color. 0 = freshly revealed (full white); 1 = fully
 * settled (base color). Returns 1 when the line is not currently
 * animating. */
float                tutorial_step_fade_settle(int line_idx, int char_idx,
                                               int line_len, float now);
int                  tutorial_line_is_fading(int line_idx, float now);
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
