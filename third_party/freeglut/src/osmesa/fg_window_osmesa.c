/*
 * fg_window_osmesa.c
 *
 * Window (off-screen framebuffer + OSMesa context) management.
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
 * Map the requested glutInitDisplayMode bits to OSMesa depth/stencil/accum bit
 * counts and create the off-screen context. OSMesaCreateContextExt always
 * yields a legacy (compatibility) context, which is exactly what makes
 * fixed-function rendering and glRenderMode(GL_FEEDBACK) work; a requested core
 * profile is therefore silently ignored.
 */
static OSMesaContext fghCreateNewContextOSMesa( SFG_Window *window )
{
    GLint depthBits   = ( fgState.DisplayMode & GLUT_DEPTH )   ? 24 : 0;
    GLint stencilBits = ( fgState.DisplayMode & GLUT_STENCIL ) ?  8 : 0;
    GLint accumBits   = ( fgState.DisplayMode & GLUT_ACCUM )   ? 16 : 0;
    OSMesaContext context;

    context = OSMesaCreateContextExt( OSMESA_RGBA, depthBits, stencilBits,
                                      accumBits, NULL );
    if( !context )
        fghContextCreationError();

    window->Window.pContext.DepthBits   = depthBits;
    window->Window.pContext.StencilBits = stencilBits;
    window->Window.pContext.AccumBits   = accumBits;
    window->Window.pContext.Format      = OSMESA_RGBA;

    return context;
}

void fgPlatformOpenWindow( SFG_Window *window, const char *title,
                           GLboolean positionUse, int x, int y,
                           GLboolean sizeUse, int w, int h,
                           GLboolean gameMode, GLboolean isSubWindow )
{
    void *buffer;

    /* No window system: position, game-mode and sub-window requests are
     * meaningless. Honor only the requested pixel dimensions. */
    if( w <= 0 ) w = 300;
    if( h <= 0 ) h = 300;

    window->Window.Context = fghCreateNewContextOSMesa( window );

    buffer = malloc( (size_t)w * (size_t)h * 4 );
    if( !buffer )
        fgError( "OSMesa: failed to allocate %dx%d RGBA framebuffer", w, h );

    window->Window.pContext.Buffer = buffer;
    window->Window.pContext.Width  = w;
    window->Window.pContext.Height = h;

    window->State.Width   = w;
    window->State.Height  = h;
    window->State.Xpos    = 0;
    window->State.Ypos    = 0;
    window->State.Visible = GL_TRUE;

    if( !OSMesaMakeCurrent( window->Window.Context, buffer, GL_UNSIGNED_BYTE,
                            w, h ) )
        fgError( "OSMesaMakeCurrent failed" );

    /* OSMesa is single-buffered. Pin the draw/read buffer to GL_FRONT so a
     * GLUT_DOUBLE request (which the generic code would otherwise treat as
     * double-buffered, see fg_main_osmesa.c) still renders into, and reads
     * back from, the one physical buffer. */
    glDrawBuffer( GL_FRONT );
    glReadBuffer( GL_FRONT );

    /* A real window manager would deliver the initial visibility event; post
     * the work directly so the WindowStatus callback fires. (The reshape /
     * first display are synthesized in fgPlatformInitWork.) */
    window->State.WorkMask |= GLUT_INIT_WORK | GLUT_VISIBILITY_WORK;
}

void fgPlatformSetWindow( SFG_Window *window )
{
    if( window && window->Window.Context )
    {
        /* Re-binding the already-current context is a no-op to OSMesa's API
         * but still drives the Gallium state-tracker's framebuffer purge,
         * which faults on swrast during teardown (fgDestroyWindow re-asserts
         * the current window before closing it). Skip the redundant bind. */
        if( OSMesaGetCurrentContext() == window->Window.Context )
            return;
        OSMesaMakeCurrent( window->Window.Context,
                           window->Window.pContext.Buffer, GL_UNSIGNED_BYTE,
                           window->Window.pContext.Width,
                           window->Window.pContext.Height );
    }
}

void fgPlatformCloseWindow( SFG_Window *window )
{
    if( window->Window.Context )
    {
        /* During process exit the Gallium driver may already have finalized
         * itself; calling OSMesaDestroyContext then jumps through a nulled
         * vtable. Leak the context (reclaimed by the OS) rather than crash.
         * See fghOSMesaProcessExiting in fg_internal_osmesa.h. */
        if( !fghOSMesaProcessExiting )
            OSMesaDestroyContext( window->Window.Context );
        window->Window.Context = NULL;
    }
    if( window->Window.pContext.Buffer )
    {
        free( window->Window.pContext.Buffer );
        window->Window.pContext.Buffer = NULL;
    }
}

/*
 * The remaining window-manager operations have no meaning without a window
 * system; they are accepted and ignored.
 */
void fgPlatformReshapeWindow ( SFG_Window *window, int width, int height )
{
}

void fgPlatformShowWindow( SFG_Window *window )
{
}

void fgPlatformHideWindow( SFG_Window *window )
{
}

void fgPlatformIconifyWindow( SFG_Window *window )
{
}

void fgPlatformGlutSetWindowTitle( const char *title )
{
}

void fgPlatformGlutSetIconTitle( const char *title )
{
}

void fgPlatformPositionWindow( SFG_Window *window, int x, int y )
{
}

void fgPlatformPushWindow( SFG_Window *window )
{
}

void fgPlatformPopWindow( SFG_Window *window )
{
}

void fgPlatformFullScreenToggle( SFG_Window *window )
{
}
