#ifndef GL_INCLUDES_H
#define GL_INCLUDES_H

/* NULL / size_t. The freeglut+Mesa headers below pull <stddef.h> in
 * transitively, but the Apple-GLUT framework path (OpenGL/gl.h +
 * GLUT/glut.h) does not. Include it here so every TU that uses this shim
 * sees NULL on both paths; otherwise a scene TU that includes only
 * <math.h> compiles under the default freeglut build yet fails the
 * `make glut` framework build (axes.c / grid.c / postprocess_filter.c). */
#include <stddef.h>

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

#endif
