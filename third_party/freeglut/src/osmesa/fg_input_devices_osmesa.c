/*
 * fg_input_devices_osmesa.c
 *
 * Spaceball / dial-box / serial-port stubs for the OSMesa backend
 * (no input devices on a headless target).
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

void fgPlatformInitialiseInputDevices( void )
{
}

void fgPlatformCloseInputDevices( void )
{
}

void fgPlatformInitializeSpaceball( void )
{
}

void fgPlatformSpaceballClose( void )
{
}

void fgPlatformSpaceballSetWindow( SFG_Window *window )
{
}

int fgPlatformHasSpaceball( void )
{
    return 0;
}

int fgPlatformSpaceballNumButtons( void )
{
    return 0;
}

void fgPlatformRegisterDialDevice( const char *dial_device )
{
}

typedef struct _serialport SERIALPORT;

SERIALPORT *fg_serial_open( const char *device )       { return NULL; }
void        fg_serial_close( SERIALPORT *port )        {}
int         fg_serial_getchar( SERIALPORT *port )      { return EOF; }
int         fg_serial_putchar( SERIALPORT *port, unsigned char ch ) { return 0; }
void        fg_serial_flush( SERIALPORT *port )        {}
