/*
 * tutorials.h - Built-in tutorial catalog.
 */
#ifndef REPL_TUTORIALS_H
#define REPL_TUTORIALS_H

typedef struct {
    const char       *name;
    const char *const *comments;
    const char *const *expected;
} TutorialEntry;

int         repl_tutorial_count(void);
const char *repl_tutorial_name(int idx);
int         repl_tutorial_step_count(int idx);
const char *repl_tutorial_step_comment(int idx, int step_idx);
const char *repl_tutorial_step_expected(int idx, int step_idx);

#endif /* REPL_TUTORIALS_H */