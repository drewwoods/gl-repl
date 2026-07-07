/*
 * fg_main_emscripten.c
 *
 * Emscripten platform main loop and event processing for freeglut.
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
#include <sys/time.h>

fg_time_t fgPlatformSystemTime( void )
{
    struct timeval now;
    gettimeofday( &now, NULL );
    return (fg_time_t)now.tv_sec * 1000 + now.tv_usec / 1000;
}

void fgPlatformSleepForEvents( fg_time_t ms )
{
}

void fgPlatformProcessSingleEvent( void )
{
}

void fgPlatformMainLoopPreliminaryWork( void )
{
}

void fgPlatformInitWork( SFG_Window *window )
{
}

void fgPlatformPosResZordWork( SFG_Window *window, unsigned int workMask )
{
}

void fgPlatformVisibilityWork( SFG_Window *window )
{
}

void fgPlatformSetColor( int idx, float r, float g, float b )
{
}

float fgPlatformGetColor( int idx, int comp )
{
    return 0.0f;
}

void fgPlatformCopyColormap( int win )
{
}
