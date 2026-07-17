/*
 * fg_display_osmesa.c
 *
 * Buffer-swap and GL-extension query for the OSMesa backend.
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
#include <stdio.h>
#include <stdlib.h>
#include "fg_internal.h"

/*
 * FREEGLUT_CAPTURE_STREAM=<path|-> redirects captured frames from numbered
 * per-frame PPM files to one concatenated PPM stream written to <path>
 * ("-" = stdout). Point it at a fifo and an encoder (e.g. ffmpeg -f
 * image2pipe -vcodec ppm) consumes frames as they render -- no on-disk
 * framebuffer dumps. Opened lazily on the first captured frame; read once.
 */
static FILE *fghOSMesaCaptureStream( void )
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
                fgWarning( "OSMesa capture: cannot open stream %s", path );
        }
    }
    return fp;
}

/*
 * Write the current OSMesa colour buffer to a numbered PPM (P6) -- or, in
 * FREEGLUT_CAPTURE_STREAM mode, append it to the single PPM stream. Called
 * from the swap path (below) at a safe point once a SIGUSR1 has set
 * fghOSMesaCaptureRequested. Reads the framebuffer directly via
 * OSMesaGetColorBuffer -- no glReadPixels round-trip. OSMesa's default origin
 * is bottom-up, so rows are emitted top-to-bottom (h-1 .. 0) to orient the PPM.
 */
void fghOSMesaCaptureFrame( void )
{
    OSMesaContext  ctx = OSMesaGetCurrentContext();
    GLint          w = 0, h = 0, fmt = 0;
    void          *buffer = NULL;
    const char    *prefix;
    static int     seq = 0;
    char           path[256];
    unsigned char *row;
    FILE          *fp;
    int            y, x;

    if( !ctx )
        return;
    /* Ensure the frame is fully rendered before read-back (the Gallium OSMesa
     * driver may defer until a flush); self-contained so callers need not. */
    glFinish();
    if( !OSMesaGetColorBuffer( ctx, &w, &h, &fmt, &buffer ) || !buffer ||
        w <= 0 || h <= 0 )
        return;

    fp = fghOSMesaCaptureStream();
    if( fp )
    {
        /* Stream mode: frames concatenate into one pipe/file. Log the first
         * frame only -- a per-frame line would swamp stderr on long records. */
        if( seq++ == 0 )
            fgWarning( "OSMesa capture: streaming %dx%d frames", w, h );
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
            fgWarning( "OSMesa capture: cannot open %s", path );
            return;
        }
    }

    /* Pack each RGBA row down to RGB into a scratch line, then write it. */
    row = (unsigned char *)malloc( (size_t)w * 3 );
    if( !row )
    {
        if( path[ 0 ] )
            fclose( fp );
        return;
    }

    fprintf( fp, "P6\n%d %d\n255\n", w, h );
    for( y = h - 1; y >= 0; --y )
    {
        const unsigned char *src = (const unsigned char *)buffer
                                   + (size_t)y * (size_t)w * 4;
        for( x = 0; x < w; ++x )
        {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite( row, 1, (size_t)w * 3, fp );
    }

    free( row );
    if( path[ 0 ] )
    {
        fclose( fp );
        fgWarning( "OSMesa capture: wrote %s (%dx%d)", path, w, h );
    }
    else
        /* Keep the stream hot so a pipe consumer sees each frame as soon as
         * it is rendered rather than in stdio-buffer-sized bursts. */
        fflush( fp );
}

void fgPlatformGlutSwapBuffers( SFG_PlatformDisplay *pDisplayPtr,
                                SFG_Window *CurrentWindow )
{
    /* Single-buffered: there is nothing to swap. glFinish() so the front buffer
     * is complete. NOTE: the generic glutSwapBuffers() early-returns for a
     * single-buffered window, so this is effectively never reached on OSMesa --
     * frame capture (SIGUSR1 + FREEGLUT_CAPTURE_FRAMES record mode) is therefore
     * serviced from the main-loop tick (fgPlatformProcessSingleEvent), which is
     * the per-frame hook a single-buffered backend actually has. */
    glFinish();
}

void fgPlatformInitSwapCtl( void )
{
}

void fgPlatformSwapInterval( int n )
{
}

int fgPlatformExtSupported( const char *ext )
{
    /* This is the window-system extension hook (the analogue of querying GLX or
     * WGL extensions); OSMesa has no window system. GL extensions proper are
     * queried by the generic glutExtensionSupported() before it falls back here,
     * so there is nothing for this backend to report. */
    return 0;
}
