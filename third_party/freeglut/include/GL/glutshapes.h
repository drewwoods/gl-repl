/*
 * glutshapes.h
 *
 * Public configuration API for the standalone freeglut "glutshapes" library:
 * the glutSolid / glutWire solids, the teapot/teacup/teaspoon, and the
 * stroke/bitmap text, built WITHOUT any freeglut windowing.
 *
 * The shape and font entry points themselves are declared in <GL/freeglut.h>
 * (which a consumer includes as usual). This header only adds the few hooks a
 * standalone consumer needs to point the geometry code at its GL context.
 *
 * Copyright (c) 2026 freeglut contributors. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef __GLUTSHAPES_H__
#define __GLUTSHAPES_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Install a GL proc-address resolver. Only needed on a desktop-GL context to
 * load the VBO entry points for the GL2.0 vertex-attrib draw path (e.g. pass
 * SDL_GL_GetProcAddress, glfwGetProcAddress, or OSMesaGetProcAddress). Unused on
 * a GLES2/WebGL build, where those entry points are linked directly.
 */
extern void glutShapesSetProcAddressFunc( void *( *resolver )( const char *name ) );

/*
 * Select the draw path. major >= 2 enables the GL2.0 vertex-attrib path
 * (required on GLES2/WebGL; the consumer must also bind a shader and call
 * glutSetVertexAttribCoord3/Normal/TexCoord2). The default is the GL1.1
 * fixed-function path, which works on desktop GL, OSMesa, and gl4es.
 */
extern void glutShapesSetGLVersion( int major );

/*
 * Convenience: enable the GL2.0 path by loading the VBO entry points via the
 * installed resolver (wraps freeglut's internal fgInitGL2). Safe to skip if you
 * only use the GL1.1 fixed-function path or a GLES2 build.
 */
extern void glutShapesInit( void );

#ifdef __cplusplus
}
#endif

#endif /* __GLUTSHAPES_H__ */
