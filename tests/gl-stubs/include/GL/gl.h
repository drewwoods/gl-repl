#ifndef STUB_GL_H
#define STUB_GL_H

/*
 * Minimal fixed-function OpenGL header for remote build environments that do
 * not have GL development packages installed. It intentionally implements
 * no-op functions: use it for compiling/tests, not for real rendering.
 *
 * Each stub calls gl_stub_tick() so headless benchmarks can snapshot how
 * many times each GL entry point was hit, even though the stubs
 * themselves do no wall-clock work.
 */

#include <GL/gl_stub_counts.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GLAPI
#define GLAPI extern
#endif
#ifndef GLAPIENTRY
#define GLAPIENTRY
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef CALLBACK
#define CALLBACK
#endif

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;

#define GL_FALSE 0
#define GL_TRUE 1

#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_LOOP 0x0002
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_QUADS 0x0007
#define GL_QUAD_STRIP 0x0008
#define GL_POLYGON 0x0009

#define GL_ACCUM 0x0100
#define GL_RETURN 0x0102

#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900
#define GL_CCW 0x0901

#define GL_POINT 0x1B00
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02

#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01

#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_ACCUM_BUFFER_BIT 0x00000200
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000

#define GL_CURRENT_BIT 0x00000001
#define GL_POINT_BIT 0x00000002
#define GL_LINE_BIT 0x00000004
#define GL_POLYGON_BIT 0x00000008
#define GL_LIGHTING_BIT 0x00000040
#define GL_ALL_ATTRIB_BITS 0x000fffff

#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702
#define GL_MATRIX_MODE 0x0BA0
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_TEXTURE_MATRIX 0x0BA8
#define GL_VIEWPORT 0x0BA2
#define GL_COLOR_CLEAR_VALUE 0x0C22

#define GL_ALPHA_TEST 0x0BC0
#define GL_BLEND 0x0BE2
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_FOG 0x0B60
#define GL_LIGHTING 0x0B50
#define GL_LINE_SMOOTH 0x0B20
#define GL_LINE_STIPPLE 0x0B24
#define GL_MULTISAMPLE 0x809D
#define GL_NORMALIZE 0x0BA1
#define GL_POINT_SMOOTH 0x0B10
#define GL_POLYGON_OFFSET_LINE 0x2A02
#define GL_SCISSOR_TEST 0x0C11
#define GL_STENCIL_TEST 0x0B90
#define GL_TEXTURE_2D 0x0DE1

#define GL_LIGHT0 0x4000
#define GL_LIGHT1 0x4001
#define GL_LIGHT2 0x4002
#define GL_LIGHT3 0x4003
#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_EMISSION 0x1600
#define GL_SHININESS 0x1601
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_COLOR_MATERIAL 0x0B57
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE 0x0B52

#define GL_FOG_MODE 0x0B65
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_COLOR 0x0B66
#define GL_LINEAR 0x2601
#define GL_EXP2 0x0801

#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1

#define GL_POINT_DISTANCE_ATTENUATION 0x8129

#define GL_FASTEST 0x1101
#define GL_NICEST 0x1102
#define GL_DONT_CARE 0x1100

#define GL_POINT_SMOOTH_HINT 0x0C51
#define GL_FOG_HINT 0x0C54

#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE0_ALPHA 0x8588
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND0_ALPHA 0x8598
#define GL_REPLACE 0x1E01

