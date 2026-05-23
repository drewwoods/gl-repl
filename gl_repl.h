/*
 * gl_repl.h - Minimal legacy compatibility header for gl-repl.
 *
 * This root header now carries only the shared C/OpenGL includes and the
 * `M_PI` fallback used by older translation units. Real runtime types and APIs
 * have moved to their owning module headers under src/; callers should prefer
 * those narrower headers when possible.
 */
#ifndef GL_REPL_H
#define GL_REPL_H

#include <gl_includes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#endif /* GL_REPL_H */
