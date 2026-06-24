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

#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207

#define GL_POINT 0x1B00
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02

/* Display-list compile modes (glNewList). */
#define GL_COMPILE 0x1300
#define GL_COMPILE_AND_EXECUTE 0x1301

#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01

#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_ACCUM_BUFFER_BIT 0x00000200
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000

#define GL_UNPACK_ALIGNMENT 0x0CF5

#define GL_CURRENT_BIT 0x00000001
#define GL_POINT_BIT 0x00000002
#define GL_LINE_BIT 0x00000004
#define GL_POLYGON_BIT 0x00000008
#define GL_POLYGON_STIPPLE_BIT 0x00000010
#define GL_PIXEL_MODE_BIT 0x00000020
#define GL_LIGHTING_BIT 0x00000040
#define GL_FOG_BIT 0x00000080
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_ACCUM_BUFFER_BIT 0x00000200
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_VIEWPORT_BIT 0x00000800
#define GL_TRANSFORM_BIT 0x00001000
#define GL_ENABLE_BIT 0x00002000
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_HINT_BIT 0x00008000
#define GL_EVAL_BIT 0x00010000
#define GL_LIST_BIT 0x00020000
#define GL_TEXTURE_BIT 0x00040000
#define GL_SCISSOR_BIT 0x00080000
#define GL_ALL_ATTRIB_BITS 0xFFFFFFFF

#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702
#define GL_MATRIX_MODE 0x0BA0
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_TEXTURE_MATRIX 0x0BA8
#define GL_VIEWPORT 0x0BA2
/* glGetString names — only GL_VERSION is consumed (runtime
 * point-parameter detection in glr_ctrl_init_gl); the rest round out
 * the enum so a real glGetString switch would still compile. */
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
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
#define GL_SAMPLES 0x80A9
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
#define GL_LIGHT4 0x4004
#define GL_LIGHT5 0x4005
#define GL_LIGHT6 0x4006
#define GL_LIGHT7 0x4007
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
#define GL_MODULATE 0x2100

/* Texture-capture post-processing filter (scene/postprocess_filter.c). */
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MAX_TEXTURE_SIZE 0x0D33

/* Feedback / render-mode (scene/render.c ortho-freeze eye-distance probe). */
#define GL_RENDER 0x1C00
#define GL_FEEDBACK 0x1C01
#define GL_SELECT 0x1C02
#define GL_2D 0x0600
#define GL_3D 0x0601
#define GL_3D_COLOR 0x0602
#define GL_3D_COLOR_TEXTURE 0x0603
#define GL_4D_COLOR_TEXTURE 0x0604
#define GL_PASS_THROUGH_TOKEN 0x0700
#define GL_POINT_TOKEN 0x0701
#define GL_LINE_TOKEN 0x0702
#define GL_POLYGON_TOKEN 0x0703
#define GL_BITMAP_TOKEN 0x0704
#define GL_DRAW_PIXEL_TOKEN 0x0705
#define GL_COPY_PIXEL_TOKEN 0x0706
#define GL_LINE_RESET_TOKEN 0x0707

