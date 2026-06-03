/*
 * color_limits.h - REPL color-command semantic limits.
 *
 * Shared by the parser and app-side color picker bridge so typed
 * glClearColor(...) input and picker writeback clamp to the same range.
 */
#ifndef REPL_COLOR_LIMITS_H
#define REPL_COLOR_LIMITS_H

/* Max brightness (V in HSV) allowed for glClearColor channels.
 * Since max(r,g,b) == V, capping V caps every channel. */
#define REPL_CLEAR_COLOR_MAX_V 0.1f

#endif /* REPL_COLOR_LIMITS_H */
