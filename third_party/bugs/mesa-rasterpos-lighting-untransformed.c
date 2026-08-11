/*
 * Mesa bug repro: the raster position is lit with untransformed inputs.
 * GL_CURRENT_RASTER_COLOR comes out wrong in two independent ways under the
 * transformed modelviews exercised below:
 *
 *   1. the vertex position fed to the lighting equation is the OBJECT-space
 *      position, not the eye-space one, so the vector to a positional light is
 *      wrong;
 *   2. GL_NORMALIZE is ignored on that path, so a modelview with a scale lights
 *      an unnormalized normal.
 *
 * Build (Linux):
 *     cc -std=c99 -O0 -o mesa-rasterpos-lighting-untransformed \
 *        mesa-rasterpos-lighting-untransformed.c -lglut -lGL -lGLU -lm
 *     ./mesa-rasterpos-lighting-untransformed
 *
 * macOS (for the cross-check; Apple GL passes both cases):
 *     cc -std=c99 -O0 -Wno-deprecated-declarations \
 *        -o mesa-rasterpos-lighting-untransformed \
 *        mesa-rasterpos-lighting-untransformed.c \
 *        -framework OpenGL -framework GLUT
 *
 * Exit status: 0 = conformant, 1 = bug reproduced, 77 = could not set up GL.
 *
 * No expected value is hardcoded. Each case compares the raster colour against
 * the colour this same driver assigns a VERTEX at the same position under the
 * identical state, captured with GL_FEEDBACK / GL_3D_COLOR. The spec says the
 * raster position's coordinates "are treated as if they were specified in a
 * Vertex command", so the two are required to agree - and on Mesa the vertex is
 * lit correctly while the raster position is not.
 *
 * Spec: OpenGL 2.1 (December 1, 2006), sec 2.13 "Current Raster Position",
 * p. 55.
 * https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf
 * (Wording carried through to the 4.6 compatibility profile unchanged.)
 *
 *   "The coordinates are treated as if they were specified in a Vertex
 *    command. [...] These coordinates, along with current values, are used to
 *    generate primary and secondary colors and texture coordinates just as is
 *    done for a vertex."
 *
 * Two further sentences decide the two cases, and neither is qualified for
 * RasterPos:
 *
 *   sec 2.14.1 "Lighting":  "All computations are carried out in eye
 *   coordinates."  -> case B: the vertex position entering the light vector is
 *   the eye-space one.
 *
 *   sec 2.11.3 "Normal Transformation": "Before use in lighting, normals are
 *   transformed to eye coordinates by a matrix derived from the model-view
 *   matrix. Rescaling and normalization operations are performed on the
 *   transformed normals to make them unit length prior to use in lighting."
 *   -> case C: with NORMALIZE enabled the normal is unit length before the
 *   equation sees it.
 *
 * Observed on Mesa 25.2.8 (iris, Intel ADL-N). Not reproducible on Apple GL
 * (2.1 Metal - 90.5) or NVIDIA 595.84, which both agree with the vertex.
 */
#ifdef __APPLE__
#include <GLUT/glut.h>
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#include <GL/glut.h>
#endif

#include <stdio.h>
#include <stdlib.h>

#define SPEC_CITE \
    "OpenGL 2.1 (2006-12-01), sec 2.13 " \
    "\"Current Raster Position\", p.55"
#define SPEC_URL \
    "https://registry.khronos.org/OpenGL/specs/gl/" \
    "glspec21.pdf"

static int g_failures;

static int close_enough(const GLfloat *a, const GLfloat *b) {
    int i;
    for (i = 0; i < 3; i++) {
        float d = a[i] - b[i];
        if (d < -0.005f || d > 0.005f)
            return 0;
    }
    return 1;
}

/* The raster colour the driver latches at the current position. Bails if the
 * position is clipped, where GL leaves the colour undefined. */
static void latch(float x, float y, float z, GLfloat *out) {
    GLboolean valid = GL_FALSE;

    glRasterPos3f(x, y, z);
    glGetBooleanv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
    if (!valid) {
        fprintf(stderr, "raster position clipped - result would be undefined\n");
        exit(77);
    }
    glGetFloatv(GL_CURRENT_RASTER_COLOR, out);
}

/* The oracle: the colour GL gives a vertex at the same place, same state.
 * GL_3D_COLOR writes the token, then x y z r g b a. */
static int lit_vertex_color(float x, float y, float z, GLfloat *out) {
    GLfloat buf[64];
    GLint written;
    int i;

    glFeedbackBuffer(64, GL_3D_COLOR, buf);
    glRenderMode(GL_FEEDBACK);
    glBegin(GL_POINTS);
    glVertex3f(x, y, z);
    glEnd();
    written = glRenderMode(GL_RENDER);
    if (written < 8)
        return 0;
    for (i = 0; i < 4; i++)
        out[i] = buf[4 + i];
    return 1;
}

