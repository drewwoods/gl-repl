#ifndef GL_STUB_COUNTS_H
#define GL_STUB_COUNTS_H

/*
 * Per-function call counters for the stub GL/GLU/GLUT headers.
 *
 * The stub headers (tests/gl-stubs/include/GL/gl.h etc.) reference gl_stub_tick() and
 * the GL_STUB_* enum unconditionally, so the enum and tick function
 * must always be visible — otherwise any build that falls back to the
 * local stubs without defining GL_STUBS fails to
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
    X(glBitmap)                        \
    X(glPixelStorei)                   \
    X(glOrtho)                         \
    X(glGenTextures)                   \
    X(glDeleteTextures)                \
    X(glTexImage2D)                    \
    X(glTexSubImage2D)                 \
    X(glCopyTexSubImage2D)             \
    X(glTexParameteri)                 \
    X(glBlendFunc)                     \
    X(glBlendEquation)                 \
    X(glClear)                         \
    X(glClearColor)                    \
    X(glClearDepth)                    \
    X(glClearStencil)                  \
    X(glClipPlane)                     \
    X(glGetClipPlane)                  \
    X(glColor3f)                       \
    X(glColor3fv)                      \
    X(glColor4dv)                      \
    X(glColor4f)                       \
    X(glColor4fv)                      \
    X(glColorMask)                     \
    X(glColorMaterial)                 \
    X(glCullFace)                      \
    X(glDepthFunc)                     \
    X(glStencilFunc)                   \
    X(glStencilOp)                     \
    X(glStencilMask)                   \
    X(glDepthMask)                     \
    X(glDepthRange)                    \
    X(glDisable)                       \
    X(glEdgeFlag)                      \
    X(glEnable)                        \
    X(glEnd)                           \
    X(glGenLists)                      \
    X(glNewList)                       \
    X(glEndList)                       \
    X(glCallList)                      \
    X(glDeleteLists)                   \
    X(glFeedbackBuffer)                \
    X(glFogf)                          \
    X(glFogfv)                         \
    X(glFogi)                          \
    X(glFrontFace)                     \
    X(glFrustum)                       \
    X(glGetError)                      \
    X(glGetFloatv)                     \
    X(glGetIntegerv)                   \
    X(glFinish)                        \
    X(glHint)                          \
    X(glIsEnabled)                     \
    X(glLightfv)                       \
    X(glLightModelfv)                  \
    X(glLightModeli)                   \
    X(glLineStipple)                   \
    X(glLineWidth)                     \
    X(glLoadIdentity)                  \
    X(glLoadMatrixf)                   \
    X(glMultMatrixf)                   \
    X(glMaterialf)                     \
    X(glMaterialfv)                    \
    X(glMatrixMode)                    \
    X(glNormal3dv)                     \
    X(glNormal3f)                      \
    X(glTexCoord3f)                    \
    X(glPassThrough)                   \
    X(glPointParameterfv)              \
    X(glPointSize)                     \
    X(glPolygonMode)                   \
    X(glPolygonOffset)                 \
    X(glPopAttrib)                     \
    X(glPopMatrix)                     \
    X(glPushAttrib)                    \
    X(glPushMatrix)                    \
    X(glRasterPos2f)                   \
    X(glRasterPos2i)                   \
    X(glRasterPos3f)                   \
    X(glRectf)                         \
    X(glRenderMode)                    \
    X(glRotatef)                       \
    X(glScalef)                        \
    X(glScissor)                       \
    X(glShadeModel)                    \
    X(glTexCoord2f)                    \
    X(glTexEnvi)                       \
    X(glTranslatef)                    \
    X(glVertex2f)                      \
    X(glVertex2i)                      \
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
    X(glutSolidCone)                   \
    X(glutBitmapCharacter)             \
    X(glutBitmapString)

enum {
#define GL_STUB_COUNT_ENUM_ENTRY(name) GL_STUB_##name,
    GL_STUB_COUNTER_LIST(GL_STUB_COUNT_ENUM_ENTRY)
#undef GL_STUB_COUNT_ENUM_ENTRY
    GL_STUB_COUNT_MAX
};

#ifdef GL_STUBS

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

/* Per-call trace dump. When gl_stub_trace_fp is non-NULL every stub
 * fprintfs one "<name> <args>\n" line before incrementing its counter.
 * Default is NULL so the hot path is just a null-check; predicting the
 * common no-trace case costs effectively nothing.
 *
 * gl_stub_trace_open(path) fopen()s the path for writing, force-sets
 * LC_NUMERIC=C so float formatting is reproducible across hosts, and
 * stores the FILE* in gl_stub_trace_fp. gl_stub_trace_close() flushes
 * and clears. Both are safe to call repeatedly; opening twice closes
 * the previous file first.
 *
 * Format contract for the line:
 *   <symbol> <arg>...                — space-separated, %g for floats,
 *                                      integer codes for enums and
 *                                      bytes (so newlines inside text
 *                                      labels can't corrupt the trace).
 *   pointer-array args are omitted    — fp parity tests compare scalar
 *                                      args only; see
 *                                      docs/plans/done/gl-stub-extensions.md
 *                                      for the array-content sketch.
 *   glutBitmapStringByte <byte>       — one supplemental line per string byte,
 *                                      used to verify display-only text
 *                                      transforms without recording a fake
 *                                      glutBitmapCharacter call. */
extern FILE *gl_stub_trace_fp;
void gl_stub_trace_open(const char *path);
void gl_stub_trace_close(void);

#define GL_STUB_TRACE_LINE(...) do {                                \
        if (gl_stub_trace_fp) fprintf(gl_stub_trace_fp, __VA_ARGS__); \
    } while (0)

#else  /* !GL_STUBS */

/* Non-stub builds: no storage, and gl_stub_tick() is a no-op so the
 * stub headers still compile if they happen to be picked up as a
 * fallback on a system without real GL headers. */
static inline void gl_stub_tick(int idx) { (void)idx; }

#define GL_STUB_TRACE_LINE(...) ((void)0)

#endif  /* GL_STUBS */

#ifdef __cplusplus
}
#endif

#endif /* GL_STUB_COUNTS_H */
