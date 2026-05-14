#ifndef STUB_GLU_H
#define STUB_GLU_H

#include <stddef.h>
#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GLUquadric GLUquadric;
typedef struct GLUtesselator GLUtesselator;
typedef void (CALLBACK *GLUfuncptr)(void);

struct GLUquadric { int stub; };
struct GLUtesselator { int stub; };

#define GLU_SMOOTH 100000
#define GLU_TESS_BEGIN 100100
#define GLU_TESS_VERTEX 100101
#define GLU_TESS_END 100102
#define GLU_TESS_ERROR 100103
#define GLU_TESS_EDGE_FLAG 100104
#define GLU_TESS_COMBINE 100105

static inline void gluLookAt(GLdouble eye_x, GLdouble eye_y, GLdouble eye_z,
                             GLdouble center_x, GLdouble center_y, GLdouble center_z,
                             GLdouble up_x, GLdouble up_y, GLdouble up_z) {
    GL_STUB_TRACE_LINE("gluLookAt %g %g %g %g %g %g %g %g %g\n",
        (double)eye_x, (double)eye_y, (double)eye_z,
        (double)center_x, (double)center_y, (double)center_z,
        (double)up_x, (double)up_y, (double)up_z);
    gl_stub_tick(GL_STUB_gluLookAt);
}

static inline GLUquadric *gluNewQuadric(void) {
    GL_STUB_TRACE_LINE("gluNewQuadric\n");
    gl_stub_tick(GL_STUB_gluNewQuadric);
    static GLUquadric quadric;
    return &quadric;
}

static inline void gluDeleteQuadric(GLUquadric *state) { GL_STUB_TRACE_LINE("gluDeleteQuadric\n"); gl_stub_tick(GL_STUB_gluDeleteQuadric); (void)state; }
static inline void gluQuadricNormals(GLUquadric *quad_object, GLenum normals) { GL_STUB_TRACE_LINE("gluQuadricNormals %u\n", (unsigned)normals); gl_stub_tick(GL_STUB_gluQuadricNormals); (void)quad_object; }
static inline void gluQuadricTexture(GLUquadric *quad_object, GLboolean texture_coords) { GL_STUB_TRACE_LINE("gluQuadricTexture %u\n", (unsigned)texture_coords); gl_stub_tick(GL_STUB_gluQuadricTexture); (void)quad_object; }
static inline void gluSphere(GLUquadric *quad_object, GLdouble radius, GLint slices, GLint stacks) { GL_STUB_TRACE_LINE("gluSphere %g %d %d\n", (double)radius, (int)slices, (int)stacks); gl_stub_tick(GL_STUB_gluSphere); (void)quad_object; }
static inline void gluCylinder(GLUquadric *quad_object, GLdouble base_radius, GLdouble top_radius, GLdouble height, GLint slices, GLint stacks) { GL_STUB_TRACE_LINE("gluCylinder %g %g %g %d %d\n", (double)base_radius, (double)top_radius, (double)height, (int)slices, (int)stacks); gl_stub_tick(GL_STUB_gluCylinder); (void)quad_object; }
static inline void gluDisk(GLUquadric *quad_object, GLdouble inner_radius, GLdouble outer_radius, GLint slices, GLint loops) { GL_STUB_TRACE_LINE("gluDisk %g %g %d %d\n", (double)inner_radius, (double)outer_radius, (int)slices, (int)loops); gl_stub_tick(GL_STUB_gluDisk); (void)quad_object; }
static inline void gluPartialDisk(GLUquadric *quad_object, GLdouble inner_radius, GLdouble outer_radius, GLint slices, GLint loops, GLdouble start_angle, GLdouble sweep_angle) { GL_STUB_TRACE_LINE("gluPartialDisk %g %g %d %d %g %g\n", (double)inner_radius, (double)outer_radius, (int)slices, (int)loops, (double)start_angle, (double)sweep_angle); gl_stub_tick(GL_STUB_gluPartialDisk); (void)quad_object; }
static inline void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top) { GL_STUB_TRACE_LINE("gluOrtho2D %g %g %g %g\n", (double)left, (double)right, (double)bottom, (double)top); gl_stub_tick(GL_STUB_gluOrtho2D); }
static inline void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble z_near, GLdouble z_far) { GL_STUB_TRACE_LINE("gluPerspective %g %g %g %g\n", (double)fovy, (double)aspect, (double)z_near, (double)z_far); gl_stub_tick(GL_STUB_gluPerspective); }

static inline GLUtesselator *gluNewTess(void) {
    GL_STUB_TRACE_LINE("gluNewTess\n");
    gl_stub_tick(GL_STUB_gluNewTess);
    static GLUtesselator tess;
    return &tess;
}

static inline void gluDeleteTess(GLUtesselator *tess) { GL_STUB_TRACE_LINE("gluDeleteTess\n"); gl_stub_tick(GL_STUB_gluDeleteTess); (void)tess; }
static inline void gluTessCallback(GLUtesselator *tess, GLenum which, GLUfuncptr callback) { GL_STUB_TRACE_LINE("gluTessCallback %u\n", (unsigned)which); gl_stub_tick(GL_STUB_gluTessCallback); (void)tess; (void)callback; }
static inline void gluTessBeginPolygon(GLUtesselator *tess, void *data) { GL_STUB_TRACE_LINE("gluTessBeginPolygon\n"); gl_stub_tick(GL_STUB_gluTessBeginPolygon); (void)tess; (void)data; }
static inline void gluTessEndPolygon(GLUtesselator *tess) { GL_STUB_TRACE_LINE("gluTessEndPolygon\n"); gl_stub_tick(GL_STUB_gluTessEndPolygon); (void)tess; }
static inline void gluTessBeginContour(GLUtesselator *tess) { GL_STUB_TRACE_LINE("gluTessBeginContour\n"); gl_stub_tick(GL_STUB_gluTessBeginContour); (void)tess; }
static inline void gluTessEndContour(GLUtesselator *tess) { GL_STUB_TRACE_LINE("gluTessEndContour\n"); gl_stub_tick(GL_STUB_gluTessEndContour); (void)tess; }
static inline void gluTessVertex(GLUtesselator *tess, GLdouble coords[3], void *data) { GL_STUB_TRACE_LINE("gluTessVertex %g %g %g\n", (double)coords[0], (double)coords[1], (double)coords[2]); gl_stub_tick(GL_STUB_gluTessVertex); (void)tess; (void)data; }

#ifdef __cplusplus
}
#endif

#endif
