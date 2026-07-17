/*
 * fg_capture.c
 *
 * Environment-gated frame capture for the windowed backends: SIGUSR1
 * single-frame snapshot and the FREEGLUT_CAPTURE_FRAMES record mode.
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

#if TARGET_HOST_OSMESA

/* The OSMesa backend carries its own self-contained capture implementation
 * (its window is single-buffered, so the swap-path hook below would never run
 * there anyway). Keep these as stubs so the two implementations -- and their
 * SIGUSR1 handlers -- cannot clash in one binary. */
void fghCaptureInit( void )
{
}

void fghCaptureTick( void )
{
}

void fghCaptureOnSwap( SFG_Window *window )
{
    (void)window;
}

#else /* windowed backends */

#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <signal.h>
#endif

/*
 * The contract matches the OSMesa backend's headless capture, on a real
 * window-system renderer:
 *
 *   - `kill -USR1 <pid>` snapshots the next presented frame to a numbered
 *     PPM. The handler only sets a flag; fghCaptureTick() (main loop) posts
 *     a redisplay so even an idle app renders one frame, and the swap path
 *     captures the completed back buffer just before it is presented.
 *   - FREEGLUT_CAPTURE_FRAMES=N captures every presented frame and exit(0)s
 *     after N. Each glutSwapBuffers() is a real rendered frame, so no
 *     content-signature priming is needed (unlike the OSMesa tick).
 *
 * Output prefix comes from FREEGLUT_CAPTURE_FILE (default "freeglut");
 * files are <prefix>-NNNN.ppm. glReadPixels from a multisampled default
 * framebuffer resolves the samples, so MSAA frames come out antialiased.
 *
 * FREEGLUT_CAPTURE_STREAM=<path|-> instead appends every captured frame to
 * one concatenated PPM stream at <path> ("-" = stdout). Point it at a fifo
 * and an encoder (ffmpeg -f image2pipe -vcodec ppm) consumes frames as they
 * render -- no on-disk framebuffer dumps.
 */

#ifndef _WIN32
static volatile sig_atomic_t fghCaptureRequested = 0;

static void fghCaptureSignal( int sig )
{
    (void)sig;
    fghCaptureRequested = 1;   /* async-signal-safe: just set the flag */
}
#endif

void fghCaptureInit( void )
{
#ifndef _WIN32
    /* sigaction with SA_RESTART so the handler stays installed and most
     * blocking calls resume; select()/nanosleep still return EINTR, which is
     * what wakes an idle X11 main loop. No-op on Windows (no SIGUSR1). */
    struct sigaction sa;
    sa.sa_handler = fghCaptureSignal;
    sigemptyset( &sa.sa_mask );
    sa.sa_flags = SA_RESTART;
    sigaction( SIGUSR1, &sa, NULL );
#endif
}

/* FREEGLUT_CAPTURE_FRAMES record-mode limit; 0 = disabled. Read once. */
static int fghCaptureFramesLimit( void )
{
    static int limit = -1;
    if( limit < 0 )
    {
        const char *e = getenv( "FREEGLUT_CAPTURE_FRAMES" );
        limit = ( e && *e ) ? atoi( e ) : 0;
        if( limit < 0 )
            limit = 0;
    }
    return limit;
}

/* FREEGLUT_CAPTURE_STREAM sink (see the contract comment above). Opened
 * lazily on the first captured frame; read once. */
static FILE *fghCaptureStream( void )
{
    static FILE *fp = NULL;
    static int   checked = 0;
    if( !checked )
    {
        const char *path = getenv( "FREEGLUT_CAPTURE_STREAM" );
        checked = 1;
        if( path && *path )
        {
            fp = ( path[ 0 ] == '-' && !path[ 1 ] ) ? stdout
                                                    : fopen( path, "wb" );
            if( !fp )
                fgWarning( "capture: cannot open stream %s", path );
        }
    }
    return fp;
}

/* Write w x h pixels of the current GL_READ_BUFFER to <prefix>-NNNN.ppm --
 * or, in FREEGLUT_CAPTURE_STREAM mode, append them to the single PPM stream.
 * glReadPixels returns rows bottom-up; emit them top-to-bottom. */
