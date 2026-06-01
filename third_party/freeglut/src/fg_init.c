/*
 * Copyright (c) 1999-2000 Pawel W. Olszta. All Rights Reserved.
 * Written by Pawel W. Olszta, <olszta@sourceforge.net>
 * Creation date: Thu Dec 2 1999
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
/* Various initialization functions */

#define FREEGLUT_BUILDING_LIB
#include <GL/freeglut.h>
#include "fg_internal.h"

/*
 * A structure pointed by fgDisplay holds all information
 * regarding the display, screen, root window etc.
 */
SFG_Display fgDisplay;

/*
 * The settings for the current freeglut session
 */
SFG_State fgState = { { -1, -1, GL_FALSE },  /* Position */
                      { 300, 300, GL_TRUE }, /* Size */
                      GLUT_RGBA | GLUT_SINGLE | GLUT_DEPTH,  /* DisplayMode */
                      GL_FALSE,              /* Initialised */
                      GLUT_TRY_DIRECT_CONTEXT,  /* DirectContext */
                      GL_FALSE,              /* ForceIconic */
                      GL_FALSE,              /* UseCurrentContext */
                      GL_FALSE,              /* GLDebugSwitch */
                      GL_FALSE,              /* XSyncSwitch */
                      GLUT_KEY_REPEAT_ON,    /* KeyRepeat */
                      INVALID_MODIFIERS,     /* Modifiers */
                      0,                     /* FPSInterval */
                      0,                     /* SwapCount */
                      0,                     /* SwapTime */
                      0,                     /* Time */
                      { NULL, NULL },         /* Timers */
                      { NULL, NULL },         /* FreeTimers */
                      NULL,                   /* IdleCallback */
                      NULL,                   /* IdleCallbackData */
                      0,                      /* ActiveMenus */
                      NULL,                   /* MenuStateCallback */
                      NULL,                   /* MenuStatusCallback */
                      NULL,                   /* MenuStatusCallbackData */
                      FREEGLUT_MENU_FONT,
                      { -1, -1, GL_TRUE },    /* GameModeSize */
                      -1,                     /* GameModeDepth */
                      -1,                     /* GameModeRefresh */
                      GLUT_ACTION_EXIT,       /* ActionOnWindowClose */
                      GLUT_EXEC_STATE_INIT,   /* ExecState */
                      NULL,                   /* ProgramName */
                      GL_FALSE,               /* JoysticksInitialised */
                      0,                      /* NumActiveJoysticks */
                      GL_FALSE,               /* InputDevsInitialised */
                      0,                      /* MouseWheelTicks */
                      1,                      /* AuxiliaryBufferNumber */
                      4,                      /* SampleNumber */
                      GL_FALSE,               /* SkipStaleMotion */
                      GL_FALSE,               /* StrokeFontDrawJoinDots */
                      GL_FALSE,               /* AllowNegativeWindowPosition */
                      1,                      /* OpenGL context MajorVersion */
                      0,                      /* OpenGL context MinorVersion */
                      0,                      /* OpenGL ContextFlags */
                      0,                      /* OpenGL ContextProfile */
                      0,                      /* HasOpenGL20 */
                      -1,                     /* HasSwapCtlTear */
                      NULL,                   /* ErrorFunc */
                      NULL,                   /* ErrorFuncData */
                      NULL,                   /* WarningFunc */
                      NULL,                   /* WarningFuncData */
                      { { { FG_CAP_RED, { FG_NONE, 0 } } }, 0, 0, GL_FALSE } /* DisplayStrCriteria */
};


extern void fgPlatformInitialize( const char* displayName );
extern void fgPlatformDeinitialiseInputDevices ( void );
extern void fgPlatformCloseDisplay ( void );
extern void fgPlatformDestroyContext ( SFG_PlatformDisplay pDisplay, SFG_WindowContextType MContext );