static void compare(const char *label, const GLfloat *raster,
                    const GLfloat *vertex) {
    int ok = close_enough(raster, vertex);
    if (!ok)
        g_failures++;
    printf("  [%s] %-34s raster %.6f %.6f %.6f\n",
           ok ? "PASS" : "FAIL", label, raster[0], raster[1], raster[2]);
    printf("         %-34s vertex %.6f %.6f %.6f\n",
           "", vertex[0], vertex[1], vertex[2]);
}

static void reset_state(void) {
    GLfloat ambdef[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    GLfloat difdef[4] = { 0.8f, 0.8f, 0.8f, 1.0f };

    glDisable(GL_NORMALIZE);
    glDisable(GL_LIGHT0);
    glDisable(GL_LIGHTING);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, ambdef);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, difdef);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

int main(int argc, char **argv) {
    GLfloat diffuse[4] = { 0.9f, 0.9f, 0.9f, 1.0f };
    /* A POSITIONAL light (w = 1). With a directional light the vertex position
     * drops out of the light vector entirely and case A cannot see the bug. */
    GLfloat lightpos[4] = { 1.0f, 1.5f, 2.0f, 1.0f };
    GLfloat raster[4], vertex[4];

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DEPTH | GLUT_SINGLE);
    glutInitWindowSize(64, 64);
    if (glutCreateWindow("rasterpos lighting probe") <= 0) {
        fprintf(stderr, "cannot create a GL window\n");
        return 77;
    }

    printf("GL_VENDOR  : %s\n", (const char *)glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VERSION : %s\n\n", (const char *)glGetString(GL_VERSION));
    printf("Spec       : %s\n", SPEC_CITE);
    printf("             %s\n\n", SPEC_URL);

    /* Control: identity modelview. Both deviations need a transform, so this
     * must pass everywhere - it shows the harness itself is sound. */
    printf("Case A: identity modelview                    [control]\n");
    reset_state();
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glLightfv(GL_LIGHT0, GL_POSITION, lightpos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, diffuse);
    glNormal3f(0.0f, 0.7071f, 0.7071f);
    latch(0.1f, 0.2f, 0.1f, raster);
    if (!lit_vertex_color(0.1f, 0.2f, 0.1f, vertex))
        return 77;
    compare("identity", raster, vertex);
    printf("\n");

    /* Case B: the same, under a rotate + translate. The light vector must be
     * computed from the EYE-space position; Mesa uses the object-space one. */
    printf("Case B: rotated + translated modelview        [the bug]\n");
    reset_state();
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, diffuse);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
    /* The light position is transformed by the modelview in force when it is
     * SET, so set it under the identity - then the eye-space light is fixed and
     * only the vertex transform varies between this case and the control. */
    glLightfv(GL_LIGHT0, GL_POSITION, lightpos);
    glRotatef(40.0f, 0.0f, 1.0f, 0.0f);
    glTranslatef(0.2f, 0.3f, -0.4f);
    glNormal3f(0.0f, 0.7071f, 0.7071f);
    latch(0.1f, 0.2f, 0.1f, raster);
    if (!lit_vertex_color(0.1f, 0.2f, 0.1f, vertex))
        return 77;
    compare("transformed modelview", raster, vertex);
    printf("\n");

    /* Case C: GL_NORMALIZE under a scale. Scaling UP shrinks the normal via the
     * inverse transpose, so both the normalized and unnormalized results stay
     * inside [0,1] and the two models are distinguishable; scaling down
     * saturates the correct one at 1.0 and hides the difference. */
    printf("Case C: GL_NORMALIZE under glScalef(4)        [the bug]\n");
    reset_state();
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, diffuse);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
    glLightfv(GL_LIGHT0, GL_POSITION, lightpos);
    glEnable(GL_NORMALIZE);
    glScalef(4.0f, 4.0f, 4.0f);
    glNormal3f(0.0f, 0.0f, 1.0f);
    latch(0.0f, 0.0f, 0.0f, raster);
    if (!lit_vertex_color(0.0f, 0.0f, 0.0f, vertex))
        return 77;
    compare("normalize + scale", raster, vertex);
    printf("\n");

    if (g_failures) {
        printf("RESULT: BUG REPRODUCED - %d check(s) failed.\n\n", g_failures);
        printf("Case A passes, so the harness and the lighting setup are\n"
               "sound: with the identity modelview the raster position and the\n"
               "vertex agree. Introducing a transform (B) or a scale with\n"
               "GL_NORMALIZE (C) makes them diverge, while the vertex stays\n"
               "correct - the raster path is lighting untransformed inputs.\n\n");
        printf("Per %s\n", SPEC_CITE);
        printf("the raster position is \"treated as if [it] were specified in a\n"
               "Vertex command\", so every case must match its vertex.\n%s\n",
               SPEC_URL);
    } else {
        printf("RESULT: conformant.\n");
    }
    return g_failures ? 1 : 0;
}
