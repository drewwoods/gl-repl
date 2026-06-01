/*
 * fg_state_osmesa.c
 *
 * State-query methods (glutGet / glutDeviceGet) for the OSMesa backend.
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

int fgPlatformGlutGet( GLenum eWhat )
{
    SFG_Window *win = fgStructure.CurrentWindow;

    switch( eWhat )
    {
    case GLUT_WINDOW_WIDTH:
        return win ? win->State.Width  : 0;
    case GLUT_WINDOW_HEIGHT:
        return win ? win->State.Height : 0;

    /* No window manager: windows are always at the origin with no decoration. */
    case GLUT_WINDOW_X:
    case GLUT_WINDOW_Y:
    case GLUT_WINDOW_BORDER_WIDTH:
    case GLUT_WINDOW_HEADER_HEIGHT:
        return 0;

    case GLUT_WINDOW_RGBA:
        return 1;
    case GLUT_WINDOW_DOUBLEBUFFER:
        return 0;   /* OSMesa is single-buffered */
    case GLUT_WINDOW_STEREO:
        return 0;

    case GLUT_WINDOW_RED_SIZE:
    case GLUT_WINDOW_GREEN_SIZE:
    case GLUT_WINDOW_BLUE_SIZE:
    case GLUT_WINDOW_ALPHA_SIZE:
        return 8;

    case GLUT_WINDOW_DEPTH_SIZE:
        return win ? win->Window.pContext.DepthBits   : 0;
    case GLUT_WINDOW_STENCIL_SIZE:
        return win ? win->Window.pContext.StencilBits : 0;
    case GLUT_WINDOW_ACCUM_RED_SIZE:
    case GLUT_WINDOW_ACCUM_GREEN_SIZE:
    case GLUT_WINDOW_ACCUM_BLUE_SIZE:
    case GLUT_WINDOW_ACCUM_ALPHA_SIZE:
        /* OSMesa packs the accumulation buffer; report a per-channel quarter. */
        return win ? win->Window.pContext.AccumBits / 4 : 0;

    case GLUT_WINDOW_COLORMAP_SIZE:
        return 0;
    case GLUT_WINDOW_NUM_SAMPLES:
        return 0;
    case GLUT_WINDOW_FORMAT_ID:
        return 0;

    default:
        fgWarning( "glutGet(): unknown/unsupported parameter %d", eWhat );
        return -1;
    }
}

int fgPlatformGlutDeviceGet( GLenum eWhat )
{
    /* A headless backend has no input devices. */
    switch( eWhat )
    {
    case GLUT_HAS_KEYBOARD:
    case GLUT_HAS_MOUSE:
    case GLUT_HAS_SPACEBALL:
    case GLUT_HAS_DIAL_AND_BUTTON_BOX:
    case GLUT_HAS_TABLET:
    case GLUT_NUM_MOUSE_BUTTONS:
    case GLUT_NUM_SPACEBALL_BUTTONS:
    case GLUT_NUM_BUTTON_BOX_BUTTONS:
    case GLUT_NUM_DIALS:
    case GLUT_NUM_TABLET_BUTTONS:
        return 0;
    default:
        return -1;
    }
}

int *fgPlatformGlutGetModeValues( GLenum eWhat, int *size )
{
    *size = 0;
    return NULL;
}