void fghParseCommandLineArguments ( int* pargc, char** argv, char **pDisplayName, char **pGeometry )
{
#ifndef _WIN32_WCE
    int i, j, argc = *pargc;

    {
        /* check if GLUT_FPS env var is set */
        const char *fps = getenv( "GLUT_FPS" );

        if( fps )
        {
            int interval;
            sscanf( fps, "%d", &interval );

            if( interval <= 0 )
                fgState.FPSInterval = 5000;  /* 5000 millisecond default */
            else
                fgState.FPSInterval = interval;
        }
    }

    *pDisplayName = getenv( "DISPLAY" );

    for( i = 1; i < argc; i++ )
    {
        if( strcmp( argv[ i ], "-display" ) == 0 )
        {
            if( ++i >= argc )
                fgError( "-display parameter must be followed by display name" );

            *pDisplayName = argv[ i ];

            argv[ i - 1 ] = NULL;
            argv[ i     ] = NULL;
            ( *pargc ) -= 2;
        }
        else if( strcmp( argv[ i ], "-geometry" ) == 0 )
        {
            if( ++i >= argc )
                fgError( "-geometry parameter must be followed by window "
                         "geometry settings" );

            *pGeometry = argv[ i ];

            argv[ i - 1 ] = NULL;
            argv[ i     ] = NULL;
            ( *pargc ) -= 2;
        }
        else if( strcmp( argv[ i ], "-direct" ) == 0)
        {
            if( fgState.DirectContext == GLUT_FORCE_INDIRECT_CONTEXT )
                fgError( "parameters ambiguity, -direct and -indirect "
                    "cannot be both specified" );

            fgState.DirectContext = GLUT_FORCE_DIRECT_CONTEXT;
            argv[ i ] = NULL;
            ( *pargc )--;
        }
        else if( strcmp( argv[ i ], "-indirect" ) == 0 )
        {
            if( fgState.DirectContext == GLUT_FORCE_DIRECT_CONTEXT )
                fgError( "parameters ambiguity, -direct and -indirect "
                    "cannot be both specified" );

            fgState.DirectContext = GLUT_FORCE_INDIRECT_CONTEXT;
            argv[ i ] = NULL;
            (*pargc)--;
        }
        else if( strcmp( argv[ i ], "-iconic" ) == 0 )
        {
            fgState.ForceIconic = GL_TRUE;
            argv[ i ] = NULL;
            ( *pargc )--;
        }
        else if( strcmp( argv[ i ], "-gldebug" ) == 0 )
        {
            fgState.GLDebugSwitch = GL_TRUE;
            argv[ i ] = NULL;
            ( *pargc )--;
        }
        else if( strcmp( argv[ i ], "-sync" ) == 0 )
        {
            fgState.XSyncSwitch = GL_TRUE;
            argv[ i ] = NULL;
            ( *pargc )--;
        }
    }

    /* Compact {argv}. */
    for( i = j = 1; i < *pargc; i++, j++ )
    {
        /* Guaranteed to end because there are "*pargc" arguments left */
        while ( argv[ j ] == NULL )
            j++;
        if ( i != j )
            argv[ i ] = argv[ j ];
    }

#endif /* _WIN32_WCE */

}


void fghCloseInputDevices ( void )
{
    if ( fgState.JoysticksInitialised )
        fgJoystickClose( );

    if ( fgState.InputDevsInitialised )
        fgInputDeviceClose( );
}