static inline void glAccum(GLenum op, GLfloat value) { gl_stub_tick(GL_STUB_glAccum); (void)op; (void)value; }
static inline void glBegin(GLenum mode) { gl_stub_tick(GL_STUB_glBegin); (void)mode; }
static inline void glBindTexture(GLenum target, GLuint texture) { gl_stub_tick(GL_STUB_glBindTexture); (void)target; (void)texture; }
static inline void glBlendFunc(GLenum sfactor, GLenum dfactor) { gl_stub_tick(GL_STUB_glBlendFunc); (void)sfactor; (void)dfactor; }
static inline void glClear(GLbitfield mask) { gl_stub_tick(GL_STUB_glClear); (void)mask; }
static inline void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { gl_stub_tick(GL_STUB_glClearColor); (void)red; (void)green; (void)blue; (void)alpha; }
static inline void glColor3f(GLfloat red, GLfloat green, GLfloat blue) { gl_stub_tick(GL_STUB_glColor3f); (void)red; (void)green; (void)blue; }
static inline void glColor3fv(const GLfloat *v) { gl_stub_tick(GL_STUB_glColor3fv); (void)v; }
static inline void glColor4dv(const GLdouble *v) { gl_stub_tick(GL_STUB_glColor4dv); (void)v; }
static inline void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) { gl_stub_tick(GL_STUB_glColor4f); (void)red; (void)green; (void)blue; (void)alpha; }
static inline void glColor4fv(const GLfloat *v) { gl_stub_tick(GL_STUB_glColor4fv); (void)v; }
static inline void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) { gl_stub_tick(GL_STUB_glColorMask); (void)red; (void)green; (void)blue; (void)alpha; }
static inline void glColorMaterial(GLenum face, GLenum mode) { gl_stub_tick(GL_STUB_glColorMaterial); (void)face; (void)mode; }
static inline void glDepthMask(GLboolean flag) { gl_stub_tick(GL_STUB_glDepthMask); (void)flag; }
static inline void glDisable(GLenum cap) { gl_stub_tick(GL_STUB_glDisable); (void)cap; }
static inline void glEdgeFlag(GLboolean flag) { gl_stub_tick(GL_STUB_glEdgeFlag); (void)flag; }
static inline void glEnable(GLenum cap) { gl_stub_tick(GL_STUB_glEnable); (void)cap; }
static inline void glEnd(void) { gl_stub_tick(GL_STUB_glEnd); }
static inline void glFogf(GLenum pname, GLfloat param) { gl_stub_tick(GL_STUB_glFogf); (void)pname; (void)param; }
static inline void glFogfv(GLenum pname, const GLfloat *params) { gl_stub_tick(GL_STUB_glFogfv); (void)pname; (void)params; }
static inline void glFogi(GLenum pname, GLint param) { gl_stub_tick(GL_STUB_glFogi); (void)pname; (void)param; }
static inline void glFrontFace(GLenum mode) { gl_stub_tick(GL_STUB_glFrontFace); (void)mode; }
static inline void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble z_near, GLdouble z_far) { gl_stub_tick(GL_STUB_glFrustum); (void)left; (void)right; (void)bottom; (void)top; (void)z_near; (void)z_far; }
static inline void glGetFloatv(GLenum pname, GLfloat *params) {
    gl_stub_tick(GL_STUB_glGetFloatv);
    if (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX) {
        for (int i = 0; i < 16; i++) params[i] = (i % 5) == 0 ? 1.0f : 0.0f;
    } else if (pname == GL_COLOR_CLEAR_VALUE) {
        params[0] = 0.0f;
        params[1] = 0.0f;
        params[2] = 0.0f;
        params[3] = 1.0f;
    } else {
        params[0] = 0.0f;
    }
}

