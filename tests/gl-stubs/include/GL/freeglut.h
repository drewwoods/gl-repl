#ifndef STUB_FREEGLUT_H
#define STUB_FREEGLUT_H

#include <stdint.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/glu.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GLUT_RGB 0
#define GLUT_RGBA GLUT_RGB
#define GLUT_INDEX 1
#define GLUT_SINGLE 0
#define GLUT_DOUBLE 2
#define GLUT_ACCUM 4
#define GLUT_ALPHA 8
#define GLUT_DEPTH 16
#define GLUT_STENCIL 32
#define GLUT_MULTISAMPLE 128

#define GLUT_LEFT_BUTTON 0
#define GLUT_MIDDLE_BUTTON 1
#define GLUT_RIGHT_BUTTON 2
#define GLUT_DOWN 0
#define GLUT_UP 1

#define GLUT_KEY_F1 1
#define GLUT_KEY_F2 2
#define GLUT_KEY_F3 3
#define GLUT_KEY_F4 4
#define GLUT_KEY_F5 5
#define GLUT_KEY_F6 6
#define GLUT_KEY_F7 7
#define GLUT_KEY_F8 8
#define GLUT_KEY_F9 9
#define GLUT_KEY_F10 10
#define GLUT_KEY_F11 11
#define GLUT_KEY_F12 12
#define GLUT_KEY_LEFT 100
#define GLUT_KEY_UP 101
#define GLUT_KEY_RIGHT 102
#define GLUT_KEY_DOWN 103
#define GLUT_KEY_PAGE_UP 104
#define GLUT_KEY_PAGE_DOWN 105
#define GLUT_KEY_HOME 106
#define GLUT_KEY_END 107
#define GLUT_KEY_INSERT 108
#define GLUT_KEY_NUM_LOCK 0x006D
#define GLUT_KEY_DELETE   0x006F
#define GLUT_KEY_SHIFT_L 0x0070
#define GLUT_KEY_SHIFT_R 0x0071
#define GLUT_KEY_CTRL_L 0x0072
#define GLUT_KEY_CTRL_R 0x0073
#define GLUT_KEY_ALT_L 0x0074
#define GLUT_KEY_ALT_R 0x0075
#define GLUT_KEY_SUPER_L 0x0076
#define GLUT_KEY_SUPER_R 0x0077

#define GLUT_ACTIVE_SHIFT 1
#define GLUT_ACTIVE_CTRL 2
#define GLUT_ACTIVE_ALT 4

#define GLUT_CURSOR_RIGHT_ARROW 0
#define GLUT_CURSOR_LEFT_ARROW 1
#define GLUT_CURSOR_INFO 2
#define GLUT_CURSOR_DESTROY 3
#define GLUT_CURSOR_HELP 4
#define GLUT_CURSOR_CYCLE 5
#define GLUT_CURSOR_SPRAY 6
#define GLUT_CURSOR_WAIT 7
#define GLUT_CURSOR_TEXT 8
#define GLUT_CURSOR_CROSSHAIR 9
#define GLUT_CURSOR_UP_DOWN 10
#define GLUT_CURSOR_LEFT_RIGHT 11
#define GLUT_CURSOR_TOP_SIDE 12
#define GLUT_CURSOR_BOTTOM_SIDE 13
#define GLUT_CURSOR_LEFT_SIDE 14
#define GLUT_CURSOR_RIGHT_SIDE 15
#define GLUT_CURSOR_TOP_LEFT_CORNER 16
#define GLUT_CURSOR_TOP_RIGHT_CORNER 17
#define GLUT_CURSOR_BOTTOM_RIGHT_CORNER 18
#define GLUT_CURSOR_BOTTOM_LEFT_CORNER 19
#define GLUT_CURSOR_INHERIT 100
#define GLUT_CURSOR_NONE 101
#define GLUT_CURSOR_FULL_CROSSHAIR 102

#define GLUT_ELAPSED_TIME 700
#define GLUT_WINDOW_WIDTH 102
#define GLUT_WINDOW_HEIGHT 103

#define GLUT_ACTION_ON_WINDOW_CLOSE 0x01F9
#define GLUT_ACTION_CONTINUE_EXECUTION 2

