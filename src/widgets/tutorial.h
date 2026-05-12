/*
 * tutorial.h - Tutorial runner and match helpers.
 */
#ifndef TUTORIAL_H
#define TUTORIAL_H

#include <stddef.h>  /* size_t */

#include "widgets/tutorial_state.h"

void                 tutorial_start(int idx);
void                 tutorial_exit(void);
int                  tutorial_handle_commit_attempt(const char *input,
                                                    TutorialMatchResult *out);
void                 tutorial_advance_after_successful_commit(void);
const char          *tutorial_current_expected_text(void);
float                tutorial_step_fade_alpha(int line_idx, int char_idx,
                                              int line_len, float now);
int                  tutorial_line_is_fading(int line_idx, float now);
int                  tutorial_line_is_locked(int line_idx);
int                  tutorial_guard_source_change(int pos, int delete_count,
                                                  int insert_count);
TutorialMatchResult  tutorial_match(const char *expected, const char *got);

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
