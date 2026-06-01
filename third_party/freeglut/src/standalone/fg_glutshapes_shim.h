/*
 * fg_glutshapes_shim.h
 *
 * Compatibility shim that lets freeglut's geometry and font sources
 * (fg_geometry.c, fg_teapot.c, fg_gl2.c, fg_font.c, the stroke/font data, and
 * the public font-handle definitions) compile into a standalone "glutshapes"
 * library WITHOUT any of freeglut's windowing / event / context machinery.
 *
 * It is included (in place of fg_internal.h) only when the translation unit is
 * built with -DFREEGLUT_GEOMETRY_STANDALONE.
 *
 * The few freeglut-internal support symbols the geometry/font code reaches for
 * are #define-renamed to private fgshapes_* names. That keeps the library
 * self-contained and, crucially, stops it from colliding with — or silently
 * binding to — a host that already implements the GLUT/GL support API (e.g.
 * Emscripten's GLUT emulation, or gl4es): the public shape/font entry points
 * keep their glut* names (that is the product), but fgState/fgStructure/
 * fgError/fgWarning/glutGetProcAddress become ours.
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

#ifndef FG_GLUTSHAPES_SHIM_H
#define FG_GLUTSHAPES_SHIM_H

/*
 * Pull the public GLUT/GL declarations and GL types FIRST, before renaming any
 * symbols, so freeglut.h's own prototypes (notably glutGetProcAddress) are
 * processed under their real names and the GL types used below are available.
 * The GL flavour (desktop / GLES2 via FREEGLUT_GLES) is chosen by the consumer's
 * build, exactly as for a normal freeglut consumer.
 */
#include <GL/freeglut.h>

/* The geometry/font sources rely on these having been pulled in by
 * fg_internal.h (e.g. fg_teapot.c uses sqrt/memset/memcpy without including
 * them itself), so the shim must provide the same standard headers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

/* -- Rename the freeglut-internal support symbols to private names. --------- */
#define fgState             fgshapes_State
#define fgStructure         fgshapes_Structure
#define fgError             fgshapes_Error
#define fgWarning           fgshapes_Warning
#define glutGetProcAddress  fgshapes_GetProcAddress

/* No glutInit in a standalone library, so the init guard is a no-op. */
#define FREEGLUT_EXIT_IF_NOT_INITIALISED( string )

/* Used by the font code; copied verbatim from fg_internal.h. */
#define  freeglut_return_if_fail( expr ) \
    if( !(expr) )                        \
        return;
#define  freeglut_return_val_if_fail( expr, val ) \
    if( !(expr) )                                 \
        return val ;

/*
 * Minimal stand-ins for the freeglut structs the geometry code dereferences.
 * Only the touched fields are mirrored; if the geometry code ever starts using
 * another field this fails to compile (a named field is simply absent) rather
 * than reading garbage.
 */
typedef struct
{
    GLint attribute_v_coord;
    GLint attribute_v_normal;
    GLint attribute_v_texture;
} SFG_Context;

typedef struct
{
    GLboolean VisualizeNormals;
    GLboolean Visible;
} SFG_WindowState;

typedef struct tagSFG_Window SFG_Window;
struct tagSFG_Window
{
    SFG_Context     Window;
    SFG_WindowState State;
};

typedef struct
{
    GLboolean Initialised;
    int       HasOpenGL20;
    int       MajorVersion;
    int       StrokeFontDrawJoinDots;
} SFG_State;

typedef struct
{
    SFG_Window *CurrentWindow;
} SFG_Structure;

/* -- The font/stroke structs, copied verbatim from fg_internal.h. ----------- */
typedef struct tagSFG_Font SFG_Font;
struct tagSFG_Font
{
    char*           Name;         /* The source font name             */
    int             Quantity;     /* Number of chars in font          */
    int             Height;       /* Height of the characters         */
    const GLubyte** Characters;   /* The characters mapping           */

    float           xorig, yorig; /* Relative origin of the character */
};

typedef struct tagSFG_StrokeVertex SFG_StrokeVertex;
struct tagSFG_StrokeVertex
{
    GLfloat         X, Y;
};

typedef struct tagSFG_StrokeStrip SFG_StrokeStrip;
struct tagSFG_StrokeStrip
{
    int             Number;
    const SFG_StrokeVertex* Vertices;
};

typedef struct tagSFG_StrokeChar SFG_StrokeChar;
struct tagSFG_StrokeChar
{
    GLfloat         Right;
    int             Number;
    const SFG_StrokeStrip* Strips;
};

typedef struct tagSFG_StrokeFont SFG_StrokeFont;
struct tagSFG_StrokeFont
{
    char*           Name;                       /* The source font name      */
    int             Quantity;                   /* Number of chars in font   */
    GLfloat         Height;                     /* Height of the characters  */
    const SFG_StrokeChar** Characters;          /* The characters mapping    */
};

/* -- The renamed globals + support functions (defined in fg_glutshapes_shim.c). */
extern SFG_State     fgshapes_State;
extern SFG_Structure fgshapes_Structure;
extern void  fgshapes_Error  ( const char *fmt, ... );
extern void  fgshapes_Warning( const char *fmt, ... );
extern void *fgshapes_GetProcAddress( const char *name );

#endif /* FG_GLUTSHAPES_SHIM_H */