static inline void glGetIntegerv(GLenum pname, GLint *params) {
    gl_stub_tick(GL_STUB_glGetIntegerv);
    if (pname == GL_VIEWPORT) {
        params[0] = 0;
        params[1] = 0;
        params[2] = 1024;
        params[3] = 768;
    } else if (pname == GL_MATRIX_MODE) {
        params[0] = GL_MODELVIEW;
    } else {
        params[0] = 0;
    }
}
static inline void glHint(GLenum target, GLenum mode) { gl_stub_tick(GL_STUB_glHint); (void)target; (void)mode; }
static inline GLboolean glIsEnabled(GLenum cap) { gl_stub_tick(GL_STUB_glIsEnabled); (void)cap; return GL_FALSE; }
static inline void glLightfv(GLenum light, GLenum pname, const GLfloat *params) { gl_stub_tick(GL_STUB_glLightfv); (void)light; (void)pname; (void)params; }
static inline void glLightModelfv(GLenum pname, const GLfloat *params) { gl_stub_tick(GL_STUB_glLightModelfv); (void)pname; (void)params; }
static inline void glLightModeli(GLenum pname, GLint param) { gl_stub_tick(GL_STUB_glLightModeli); (void)pname; (void)param; }
static inline void glLineStipple(GLint factor, GLushort pattern) { gl_stub_tick(GL_STUB_glLineStipple); (void)factor; (void)pattern; }
static inline void glLineWidth(GLfloat width) { gl_stub_tick(GL_STUB_glLineWidth); (void)width; }
static inline void glLoadIdentity(void) { gl_stub_tick(GL_STUB_glLoadIdentity); }
static inline void glLoadMatrixf(const GLfloat *m) { gl_stub_tick(GL_STUB_glLoadMatrixf); (void)m; }
static inline void glMaterialf(GLenum face, GLenum pname, GLfloat param) { gl_stub_tick(GL_STUB_glMaterialf); (void)face; (void)pname; (void)param; }
static inline void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) { gl_stub_tick(GL_STUB_glMaterialfv); (void)face; (void)pname; (void)params; }
static inline void glMatrixMode(GLenum mode) { gl_stub_tick(GL_STUB_glMatrixMode); (void)mode; }
static inline void glNormal3dv(const GLdouble *v) { gl_stub_tick(GL_STUB_glNormal3dv); (void)v; }
static inline void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) { gl_stub_tick(GL_STUB_glNormal3f); (void)nx; (void)ny; (void)nz; }
static inline void glPointParameterfv(GLenum pname, const GLfloat *params) { gl_stub_tick(GL_STUB_glPointParameterfv); (void)pname; (void)params; }
static inline void glPointSize(GLfloat size) { gl_stub_tick(GL_STUB_glPointSize); (void)size; }
static inline void glPolygonMode(GLenum face, GLenum mode) { gl_stub_tick(GL_STUB_glPolygonMode); (void)face; (void)mode; }
static inline void glPolygonOffset(GLfloat factor, GLfloat units) { gl_stub_tick(GL_STUB_glPolygonOffset); (void)factor; (void)units; }
static inline void glPopAttrib(void) { gl_stub_tick(GL_STUB_glPopAttrib); }
static inline void glPopMatrix(void) { gl_stub_tick(GL_STUB_glPopMatrix); }
static inline void glPushAttrib(GLbitfield mask) { gl_stub_tick(GL_STUB_glPushAttrib); (void)mask; }
static inline void glPushMatrix(void) { gl_stub_tick(GL_STUB_glPushMatrix); }
static inline void glRasterPos2f(GLfloat x, GLfloat y) { gl_stub_tick(GL_STUB_glRasterPos2f); (void)x; (void)y; }
static inline void glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) { gl_stub_tick(GL_STUB_glRasterPos3f); (void)x; (void)y; (void)z; }
static inline void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) { gl_stub_tick(GL_STUB_glRectf); (void)x1; (void)y1; (void)x2; (void)y2; }
static inline void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) { gl_stub_tick(GL_STUB_glRotatef); (void)angle; (void)x; (void)y; (void)z; }
static inline void glScalef(GLfloat x, GLfloat y, GLfloat z) { gl_stub_tick(GL_STUB_glScalef); (void)x; (void)y; (void)z; }
static inline void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) { gl_stub_tick(GL_STUB_glScissor); (void)x; (void)y; (void)width; (void)height; }
static inline void glShadeModel(GLenum mode) { gl_stub_tick(GL_STUB_glShadeModel); (void)mode; }
static inline void glTexCoord2f(GLfloat s, GLfloat t) { gl_stub_tick(GL_STUB_glTexCoord2f); (void)s; (void)t; }
static inline void glTexEnvi(GLenum target, GLenum pname, GLint param) { gl_stub_tick(GL_STUB_glTexEnvi); (void)target; (void)pname; (void)param; }
static inline void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { gl_stub_tick(GL_STUB_glTranslatef); (void)x; (void)y; (void)z; }
static inline void glVertex2f(GLfloat x, GLfloat y) { gl_stub_tick(GL_STUB_glVertex2f); (void)x; (void)y; }
static inline void glVertex3dv(const GLdouble *v) { gl_stub_tick(GL_STUB_glVertex3dv); (void)v; }
static inline void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { gl_stub_tick(GL_STUB_glVertex3f); (void)x; (void)y; (void)z; }
static inline void glVertex3fv(const GLfloat *v) { gl_stub_tick(GL_STUB_glVertex3fv); (void)v; }
static inline void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { gl_stub_tick(GL_STUB_glViewport); (void)x; (void)y; (void)width; (void)height; }

#ifdef __cplusplus
}
#endif

#endif
