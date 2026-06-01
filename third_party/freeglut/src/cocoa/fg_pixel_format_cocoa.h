#ifndef FG_PIXEL_FORMAT_COCOA_H
#define FG_PIXEL_FORMAT_COCOA_H

#include <GL/freeglut.h>

#import <Cocoa/Cocoa.h>

NSOpenGLPixelFormat *fgCocoaCreatePixelFormat( GLboolean isMenu );
GLboolean fgCocoaIsDisplayModePossible( GLboolean isMenu );
GLboolean fgCocoaIsValidContextRequest( int majorVersion, int minorVersion, int contextFlags, int contextProfile );

#endif