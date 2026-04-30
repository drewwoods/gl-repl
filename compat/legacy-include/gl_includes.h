#ifndef GL_INCLUDES_H
#define GL_INCLUDES_H

/* Compatibility shim for historical checkouts.
 *
 * Old SHAs of this repo (back when it lived under
 * OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/) expected
 * a project-wide gl_includes.h at ../../include/. That file used to
 * bundle five OpenGL-Vibe utility headers which transitively pulled in
 * <stdlib.h>, <stdio.h>, <string.h>, <math.h> — and the REPL's exported
 * example C files leaned on those transitive includes (exit(), printf()
 * with no explicit #include).
 *
 * This shim reproduces just the platform GL switch + that transitive
 * stdlib bulk, so historical builds compile without dragging in the
 * unused procedural-rock / -shapes / matrix-debug / texture utilities.
 *
 * Modern checkouts use the slim include/gl_includes.h at the repo root;
 * this shim is only consumed via scripts/build-historical.sh.
 */

#if defined(__APPLE__) && defined(USE_GLUT)
    #include <OpenGL/gl.h>
    #include <OpenGL/glu.h>
    #include <GLUT/glut.h>
#else
    #define GL_GLEXT_PROTOTYPES
    #include <GL/gl.h>
    #include <GL/glext.h>
    #include <GL/glu.h>
    #include <GL/freeglut.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#endif