/* Perform the freeglut deinitialization...  */
void fgDeinitialize( void )
{
    SFG_Timer *timer;

    if( !fgState.Initialised )
    {
        return;
    }

    /* If we're in game mode, we want to leave game mode */
    if( fgStructure.GameModeWindow ) {
        glutLeaveGameMode();
    }

    /* If there was a menu created, destroy the rendering context */
    if( fgStructure.MenuContext )
    {
        fgPlatformDestroyContext (fgDisplay.pDisplay, fgStructure.MenuContext->MContext );
        free( fgStructure.MenuContext );
        fgStructure.MenuContext = NULL;
    }

    fgDestroyStructure( );

    while( ( timer = fgState.Timers.First) )
    {
        fgListRemove( &fgState.Timers, &timer->Node );
        free( timer );
    }

    while( ( timer = fgState.FreeTimers.First) )
    {
        fgListRemove( &fgState.FreeTimers, &timer->Node );
        free( timer );
    }

    fgPlatformDeinitialiseInputDevices ();

    fgState.MouseWheelTicks = 0;

    fgState.MajorVersion = 1;
    fgState.MinorVersion = 0;
    fgState.ContextFlags = 0;
    fgState.ContextProfile = 0;

    fgState.Initialised = GL_FALSE;

    fgState.Position.X = -1;
    fgState.Position.Y = -1;
    fgState.Position.Use = GL_FALSE;

    fgState.Size.X = 300;
    fgState.Size.Y = 300;
    fgState.Size.Use = GL_TRUE;

    fgState.DisplayMode = GLUT_RGBA | GLUT_SINGLE | GLUT_DEPTH;

    fgState.DirectContext  = GLUT_TRY_DIRECT_CONTEXT;
    fgState.ForceIconic         = GL_FALSE;
    fgState.UseCurrentContext   = GL_FALSE;
    fgState.GLDebugSwitch       = GL_FALSE;
    fgState.XSyncSwitch         = GL_FALSE;
    fgState.ActionOnWindowClose = GLUT_ACTION_EXIT;
    fgState.ExecState           = GLUT_EXEC_STATE_INIT;

    fgState.KeyRepeat       = GLUT_KEY_REPEAT_ON;
    fgState.Modifiers       = INVALID_MODIFIERS;

    fgState.GameModeSize.X  = -1;
    fgState.GameModeSize.Y  = -1;
    fgState.GameModeDepth   = -1;
    fgState.GameModeRefresh = -1;

    fgListInit( &fgState.Timers );
    fgListInit( &fgState.FreeTimers );

    fgState.IdleCallback           = ( FGCBIdleUC )NULL;
    fgState.IdleCallbackData       = NULL;
    fgState.MenuStateCallback      = ( FGCBMenuState )NULL;
    fgState.MenuStatusCallback     = ( FGCBMenuStatusUC )NULL;
    fgState.MenuStatusCallbackData = NULL;

    fgState.SwapCount   = 0;
    fgState.SwapTime    = 0;
    fgState.FPSInterval = 0;

    if( fgState.ProgramName )
    {
        free( fgState.ProgramName );
        fgState.ProgramName = NULL;
    }

    fgPlatformCloseDisplay ();

    fgState.Initialised = GL_FALSE;
}

/* parseCriteria() returns this when a comparator is present but malformed
 * (e.g. "depth=" or "depth=16x"); fghAddCriterion() treats it as unspecified
 * and substitutes the capability's documented default. */
#define FG_INVALID ((FGCriterionComparison)(-1))

/* Parse a display string token and return a Criterion struct.
 * Based on the original GLUT implementation:
 * https://github.com/markkilgard/glut/blob/master/lib/glut/glut_dstr.c
 * Implements all valid comparators as described in glutInitDisplayString man page:
 *   = (equal), != (not equal)
 *   < (less than), > (greater than)
 *   <= (less than or equal), >= (greater than or equal)
 *   ~ (greater than or equal but preferring less)
 */
static FGCriterion parseCriteria(const char *word)
{
    FGCriterion c = { FG_NONE, 0 };
    const char *cstr, *vstr = NULL;
    char *endptr;

    cstr = strpbrk( word, "=><!~" );

    if ( cstr ) {
        switch ( cstr[0] ) {
        case '=':
            c.comparison = FG_EQ;
            vstr         = &cstr[1];
            break;
        case '~':
            c.comparison = FG_MIN;
            vstr         = &cstr[1];
            break;
        case '>':
            if ( cstr[1] == '=' ) {
                c.comparison = FG_GTE;
                vstr         = &cstr[2];
            }
            else {
                c.comparison = FG_GT;
                vstr         = &cstr[1];
            }
            break;
        case '<':
            if ( cstr[1] == '=' ) {
                c.comparison = FG_LTE;
                vstr         = &cstr[2];
            }
            else {
                c.comparison = FG_LT;
                vstr         = &cstr[1];
            }
            break;
        case '!':
            if ( cstr[1] == '=' ) {
                c.comparison = FG_NEQ;
                vstr         = &cstr[2];
            }
            else {
                c.comparison = FG_INVALID;
                return c;
            }
            break;
        default:
            c.comparison = FG_INVALID;
            return c;
        }

        if ( vstr && *vstr ) {
            c.value = (int)strtol( vstr, &endptr, 0 );
            if ( endptr == vstr || *endptr != '\0' ) {
                /* Not a valid number or has trailing garbage */
                c.comparison = FG_INVALID;
                c.value = 0;
            }
        } else {
            /* Comparator present but no value */
            c.comparison = FG_INVALID;
        }
    } else {
        /* No comparator found */
        c.comparison = FG_UNSPECIFIED;
        c.value = 0;
    }

    return c;
}

