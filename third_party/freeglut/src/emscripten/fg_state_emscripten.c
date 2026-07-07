/*
 * fg_state_emscripten.c
 *
 * Emscripten platform state query support for freeglut.
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

#define FREEGLUT_BUILDING_LIB
#include <GL/freeglut.h>
#include "fg_internal.h"

int fgPlatformGlutGet( GLenum eWhat )
{
    switch( eWhat )
    {
    case GLUT_WINDOW_X:       return 0;
    case GLUT_WINDOW_Y:       return 0;
    case GLUT_WINDOW_WIDTH:   return 800;
    case GLUT_WINDOW_HEIGHT:  return 600;
    default:                  return -1;
    }
}

int fgPlatformGlutDeviceGet( GLenum eWhat )
{
    return 0;
}

int *fgPlatformGlutGetModeValues( GLenum eWhat, int *size )
{
    *size = 0;
    return NULL;
}
