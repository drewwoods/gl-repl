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

static inline void glAccum(GLenum op, GLfloat value) { GL_STUB_TRACE_LINE("glAccum %u %g\n", (unsigned)op, (double)value); gl_stub_tick(GL_STUB_glAccum); }
static inline void glBegin(GLenum mode) { GL_STUB_TRACE_LINE("glBegin %u\n", (unsigned)mode); gl_stub_tick(GL_STUB_glBegin); }
static inline void glBindTexture(GLenum target, GLuint texture) { GL_STUB_TRACE_LINE("glBindTexture %u %u\n", (unsigned)target, (unsigned)texture); gl_stub_tick(GL_STUB_glBindTexture); }
static inline void glBlendFunc(GLenum sfactor, GLenum dfactor) { GL_STUB_TRACE_LINE("glBlendFunc %u %u\n", (unsigned)sfactor, (unsigned)dfactor); gl_stub_tick(GL_STUB_glBlendFunc); }
static inline void glClear(GLbitfield mask) { GL_STUB_TRACE_LINE("glClear %u\n", (unsigned)mask); gl_stub_tick(GL_STUB_glClear); }
static inline void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { GL_STUB_TRACE_LINE("glClearColor %g %g %g %g\n", (double)red, (double)green, (double)blue, (double)alpha); gl_stub_tick(GL_STUB_glClearColor); }
static inline void glColor3f(GLfloat red, GLfloat green, GLfloat blue) { GL_STUB_TRACE_LINE("glColor3f %g %g %g\n", (double)red, (double)green, (double)blue); gl_stub_tick(GL_STUB_glColor3f); }
static inline void glColor3fv(const GLfloat *v) { GL_STUB_TRACE_LINE("glColor3fv\n"); gl_stub_tick(GL_STUB_glColor3fv); (void)v; }
static inline void glColor4dv(const GLdouble *v) { GL_STUB_TRACE_LINE("glColor4dv\n"); gl_stub_tick(GL_STUB_glColor4dv); (void)v; }
static inline void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) { GL_STUB_TRACE_LINE("glColor4f %g %g %g %g\n", (double)red, (double)green, (double)blue, (double)alpha); gl_stub_tick(GL_STUB_glColor4f); }
static inline void glColor4fv(const GLfloat *v) { GL_STUB_TRACE_LINE("glColor4fv\n"); gl_stub_tick(GL_STUB_glColor4fv); (void)v; }
static inline void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) { GL_STUB_TRACE_LINE("glColorMask %u %u %u %u\n", (unsigned)red, (unsigned)green, (unsigned)blue, (unsigned)alpha); gl_stub_tick(GL_STUB_glColorMask); }
static inline void glColorMaterial(GLenum face, GLenum mode) { GL_STUB_TRACE_LINE("glColorMaterial %u %u\n", (unsigned)face, (unsigned)mode); gl_stub_tick(GL_STUB_glColorMaterial); }
static inline void glDepthMask(GLboolean flag) { GL_STUB_TRACE_LINE("glDepthMask %u\n", (unsigned)flag); gl_stub_tick(GL_STUB_glDepthMask); }
static inline void glDisable(GLenum cap) { GL_STUB_TRACE_LINE("glDisable %u\n", (unsigned)cap); gl_stub_tick(GL_STUB_glDisable); }
static inline void glEdgeFlag(GLboolean flag) { GL_STUB_TRACE_LINE("glEdgeFlag %u\n", (unsigned)flag); gl_stub_tick(GL_STUB_glEdgeFlag); }
static inline void glEnable(GLenum cap) { GL_STUB_TRACE_LINE("glEnable %u\n", (unsigned)cap); gl_stub_tick(GL_STUB_glEnable); }
static inline void glEnd(void) { GL_STUB_TRACE_LINE("glEnd\n"); gl_stub_tick(GL_STUB_glEnd); }
static inline void glFogf(GLenum pname, GLfloat param) { GL_STUB_TRACE_LINE("glFogf %u %g\n", (unsigned)pname, (double)param); gl_stub_tick(GL_STUB_glFogf); }
static inline void glFogfv(GLenum pname, const GLfloat *params) { GL_STUB_TRACE_LINE("glFogfv %u\n", (unsigned)pname); gl_stub_tick(GL_STUB_glFogfv); (void)params; }
static inline void glFogi(GLenum pname, GLint param) { GL_STUB_TRACE_LINE("glFogi %u %d\n", (unsigned)pname, (int)param); gl_stub_tick(GL_STUB_glFogi); }
static inline void glFrontFace(GLenum mode) { GL_STUB_TRACE_LINE("glFrontFace %u\n", (unsigned)mode); gl_stub_tick(GL_STUB_glFrontFace); }
static inline void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble z_near, GLdouble z_far) { GL_STUB_TRACE_LINE("glFrustum %g %g %g %g %g %g\n", (double)left, (double)right, (double)bottom, (double)top, (double)z_near, (double)z_far); gl_stub_tick(GL_STUB_glFrustum); }
static inline void glGetFloatv(GLenum pname, GLfloat *params) {
    GL_STUB_TRACE_LINE("glGetFloatv %u\n", (unsigned)pname);
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
    GL_STUB_TRACE_LINE("glGetIntegerv %u\n", (unsigned)pname);
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
static inline void glHint(GLenum target, GLenum mode) { GL_STUB_TRACE_LINE("glHint %u %u\n", (unsigned)target, (unsigned)mode); gl_stub_tick(GL_STUB_glHint); }
static inline GLboolean glIsEnabled(GLenum cap) { GL_STUB_TRACE_LINE("glIsEnabled %u\n", (unsigned)cap); gl_stub_tick(GL_STUB_glIsEnabled); return GL_FALSE; }
static inline void glLightfv(GLenum light, GLenum pname, const GLfloat *params) { GL_STUB_TRACE_LINE("glLightfv %u %u\n", (unsigned)light, (unsigned)pname); gl_stub_tick(GL_STUB_glLightfv); (void)params; }
static inline void glLightModelfv(GLenum pname, const GLfloat *params) { GL_STUB_TRACE_LINE("glLightModelfv %u\n", (unsigned)pname); gl_stub_tick(GL_STUB_glLightModelfv); (void)params; }
static inline void glLightModeli(GLenum pname, GLint param) { GL_STUB_TRACE_LINE("glLightModeli %u %d\n", (unsigned)pname, (int)param); gl_stub_tick(GL_STUB_glLightModeli); }
static inline void glLineStipple(GLint factor, GLushort pattern) { GL_STUB_TRACE_LINE("glLineStipple %d %u\n", (int)factor, (unsigned)pattern); gl_stub_tick(GL_STUB_glLineStipple); }
static inline void glLineWidth(GLfloat width) { GL_STUB_TRACE_LINE("glLineWidth %g\n", (double)width); gl_stub_tick(GL_STUB_glLineWidth); }
static inline void glLoadIdentity(void) { GL_STUB_TRACE_LINE("glLoadIdentity\n"); gl_stub_tick(GL_STUB_glLoadIdentity); }
static inline void glLoadMatrixf(const GLfloat *m) { GL_STUB_TRACE_LINE("glLoadMatrixf\n"); gl_stub_tick(GL_STUB_glLoadMatrixf); (void)m; }
static inline void glMaterialf(GLenum face, GLenum pname, GLfloat param) { GL_STUB_TRACE_LINE("glMaterialf %u %u %g\n", (unsigned)face, (unsigned)pname, (double)param); gl_stub_tick(GL_STUB_glMaterialf); }
static inline void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) { GL_STUB_TRACE_LINE("glMaterialfv %u %u\n", (unsigned)face, (unsigned)pname); gl_stub_tick(GL_STUB_glMaterialfv); (void)params; }
static inline void glMatrixMode(GLenum mode) { GL_STUB_TRACE_LINE("glMatrixMode %u\n", (unsigned)mode); gl_stub_tick(GL_STUB_glMatrixMode); }
static inline void glNormal3dv(const GLdouble *v) { GL_STUB_TRACE_LINE("glNormal3dv\n"); gl_stub_tick(GL_STUB_glNormal3dv); (void)v; }
static inline void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) { GL_STUB_TRACE_LINE("glNormal3f %g %g %g\n", (double)nx, (double)ny, (double)nz); gl_stub_tick(GL_STUB_glNormal3f); }
static inline void glPointParameterfv(GLenum pname, const GLfloat *params) { GL_STUB_TRACE_LINE("glPointParameterfv %u\n", (unsigned)pname); gl_stub_tick(GL_STUB_glPointParameterfv); (void)params; }
static inline void glPointSize(GLfloat size) { GL_STUB_TRACE_LINE("glPointSize %g\n", (double)size); gl_stub_tick(GL_STUB_glPointSize); }
static inline void glPolygonMode(GLenum face, GLenum mode) { GL_STUB_TRACE_LINE("glPolygonMode %u %u\n", (unsigned)face, (unsigned)mode); gl_stub_tick(GL_STUB_glPolygonMode); }
static inline void glPolygonOffset(GLfloat factor, GLfloat units) { GL_STUB_TRACE_LINE("glPolygonOffset %g %g\n", (double)factor, (double)units); gl_stub_tick(GL_STUB_glPolygonOffset); }
static inline void glPopAttrib(void) { GL_STUB_TRACE_LINE("glPopAttrib\n"); gl_stub_tick(GL_STUB_glPopAttrib); }
static inline void glPopMatrix(void) { GL_STUB_TRACE_LINE("glPopMatrix\n"); gl_stub_tick(GL_STUB_glPopMatrix); }
static inline void glPushAttrib(GLbitfield mask) { GL_STUB_TRACE_LINE("glPushAttrib %u\n", (unsigned)mask); gl_stub_tick(GL_STUB_glPushAttrib); }
static inline void glPushMatrix(void) { GL_STUB_TRACE_LINE("glPushMatrix\n"); gl_stub_tick(GL_STUB_glPushMatrix); }
static inline void glRasterPos2f(GLfloat x, GLfloat y) { GL_STUB_TRACE_LINE("glRasterPos2f %g %g\n", (double)x, (double)y); gl_stub_tick(GL_STUB_glRasterPos2f); }
static inline void glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) { GL_STUB_TRACE_LINE("glRasterPos3f %g %g %g\n", (double)x, (double)y, (double)z); gl_stub_tick(GL_STUB_glRasterPos3f); }
static inline void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) { GL_STUB_TRACE_LINE("glRectf %g %g %g %g\n", (double)x1, (double)y1, (double)x2, (double)y2); gl_stub_tick(GL_STUB_glRectf); }
static inline void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) { GL_STUB_TRACE_LINE("glRotatef %g %g %g %g\n", (double)angle, (double)x, (double)y, (double)z); gl_stub_tick(GL_STUB_glRotatef); }
static inline void glScalef(GLfloat x, GLfloat y, GLfloat z) { GL_STUB_TRACE_LINE("glScalef %g %g %g\n", (double)x, (double)y, (double)z); gl_stub_tick(GL_STUB_glScalef); }
static inline void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) { GL_STUB_TRACE_LINE("glScissor %d %d %d %d\n", (int)x, (int)y, (int)width, (int)height); gl_stub_tick(GL_STUB_glScissor); }
static inline void glShadeModel(GLenum mode) { GL_STUB_TRACE_LINE("glShadeModel %u\n", (unsigned)mode); gl_stub_tick(GL_STUB_glShadeModel); }
static inline void glTexCoord2f(GLfloat s, GLfloat t) { GL_STUB_TRACE_LINE("glTexCoord2f %g %g\n", (double)s, (double)t); gl_stub_tick(GL_STUB_glTexCoord2f); }
static inline void glTexEnvi(GLenum target, GLenum pname, GLint param) { GL_STUB_TRACE_LINE("glTexEnvi %u %u %d\n", (unsigned)target, (unsigned)pname, (int)param); gl_stub_tick(GL_STUB_glTexEnvi); }
static inline void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { GL_STUB_TRACE_LINE("glTranslatef %g %g %g\n", (double)x, (double)y, (double)z); gl_stub_tick(GL_STUB_glTranslatef); }
static inline void glVertex2f(GLfloat x, GLfloat y) { GL_STUB_TRACE_LINE("glVertex2f %g %g\n", (double)x, (double)y); gl_stub_tick(GL_STUB_glVertex2f); }
static inline void glVertex3dv(const GLdouble *v) { GL_STUB_TRACE_LINE("glVertex3dv\n"); gl_stub_tick(GL_STUB_glVertex3dv); (void)v; }
static inline void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { GL_STUB_TRACE_LINE("glVertex3f %g %g %g\n", (double)x, (double)y, (double)z); gl_stub_tick(GL_STUB_glVertex3f); }
static inline void glVertex3fv(const GLfloat *v) { GL_STUB_TRACE_LINE("glVertex3fv\n"); gl_stub_tick(GL_STUB_glVertex3fv); (void)v; }
static inline void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { GL_STUB_TRACE_LINE("glViewport %d %d %d %d\n", (int)x, (int)y, (int)width, (int)height); gl_stub_tick(GL_STUB_glViewport); }

#ifdef __cplusplus
}
#endif

#endif
