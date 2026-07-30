#ifndef GLPROBE_COMPAT_GLUT_GLUT_H
#define GLPROBE_COMPAT_GLUT_GLUT_H

/* Stand-in for the Apple GLUT framework header, reached only because the
 * headless glprobe build puts tools/glprobe/compat/ ahead of the macOS SDK on
 * the include path.
 *
 * A sample written the usual way opens with
 *
 *     #ifdef __APPLE__
 *       #include <GLUT/glut.h>
 *     #else
 *       #include <GL/glut.h>
 *     #endif
 *
 * and that first branch is what pins it to the Apple framework, which has no
 * headless backend. Redirecting the include is what lets such a sample build
 * against the vendored OSMesa freeglut with ZERO source changes -- which
 * matters most for the preload probe, whose entire premise is not touching the
 * file. (The #else branch needs no help: the vendored freeglut ships its own
 * GL/glut.h.)
 *
 * Only the headless target installs this. A native build still gets the real
 * framework, because on a machine with a window server that is the right
 * GLUT. */
#include "glprobe_glut.h"

#endif
