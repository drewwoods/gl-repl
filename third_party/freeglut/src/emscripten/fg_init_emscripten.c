/*
 * fg_init_emscripten.c
 *
 * Emscripten platform initialization for freeglut.
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

void fgPlatformInitialize( const char *displayName )
{
    fgState.Time = fgSystemTime();
    fgState.Initialised = GL_TRUE;
}

void fgPlatformDeinitialiseInputDevices( void )
{
}

void fgPlatformCloseDisplay( void )
{
}

void fgPlatformDestroyContext( SFG_PlatformDisplay pDisplay,
                               SFG_WindowContextType MContext )
{
}

/* Font pointers */
int glutStrokeRoman_impl;
int glutStrokeMonoRoman_impl;
int glutBitmap9By15_impl;
int glutBitmap8By13_impl;
int glutBitmapTimesRoman10_impl;
int glutBitmapTimesRoman24_impl;
int glutBitmapHelvetica10_impl;
int glutBitmapHelvetica12_impl;
int glutBitmapHelvetica18_impl;

void *glutStrokeRoman     = &glutStrokeRoman_impl;
void *glutStrokeMonoRoman = &glutStrokeMonoRoman_impl;
void *glutBitmap9By15        = &glutBitmap9By15_impl;
void *glutBitmap8By13        = &glutBitmap8By13_impl;
void *glutBitmapTimesRoman10 = &glutBitmapTimesRoman10_impl;
void *glutBitmapTimesRoman24 = &glutBitmapTimesRoman24_impl;
void *glutBitmapHelvetica10  = &glutBitmapHelvetica10_impl;
void *glutBitmapHelvetica12  = &glutBitmapHelvetica12_impl;
void *glutBitmapHelvetica18  = &glutBitmapHelvetica18_impl;
