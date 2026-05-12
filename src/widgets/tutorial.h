/*
 * tutorial.h - Tutorial runner and match helpers.
 */
#ifndef TUTORIAL_H
#define TUTORIAL_H

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

#endif /* TUTORIAL_H */