static inline void glAccum(GLenum op, GLfloat value) { GL_STUB_TRACE_LINE("glAccum %u %g\n", (unsigned)op, (double)value); gl_stub_tick(GL_STUB_glAccum); }
static inline void glBegin(GLenum mode) { GL_STUB_TRACE_LINE("glBegin %u\n", (unsigned)mode); gl_stub_tick(GL_STUB_glBegin); }
static inline void glBindTexture(GLenum target, GLuint texture) { GL_STUB_TRACE_LINE("glBindTexture %u %u\n", (unsigned)target, (unsigned)texture); gl_stub_tick(GL_STUB_glBindTexture); }
static inline void glBitmap(GLsizei w, GLsizei h, GLfloat xo, GLfloat yo, GLfloat xm, GLfloat ym, const GLubyte *bits) { GL_STUB_TRACE_LINE("glBitmap %d %d\n", (int)w, (int)h); gl_stub_tick(GL_STUB_glBitmap); (void)xo; (void)yo; (void)xm; (void)ym; (void)bits; }
static inline void glPixelStorei(GLenum pname, GLint param) { GL_STUB_TRACE_LINE("glPixelStorei %u %d\n", (unsigned)pname, (int)param); gl_stub_tick(GL_STUB_glPixelStorei); }
static inline void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) { GL_STUB_TRACE_LINE("glOrtho %g %g %g %g %g %g\n", (double)l, (double)r, (double)b, (double)t, (double)n, (double)f); gl_stub_tick(GL_STUB_glOrtho); }
static inline void glDepthRange(GLclampd n, GLclampd f) { GL_STUB_TRACE_LINE("glDepthRange %g %g\n", (double)n, (double)f); gl_stub_tick(GL_STUB_glDepthRange); }
static inline void glGenTextures(GLsizei n, GLuint *textures) { GL_STUB_TRACE_LINE("glGenTextures %d\n", (int)n); gl_stub_tick(GL_STUB_glGenTextures); for (GLsizei i = 0; i < n; i++) textures[i] = 1; }
static inline void glDeleteTextures(GLsizei n, const GLuint *textures) { GL_STUB_TRACE_LINE("glDeleteTextures %d\n", (int)n); gl_stub_tick(GL_STUB_glDeleteTextures); (void)textures; }
static inline void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) { GL_STUB_TRACE_LINE("glTexImage2D %u %d %d %d %d\n", (unsigned)target, (int)level, (int)internalformat, (int)width, (int)height); gl_stub_tick(GL_STUB_glTexImage2D); (void)border; (void)format; (void)type; (void)pixels; }
static inline void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) { GL_STUB_TRACE_LINE("glCopyTexSubImage2D %u %d %d %d %d %d %d %d\n", (unsigned)target, (int)level, (int)xoffset, (int)yoffset, (int)x, (int)y, (int)width, (int)height); gl_stub_tick(GL_STUB_glCopyTexSubImage2D); }
static inline void glTexParameteri(GLenum target, GLenum pname, GLint param) { GL_STUB_TRACE_LINE("glTexParameteri %u %u %d\n", (unsigned)target, (unsigned)pname, (int)param); gl_stub_tick(GL_STUB_glTexParameteri); }
static inline void glBlendFunc(GLenum sfactor, GLenum dfactor) { GL_STUB_TRACE_LINE("glBlendFunc %u %u\n", (unsigned)sfactor, (unsigned)dfactor); gl_stub_tick(GL_STUB_glBlendFunc); }
static inline void glClear(GLbitfield mask) { GL_STUB_TRACE_LINE("glClear %u\n", (unsigned)mask); gl_stub_tick(GL_STUB_glClear); }
static inline void glClearDepth(GLclampd depth) { (void)depth; }
static inline void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { GL_STUB_TRACE_LINE("glClearColor %g %g %g %g\n", (double)red, (double)green, (double)blue, (double)alpha); gl_stub_tick(GL_STUB_glClearColor); }
static inline void glColor3f(GLfloat red, GLfloat green, GLfloat blue) { GL_STUB_TRACE_LINE("glColor3f %g %g %g\n", (double)red, (double)green, (double)blue); gl_stub_tick(GL_STUB_glColor3f); }
static inline void glColor3fv(const GLfloat *v) { GL_STUB_TRACE_LINE("glColor3fv\n"); gl_stub_tick(GL_STUB_glColor3fv); (void)v; }
static inline void glColor4dv(const GLdouble *v) { GL_STUB_TRACE_LINE("glColor4dv\n"); gl_stub_tick(GL_STUB_glColor4dv); (void)v; }
static inline void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) { GL_STUB_TRACE_LINE("glColor4f %g %g %g %g\n", (double)red, (double)green, (double)blue, (double)alpha); gl_stub_tick(GL_STUB_glColor4f); }
static inline void glColor4fv(const GLfloat *v) { GL_STUB_TRACE_LINE("glColor4fv\n"); gl_stub_tick(GL_STUB_glColor4fv); (void)v; }
static inline void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) { GL_STUB_TRACE_LINE("glColorMask %u %u %u %u\n", (unsigned)red, (unsigned)green, (unsigned)blue, (unsigned)alpha); gl_stub_tick(GL_STUB_glColorMask); }
static inline void glColorMaterial(GLenum face, GLenum mode) { GL_STUB_TRACE_LINE("glColorMaterial %u %u\n", (unsigned)face, (unsigned)mode); gl_stub_tick(GL_STUB_glColorMaterial); }
static inline void glDepthFunc(GLenum func) { GL_STUB_TRACE_LINE("glDepthFunc %u\n", (unsigned)func); gl_stub_tick(GL_STUB_glDepthFunc); }
static inline void glDepthMask(GLboolean flag) { GL_STUB_TRACE_LINE("glDepthMask %u\n", (unsigned)flag); gl_stub_tick(GL_STUB_glDepthMask); }
static inline void glDisable(GLenum cap) { GL_STUB_TRACE_LINE("glDisable %u\n", (unsigned)cap); gl_stub_tick(GL_STUB_glDisable); }
static inline void glEdgeFlag(GLboolean flag) { GL_STUB_TRACE_LINE("glEdgeFlag %u\n", (unsigned)flag); gl_stub_tick(GL_STUB_glEdgeFlag); }
static inline void glEnable(GLenum cap) { GL_STUB_TRACE_LINE("glEnable %u\n", (unsigned)cap); gl_stub_tick(GL_STUB_glEnable); }
static inline void glEnd(void) { GL_STUB_TRACE_LINE("glEnd\n"); gl_stub_tick(GL_STUB_glEnd); }
static inline GLuint glGenLists(GLsizei range) { GL_STUB_TRACE_LINE("glGenLists %d\n", (int)range); gl_stub_tick(GL_STUB_glGenLists); return 1; }
static inline void glNewList(GLuint list, GLenum mode) { GL_STUB_TRACE_LINE("glNewList %u %u\n", (unsigned)list, (unsigned)mode); gl_stub_tick(GL_STUB_glNewList); }
static inline void glEndList(void) { GL_STUB_TRACE_LINE("glEndList\n"); gl_stub_tick(GL_STUB_glEndList); }
static inline void glCallList(GLuint list) { GL_STUB_TRACE_LINE("glCallList %u\n", (unsigned)list); gl_stub_tick(GL_STUB_glCallList); }
static inline void glDeleteLists(GLuint list, GLsizei range) { GL_STUB_TRACE_LINE("glDeleteLists %u %d\n", (unsigned)list, (int)range); gl_stub_tick(GL_STUB_glDeleteLists); }
static inline void glFinish(void) { GL_STUB_TRACE_LINE("glFinish\n"); gl_stub_tick(GL_STUB_glFinish); }
static inline void glFogf(GLenum pname, GLfloat param) { GL_STUB_TRACE_LINE("glFogf %u %g\n", (unsigned)pname, (double)param); gl_stub_tick(GL_STUB_glFogf); }
static inline void glFogfv(GLenum pname, const GLfloat *params) { GL_STUB_TRACE_LINE("glFogfv %u\n", (unsigned)pname); gl_stub_tick(GL_STUB_glFogfv); (void)params; }
static inline void glFogi(GLenum pname, GLint param) { GL_STUB_TRACE_LINE("glFogi %u %d\n", (unsigned)pname, (int)param); gl_stub_tick(GL_STUB_glFogi); }
static inline void glFrontFace(GLenum mode) { GL_STUB_TRACE_LINE("glFrontFace %u\n", (unsigned)mode); gl_stub_tick(GL_STUB_glFrontFace); }
static inline void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble z_near, GLdouble z_far) { GL_STUB_TRACE_LINE("glFrustum %g %g %g %g %g %g\n", (double)left, (double)right, (double)bottom, (double)top, (double)z_near, (double)z_far); gl_stub_tick(GL_STUB_glFrustum); }
static inline void glFeedbackBuffer(GLsizei size, GLenum type, GLfloat *buffer) { GL_STUB_TRACE_LINE("glFeedbackBuffer %d %u\n", (int)size, (unsigned)type); gl_stub_tick(GL_STUB_glFeedbackBuffer); (void)buffer; }
static inline void glGetFloatv(GLenum pname, GLfloat *params) {
    GL_STUB_TRACE_LINE("glGetFloatv %u\n", (unsigned)pname);
    gl_stub_tick(GL_STUB_glGetFloatv);
    if (pname == GL_MODELVIEW_MATRIX) {
        extern float g_gl_stub_modelview_matrix[16];
        for (int i = 0; i < 16; i++) params[i] = g_gl_stub_modelview_matrix[i];
    } else if (pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX) {
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
    } else if (pname == GL_SAMPLES) {
        extern int g_gl_stub_samples;
        params[0] = g_gl_stub_samples;
    } else {
        params[0] = 0;
    }
}
static inline const GLubyte *glGetString(GLenum name) {
    (void)name;
    /* Major version 2 → the runtime point-parameter detection in
     * glr_ctrl_init_gl treats the stub context as "supported",
     * matching the glutExtensionSupported stub (returns 1) and
     * today's default build. Not counted/traced (no scalar args). */
    return (const GLubyte *)"2.1 stub";
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
static inline void glMultMatrixf(const GLfloat *m) { (void)m; }
static inline void glMaterialf(GLenum face, GLenum pname, GLfloat param) { GL_STUB_TRACE_LINE("glMaterialf %u %u %g\n", (unsigned)face, (unsigned)pname, (double)param); gl_stub_tick(GL_STUB_glMaterialf); }
static inline void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) { GL_STUB_TRACE_LINE("glMaterialfv %u %u\n", (unsigned)face, (unsigned)pname); gl_stub_tick(GL_STUB_glMaterialfv); (void)params; }
static inline void glMatrixMode(GLenum mode) { GL_STUB_TRACE_LINE("glMatrixMode %u\n", (unsigned)mode); gl_stub_tick(GL_STUB_glMatrixMode); }
static inline void glNormal3dv(const GLdouble *v) { GL_STUB_TRACE_LINE("glNormal3dv\n"); gl_stub_tick(GL_STUB_glNormal3dv); (void)v; }
static inline void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) { GL_STUB_TRACE_LINE("glNormal3f %g %g %g\n", (double)nx, (double)ny, (double)nz); gl_stub_tick(GL_STUB_glNormal3f); }
static inline void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) { GL_STUB_TRACE_LINE("glTexCoord3f %g %g %g\n", (double)s, (double)t, (double)r); gl_stub_tick(GL_STUB_glTexCoord3f); }
static inline void glPassThrough(GLfloat token) { GL_STUB_TRACE_LINE("glPassThrough %g\n", (double)token); gl_stub_tick(GL_STUB_glPassThrough); }
static inline void glPointParameterfv(GLenum pname, const GLfloat *params) { GL_STUB_TRACE_LINE("glPointParameterfv %u\n", (unsigned)pname); gl_stub_tick(GL_STUB_glPointParameterfv); (void)params; }
/* Timer-query entry points (GL 1.5 occlusion-query API + GL_EXT_timer_query's
 * 64-bit getter), consumed only through the runtime-loaded GPU-profiler path
 * (src/support/gpuprof.c via glutGetProcAddress) — never called directly by
 * app code, so they don't tick gl_stub counters. glGenQueries hands out ids
 * (callers key results on them); the getters write 0 so a stub "GPU result"
 * is never available and gpuprof simply reports no data. */
