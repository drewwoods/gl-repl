#ifndef GL_INCLUDES_H
#define GL_INCLUDES_H

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
