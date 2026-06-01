/*
 * fg_init_osmesa.c
 *
 * Initialization methods for the OSMesa (off-screen) backend.
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

/*
 * OSMesa renders into a client-supplied memory buffer with no window system,
 * so there is no display server to open. We set up a nominal virtual screen
 * size (for glutGet(GLUT_SCREEN_*)) and the monotonic time base.
 */
int fghOSMesaProcessExiting = 0;

static void fghOSMesaMarkExiting( void )
{
    fghOSMesaProcessExiting = 1;
}

/* Frame-capture-on-SIGUSR1 (see fg_internal_osmesa.h / fg_display_osmesa.c). */
volatile sig_atomic_t fghOSMesaCaptureRequested = 0;

#ifndef _WIN32
static void fghOSMesaCaptureSignal( int sig )
{
    (void)sig;
    fghOSMesaCaptureRequested = 1;   /* async-signal-safe: just set the flag */
}
#endif

void fgPlatformInitialize( const char *displayName )
{
    fgDisplay.ScreenWidth    = 1024;
    fgDisplay.ScreenHeight   = 768;
    fgDisplay.ScreenWidthMM  = 0;
    fgDisplay.ScreenHeightMM = 0;

    fgState.Time = fgSystemTime();
    fgState.Initialised = GL_TRUE;

    atexit( fgDeinitialize );
    /* Registered after fgDeinitialize, so atexit's LIFO order runs this
     * marker first at process exit -- before fgDeinitialize tears down any
     * still-live window. See fghOSMesaProcessExiting in fg_internal_osmesa.h. */
    atexit( fghOSMesaMarkExiting );

#ifndef _WIN32
    /* `kill -USR1 <pid>` snapshots the current frame to a PPM. sigaction with
     * SA_RESTART so the handler stays installed and does not interrupt the
     * app's blocking calls. No-op on Windows (no SIGUSR1). */
    {
        struct sigaction sa;
        sa.sa_handler = fghOSMesaCaptureSignal;
        sigemptyset( &sa.sa_mask );
        sa.sa_flags = SA_RESTART;
        sigaction( SIGUSR1, &sa, NULL );
    }
#endif
}

void fgPlatformCloseDisplay( void )
{
    /* No display server / window system to tear down. */
}

void fgPlatformDestroyContext( SFG_PlatformDisplay pDisplay,
                               SFG_WindowContextType MContext )
{
    /* See fgPlatformCloseWindow: skip the Mesa teardown once the runtime has
     * begun process exit, when the Gallium driver may already be finalized. */
    if( MContext && !fghOSMesaProcessExiting )
        OSMesaDestroyContext( MContext );
}

void fgPlatformDeinitialiseInputDevices( void )
{
    fgState.JoysticksInitialised = GL_FALSE;
    fgState.InputDevsInitialised = GL_FALSE;
}
