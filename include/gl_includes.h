#ifndef GL_INCLUDES_H
#define GL_INCLUDES_H

/* NULL / size_t. The freeglut+Mesa headers below pull <stddef.h> in
 * transitively, but the Apple-GLUT framework path (OpenGL/gl.h +
 * GLUT/glut.h) does not. Include it here so every TU that uses this shim
 * sees NULL on both paths; otherwise a render3d TU that includes only
 * <math.h> compiles under the default freeglut build yet fails the
 * `make glut` framework build (axes.c / grid.c / postprocess_filter.c). */
#include <stddef.h>

#if defined(__APPLE__)
    #include <OpenGL/gl.h>
    #include <OpenGL/glext.h>   /* glGetQueryObjectui64vEXT (GPU profiler) */
    #include <OpenGL/glu.h>
    #ifdef USE_GLUT
        #include <GLUT/glut.h>
    #else
        #include <GL/freeglut.h>
    #endif
#else
    #ifndef GL_GLEXT_PROTOTYPES
    #define GL_GLEXT_PROTOTYPES
    #endif
    #include <GL/gl.h>
    #include <GL/glext.h>
    #include <GL/glu.h>
    #include <GL/freeglut.h>
#endif

/* M_PI is part of POSIX <math.h> but optional in ISO C. Define the
 * fallback here (the canonical home shared by gl_repl.h) so every TU
 * that pulls in this aggregator can use M_PI without each file
 * carrying its own #ifndef block. TUs that need the actual math
 * functions still must include <math.h> themselves - only the
 * preprocessor symbol is exposed here, costing nothing. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#endif