#define GL_QUERY_RESULT            0x8866
#define GL_QUERY_RESULT_AVAILABLE  0x8867
#define GL_TIME_ELAPSED_EXT        0x88BF
#define GL_TIMESTAMP               0x8E28
typedef unsigned long long GLuint64EXT;
static inline void glGenQueries(GLsizei n, GLuint *ids) { GLsizei i; for (i = 0; i < n; i++) ids[i] = (GLuint)(i + 1); }
static inline void glDeleteQueries(GLsizei n, const GLuint *ids) { (void)n; (void)ids; }
static inline void glBeginQuery(GLenum target, GLuint id) { (void)target; (void)id; }
static inline void glEndQuery(GLenum target) { (void)target; }
static inline void glQueryCounter(GLuint id, GLenum target) { (void)id; (void)target; }
static inline void glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params) { (void)id; (void)pname; if (params) *params = 0; }
static inline void glGetQueryObjectui64vEXT(GLuint id, GLenum pname, GLuint64EXT *params) { (void)id; (void)pname; if (params) *params = 0; }
static inline void glPointSize(GLfloat size) { GL_STUB_TRACE_LINE("glPointSize %g\n", (double)size); gl_stub_tick(GL_STUB_glPointSize); }
static inline void glPolygonMode(GLenum face, GLenum mode) { GL_STUB_TRACE_LINE("glPolygonMode %u %u\n", (unsigned)face, (unsigned)mode); gl_stub_tick(GL_STUB_glPolygonMode); }
static inline void glPolygonOffset(GLfloat factor, GLfloat units) { GL_STUB_TRACE_LINE("glPolygonOffset %g %g\n", (double)factor, (double)units); gl_stub_tick(GL_STUB_glPolygonOffset); }
static inline void glPopAttrib(void) { GL_STUB_TRACE_LINE("glPopAttrib\n"); gl_stub_tick(GL_STUB_glPopAttrib); }
static inline void glPopMatrix(void) { GL_STUB_TRACE_LINE("glPopMatrix\n"); gl_stub_tick(GL_STUB_glPopMatrix); }
static inline void glPushAttrib(GLbitfield mask) { GL_STUB_TRACE_LINE("glPushAttrib %u\n", (unsigned)mask); gl_stub_tick(GL_STUB_glPushAttrib); }
static inline void glPushMatrix(void) { GL_STUB_TRACE_LINE("glPushMatrix\n"); gl_stub_tick(GL_STUB_glPushMatrix); }
static inline void glRasterPos2f(GLfloat x, GLfloat y) { GL_STUB_TRACE_LINE("glRasterPos2f %g %g\n", (double)x, (double)y); gl_stub_tick(GL_STUB_glRasterPos2f); }
static inline void glRasterPos2i(GLint x, GLint y) { GL_STUB_TRACE_LINE("glRasterPos2i %d %d\n", (int)x, (int)y); gl_stub_tick(GL_STUB_glRasterPos2i); }
static inline void glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) { GL_STUB_TRACE_LINE("glRasterPos3f %g %g %g\n", (double)x, (double)y, (double)z); gl_stub_tick(GL_STUB_glRasterPos3f); }
static inline void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) { GL_STUB_TRACE_LINE("glRectf %g %g %g %g\n", (double)x1, (double)y1, (double)x2, (double)y2); gl_stub_tick(GL_STUB_glRectf); }
static inline GLint glRenderMode(GLenum mode) { GL_STUB_TRACE_LINE("glRenderMode %u\n", (unsigned)mode); gl_stub_tick(GL_STUB_glRenderMode); return 0; }
static inline void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) { GL_STUB_TRACE_LINE("glRotatef %g %g %g %g\n", (double)angle, (double)x, (double)y, (double)z); gl_stub_tick(GL_STUB_glRotatef); }
static inline void glScalef(GLfloat x, GLfloat y, GLfloat z) { GL_STUB_TRACE_LINE("glScalef %g %g %g\n", (double)x, (double)y, (double)z); gl_stub_tick(GL_STUB_glScalef); }
static inline void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) { GL_STUB_TRACE_LINE("glScissor %d %d %d %d\n", (int)x, (int)y, (int)width, (int)height); gl_stub_tick(GL_STUB_glScissor); }
static inline void glShadeModel(GLenum mode) { GL_STUB_TRACE_LINE("glShadeModel %u\n", (unsigned)mode); gl_stub_tick(GL_STUB_glShadeModel); }
static inline void glTexCoord2f(GLfloat s, GLfloat t) { GL_STUB_TRACE_LINE("glTexCoord2f %g %g\n", (double)s, (double)t); gl_stub_tick(GL_STUB_glTexCoord2f); }
static inline void glTexEnvi(GLenum target, GLenum pname, GLint param) { GL_STUB_TRACE_LINE("glTexEnvi %u %u %d\n", (unsigned)target, (unsigned)pname, (int)param); gl_stub_tick(GL_STUB_glTexEnvi); }
static inline void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { GL_STUB_TRACE_LINE("glTranslatef %g %g %g\n", (double)x, (double)y, (double)z); gl_stub_tick(GL_STUB_glTranslatef); }
static inline void glVertex2f(GLfloat x, GLfloat y) { GL_STUB_TRACE_LINE("glVertex2f %g %g\n", (double)x, (double)y); gl_stub_tick(GL_STUB_glVertex2f); }
static inline void glVertex2i(GLint x, GLint y) { GL_STUB_TRACE_LINE("glVertex2i %d %d\n", (int)x, (int)y); gl_stub_tick(GL_STUB_glVertex2i); }
static inline void glVertex3dv(const GLdouble *v) { GL_STUB_TRACE_LINE("glVertex3dv\n"); gl_stub_tick(GL_STUB_glVertex3dv); (void)v; }
static inline void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { GL_STUB_TRACE_LINE("glVertex3f %g %g %g\n", (double)x, (double)y, (double)z); gl_stub_tick(GL_STUB_glVertex3f); }
static inline void glVertex3fv(const GLfloat *v) { GL_STUB_TRACE_LINE("glVertex3fv\n"); gl_stub_tick(GL_STUB_glVertex3fv); (void)v; }
static inline void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { GL_STUB_TRACE_LINE("glViewport %d %d %d %d\n", (int)x, (int)y, (int)width, (int)height); gl_stub_tick(GL_STUB_glViewport); }

#ifdef __cplusplus
}
#endif

#endif
