/*
 * fg_window_emscripten.c
 *
 * Emscripten platform window management stubs for freeglut.
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

void fgPlatformSetWindow( SFG_Window *window )
{
}

void fgPlatformOpenWindow( SFG_Window *window, const char *title,
                           GLboolean positionUse, int x, int y,
                           GLboolean sizeUse, int w, int h,
                           GLboolean gameMode, GLboolean isSubWindow )
{
}

void fgPlatformReshapeWindow( SFG_Window *window, int width, int height )
{
}

void fgPlatformCloseWindow( SFG_Window *window )
{
}

void fgPlatformShowWindow( SFG_Window *window )
{
}

void fgPlatformHideWindow( SFG_Window *window )
{
}

void fgPlatformIconifyWindow( SFG_Window *window )
{
}

void fgPlatformGlutSetWindowTitle( const char *title )
{
}

void fgPlatformGlutSetIconTitle( const char *title )
{
}

void fgPlatformPositionWindow( SFG_Window *window, int x, int y )
{
}

void fgPlatformPushWindow( SFG_Window *window )
{
}

void fgPlatformPopWindow( SFG_Window *window )
{
}

void fgPlatformFullScreenToggle( SFG_Window *window )
{
}