#if defined(NEED_XPARSEGEOMETRY_IMPL) || defined(TARGET_HOST_MS_WINDOWS)
#   include "util/xparsegeometry_repl.h"
#endif

/*
 * Perform initialization. This usually happens on the program startup
 * and restarting after glutMainLoop termination...
 */
void FGAPIENTRY glutInit( int* pargc, char** argv )
{
    char* displayName = NULL;
    char* geometry = NULL;
    if( fgState.Initialised )
        fgError( "illegal glutInit() reinitialization attempt" );

    if (pargc && *pargc && argv && *argv && **argv)
    {
        fgState.ProgramName = strdup (*argv);

        if( !fgState.ProgramName )
            fgError ("Could not allocate space for the program's name.");
    }

    fgCreateStructure( );

    fghParseCommandLineArguments ( pargc, argv, &displayName, &geometry );

    /*
     * Have the display created now. If there wasn't a "-display"
     * in the program arguments, we will use the DISPLAY environment
     * variable for opening the X display (see code above):
     */
    fgPlatformInitialize( displayName );

    /*
     * Geometry parsing deferred until here because we may need the screen
     * size.
     */

    if ( geometry )
    {
        unsigned int parsedWidth, parsedHeight;
        int mask = XParseGeometry( geometry,
                                   &fgState.Position.X, &fgState.Position.Y,
                                   &parsedWidth, &parsedHeight );
        /* TODO: Check for overflow? */
        fgState.Size.X = parsedWidth;
        fgState.Size.Y = parsedHeight;

        if( (mask & (WidthValue|HeightValue)) == (WidthValue|HeightValue) )
            fgState.Size.Use = GL_TRUE;

        if( ( mask & XNegative ) && !fgState.AllowNegativeWindowPosition )
            fgState.Position.X += fgDisplay.ScreenWidth - fgState.Size.X;

        if( ( mask & YNegative ) && !fgState.AllowNegativeWindowPosition )
            fgState.Position.Y += fgDisplay.ScreenHeight - fgState.Size.Y;

        if( (mask & (XValue|YValue)) == (XValue|YValue) )
            fgState.Position.Use = GL_TRUE;
    }
}

/* Undoes all the "glutInit" stuff */
void FGAPIENTRY glutExit ( void )
{
  fgDeinitialize ();
}

/* Sets the default initial window position for new windows */
void FGAPIENTRY glutInitWindowPosition( int x, int y )
{
    fgState.Position.X = x;
    fgState.Position.Y = y;

    if( ( ( x >= 0 ) && ( y >= 0 ) ) || fgState.AllowNegativeWindowPosition )
        fgState.Position.Use = GL_TRUE;
    else
        fgState.Position.Use = GL_FALSE;
}

/* Sets the default initial window size for new windows */
void FGAPIENTRY glutInitWindowSize( int width, int height )
{
    fgState.Size.X = width;
    fgState.Size.Y = height;

    if( ( width > 0 ) && ( height > 0 ) )
        fgState.Size.Use = GL_TRUE;
    else
        fgState.Size.Use = GL_FALSE;
}

/* Sets the default display mode for all new windows */
void FGAPIENTRY glutInitDisplayMode( unsigned int displayMode )
{
    /* We will make use of this value when creating a new OpenGL context... */
    fgState.DisplayMode = displayMode;

    /* glutInitDisplayMode supersedes any prior glutInitDisplayString: clear
     * the parsed criteria so backends stop scoring against stale (possibly
     * impossible) constraints. */
    memset( &fgState.DisplayStrCriteria, 0, sizeof(FGDisplayStringCriteria) );
}


