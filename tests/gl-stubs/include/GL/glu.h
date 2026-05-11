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
    gl_stub_tick(GL_STUB_gluLookAt);
    (void)eye_x; (void)eye_y; (void)eye_z; (void)center_x; (void)center_y;
    (void)center_z; (void)up_x; (void)up_y; (void)up_z;
}

static inline GLUquadric *gluNewQuadric(void) {
    gl_stub_tick(GL_STUB_gluNewQuadric);
    static GLUquadric quadric;
    return &quadric;
}

static inline void gluDeleteQuadric(GLUquadric *state) { gl_stub_tick(GL_STUB_gluDeleteQuadric); (void)state; }
static inline void gluQuadricNormals(GLUquadric *quad_object, GLenum normals) { gl_stub_tick(GL_STUB_gluQuadricNormals); (void)quad_object; (void)normals; }
static inline void gluQuadricTexture(GLUquadric *quad_object, GLboolean texture_coords) { gl_stub_tick(GL_STUB_gluQuadricTexture); (void)quad_object; (void)texture_coords; }
static inline void gluSphere(GLUquadric *quad_object, GLdouble radius, GLint slices, GLint stacks) { gl_stub_tick(GL_STUB_gluSphere); (void)quad_object; (void)radius; (void)slices; (void)stacks; }
static inline void gluCylinder(GLUquadric *quad_object, GLdouble base_radius, GLdouble top_radius, GLdouble height, GLint slices, GLint stacks) { gl_stub_tick(GL_STUB_gluCylinder); (void)quad_object; (void)base_radius; (void)top_radius; (void)height; (void)slices; (void)stacks; }
static inline void gluDisk(GLUquadric *quad_object, GLdouble inner_radius, GLdouble outer_radius, GLint slices, GLint loops) { gl_stub_tick(GL_STUB_gluDisk); (void)quad_object; (void)inner_radius; (void)outer_radius; (void)slices; (void)loops; }
static inline void gluPartialDisk(GLUquadric *quad_object, GLdouble inner_radius, GLdouble outer_radius, GLint slices, GLint loops, GLdouble start_angle, GLdouble sweep_angle) { gl_stub_tick(GL_STUB_gluPartialDisk); (void)quad_object; (void)inner_radius; (void)outer_radius; (void)slices; (void)loops; (void)start_angle; (void)sweep_angle; }
static inline void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top) { gl_stub_tick(GL_STUB_gluOrtho2D); (void)left; (void)right; (void)bottom; (void)top; }
static inline void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble z_near, GLdouble z_far) { gl_stub_tick(GL_STUB_gluPerspective); (void)fovy; (void)aspect; (void)z_near; (void)z_far; }

static inline GLUtesselator *gluNewTess(void) {
    gl_stub_tick(GL_STUB_gluNewTess);
    static GLUtesselator tess;
    return &tess;
}

static inline void gluDeleteTess(GLUtesselator *tess) { gl_stub_tick(GL_STUB_gluDeleteTess); (void)tess; }
static inline void gluTessCallback(GLUtesselator *tess, GLenum which, GLUfuncptr callback) { gl_stub_tick(GL_STUB_gluTessCallback); (void)tess; (void)which; (void)callback; }
static inline void gluTessBeginPolygon(GLUtesselator *tess, void *data) { gl_stub_tick(GL_STUB_gluTessBeginPolygon); (void)tess; (void)data; }
static inline void gluTessEndPolygon(GLUtesselator *tess) { gl_stub_tick(GL_STUB_gluTessEndPolygon); (void)tess; }
static inline void gluTessBeginContour(GLUtesselator *tess) { gl_stub_tick(GL_STUB_gluTessBeginContour); (void)tess; }
static inline void gluTessEndContour(GLUtesselator *tess) { gl_stub_tick(GL_STUB_gluTessEndContour); (void)tess; }
static inline void gluTessVertex(GLUtesselator *tess, GLdouble coords[3], void *data) { gl_stub_tick(GL_STUB_gluTessVertex); (void)tess; (void)coords; (void)data; }

#ifdef __cplusplus
}
#endif

#endif
