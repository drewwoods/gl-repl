/*
 * fg_joystick_osmesa.c
 *
 * Joystick stubs for the OSMesa backend (no joystick hardware).
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
 * PAWEL W. OLSZTA BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <GL/freeglut.h>
#include "fg_internal.h"

void fgPlatformJoystickRawRead( SFG_Joystick *joy, int *buttons, float *axes )
{
    if( buttons )
        *buttons = 0;
    if( joy )
        joy->error = GL_TRUE;
}

void fgPlatformJoystickOpen( SFG_Joystick *joy )
{
    if( joy )
        joy->error = GL_TRUE;
}

void fgPlatformJoystickInit( SFG_Joystick *fgJoystick[], int ident )
{
    SFG_Joystick *joy = fgJoystick[ ident ];
    if( joy )
    {
        joy->id          = ident;
        joy->error       = GL_TRUE;   /* no joystick hardware */
        joy->num_axes    = 0;
        joy->num_buttons = 0;
        joy->name[ 0 ]   = '\0';
    }
}

void fgPlatformJoystickClose( int ident )
{
}
