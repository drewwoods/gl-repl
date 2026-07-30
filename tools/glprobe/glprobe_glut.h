#ifndef GLPROBE_GLUT_H
#define GLPROBE_GLUT_H

/* GL + GLU + GLUT include shim for standalone samples that want to be
 * probe-able BOTH in a window and headless.
 *
 * A sample written with the usual `#ifdef __APPLE__ / <GLUT/glut.h>` block
 * cannot be built against the vendored OSMesa freeglut, because on macOS that
 * block picks the Apple framework unconditionally -- and the Apple framework
 * has no headless backend. Swapping that block for this header is the only
 * source change a sample needs to gain `make glprobe ... FREEGLUT_OSMESA=1`.
 *
 * This is deliberately NOT include/gl_includes.h: that one is the app's shim
 * (it forces freeglut on macOS, pulls in glext for the GPU profiler, and
 * defines M_PI), and samples under tools/glprobe/ are meant to compile with
 * nothing but a C compiler and a GL. */

#include <stddef.h>   /* NULL / size_t: the Apple framework path omits it */

#if defined(FREEGLUT_OSMESA)
    /* Headless: Mesa's GL/GLU plus the vendored freeglut OSMesa backend.
     * Checked first so it wins on macOS too, where __APPLE__ is also set. */
    #include <GL/gl.h>
    #include <GL/glu.h>
    #include <GL/freeglut.h>
#elif defined(__APPLE__)
    #include <OpenGL/gl.h>
    #include <OpenGL/glu.h>
    #include <GLUT/glut.h>
#else
    #include <GL/gl.h>
    #include <GL/glu.h>
    #include <GL/glut.h>
#endif

#endif /* GLPROBE_GLUT_H */
