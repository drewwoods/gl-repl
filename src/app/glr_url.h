/*
 * glr_url.h - Cross-platform URL opener.
 *
 * Provides a non-blocking URL launcher service across macOS, Linux/BSD,
 * Windows, and Web (Emscripten). Includes test seams to safely mock launches
 * during automated testing.
 */
#ifndef GLR_URL_H
#define GLR_URL_H

#define GLR_USER_GUIDE_URL "https://github.com/drewwoods/gl-repl/blob/main/docs/USER_GUIDE.md"

/* Launches `url` in the default system browser in a detached, non-blocking manner.
 * Returns 1 if launch was successfully dispatched/requested, 0 on failure. */
int glr_url_open(const char *url);

/* Opens the canonical User Guide URL and updates status text.
 * Returns 1 on successful launch request, 0 on failure. */
int glr_url_open_user_guide(void);

/* Per-frame tick to reap any exited URL launcher child processes asynchronously. */
void glr_url_tick(void);

/* Cleanup tracked child state on app shutdown. */
void glr_url_shutdown(void);

/* Test seams for unit tests to intercept URL launches without spawning real browsers. */
typedef int (*GlrUrlLauncherFn)(const char *url);
void glr_url_set_launcher_for_test(GlrUrlLauncherFn fn);
void glr_url_reset_launcher_for_test(void);

#endif /* GLR_URL_H */
