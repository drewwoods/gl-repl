/*
 * fg_clipboard_cocoa.m
 *
 * Clipboard access for the Cocoa backend (NSPasteboard).
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

#import <Cocoa/Cocoa.h>

#include <GL/freeglut.h>
#include "fg_internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * NSPasteboard is a process-wide resource that needs no window, so unlike
 * most of this backend these two do not touch fgStructure or an NSView --
 * they work before the first window exists and after the last one is gone.
 *
 * Both wrap themselves in an @autoreleasepool: freeglut calls in here from
 * an application callback, which is not guaranteed to be running inside one
 * of the main loop's pools (glutSetClipboardString may be called from an
 * idle func, a timer, or before glutMainLoop).
 */

void fgPlatformSetClipboardString( const char *string )
{
    @autoreleasepool
    {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];

        /* +stringWithUTF8String: returns nil on malformed UTF-8; leave the
         * clipboard untouched rather than clearing it to nothing. */
        NSString *text = [NSString stringWithUTF8String:string];

        if( !text )
        {
            fgWarning( "fgPlatformSetClipboardString: not valid UTF-8, "
                       "clipboard left unchanged" );
            return;
        }

        [pasteboard clearContents];
        [pasteboard setString:text forType:NSPasteboardTypeString];
    }
}

char *fgPlatformGetClipboardString( void )
{
    char *copy = NULL;

    @autoreleasepool
    {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSString *text;
        const char *utf8;
        size_t len;

        if( ![[pasteboard types] containsObject:NSPasteboardTypeString] )
            return NULL;

        text = [pasteboard stringForType:NSPasteboardTypeString];
        if( !text )
            return NULL;

        utf8 = [text UTF8String];
        if( !utf8 )
            return NULL;

        /* Copied out of the pool: the NSString's buffer is autoreleased,
         * while the caller (fg_clipboard.c) caches what we return until the
         * next glutGetClipboardString(). */
        len = strlen( utf8 );
        copy = malloc( len + 1 );
        if( !copy )
        {
            fgWarning( "fgPlatformGetClipboardString: out of memory" );
            return NULL;
        }
        memcpy( copy, utf8, len + 1 );
    }

    return copy;
}
