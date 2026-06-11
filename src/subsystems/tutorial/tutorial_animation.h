/*
 * tutorial_animation.h - Pure tutorial fade helpers over a frozen fade view.
 */
#ifndef TUTORIAL_ANIMATION_H
#define TUTORIAL_ANIMATION_H

#include "subsystems/tutorial/tutorial.h"

typedef struct TutorialFadeView {
    int   active;
    int   fade_line_idx;
    float fade_start_t;
    float fade_duration;
} TutorialFadeView;

int   tutorial_fade_front(const TutorialFadeView *fade, int line_idx,
                          int line_len, float now);
float tutorial_fade_alpha(const TutorialFadeView *fade, int line_idx,
                          int char_idx, int line_len, float now);
float tutorial_fade_settle(const TutorialFadeView *fade, int line_idx,
                           int char_idx, int line_len, float now);
int   tutorial_fade_line_active(const TutorialFadeView *fade, int line_idx,
                                float now);

#endif /* TUTORIAL_ANIMATION_H */