static const char glut_bitmap_8_by_13_stub = 0;
static const char glut_bitmap_9_by_15_stub = 0;
static const char glut_bitmap_helvetica_10_stub = 0;
static const char glut_bitmap_helvetica_12_stub = 0;
static const char glut_bitmap_helvetica_18_stub = 0;
static const char glut_bitmap_times_roman_24_stub = 0;
#define GLUT_BITMAP_8_BY_13 ((void *)(uintptr_t)&glut_bitmap_8_by_13_stub)
#define GLUT_BITMAP_9_BY_15 ((void *)(uintptr_t)&glut_bitmap_9_by_15_stub)
#define GLUT_BITMAP_HELVETICA_10 ((void *)(uintptr_t)&glut_bitmap_helvetica_10_stub)
#define GLUT_BITMAP_HELVETICA_12 ((void *)(uintptr_t)&glut_bitmap_helvetica_12_stub)
#define GLUT_BITMAP_HELVETICA_18 ((void *)(uintptr_t)&glut_bitmap_helvetica_18_stub)
#define GLUT_BITMAP_TIMES_ROMAN_24 ((void *)(uintptr_t)&glut_bitmap_times_roman_24_stub)

static const char glut_stroke_roman_stub = 0;
static const char glut_stroke_mono_roman_stub = 0;
#define GLUT_STROKE_ROMAN ((void *)(uintptr_t)&glut_stroke_roman_stub)
#define GLUT_STROKE_MONO_ROMAN ((void *)(uintptr_t)&glut_stroke_mono_roman_stub)

typedef void (*GLUTdisplayCB)(void);
typedef void (*GLUTreshapeCB)(int width, int height);
typedef void (*GLUTkeyboardCB)(unsigned char key, int x, int y);
typedef void (*GLUTspecialCB)(int key, int x, int y);
typedef void (*GLUTmouseCB)(int button, int state, int x, int y);
typedef void (*GLUTmotionCB)(int x, int y);
typedef void (*GLUTpassiveCB)(int x, int y);
typedef void (*GLUTtimerCB)(int value);
typedef void (*GLUTidleCB)(void);
typedef void (*GLUTmouseWheelCB)(int wheel, int direction, int x, int y);
typedef void (*GLUTcloseCB)(void);
typedef void (*GLUTproc)(void);

static inline void glutInit(int *argc, char **argv) { (void)argc; (void)argv; }
static inline void glutInitDisplayMode(unsigned int display_mode) { (void)display_mode; }
static inline void glutInitWindowSize(int width, int height) { (void)width; (void)height; }
static inline int glutCreateWindow(const char *title) { (void)title; return 1; }
static inline void glutDisplayFunc(GLUTdisplayCB callback) { (void)callback; }
static inline void glutReshapeFunc(GLUTreshapeCB callback) { (void)callback; }
static inline void glutKeyboardFunc(GLUTkeyboardCB callback) { (void)callback; }
static inline void glutSpecialFunc(GLUTspecialCB callback) { (void)callback; }
static inline void glutMouseFunc(GLUTmouseCB callback) { (void)callback; }
static inline void glutMotionFunc(GLUTmotionCB callback) { (void)callback; }
static inline void glutPassiveMotionFunc(GLUTpassiveCB callback) { (void)callback; }
static inline void glutMouseWheelFunc(GLUTmouseWheelCB callback) { (void)callback; }
static inline void glutCloseFunc(GLUTcloseCB callback) { (void)callback; }
static inline void glutTimerFunc(unsigned int millis, GLUTtimerCB callback, int value) { (void)millis; (void)callback; (void)value; }
static inline void glutIdleFunc(GLUTidleCB callback) { (void)callback; }
static inline void glutSetOption(GLenum option_flag, int value) { (void)option_flag; (void)value; }
static inline void glutMainLoop(void) {}
static inline void glutPostRedisplay(void) {}
static inline void glutSwapBuffers(void) {}
static inline int glutGet(GLenum type) { static int elapsed; (void)type; elapsed += 16; return elapsed; }
static inline int glutGetWindow(void) { return 1; }
static inline int glutGetModifiers(void) { return 0; }
/* Returns 1 so the stub-built controller's runtime point-parameter
 * detection defaults to "supported" == today's default build. */
static inline int glutExtensionSupported(const char *name) { (void)name; return 1; }
/* Minimal proc resolver for the runtime point-parameter path. The
 * controller probes the core, ARB, and EXT spellings; the stub build
 * routes all three to the same no-op glPointParameterfv implementation
 * in GL/gl.h. */