/* -- INIT DISPLAY STRING PARSING ------------------------------------------ */

static char* Tokens[] =
{
    "alpha", "acca", "acc", "blue", "buffer", "conformant", "depth", "double",
    "green", "index", "num", "red", "rgba", "rgb", "luminance", "stencil",
    "single", "stereo", "samples", "slow", "win32pdf", "win32pfd", "xvisual",
    "xstaticgray", "xgrayscale", "xstaticcolor", "xpseudocolor",
    "xtruecolor", "xdirectcolor",
    "xstaticgrey", "xgreyscale", "xstaticcolour", "xpseudocolour",
    "xtruecolour", "xdirectcolour", "borderless", "aux", "auxbufs",
    "srgb", "captionless"
};
#define NUM_TOKENS             (sizeof(Tokens) / sizeof(*Tokens))

void FGAPIENTRY glutInitDisplayString( const char* displayMode )
{
    FGDisplayStringCriteria *dsc = &fgState.DisplayStrCriteria;
    FGCriterion parsed;
    unsigned int glut_state_flag = 0;
    char *token;
    size_t len = strlen( displayMode );
    char *buffer = (char *)malloc( (len+1) * sizeof(char) );

    /* Start from an empty, ordered criteria list. Entry order mirrors the
     * display-string left-to-right order, which defines matching precedence. */
    memset( dsc, 0, sizeof(FGDisplayStringCriteria) );
    dsc->haveDisplayString = GL_TRUE;

    /* Unpack options from the display string delimited by blanks or tabs */
    memcpy( buffer, displayMode, len );
    buffer[len] = '\0';

    token = strtok( buffer, " \t" );

    while ( token )
    {
        int i;
        size_t cleanlength = strcspn( token, "=<>~!" );

        for ( i = 0; i < NUM_TOKENS; i++ )
        {
            if ( strncmp( token, Tokens[i], cleanlength ) == 0 ) break;
        }

        parsed = parseCriteria( token );

        switch ( i )
        {
        case 0:  /* "alpha":  Alpha color buffer precision. Default >=1 */
            glut_state_flag |= GLUT_ALPHA;
            fghAddCriterion( dsc, FG_CAP_ALPHA, parsed, fghMakeCriterion(FG_GTE, 1) );
            break;

        case 1:  /* "acca":  RGBA accumulation buffer precision. Default >=1 each */
            glut_state_flag |= GLUT_ACCUM;
            fghAddCriterion( dsc, FG_CAP_ACCUM_RED,   parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_ACCUM_GREEN, parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_ACCUM_BLUE,  parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_ACCUM_ALPHA, parsed, fghMakeCriterion(FG_GTE, 1) );
            break;

        case 2:  /* "acc":  RGB accumulation, zero alpha. Default >=1 RGB, ~0 alpha */
            glut_state_flag |= GLUT_ACCUM;
            fghAddCriterion( dsc, FG_CAP_ACCUM_RED,   parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_ACCUM_GREEN, parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_ACCUM_BLUE,  parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_ACCUM_ALPHA,
                             fghMakeCriterion(FG_UNSPECIFIED, 0), fghMakeCriterion(FG_MIN, 0) );
            break;

        case 3:  /* "blue":  Blue color buffer precision. Default >=1 */
            fghAddCriterion( dsc, FG_CAP_BLUE, parsed, fghMakeCriterion(FG_GTE, 1) );
            break;

        case 4:  /* "buffer":  Color (index) buffer bits. Default >=1 */
            fghAddCriterion( dsc, FG_CAP_BUFFER, parsed, fghMakeCriterion(FG_GTE, 1) );
            break;

        case 5:  /* "conformant":  Not modeled by freeglut */
            break;

        case 6:  /* "depth":  Depth buffer precision. Default >=12 */
            glut_state_flag |= GLUT_DEPTH;
            fghAddCriterion( dsc, FG_CAP_DEPTH, parsed, fghMakeCriterion(FG_GTE, 12) );
            break;

        case 7:  /* "double":  Double buffering */
            glut_state_flag |= GLUT_DOUBLE;
            break;

        case 8:  /* "green":  Green color buffer precision. Default >=1 */
            fghAddCriterion( dsc, FG_CAP_GREEN, parsed, fghMakeCriterion(FG_GTE, 1) );
            break;

        case 9:  /* "index":  Color index model. Default buffer >=1 */
            glut_state_flag |= GLUT_INDEX;
            fghAddCriterion( dsc, FG_CAP_BUFFER, parsed, fghMakeCriterion(FG_GTE, 1) );
            break;

        case 10:  /* "num":  Select Nth matching configuration */
            if ( parsed.comparison == FG_EQ && parsed.value >= 0 )
                dsc->num = parsed.value;
            break;

        case 11:  /* "red":  Red color buffer precision. Default >=1 */
            fghAddCriterion( dsc, FG_CAP_RED, parsed, fghMakeCriterion(FG_GTE, 1) );
            break;

        case 12:  /* "rgba":  RGBA precision. Default >=1 each channel */
            glut_state_flag |= GLUT_RGBA;
            fghAddCriterion( dsc, FG_CAP_RED,   parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_GREEN, parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_BLUE,  parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_ALPHA, parsed, fghMakeCriterion(FG_GTE, 1) );
            break;

        case 13:  /* "rgb":  RGB precision, zero alpha. Default >=1 RGB, ~0 alpha */
            glut_state_flag |= GLUT_RGB;
            fghAddCriterion( dsc, FG_CAP_RED,   parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_GREEN, parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_BLUE,  parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_ALPHA,
                             fghMakeCriterion(FG_UNSPECIFIED, 0), fghMakeCriterion(FG_MIN, 0) );
            break;

        case 14:  /* "luminance":  Red only. Default red >=1, green/blue =0 */
            glut_state_flag |= GLUT_LUMINANCE;
            fghAddCriterion( dsc, FG_CAP_RED,   parsed, fghMakeCriterion(FG_GTE, 1) );
            fghAddCriterion( dsc, FG_CAP_GREEN, fghMakeCriterion(FG_EQ, 0), fghMakeCriterion(FG_EQ, 0) );
            fghAddCriterion( dsc, FG_CAP_BLUE,  fghMakeCriterion(FG_EQ, 0), fghMakeCriterion(FG_EQ, 0) );
            break;

        case 15:  /* "stencil":  Stencil bits. Presence implies >=1 */
            glut_state_flag |= GLUT_STENCIL;
            fghAddCriterion( dsc, FG_CAP_STENCIL, parsed, fghMakeCriterion(FG_GTE, 1) );
            break;

        case 16:  /* "single":  Single buffered */
            glut_state_flag |= GLUT_SINGLE;
            break;

        case 17:  /* "stereo":  Stereo color buffer */
            glut_state_flag |= GLUT_STEREO;
            break;

        case 18:  /* "samples":  Multisample count. Default <=4 */
            glut_state_flag |= GLUT_MULTISAMPLE;
            fghAddCriterion( dsc, FG_CAP_SAMPLES, parsed, fghMakeCriterion(FG_LTE, 4) );
            /* Seed the coarse GLX/Cocoa pre-filter; final selection is by the
             * criteria list. Only an exact request constrains the pre-filter. */
            fgState.SampleNumber = ( parsed.comparison == FG_EQ ) ? parsed.value : 0;
            break;

        case 19:  /* "slow":  Not modeled by freeglut */
            break;

        case 20:  /* "win32pdf": misspelling, kept for compatibility */
        case 21:  /* "win32pfd":  Win32-only, not modeled */
            break;

        case 22:  /* "xvisual":  X11-only, not modeled */
            break;

        case 23:  /* "xstaticgray" */
        case 29:  /* "xstaticgrey" */
        case 24:  /* "xgrayscale" */
        case 30:  /* "xgreyscale" */
        case 25:  /* "xstaticcolor" */
        case 31:  /* "xstaticcolour" */
        case 26:  /* "xpseudocolor" */
        case 32:  /* "xpseudocolour" */
        case 27:  /* "xtruecolor" */
        case 33:  /* "xtruecolour" */
        case 28:  /* "xdirectcolor" */
        case 34:  /* "xdirectcolour":  X11-only, not modeled */
            break;

        case 35:  /* "borderless":  Window without borders */
            glut_state_flag |= GLUT_BORDERLESS;
            break;

        case 36:  /* "aux" */
        case 37:  /* "auxbufs":  Auxiliary buffers. Presence implies >=1 */
            glut_state_flag |= GLUT_AUX;
            fghAddCriterion( dsc, FG_CAP_AUX, parsed, fghMakeCriterion(FG_GTE, 1) );
            fgState.AuxiliaryBufferNumber = ( parsed.comparison == FG_EQ ) ? parsed.value : 1;
            break;

        case 38:  /* "srgb":  sRGB-capable framebuffer (freeglut extension) */
            glut_state_flag |= GLUT_SRGB;
            break;

        case 39:  /* "captionless":  Window without a caption (freeglut extension) */
            glut_state_flag |= GLUT_CAPTIONLESS;
            break;

        case 40:  /* Unrecognized */
            fgWarning( "Display string token not recognized: %s", token );
            break;
        }

        token = strtok( NULL, " \t" );
    }

    free( buffer );

    /* Set the DisplayMode for compatibility */
    fgState.DisplayMode = glut_state_flag;
}

