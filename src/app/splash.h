#ifndef GLR_SPLASH_H
#define GLR_SPLASH_H

/*
 * Startup splash banner: the open-cube logo lockup (mark + name + tagline)
 * in a band along the bottom of the window, fading out after a few seconds.
 * Frame-counted (not wall-clock) so headless/record runs are deterministic.
 * Not a full-screen overlay — the scene and UI render normally underneath.
 */

/* Nonzero while the banner still has frames to draw. */
int splash_active(void);

/* Dismiss the banner immediately (any keypress). */
void splash_skip(void);

/* Draw the banner over the composited frame; call between
 * glr_ctrl_display_frame() and glutSwapBuffers().  No-op once done. */
void splash_render(int win_w, int win_h);

#endif /* GLR_SPLASH_H */
