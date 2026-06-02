/*
 * fg_internal_osmesa.h
 *
 * The freeglut library private include file for the OSMesa
 * (off-screen, window-system-less) backend.
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

#ifndef FREEGLUT_INTERNAL_OSMESA_H
#define FREEGLUT_INTERNAL_OSMESA_H

/* -- PLATFORM-SPECIFIC INCLUDES ------------------------------------------- */
#include <GL/osmesa.h>
#include <signal.h>     /* sig_atomic_t for the SIGUSR1 frame-capture flag */

/* -- GLOBAL TYPE DEFINITIONS ---------------------------------------------- */

/*
 * OSMesa renders into a client-supplied memory buffer with no window system,
 * display server, or surface object at all, so the platform "display" carries
 * nothing.
 */
typedef struct tagSFG_PlatformDisplay SFG_PlatformDisplay;
struct tagSFG_PlatformDisplay
{
    int unused;
};

/*
 * Make "freeglut" window handle and context types so that we don't need so
 * much conditionally-compiled code later in the library.
 */
typedef void          *SFG_WindowHandleType;   /* unused (no native window) */
typedef OSMesaContext  SFG_WindowContextType;  /* the OSMesa GL context     */
typedef int            SFG_WindowColormapType; /* dummy                     */

/*
 * Per-window platform data: the malloc'd RGBA framebuffer OSMesa renders into,
 * plus the config the context was created with (so the state backend can report
 * GLUT_WINDOW_{DEPTH,STENCIL,ACCUM_*}_SIZE without re-querying GL).
 */
typedef struct tagSFG_PlatformContext SFG_PlatformContext;
struct tagSFG_PlatformContext
{
    void   *Buffer;                          /* malloc'd Width*Height*4 RGBA */
    GLsizei Width, Height;
    GLint   DepthBits, StencilBits, AccumBits;
    GLenum  Format;                          /* OSMESA_RGBA etc.             */
};

/* No per-window window-manager state and no joystick hardware. */
typedef struct tagSFG_PlatformWindowState SFG_PlatformWindowState;
struct tagSFG_PlatformWindowState
{
    int unused;
};

typedef struct tagSFG_PlatformJoystick SFG_PlatformJoystick;
struct tagSFG_PlatformJoystick
{
    int unused;
};

/* -- MENU FONT/COLOR DEFINITIONS (unused; OSMesa has no menu sub-windows) -- */
#define  FREEGLUT_MENU_FONT    NULL

#define  FREEGLUT_MENU_PEN_FORE_COLORS   {0.0f,  0.0f,  0.0f,  1.0f}
#define  FREEGLUT_MENU_PEN_BACK_COLORS   {0.70f, 0.70f, 0.70f, 1.0f}
#define  FREEGLUT_MENU_PEN_HFORE_COLORS  {0.0f,  0.0f,  0.0f,  1.0f}
#define  FREEGLUT_MENU_PEN_HBACK_COLORS  {1.0f,  1.0f,  1.0f,  1.0f}

/* -- JOYSTICK (none on a headless backend) -------------------------------- */
#define _JS_MAX_AXES       4
#define MAX_NUM_JOYSTICKS  8

/*
 * Set once the C runtime begins process exit (see fg_init_osmesa.c). The
 * Gallium/swrast OSMesa driver runs its own __attribute__((destructor))
 * teardown during __cxa_finalize, which nulls internal driver vtables; if
 * freeglut's own atexit(fgDeinitialize) then destroys a still-live window it
 * calls OSMesaDestroyContext on that half-finalized state and crashes. When
 * this flag is set, the window close path leaks the context instead (the OS
 * reclaims it at exit). Runtime glutDestroyWindow is unaffected.
 */
extern int fghOSMesaProcessExiting;

/*
 * Headless frame capture. A SIGUSR1 handler (installed in fg_init_osmesa.c on
 * POSIX) sets fghOSMesaCaptureRequested from async-signal context; the main-loop
 * tick (fg_main_osmesa.c, fgPlatformProcessSingleEvent) notices the flag at a
 * safe point and writes the current OSMesa colour buffer to a PPM via
 * fghOSMesaCaptureFrame() (which does its own glFinish()). The tick, not the
 * platform swap, is the per-frame hook because a single-buffered window never
 * reaches the swap path. So `kill -USR1 <pid>` snapshots a running headless app
 * with no app-side code. The same tick also drives the FREEGLUT_CAPTURE_FRAMES
 * record mode (capture every frame, exit after N). Output path prefix comes
 * from the FREEGLUT_CAPTURE_FILE env var (default "freeglut"); files are
 * numbered <prefix>-NNNN.ppm.
 */
extern volatile sig_atomic_t fghOSMesaCaptureRequested;
void fghOSMesaCaptureFrame( void );

#endif /* FREEGLUT_INTERNAL_OSMESA_H */
