/*
 * fg_main_osmesa.c
 *
 * The OSMesa-specific windowing event loop. Being headless, there are no
 * window-system events; the loop exists only to drive timers, the redisplay
 * work list, and the synthesized first-frame callbacks.
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
#include <time.h>

/*
 * Defined in the generic fg_main.c but not exported through a shared header;
 * each backend declares the ones it needs (as the X11/ogc backends do).
 */
extern void fghOnReshapeNotify( SFG_Window *window, int width, int height,
                                GLboolean forceNotify );
extern void fghOnPositionNotify( SFG_Window *window, int x, int y,
                                 GLboolean forceNotify );

fg_time_t fgPlatformSystemTime( void )
{
    struct timespec now;
    clock_gettime( CLOCK_MONOTONIC, &now );
    return (fg_time_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void fgPlatformSleepForEvents( fg_time_t msec )
{
    struct timespec ts;

    /* The only things that ever generate work are timers and
     * glutPostRedisplay(); cap the sleep so the work list stays responsive
     * without busy-spinning. */
    if( msec > 10 )
        msec = 10;

    ts.tv_sec  = msec / 1000;
    ts.tv_nsec = ( msec % 1000 ) * 1000000;
    nanosleep( &ts, NULL );
}

void fgPlatformProcessSingleEvent( void )
{
    /* Headless: no window-system events are ever delivered. The one thing that
     * can arrive asynchronously is a SIGUSR1 frame-capture request. Service it
     * here -- this runs every main-loop iteration, and the signal interrupts
     * fgPlatformSleepForEvents's nanosleep, so an *idle* app (one that has
     * stopped calling glutSwapBuffers) still gets captured. The last completed
     * frame is still in the colour buffer, so no redraw is needed. The swap
     * path services the same flag for the actively-animating case; whichever
     * runs first clears it. */
    if( fghOSMesaCaptureRequested )
    {
        fghOSMesaCaptureRequested = 0;
        fghOSMesaCaptureFrame();
    }
}

void fgPlatformMainLoopPreliminaryWork( void )
{
}

/*
 * A real window manager delivers the initial position/reshape/visibility as
 * events; with no WM we synthesize them here so the first display callback
 * fires. (Composed from the ogc reshape-notify and the cocoa position+reshape
 * idioms; State.Visible was already set in fgPlatformOpenWindow, and the
 * WindowStatus callback is posted via GLUT_VISIBILITY_WORK.)
 */
void fgPlatformInitWork( SFG_Window *window )
{
    /* OSMesa is single-buffered, but the generic code sets DoubleBuffered from
     * the display mode AFTER fgPlatformOpenWindow returns (fg_window.c:108).
     * Reset it here (this runs later, from the work list) so glutSwapBuffers is
     * a no-op and the draw buffer stays at the GL_FRONT we pinned. */
    window->Window.DoubleBuffered = 0;

    fghOnPositionNotify( window, 0, 0, GL_TRUE );
    fghOnReshapeNotify( window, window->State.Width, window->State.Height,
                        GL_TRUE );
}

void fgPlatformPosResZordWork( SFG_Window *window, unsigned int workMask )
{
    /* No window manager: position / size / z-order changes are not supported.
     * In particular glutReshapeWindow does not reallocate the OSMesa buffer. */
}

void fgPlatformVisibilityWork( SFG_Window *window )
{
    INVOKE_WCB( *window, WindowStatus, ( GLUT_FULLY_RETAINED ) );
}

/* OSMesa is RGBA-only; there is no color-index colormap. */
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