static inline GLUTproc glutGetProcAddress(const char *name) {
    if (!name)
        return (GLUTproc)0;
    if (strcmp(name, "glPointParameterfv") == 0 ||
        strcmp(name, "glPointParameterfvARB") == 0 ||
        strcmp(name, "glPointParameterfvEXT") == 0)
        return (GLUTproc)glPointParameterfv;
    /* Timer-query procs for the runtime-loaded GPU-profiler path. The
     * GL/gl.h stubs are inert (results never become available), so a
     * stub build exercises gpuprof's bookkeeping but reports no data. */
    if (strcmp(name, "glGenQueries") == 0 || strcmp(name, "glGenQueriesARB") == 0)
        return (GLUTproc)glGenQueries;
    if (strcmp(name, "glDeleteQueries") == 0 || strcmp(name, "glDeleteQueriesARB") == 0)
        return (GLUTproc)glDeleteQueries;
    if (strcmp(name, "glBeginQuery") == 0 || strcmp(name, "glBeginQueryARB") == 0)
        return (GLUTproc)glBeginQuery;
    if (strcmp(name, "glEndQuery") == 0 || strcmp(name, "glEndQueryARB") == 0)
        return (GLUTproc)glEndQuery;
    if (strcmp(name, "glGetQueryObjectiv") == 0 ||
        strcmp(name, "glGetQueryObjectivARB") == 0)
        return (GLUTproc)glGetQueryObjectiv;
    if (strcmp(name, "glGetQueryObjectui64vEXT") == 0 ||
        strcmp(name, "glGetQueryObjectui64v") == 0)
        return (GLUTproc)glGetQueryObjectui64vEXT;
    if (strcmp(name, "glQueryCounter") == 0)
        return (GLUTproc)glQueryCounter;
    return (GLUTproc)0;
}
static inline void glutSetCursor(int cursor) { (void)cursor; }
static inline void glutSetWindowTitle(const char *title) { (void)title; }
static inline void glutBitmapCharacter(void *font, int character) { GL_STUB_TRACE_LINE("glutBitmapCharacter %d\n", (int)character); gl_stub_tick(GL_STUB_glutBitmapCharacter); (void)font; }
static inline void glutBitmapString(void *font, const unsigned char *string) {
    const unsigned char *ch = string;
    GL_STUB_TRACE_LINE("glutBitmapString\n");
    gl_stub_tick(GL_STUB_glutBitmapString);
    while (ch && *ch) {
        /* Preserve byte-level visibility for renderer tests without
         * pretending the implementation called glutBitmapCharacter. */
        GL_STUB_TRACE_LINE("glutBitmapStringByte %d\n", (int)*ch);
        ch++;
    }
    (void)font;
}
static inline int glutBitmapWidth(void *font, int character) { (void)font; (void)character; return 8; }
static inline void glutStrokeCharacter(void *font, int character) { (void)font; (void)character; }
static inline int glutStrokeWidth(void *font, int character) { (void)font; (void)character; return 104; }
static inline void glutSolidSphere(double radius, int slices, int stacks) { GL_STUB_TRACE_LINE("glutSolidSphere %g %d %d\n", (double)radius, (int)slices, (int)stacks); gl_stub_tick(GL_STUB_glutSolidSphere); }
static inline void glutWireSphere(double radius, int slices, int stacks) { GL_STUB_TRACE_LINE("glutWireSphere %g %d %d\n", (double)radius, (int)slices, (int)stacks); gl_stub_tick(GL_STUB_glutWireSphere); }
static inline void glutSolidTorus(double inner_radius, double outer_radius, int sides, int rings) { GL_STUB_TRACE_LINE("glutSolidTorus %g %g %d %d\n", (double)inner_radius, (double)outer_radius, (int)sides, (int)rings); gl_stub_tick(GL_STUB_glutSolidTorus); }
static inline void glutWireTorus(double inner_radius, double outer_radius, int sides, int rings) { GL_STUB_TRACE_LINE("glutWireTorus %g %g %d %d\n", (double)inner_radius, (double)outer_radius, (int)sides, (int)rings); gl_stub_tick(GL_STUB_glutWireTorus); }
static inline void glutSolidCube(double size) { GL_STUB_TRACE_LINE("glutSolidCube %g\n", (double)size); gl_stub_tick(GL_STUB_glutSolidCube); }
static inline void glutWireCube(double size) { GL_STUB_TRACE_LINE("glutWireCube %g\n", (double)size); gl_stub_tick(GL_STUB_glutWireCube); }
static inline void glutSolidTeapot(double size) { GL_STUB_TRACE_LINE("glutSolidTeapot %g\n", (double)size); gl_stub_tick(GL_STUB_glutSolidTeapot); }
static inline void glutSolidCone(double base, double height, int slices, int stacks) { GL_STUB_TRACE_LINE("glutSolidCone %g %g %d %d\n", (double)base, (double)height, (int)slices, (int)stacks); gl_stub_tick(GL_STUB_glutSolidCone); }

#ifdef __cplusplus
}
#endif

#endif
