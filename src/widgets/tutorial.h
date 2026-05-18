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

#include "widgets/tutorial_state.h"

/* Lifecycle and commit-path integration. */
void                 tutorial_start(int idx);
void                 tutorial_exit(void);
int                  tutorial_handle_commit_attempt(const char *input,
                                                    TutorialMatchResult *out);
void                 tutorial_advance_after_successful_commit(void);
const char          *tutorial_current_expected_text(void);

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