/* -- SETTING OPENGL 3.0 CONTEXT CREATION PARAMETERS ---------------------- */

void FGAPIENTRY glutInitContextVersion( int majorVersion, int minorVersion )
{
    /* We will make use of these value when creating a new OpenGL context... */
    fgState.MajorVersion = majorVersion;
    fgState.MinorVersion = minorVersion;
}


void FGAPIENTRY glutInitContextFlags( int flags )
{
    /* We will make use of this value when creating a new OpenGL context... */
    fgState.ContextFlags = flags;
}

void FGAPIENTRY glutInitContextProfile( int profile )
{
    /* We will make use of this value when creating a new OpenGL context... */
    fgState.ContextProfile = profile;
}

/* -------------- User Defined Error/Warning Handler Support -------------- */

/*
 * Sets the user error handler (note the use of va_list for the args to the fmt)
 */
void FGAPIENTRY glutInitErrorFuncUcall( FGErrorUC callback, FGCBUserData userData )
{
    /* This allows user programs to handle freeglut errors */
    fgState.ErrorFunc = callback;
    fgState.ErrorFuncData = userData;
}

static void fghInitErrorFuncCallback( const char *fmt, va_list ap, FGCBUserData userData )
{
    FGError* callback = (FGError*)&userData;
    (*callback)( fmt, ap );
}