static void fghCaptureWritePPM( int w, int h )
{
    const char    *prefix;
    static int     seq = 0;
    char           path[256];
    unsigned char *pixels;
    GLint          prevAlign = 4;
    FILE          *fp;
    int            y;

    if( w <= 0 || h <= 0 )
        return;

    pixels = (unsigned char *)malloc( (size_t)w * (size_t)h * 3 );
    if( !pixels )
        return;

    glGetIntegerv( GL_PACK_ALIGNMENT, &prevAlign );
    glPixelStorei( GL_PACK_ALIGNMENT, 1 );
    glReadPixels( 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels );
    glPixelStorei( GL_PACK_ALIGNMENT, prevAlign );

    fp = fghCaptureStream();
    if( fp )
    {
        /* Stream mode: frames concatenate into one pipe/file. Log the first
         * frame only -- a per-frame line would swamp stderr on long records. */
        if( seq++ == 0 )
            fgWarning( "capture: streaming %dx%d frames", w, h );
        path[ 0 ] = '\0';
    }
    else
    {
        prefix = getenv( "FREEGLUT_CAPTURE_FILE" );
        if( !prefix || !*prefix )
            prefix = "freeglut";
        snprintf( path, sizeof( path ), "%s-%04d.ppm", prefix, seq++ );

        fp = fopen( path, "wb" );
        if( !fp )
        {
            fgWarning( "capture: cannot open %s", path );
            free( pixels );
            return;
        }
    }

    fprintf( fp, "P6\n%d %d\n255\n", w, h );
    for( y = h - 1; y >= 0; --y )
        fwrite( pixels + (size_t)y * (size_t)w * 3, 1, (size_t)w * 3, fp );

    if( path[ 0 ] )
    {
        fclose( fp );
        fgWarning( "capture: wrote %s (%dx%d)", path, w, h );
    }
    else
        /* Keep the stream hot so a pipe consumer sees each frame as soon as
         * it is rendered rather than in stdio-buffer-sized bursts. */
        fflush( fp );
    free( pixels );
}

/* Capture from the given read buffer, preserving the app's read-buffer
 * binding. */
static void fghCaptureFromBuffer( const SFG_Window *window, GLenum buffer )
{
    GLint prevRead = (GLint)buffer;

    glGetIntegerv( GL_READ_BUFFER, &prevRead );
    glReadBuffer( buffer );
    fghCaptureWritePPM( window->State.Width, window->State.Height );
    glReadBuffer( (GLenum)prevRead );
}

static void fghCapturePostRedisplay( SFG_Window *window,
                                     SFG_Enumerator *enumerator )
{
    (void)enumerator;
    window->State.WorkMask |= GLUT_DISPLAY_WORK;
}

/*
 * Main-loop tick (glutMainLoopEvent). Services a pending SIGUSR1 request:
 * for a double-buffered window we post a redisplay everywhere and let the
 * resulting glutSwapBuffers() capture the freshly rendered frame (reading
 * GL_FRONT between presentations is unreliable, notably on Cocoa); a
 * single-buffered window draws to the front buffer, whose content *is* the
 * last frame, so it is read directly even when the app is idle.
 */
void fghCaptureTick( void )
{
    SFG_Enumerator enumerator;
    int            want = 0;

    /* Record mode: keep frames coming even for a static scene (an app that
     * never re-posts its own redisplay would otherwise stop swapping and the
     * record would stall short of N). */
    if( fghCaptureFramesLimit() > 0 )
        want = 1;

#ifndef _WIN32
    if( fghCaptureRequested )
    {
        if( fgStructure.CurrentWindow &&
            !fgStructure.CurrentWindow->Window.DoubleBuffered )
        {
            fghCaptureRequested = 0;
            fghCaptureFromBuffer( fgStructure.CurrentWindow, GL_FRONT );
        }
        else
            /* Leave the flag set: the next swap (this same loop iteration,
             * given the redisplay posted below) captures and clears it. */
            want = 1;
    }
#endif
    if( !want )
        return;

    enumerator.found = GL_FALSE;
    enumerator.data  = NULL;
    fgEnumWindows( fghCapturePostRedisplay, &enumerator );
}

/*
 * Swap-path hook (glutSwapBuffers, after the single-buffer early-return and
 * before the platform swap): the back buffer holds the exactly-complete
 * frame about to be presented.
 */
void fghCaptureOnSwap( SFG_Window *window )
{
    static int count = 0;
    int        limit = fghCaptureFramesLimit();
    int        want  = ( limit > 0 );

#ifndef _WIN32
    if( fghCaptureRequested )
    {
        fghCaptureRequested = 0;
        want = 1;
    }
#endif
    if( !want )
        return;

    fghCaptureFromBuffer( window, GL_BACK );

    if( limit > 0 && ++count >= limit )
        exit( 0 );
}

#endif /* TARGET_HOST_OSMESA */
