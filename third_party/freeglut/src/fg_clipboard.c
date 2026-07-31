/*
 * fg_clipboard.c
 *
 * Access to the window system's clipboard as UTF-8 text.
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

#include <stdlib.h>

/*
 * The backends supply only the conversion to and from the platform clipboard;
 * every lifetime question is settled here, in portable C. fgPlatformGet-
 * ClipboardString() hands back a freshly allocated string (or NULL), which
 * becomes the cached buffer below -- so a backend never has to reason about
 * how long the application holds the pointer glutGetClipboardString()
 * returned, and the cache is freed in exactly one place.
 */

static char *fghClipboardText = NULL;

#if !TARGET_HOST_MACOS_COCOA

/*
 * Backends with no implementation yet behave like a permanently empty
 * clipboard rather than failing the call: an application can call these
 * unconditionally, and a paste simply finds nothing.
 */
void fgPlatformSetClipboardString( const char *string )
{
    (void) string;
}

char *fgPlatformGetClipboardString( void )
{
    return NULL;
}

#endif

/* Drops the cached string handed out by the last glutGetClipboardString().
 * Called from fgDeinitialize(); what the platform clipboard itself holds is
 * deliberately left alone, so text copied out of the application survives it
 * exiting (on the platforms whose clipboard outlives its owner). */
void fghClipboardDeinitialise( void )
{
    free( fghClipboardText );
    fghClipboardText = NULL;
}

void FGAPIENTRY glutSetClipboardString( const char *string )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutSetClipboardString" );

    fgPlatformSetClipboardString( string ? string : "" );
}

const char * FGAPIENTRY glutGetClipboardString( void )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutGetClipboardString" );

    /* Freed only once the replacement is in hand, so a backend that reads
     * its own window's selection cannot invalidate the old string while
     * still producing the new one. */
    {
        char *text = fgPlatformGetClipboardString( );

        free( fghClipboardText );
        fghClipboardText = text;
    }

    return fghClipboardText;
}
