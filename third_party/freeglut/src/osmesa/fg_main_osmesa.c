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
#include <stdlib.h>

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

/*
 * Cheap content signature of the current OSMesa colour buffer. A single-buffered
 * backend has no per-display platform hook (the generic glutSwapBuffers() short-
 * circuits, and fghRedrawWindow() exposes none), so the record mode below runs
 * once per main-loop iteration and uses this to tell when a *new* frame has been
 * rendered -- it scatters a few thousand byte reads (FNV-1a), negligible next to
 * a software render.
 */
static unsigned long fghOSMesaFrameSignature( void )
{
    OSMesaContext        ctx = OSMesaGetCurrentContext();
    GLint                w = 0, h = 0, fmt = 0;
    void                *buffer = NULL;
    const unsigned char *p;
    unsigned long        sig = 2166136261UL;
    size_t               total, step, i;

    if( !ctx )
        return 0;

    /* Gallium-based OSMesa resolves rendering into the user buffer only on
     * a flush; without one the buffer holds stale pre-resolve contents, the
     * signature never changes, and the record mode never starts. Sample
     * after glFinish, like fghOSMesaCaptureFrame does before its read. Only
     * the pre-start iterations of an active record pay this (limit > 0 and
     * not yet started); finishing an already-idle software pipe is cheap. */
    glFinish( );

    if( !OSMesaGetColorBuffer( ctx, &w, &h, &fmt, &buffer ) ||
        !buffer || w <= 0 || h <= 0 )
        return 0;

    p = (const unsigned char *)buffer;
    total = (size_t)w * (size_t)h * 4;
    step = total / 4096;
    if( step == 0 )
        step = 1;
    for( i = 0; i < total; i += step )
        sig = ( sig ^ p[i] ) * 16777619UL;
    return sig;
}

void fgPlatformProcessSingleEvent( void )
{
    /* Headless: no window-system events are ever delivered. Both frame-capture
     * paths are serviced here -- this runs every main-loop iteration, just
     * before that iteration's display work, and SIGUSR1 interrupts the
     * fgPlatformSleepForEvents nanosleep so even an idle app responds. For a
     * single-buffered backend this is the only per-frame hook freeglut offers
     * (glutSwapBuffers short-circuits before the platform swap). */

    /* (1) FREEGLUT_CAPTURE_FRAMES=N record mode: write every newly rendered
     * frame to a numbered PPM and exit(0) after N. A content signature skips the
     * pre-first-render buffer and triggers on the first real frame; thereafter
     * every iteration is captured (the iteration:frame ratio is 1:1 under the
     * software renderer), so a static scene yields N identical frames and an
     * animation N distinct ones -- deterministic for offline GIF/MP4 assembly. */
    {
        static int           limit = -1;     /* -1 unread; 0 disabled */
        static int           count = 0;
        static int           started = 0;
        static int           primed = 0;
        static unsigned long prime_sig = 0;
        if( limit < 0 )
        {
            const char *e = getenv( "FREEGLUT_CAPTURE_FRAMES" );
            limit = ( e && *e ) ? atoi( e ) : 0;
            if( limit < 0 )
                limit = 0;
        }
        if( limit > 0 )
        {
            if( !started )
            {
                unsigned long sig = fghOSMesaFrameSignature();
                if( !primed )
                {
                    prime_sig = sig;
                    primed = 1;
                }
                else if( sig != prime_sig )
                    started = 1;             /* first real frame is rendered */
            }
            if( started )
            {
                fghOSMesaCaptureFrame();
                if( ++count >= limit )
                    exit( 0 );
            }
        }
    }

    /* (2) SIGUSR1 single snapshot, on demand -- captures the current frame even
     * when the app is idle (the colour buffer still holds the last frame). */
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
