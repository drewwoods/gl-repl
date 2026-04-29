#ifndef OPENGL_VIBE_GL_STUB_COUNTS_H
#define OPENGL_VIBE_GL_STUB_COUNTS_H

/*
 * Per-function call counters for the stub GL/GLU/GLUT headers.
 *
 * The stub headers (include/GL/gl.h etc.) reference gl_stub_tick() and
 * the GL_STUB_* enum unconditionally, so the enum and tick function
 * must always be visible — otherwise any build that falls back to the
 * local stubs without defining OPENGL_VIBE_USE_GL_STUBS fails to
 * compile. Real storage only exists in stub builds; in every other
 * build gl_stub_tick() is a trivial no-op that the optimizer elides.
 *
 * Callers (the benchmark harness) snapshot and reset the array around
 * the region of interest to see which GL paths are actually exercised
 * — the stubs compile to no-ops so no wall-clock measurement tells you
 * that on its own.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* X-macro list of every counted stub. Add new entries here and the
 * enum / name table / tick macros below pick them up automatically. */
#define GL_STUB_COUNTER_LIST(X)        \
    X(glAccum)                         \
    X(glBegin)                         \
    X(glBindTexture)                   \
    X(glBlendFunc)                     \
    X(glClear)                         \
    X(glClearColor)                    \
    X(glColor3f)                       \
    X(glColor4dv)                      \
    X(glColor4f)                       \
    X(glColorMask)                     \
    X(glColorMaterial)                 \
    X(glDepthMask)                     \
    X(glDisable)                       \
    X(glEdgeFlag)                      \
    X(glEnable)                        \
    X(glEnd)                           \
    X(glFogf)                          \
    X(glFogfv)                         \
    X(glFogi)                          \
    X(glFrontFace)                     \
    X(glFrustum)                       \
    X(glGetFloatv)                     \
    X(glGetIntegerv)                   \
    X(glIsEnabled)                     \
    X(glLightfv)                       \
    X(glLightModelfv)                  \
    X(glLightModeli)                   \
    X(glLineStipple)                   \
    X(glLineWidth)                     \
    X(glLoadIdentity)                  \
    X(glLoadMatrixf)                   \
    X(glMaterialf)                     \
    X(glMaterialfv)                    \
    X(glMatrixMode)                    \
    X(glNormal3dv)                     \
    X(glNormal3f)                      \
    X(glPointParameterfv)              \
    X(glPointSize)                     \
    X(glPolygonMode)                   \
    X(glPolygonOffset)                 \
    X(glPopAttrib)                     \
    X(glPopMatrix)                     \
    X(glPushAttrib)                    \
    X(glPushMatrix)                    \
    X(glRasterPos2f)                   \
    X(glRasterPos3f)                   \
    X(glRectf)                         \
    X(glRotatef)                       \
    X(glScalef)                        \
    X(glScissor)                       \
    X(glShadeModel)                    \
    X(glTexCoord2f)                    \
    X(glTexEnvi)                       \
    X(glTranslatef)                    \
    X(glVertex2f)                      \
    X(glVertex3dv)                     \
    X(glVertex3f)                      \
    X(glVertex3fv)                     \
    X(glViewport)                      \
    X(gluLookAt)                       \
    X(gluNewQuadric)                   \
    X(gluDeleteQuadric)                \
    X(gluQuadricNormals)               \
    X(gluQuadricTexture)               \
    X(gluSphere)                       \
    X(gluCylinder)                     \
    X(gluDisk)                         \
    X(gluPartialDisk)                  \
    X(gluOrtho2D)                      \
    X(gluPerspective)                  \
    X(gluNewTess)                      \
    X(gluDeleteTess)                   \
    X(gluTessCallback)                 \
    X(gluTessBeginPolygon)             \
    X(gluTessEndPolygon)               \
    X(gluTessBeginContour)             \
    X(gluTessEndContour)               \
    X(gluTessVertex)                   \
    X(glutSolidSphere)                 \
    X(glutWireSphere)                  \
    X(glutSolidTorus)                  \
    X(glutWireTorus)                   \
    X(glutSolidCube)                   \
    X(glutWireCube)                    \
    X(glutSolidTeapot)                 \
    X(glutSolidCone)

enum {
#define GL_STUB_COUNT_ENUM_ENTRY(name) GL_STUB_##name,
    GL_STUB_COUNTER_LIST(GL_STUB_COUNT_ENUM_ENTRY)
#undef GL_STUB_COUNT_ENUM_ENTRY
    GL_STUB_COUNT_MAX
};

#ifdef OPENGL_VIBE_USE_GL_STUBS

#include <stdio.h>

extern unsigned long long gl_stub_counts[GL_STUB_COUNT_MAX];

/* The tick is the only thing on the hot path. Keep it trivial so the
 * compiler inlines it to a single INC on the counter slot. */
static inline void gl_stub_tick(int idx) {
    ++gl_stub_counts[idx];
}

const char *gl_stub_count_name(int idx);
void         gl_stub_counts_reset(void);

/* Dump all non-zero counters (one per line) to `out`. `divisor` scales
 * each counter — pass the number of outer operations to see per-op
 * averages, or 1 for raw totals. */
void gl_stub_counts_dump(FILE *out, const char *prefix, long long divisor);

#else  /* !OPENGL_VIBE_USE_GL_STUBS */

/* Non-stub builds: no storage, and gl_stub_tick() is a no-op so the
 * stub headers still compile if they happen to be picked up as a
 * fallback on a system without real GL headers. */
static inline void gl_stub_tick(int idx) { (void)idx; }

#endif  /* OPENGL_VIBE_USE_GL_STUBS */

#ifdef __cplusplus
}
#endif

#endif /* OPENGL_VIBE_GL_STUB_COUNTS_H */
