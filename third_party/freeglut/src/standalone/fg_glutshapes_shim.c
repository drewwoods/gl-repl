/*
 * fg_glutshapes_shim.c
 *
 * Defines the renamed support symbols (fgshapes_*) declared by
 * fg_glutshapes_shim.h, plus the small public configuration API
 * (glutShapes*) for the standalone "glutshapes" library.
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
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "fg_glutshapes_shim.h"
#include <GL/glutshapes.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

/*
 * Defaults: the GL1.1 fixed-function draw path is live (HasOpenGL20 == 0),
 * which works on desktop GL, OSMesa and gl4es with no extra setup. A consumer
 * targeting GLES2/WebGL (or wanting the GL2.0 vertex-attrib path on desktop)
 * calls glutShapesSetGLVersion(2) and binds attribute locations.
 */
SFG_State fgshapes_State =
{
    GL_TRUE,   /* Initialised            */
    0,         /* HasOpenGL20            */
    2,         /* MajorVersion           */
    0          /* StrokeFontDrawJoinDots */
};

/*
 * A single static "current window" so glutSetVertexAttribCoord3/Normal/
 * TexCoord2 and the draw dispatchers have somewhere to read/write the vertex
 * attribute locations. Attributes default to -1 (=> GL1.1 path until the
 * consumer binds them).
 */
static SFG_Window fgshapes_dummyWindow =
{
    { -1, -1, -1 },        /* Window.attribute_v_{coord,normal,texture} */
    { GL_FALSE, GL_TRUE }  /* State.{VisualizeNormals,Visible}          */
};

SFG_Structure fgshapes_Structure = { &fgshapes_dummyWindow };

void fgshapes_Error( const char *fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    fputs( "glutshapes: fatal: ", stderr );
    vfprintf( stderr, fmt, ap );
    fputc( '\n', stderr );
    va_end( ap );
    /* The real freeglut fgError calls exit(1) and its call sites
     * (memory-allocation failures) assume it never returns. */
    exit( EXIT_FAILURE );
}

void fgshapes_Warning( const char *fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    fputs( "glutshapes: warning: ", stderr );
    vfprintf( stderr, fmt, ap );
    fputc( '\n', stderr );
    va_end( ap );
}

static void *( *fgshapes_resolver )( const char * ) = NULL;

void *fgshapes_GetProcAddress( const char *name )
{
    return fgshapes_resolver ? fgshapes_resolver( name ) : NULL;
}

/* -- Public configuration API (declared in <GL/glutshapes.h>) --------------- */

void glutShapesSetProcAddressFunc( void *( *resolver )( const char *name ) )
{
    fgshapes_resolver = resolver;
}

void glutShapesSetGLVersion( int major )
{
    fgshapes_State.MajorVersion = major;
    fgshapes_State.HasOpenGL20  = ( major >= 2 );
}

/* Provided by fg_gl2.c (loads the GL2.0 VBO entry points via the resolver). */
extern void fgInitGL2( void );

void glutShapesInit( void )
{
#ifndef GL_ES_VERSION_2_0
    /* On a desktop GL context fgInitGL2() resolves the VBO entry points through
     * the installed resolver; with none, it would store NULL pointers and a
     * later GL2.0 draw would crash. Warn at init rather than at crash time.
     * (On GLES2/WebGL the entry points are linked directly, so no resolver is
     * needed and this check is compiled out.) */
    if( !fgshapes_resolver )
        fgshapes_Warning( "glutShapesInit() called with no proc-address resolver; "
                          "install one via glutShapesSetProcAddressFunc() before "
                          "using the GL2.0 vertex-attrib path on desktop GL" );
#endif
    fgInitGL2();
}
