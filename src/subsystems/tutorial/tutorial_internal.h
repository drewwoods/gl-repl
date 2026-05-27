#ifndef TUTORIAL_INTERNAL_H
#define TUTORIAL_INTERNAL_H

#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "repl/core_internal.h"
#include "repl/core.h"
#include "repl/eval.h"
#include "repl/pipeline.h"
#include "config.h"

/* Common internal definitions for tutorial subsystem */

void tutorial_normalize_text(const char *src, char *dst, size_t dst_size);
void tutorial_set_expected_message(TutorialMatchResult *result,
                                   TutorialMatchKind kind,
                                   const char *expected);

#endif /* TUTORIAL_INTERNAL_H */