void FGAPIENTRY glutInitErrorFunc( FGError callback )
{
    if (callback)
    {
        FGError* reference = &callback;
        glutInitErrorFuncUcall( fghInitErrorFuncCallback, *((FGCBUserData*)reference) );
    }
    else
    {
        glutInitErrorFuncUcall( NULL, NULL );
    }
}

/*
 * Sets the user warning handler (note the use of va_list for the args to the fmt)
 */
void FGAPIENTRY glutInitWarningFuncUcall( FGWarningUC callback, FGCBUserData userData )
{
    /* This allows user programs to handle freeglut warnings */
    fgState.WarningFunc = callback;
    fgState.WarningFuncData = userData;
}

static void fghInitWarningFuncCallback( const char *fmt, va_list ap, FGCBUserData userData )
{
    FGWarning* callback = (FGWarning*)&userData;
    (*callback)( fmt, ap );
}

void FGAPIENTRY glutInitWarningFunc( FGWarning callback )
{
    if (callback)
    {
        FGWarning* reference = &callback;
        glutInitWarningFuncUcall( fghInitWarningFuncCallback, *((FGCBUserData*)reference) );
    }
    else
    {
        glutInitWarningFuncUcall( NULL, NULL );
    }
}

/* glutInitDisplayString parsing is exercised by progs/demos/dstrprobe. */
