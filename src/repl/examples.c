#include "repl/examples.h"

#include <stddef.h>

/* Each example is an array of source lines terminated by NULL.
 * Lines are processed sequentially:
 *   "for(...) {"  -> CMD_FOR_BEGIN + CMD_FOR_END, enters block
 *   "funcN {"     -> CMD_FUNC_DEF + CMD_FUNC_END, enters block
 *   "if(...) {"   -> CMD_IF_BEGIN + CMD_IF_END, enters block
 *   "}"           -> closes current block
 *   "x = expr;"   -> CMD_VAR_ASSIGN
 *   "funcN()"     -> CMD_CALL
 *   anything else -> parse_command() as a regular GL command
 *
 * Predefined examples can optionally start with contiguous scene-presentation
 * config metadata lines using the exported-file format:
 *   "// @cfg axes = 4"
 *   "// @cfg vertex_outlines = 0"
 * Supported slugs are limited to visual scene settings such as wireframe,
 * grid, axes, vertex overlays, backdrop, and camera_rotate. These metadata
 * lines are consumed by the example loader and are not shown in the code panel.
 *
 * A camera preset may then follow immediately after the cfg lines:
 *   "// camera"
 *   "glTranslatef(0.0f, 0.0f, -dist);"
 *   "glRotatef(rx, 1.0f, 0.0f, 0.0f);"
 *   "glRotatef(ry, 0.0f, 1.0f, 0.0f);"
 *   "glTranslatef(-tx, -ty, -tz);"
 * Both cfg and camera metadata are consumed by the example loader and are not
 * shown in the code panel.
 */

/* Example 0: Lit cube (default) */
static const char *const g_example_cube[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_NORMALIZE);",
    "glShadeModel(GL_SMOOTH);",
    "glEnable(GL_LIGHT3);",
    "glEnable(GL_LIGHT2);",
    "//glEnable(GL_LINE_SMOOTH);",
    "glColor3f(1, 1, 1);",
    "glBegin(GL_QUAD_STRIP);",
    "glNormal3f(0, 1, 0);",
    "glVertex3f(1, 1, 1);",
    "glNormal3f(0, 1, 0);",
    "glVertex3f(-1, 1, 1);",
    "glNormal3f(0, 0, 1);",
    "glVertex3f(1, -1, 1);",
    "glNormal3f(0, 0, 1);",
    "glVertex3f(-1, -1, 1);",
    "glNormal3f(-0, -1, -0);",
    "glVertex3f(1, -1, -1);",
    "glNormal3f(-0, -1, -0);",
    "glVertex3f(-1, -1, -1);",
    "glNormal3f(0, 0, -1);",
    "glVertex3f(1, 1, -1);",
    "glNormal3f(0, 0, -1);",
    "glVertex3f(-1, 1, -1);",
    "glNormal3f(0, 1, -0);",
    "glVertex3f(1, 1, 1);",
    "glNormal3f(0, 1, -0);",
    "glVertex3f(-1, 1, 1);",
    "glEnd();",
    NULL
};

/* Example 1: Animated ring - for-loop + t variable */
static const char *const g_example_ring[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glEnable(GL_DEPTH_TEST);",
    "glBegin(GL_LINE_LOOP);",
    "for(i, 0, 48) {",
        "glColor3f(sin(i*TAU/48)*0.5+0.5, cos(i*TAU/48)*0.5+0.5, 0.5);",
        "glVertex3f(cos(i*TAU/48+t)*2, sin(i*TAU/48+t)*2, sin(i*TAU/12+t)*0.5);",
    "}",
    "glEnd();",
    "glBegin(GL_TRIANGLE_FAN);",
    "glColor3f(0.2, 0.5, 1);",
    "glVertex3f(0, 0, 0);",
    "for(i, 0, 25) {",
        "glColor3f(sin(i*TAU/24+t)*0.5+0.5, 0.3, cos(i*TAU/24+t)*0.5+0.5);",
        "glVertex3f(cos(i*TAU/24)*1.5, sin(i*TAU/24)*1.5, 0);",
    "}",
    "glEnd();",
    NULL
};

/* Example 2: Function demo - define a named reusable function (alias for a
 * funcN slot), call it repeatedly with transforms between calls. */
static const char *const g_example_func[] = {
    "// @cfg vertex_points = 0",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glEnable(GL_DEPTH_TEST);",
    "triangle() {",
        "glBegin(GL_TRIANGLES);",
        "glNormal3f(0, 0, 1);",
        "glVertex3f(0, 0.8, 0);",
        "glVertex3f(-0.7, -0.4, 0);",
        "glVertex3f(0.7, -0.4, 0);",
        "glEnd();",
    "}",
    "glColor3f(1, 0.3, 0.3);",
    "triangle();",
    "glTranslatef(2, 0, 0);",
    "glColor3f(0.3, 1, 0.3);",
    "triangle();",
    "glTranslatef(-4, 0, 0);",
    "glColor3f(0.3, 0.3, 1);",
    "triangle();",
    NULL
};

/* Example 3: Parametric polygon helper - function args driving local for-loops */
static const char *const g_example_func_loop[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_NORMALIZE);",
    "polygon(radius, sides, phase) {",
        "glBegin(GL_TRIANGLE_FAN);",
        "glNormal3f(0, 0, 1);",
        "glColor3f(1, 1, 1);",
        "glVertex3f(0, 0, 0);",
        "for(i, 0, sides + 1) {",
            "glColor3f(sin(i*TAU/sides + phase)*0.5+0.5, cos(i*TAU/sides + phase)*0.5+0.5, 0.8);",
            "glVertex3f(cos(i*TAU/sides + phase)*radius, sin(i*TAU/sides + phase)*radius, 0);",
        "}",
        "glEnd();",
    "}",
    "glPushMatrix();",
    "glTranslatef(-2.4, 0, 0);",
    "polygon(0.9, 3, t*0.4);",
    "glPopMatrix();",
    "glPushMatrix();",
    "polygon(0.9, 6, -t*0.25);",
    "glPopMatrix();",
    "glPushMatrix();",
    "glTranslatef(2.4, 0, 0);",
    "polygon(0.9, 10, t*0.15);",
    "glPopMatrix();",
    NULL
};

/* Example 4: Branching helper - function args driving local if-blocks */
static const char *const g_example_func_if[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_NORMALIZE);",
    "shape(scale, phase) {",
        "if(scale > 1) {",
            "glColor3f(1, 0.35, 0.2);",
            "glBegin(GL_QUADS);",
            "glNormal3f(0, 0, 1);",
            "glVertex3f(-0.7*scale, -0.7*scale, 0);",
            "glVertex3f(0.7*scale, -0.7*scale, 0);",
            "glVertex3f(0.7*scale, 0.7*scale, 0);",
            "glVertex3f(-0.7*scale, 0.7*scale, 0);",
            "glEnd();",
        "}",
        "if(scale <= 1) {",
            "glColor3f(0.2, 0.75, 1);",
            "glBegin(GL_TRIANGLES);",
            "glNormal3f(0, 0, 1);",
            "glVertex3f(cos(phase)*0.9*scale, sin(phase)*0.9*scale, 0);",
            "glVertex3f(cos(phase+TAU/3)*0.9*scale, sin(phase+TAU/3)*0.9*scale, 0);",
            "glVertex3f(cos(phase+2*TAU/3)*0.9*scale, sin(phase+2*TAU/3)*0.9*scale, 0);",
            "glEnd();",
        "}",
    "}",
    "glPushMatrix();",
    "glTranslatef(-2.2, 0, 0);",
    "shape(0.9, t);",
    "glPopMatrix();",
    "glPushMatrix();",
    "shape(1.4, t*0.5);",
    "glPopMatrix();",
    "glPushMatrix();",
    "glTranslatef(2.2, 0, 0);",
    "shape(0.65, -t*0.8);",
    "glPopMatrix();",
    NULL
};

/* Example 5: Recursive helper - transformed child calls with depth countdown */
static const char *const g_example_func_recurse[] = {
    "// @cfg vertex_outlines = 0",
    "// @cfg vertex_points = 0",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_NORMALIZE);",
    "// Recursive triangle tree: child calls shrink and rotate from the parent",
    "branch(depth, size, spin) {",
        "glColor3f(0.25 + depth*0.14, 0.45 + 0.2*sin(spin), 1 - depth*0.12);",
        "glBegin(GL_LINE_LOOP);",
        "glNormal3f(0, 0, 1);",
        "glVertex3f(0, size, 0);",
        "glVertex3f(-0.866*size, -0.5*size, 0);",
        "glVertex3f(0.866*size, -0.5*size, 0);",
        "glEnd();",
        "if(depth <= 0) {",
            "glColor3f(1, 0.8, 0.2);",
            "glBegin(GL_TRIANGLES);",
            "glNormal3f(0, 0, 1);",
            "glVertex3f(0, size*0.55, 0);",
            "glVertex3f(-0.48*size, -0.25*size, 0);",
            "glVertex3f(0.48*size, -0.25*size, 0);",
            "glEnd();",
        "}",
        "if(depth > 0) {",
            "glPushMatrix();",
            "glTranslatef(0, size*1.02, 0);",
            "glRotatef(18 + spin*14, 0, 0, 1);",
            "branch(depth - 1, size*0.62, spin + 0.15);",
            "glPopMatrix();",
            "glPushMatrix();",
            "glTranslatef(-size*0.9, -size*0.38, 0);",
            "glRotatef(-30 - spin*12, 0, 0, 1);",
            "branch(depth - 1, size*0.58, spin + 0.12);",
            "glPopMatrix();",
            "glPushMatrix();",
            "glTranslatef(size*0.9, -size*0.38, 0);",
            "glRotatef(30 + spin*12, 0, 0, 1);",
            "branch(depth - 1, size*0.58, spin + 0.12);",
            "glPopMatrix();",
        "}",
    "}",
    "glRotatef(sin(t*0.35)*12, 0, 0, 1);",
    "branch(4, 1.05, sin(t*0.4));",
    NULL
};

/* Example 6: Conditional colors - if-blocks + t variable */
static const char *const g_example_cond[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "glBegin(GL_QUADS);",
    "if(sin(t) > 0) {",
        "glColor3f(1, 0.2, 0.2);",
    "}",
    "if(sin(t) <= 0) {",
        "glColor3f(0.2, 0.2, 1);",
    "}",
    "glNormal3f(0, 0, 1);",
    "glVertex3f(-1, -1, 0);",
    "glVertex3f(1, -1, 0);",
    "glVertex3f(1, 1, 0);",
    "glVertex3f(-1, 1, 0);",
    "if(cos(t) > 0) {",
        "glColor3f(0.2, 1, 0.2);",
    "}",
    "if(cos(t) <= 0) {",
        "glColor3f(1, 1, 0.2);",
    "}",
    "glNormal3f(0, 0, -1);",
    "glVertex3f(-1, -1, -1);",
    "glVertex3f(-1, 1, -1);",
    "glVertex3f(1, 1, -1);",
    "glVertex3f(1, -1, -1);",
    "glEnd();",
    NULL
};

/* Example 7: Parametric torus - nested for-loops */
static const char *const g_example_torus[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glEnable(GL_DEPTH_TEST);",
    "//glEnable(GL_LIGHTING);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "glEnable(GL_LIGHT2);",
    "glShadeModel(GL_SMOOTH);",
    "for(i, 0, 24) {",
        "glBegin(GL_QUAD_STRIP);",
        "for(j, 0, 25) {",
            "glColor3f(sin(i*TAU/24)*0.4+0.6, cos(j*TAU/24)*0.4+0.6, 0.5);",
            "glVertex3f((2+cos(j*TAU/24))*cos(i*TAU/24), (2+cos(j*TAU/24))*sin(i*TAU/24), sin(j*TAU/24));",
            "glVertex3f((2+cos(j*TAU/24))*cos((i+1)*TAU/24), (2+cos(j*TAU/24))*sin((i+1)*TAU/24), sin(j*TAU/24));",
        "}",
        "glEnd();",
    "}",
    NULL
};

/* Example 8: GLU tessellator - concave arrow polygon with per-vertex color */
static const char *const g_example_tess[] = {
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "glEnable(GL_LIGHT2);",
    "glShadeModel(GL_SMOOTH);",
    "glFrontFace(GL_CW);",
    "// Arrow shape - concave, tessellated with per-vertex color",
    "gluBegin(GLU_POLYGON);",
    "gluBegin(GLU_CONTOUR);",
    "gluNormal(0, 0, 1);",
    "gluColor(0.2, 0.4, 1, 1);",
    "gluVertex(-1.2, -0.45, 0);",
    "gluColor(0.2, 0.4, 1, 1);",
    "gluVertex(-1.2, 0.45, 0);",
    "gluColor(0.55, 0.3, 1, 1);",
    "gluVertex(0, 0.45, 0);",
    "gluColor(1, 0.45, 0.05, 1);",
    "gluVertex(0, 1.1, 0);",
    "gluColor(1, 0.9, 0.1, 1);",
    "gluVertex(1.3, 0, 0);",
    "gluColor(1, 0.45, 0.05, 1);",
    "gluVertex(0, -1.1, 0);",
    "gluColor(0.55, 0.3, 1, 1);",
    "gluVertex(0, -0.45, 0);",
    "gluEnd();",
    "gluEnd();",
    NULL
};

/* Example 9: GLU tessellator - concave arrow polygon cutout */
static const char *const g_example_tess_cutout[] = {
    "static float z;",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "glEnable(GL_LIGHT2);",
    "glShadeModel(GL_SMOOTH);",
    "glFrontFace(GL_CW);",
    "z = -0.55;",
    "glRotatef(10*t, 0, 0, 1);",
    "glScalef(0.5, 0.5, 0.5);",
    "// Arrow shape - concave, tessellated with per-vertex color",
    "gluBegin(GLU_POLYGON);",
    "  gluBegin(GLU_CONTOUR);",
    "    glBegin(GL_QUAD_STRIP);",
    "    gluNormal(0, 0, 1);",
    "  // Side 0",
    "    gluColor(0.2, 0.4, 1, 1);",
    "      glColor3f(0.2, 0.4, 1);",
    "      glNormal3f(-1, 0, 0);",
    "      glVertex3f(-1.2, -0.45, 0);",
    "      glVertex3f(-1.2, -0.45, z);",
    "    gluVertex(-1.2, -0.45, 0);",
    "  // Side 1",
    "    gluColor(0.2, 0.4, 1, 1);",
    "      glColor3f(0.2, 0.4, 1);",
    "      glNormal3f(0, 1, 0);",
    "      glVertex3f(-1.2, 0.45, 0);",
    "      glVertex3f(-1.2, 0.45, z);",
    "    gluVertex(-1.2, 0.45, 0);",
    "  // Side 2",
    "    gluColor(0.55, 0.3, 1, 1);",
    "      glColor3f(0.55, 0.3, 1);",
    "      glNormal3f(-1, 1, 0);",
    "      glVertex3f(0, 0.45, 0);",
    "      glVertex3f(0, 0.45, z);",
    "    gluVertex(0, 0.45, 0);",
    "  // Side 3",
    "    gluColor(1, 0.45, 0.05, 1);",
    "      glColor3f(1, 0.45, 0.05);",
    "      glNormal3f(-1, 0, 0);",
    "      glVertex3f(0, 1.1, 0);",
    "      glVertex3f(0, 1.1, z);",
    "    gluVertex(0, 1.1, 0);",
    "  // Side 4",
    "    gluColor(1, 0.9, 0.1, 1);",
    "      glColor3f(1, 0.9, 0.1);",
    "      glNormal3f(1, 0, 0);",
    "      glVertex3f(1.3, 0, 0);",
    "      glVertex3f(1.3, 0, z);",
    "    gluVertex(1.3, 0, 0);",
    "  // Side 5",
    "    gluColor(1, 0.45, 0.05, 1);",
    "      glColor3f(1, 0.45, 0.05);",
    "      glNormal3f(-1, 0, 0);",
    "      glVertex3f(0, -1.1, 0);",
    "      glVertex3f(0, -1.1, z);",
    "    gluVertex(0, -1.1, 0);",
    "  // Side 6",
    "    gluColor(0.55, 0.3, 1, 1);",
    "      glColor3f(0.55, 0.3, 1);",
    "      glNormal3f(-1, -1, 0);",
    "      glVertex3f(0, -0.45, 0);",
    "      glVertex3f(0, -0.45, z);",
    "    gluVertex(0, -0.45, 0);",
    "  // Quade side 7",
    "      glColor3f(0.2, 0.4, 1);",
    "      glNormal3f(0, -1, 0);",
    "      glVertex3f(-1.2, -0.45, 0);",
    "      glVertex3f(-1.2, -0.45, z);",
    "    glEnd();",
    "  gluEnd();",
    "  gluBegin(GLU_CONTOUR);",
    "    glBegin(GL_QUAD_STRIP);",
    "      glColor3f(0.5, 0.5, 0.5);",
    "    gluNormal(0, 0, 1);",
    "  // Side 0",
    "      glNormal3f(-1, 0, 0);",
    "      glVertex3f(-0.6, -0.225, 0);",
    "      glVertex3f(-0.6, -0.225, z);",
    "    gluVertex(-0.6, -0.225, 0);",
    "  // Side 1",
    "      glNormal3f(0, 1, 0);",
    "      glVertex3f(-0.6, 0.225, 0);",
    "      glVertex3f(-1.2/2, 0.45/2, z);",
    "    gluVertex(-0.6, 0.225, 0);",
    "  // Side 2",
    "      glNormal3f(-1, 1, 0);",
    "      glVertex3f(0.2, 0.225, 0);",
    "      glVertex3f(0.2, 0.45/2, z);",
    "    gluVertex(0.2, 0.225, 0);",
    "  // Side 3",
    "      glNormal3f(-1, 0, 0);",
    "      glVertex3f(0.2, 0.55, 0);",
    "      glVertex3f(0.2, 1.1/2, z);",
    "    gluVertex(0.2, 0.55, 0);",
    "  // Side 4",
    "      glNormal3f(1, 0, 0);",
    "      glVertex3f(0.85, 0, 0);",
    "      glVertex3f(1.7/2, 0, z);",
    "    gluVertex(0.85, 0, 0);",
    "  // Side 5",
    "      glNormal3f(-1, 0, 0);",
    "      glVertex3f(0.2, -0.55, 0);",
    "      glVertex3f(0.2, -1.1/2, z);",
    "    gluVertex(0.2, -0.55, 0);",
    "  // Side 6",
    "      glNormal3f(-1, -1, 0);",
    "      glVertex3f(0.2, -0.225, 0);",
    "      glVertex3f(0.2, -0.45/2, z);",
    "    gluVertex(0.2, -0.225, 0);",
    "  // Quad side 7",
    "      glNormal3f(0, -1, 0);",
    "      glVertex3f(-0.6, -0.225, 0);",
    "      glVertex3f(-1.2/2, -0.45/2, z);",
    "    glEnd();",
    "  gluEnd();",
    "gluEnd();",
    NULL
};

/* Example 10: 2D assignment sketch - runtime assignment without goto.
 * No predefined goto examples are shipped because goto support is partial:
 * top-level only, not replay-safe, and not suitable for variable-driven
 * geometry loops. Keep coverage in tests/docs instead of F12 examples. */
static const char *const g_example_assign_2d[] = {
    "static float x, y;",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "// 2D assignment sketch: tests runtime variable assignment without goto",
    "glDisable(GL_LIGHTING);",
    "x = -1.7;",
    "glBegin(GL_LINE_STRIP);",
    "for(i, 0, 48) {",
    "  x = x + 0.07;",
    "  y = sin(x*2.5 + t)*0.52 + cos(i*0.25 + t*0.6)*0.16;",
    "  glColor3f(0.30 + i*0.010, 0.55 + 0.28*sin(i*0.20 + t), 0.95 - i*0.008);",
    "  glVertex3f(x, y, 0);",
    "}",
    "glEnd();",
    NULL
};

/* Example 11: Additive glow particles - glPointSize + distance attenuation + blend */
static const char *const g_example_glow_particles[] = {
    "// @cfg vertex_outlines = 0",
    "// @cfg vertex_points = 0",
    "static float n, x, y, z, j, k;",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "// Glow sprites: additive blend + distance-attenuated point size",
    "glDisable(GL_LIGHTING);",
    "glDisable(GL_DEPTH_TEST);",
    "glEnable(GL_BLEND);",
    "glBlendFunc(GL_SRC_ALPHA, GL_ONE);",
    "glEnable(GL_POINT_SMOOTH);",
    "glPointSize(24);",
    "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 0.2, 0, 0.15);",
    "n = 160;",
    "glBegin(GL_POINTS);",
    "for(i, 0, n) {",
    "  k = rand(i, 0)*TAU;",
    "  j = 0.4 + rand(i, 1)*1.8;",
    "  x = cos(k + t*0.4)*j;",
    "  y = sin(k*1.3 + t*0.6)*0.9;",
    "  z = sin(k + t*0.4)*j;",
    "  glColor4f(0.4 + 0.6*rand(i, 2), 0.3 + 0.5*rand(i, 3), 0.8 + 0.2*rand(i, 4), 0.55);",
    "  glVertex3f(x, y, z);",
    "}",
    "glEnd();",
    "glDisable(GL_POINT_SMOOTH);",
    "glDisable(GL_BLEND);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    NULL
};

/* Example 13: Procedural terrain - rand(x,z) samples heights at each grid vertex,
 * building GL_QUAD_STRIP rows via nested for-loops. An animated sine ripple
 * (k = 0.25*sin(t/2 * ...)) is added to alternate columns each frame, giving
 * the flat random field a rolling wave motion. Lit with GL_LIGHT0 + GL_LIGHT3,
 * smooth-shaded. */

static const char *const g_example_random_surface[] = {
  "static float n, i, j, k;",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
  "glEnable(GL_DEPTH_TEST);",
  "glEnable(GL_LIGHTING);",
  "glEnable(GL_NORMALIZE);",
  "glShadeModel(GL_SMOOTH);",
  "glEnable(GL_LIGHT3);",
  "// glEnable(GL_LIGHT1)",
  "glEnable(GL_LIGHT0);",
  "//glEnable(GL_LINE_SMOOTH)",
  "// glDisable(GL_LIGHTING)",
  "glPointSize(4);",
  "glEnable(GL_BLEND);",
  "glColor4f(0.176, 0.51392, 0.88, 0.713333);",
  "n = 10;",
  "glTranslatef(0, -0.25, 0);",
  "for (x, -n, n) {",
  "  glBegin(GL_QUAD_STRIP);",
  "    for (z, -n, n) {",
  "      i = rand(x,z);",
  "      j = rand(x+1,z);",
  "      k = 0.25 * sin((100+t/2) * (z/200+4));",
  "      if(fmod(x, 2) == 0) {",
  "        i = i + k;",
  "        j = j + k;",
  "      }",
  "      if(abs(fmod(x,2)) == 1) {",
  "        j = j + k;",
  "        i = i +k;",
  "      }",
  "      glVertex3f(x, i, z);",
  "      glVertex3f(x+1, j, z);",
  "    }",
  "  glEnd();",
  "}",
    NULL
};


/* Example 14: Animated wave surface - y = sin(x*2.5+t)*cos(z*2.5+t*0.7)*0.4
 * rendered as GL_TRIANGLE_STRIP rows. Normals are the exact analytic partial
 * derivatives of the wave function (no finite-difference approximation), giving
 * correct smooth lighting across all four lights. Per-vertex color varies with
 * position and t for a shifting iridescent look. */

static const char *const g_example_waves[] = {
    "// @cfg vertex_points = 0",
    "static float n, b, x, y, z, a;",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "// Waves: nested for-loops + math",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "glEnable(GL_LIGHT2);",
    "glEnable(GL_LIGHT1);",
    "glEnable(GL_LIGHT0);",
    "glShadeModel(GL_SMOOTH);",
    "n = 16;",
    "b = 3.0; // breath",
    "for(i, 0, n) {",
        "glBegin(GL_TRIANGLE_STRIP);",
        "for(j, 0, n+1) {",
            "x = -b/2 + b*j/n;",
            "z = -b/2 + b*i/n;",
            "y = sin(x*2.5 + t)*cos(z*2.5 + t*0.7)*0.4;",
            "a = 1.0/sqrt(1 + 0.4*0.4*6.25*(cos(x*2.5 + t)*cos(x*2.5 + t)*cos(z*2.5 + t*0.7)*cos(z*2.5 + t*0.7) + sin(x*2.5 + t)*sin(x*2.5 + t)*sin(z*2.5 + t*0.7)*sin(z*2.5 + t*0.7)));",
            "glNormal3f(-0.4*2.5*cos(x*2.5 + t)*cos(z*2.5 + t*0.7)*a, a, 0.4*2.5*sin(x*2.5 + t)*sin(z*2.5 + t*0.7)*a);",
            "glColor3f(0.35 + 0.35*sin(x + t), 0.5 + 0.3*cos(z + t*0.5), 0.65 + 0.25*sin(x*z + t));",
            "glVertex3f(x, y, z);",
            "z = -b/2 + b*(i + 1)/n;",
            "y = sin(x*2.5 + t)*cos(z*2.5 + t*0.7)*0.4;",
            "a = 1.0/sqrt(1 + 0.4*0.4*6.25*(cos(x*2.5 + t)*cos(x*2.5 + t)*cos(z*2.5 + t*0.7)*cos(z*2.5 + t*0.7) + sin(x*2.5 + t)*sin(x*2.5 + t)*sin(z*2.5 + t*0.7)*sin(z*2.5 + t*0.7)));",
            "glNormal3f(-0.4*2.5*cos(x*2.5 + t)*cos(z*2.5 + t*0.7)*a, a, 0.4*2.5*sin(x*2.5 + t)*sin(z*2.5 + t*0.7)*a);",
            "glColor3f(0.35 + 0.35*sin(x + t), 0.5 + 0.3*cos(z + t*0.5), 0.65 + 0.25*sin(x*z + t));",
            "glVertex3f(x, y, z);",
        "}",
        "glEnd();",
    "}",
    NULL
};

/* Example 15: Animated spirograph curve - closed parametric line loop
 * driven by t, showing dense iteration and trig-heavy vertex generation. */
static const char *const g_example_spirograph_curve[] = {
    "// @cfg vertex_outlines = 0",
    "// @cfg vertex_points = 0",
    "// camera",
    "glTranslatef(0.0f, 0.0f, -7.5f);",
    "glRotatef(0.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(0.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, -0.25f, 0.0f);",
    "static float dist, n, x, y, ang;",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "dist = 1.4;",
    "n = 400;",
    "glBegin(GL_LINE_LOOP);",
    "  for(i, 0, n) {",
    "    ang = TAU * i/n;",
    "    x = dist * cos(t*ang) + cos(ang);",
    "    y = dist * sin(t*ang) + sin(ang);",
    "    glVertex3f(x, y, 0);",
    "  }",
    "glEnd();",
    NULL
};

/* Example 16: Traveling ripple ring - circular line loop with a narrow
 * modulo-selected radial wave, exercising fmod math and conditional edits. */
static const char *const g_example_traveling_ripple_ring[] = {
    "// @cfg vertex_outlines = 0",
    "// @cfg vertex_points = 0",
    "// camera",
    "glTranslatef(0.0f, 0.0f, -3.5f);",
    "glRotatef(0.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(0.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, -0.15f, 0.0f);",
    "// Traveling ripple ring: nested for + fmod + conditional deformation",
    "static float ripplephase, ripplewidth, rippledelta, rippleamp, x, y;",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "rippleamp = 0.1 * sin(t * TAU);",
    "ripplephase = fmod(t / 3, TAU);",
    "ripplewidth = PI / 8;",
    "glBegin(GL_LINE_LOOP);",
    "  for(i, 0, 400) {",
    "    x = cos(TAU * i / 400);",
    "    y = sin(TAU * i / 400);",
    "    rippledelta = fmod(TAU * i / 400 - ripplephase + TAU, TAU);",
    "    if(rippledelta < ripplewidth) {",
    "      x = x + rippleamp * x * sin(rippledelta * TAU / ripplewidth);",
    "      y = y + rippleamp * y * sin(rippledelta * TAU / ripplewidth);",
    "    }",
    "    glVertex3f(x, y, 0);",
    "  }",
    "glEnd();",
    NULL
};

/* Animated quadratic Bezier curve - vertex + line loop with parametric
 * function, showing variable assignment and reuse across calls. */
static const char *const g_example_bezier[] = {
    "// @cfg vertex_outlines = 0",
    "// @cfg vertex_points = 0",
    "// @cfg light_indicators = 0",
    "// @cfg poly_highlight = 0",
    "// @cfg grid = 9",
    "// @cfg msaa = 0",
    "// @cfg line_smooth = 1",
    "// camera",
    "glTranslatef(0.0f, 0.0f, -3.5f);",
    "glRotatef(0.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(0.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    "static float x, y, x0, y0, x1 = 1, y1 = 1, x2 = 2, y2;",
    "static float period = 5;",
    "static float x_a, x_b, y_a, y_b;",
    "// current progress",
    "static float t0;",
    "bezier(x0, y0, x1, y1, x2, y2, u) {",
    "  x = pow(1-u, 2) * x0 + 2 * (1-u)*u * x1 + u*u * x2;",
    "  y = pow(1-u, 2) * y0 + 2 * (1-u)*u * y1 + u*u * y2;",
    "}",
    "glClearColor(0, 0, 0, 1);",
    "glEnable(GL_DEPTH_TEST);",
    "glDisable(GL_LIGHTING);",
    "glDisable(GL_MULTISAMPLE);",
    "glEnable(GL_LINE_SMOOTH);",
    "glEnable(GL_BLEND);",
    "glPointSize(8);",
    "glColor4f(0.8, 0.7, 0.3, 0.9);",
    "glLineWidth(1.3);",
    "t0 = (fmod(t, period)) / period;",
    "glBegin(GL_POINTS);",
    "  glVertex2f(x0, y0);",
    "  glVertex3f(x1, y1, 0);",
    "  glVertex2f(x2, y2);",
    "  bezier(x0, y0, x1, y1, x2, y2, t0);",
    "  glColor3f(0.3, 0.7, 0.9);",
    "  glVertex2f(x, y);",
    "glEnd();",
    "glColor4f(1, 1, 1, 0.9);",
    "glBegin(GL_LINE_STRIP);",
    "  for (u, 0, 1, 0.01f) {",
    "    bezier(x0, y0, x1, y1, x2, y2, u);",
    "    glVertex2f(x, y);",
    "  }",
    "glEnd();",
    "glColor4f(1, 1, 1, 0.3);",
    "x_a = x0*(1-t0) + x1*t0;",
    "y_a = y0*(1-t0) + y1*t0;",
    "x_b = x1*(1-t0) + x2*t0;",
    "y_b = y1*(1-t0) + y2*t0;",
    "glBegin(GL_POINTS);",
    "  glVertex2f(x_a, y_a);",
    "  glVertex2f(x_b, y_b);",
    "glEnd();",
    "glBegin(GL_LINES);",
    "  // Guide Line 1",
    "  glVertex2f(x0, y0);",
    "  glVertex2f(x1, y1);",
    "  // Guide Line 2",
    "  glVertex2f(x1, y1);",
    "  glVertex2f(x2, y2);",
    "  // Derived Line",
    "  glVertex2f(x_a, y_a);",
    "  glVertex2f(x_b, y_b);",
    "glEnd();",
    NULL
};

/* Snowfall - 550 GL_POINTS with x/y/z from rand(seed, iter) + time,
 * creating a looping field of falling points with depth-based color. */
static const char *const g_example_snowfall[] = {
    "// @cfg vertex_outlines = 0",
    "// @cfg vertex_points = 0",
    "// @cfg light_indicators = 0",
    "// @cfg poly_highlight = 0",
    "// @cfg grid = 0",
    "// @cfg msaa = 1",
    "// @cfg line_smooth = 0",
    "// @cfg variable_panel = 0",
    "// camera",
    "glTranslatef(0.0f, 0.0f, -13.5f);",
    "glRotatef(0.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(0.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    "static float x, y;",
    "static float xVel, yVel;",
    "static float zColProp;",
    "static float xVelMax = 0.5;",
    "static float g = -1.5;",
    "static float xMax = 12, yMax = 5, zMax = 10;",
    "glClearColor(0.05, 0.05, 0.05, 1);",
    "glEnable(GL_DEPTH_TEST);",
    "glPointSize(10);",
    "glBegin(GL_POINTS);",
    "  for (p, 0, 550) {",
    "    xVel = xVelMax * rand2(p, 1);",
    "    x = rem(xMax * rand2(p,4) + t * xVel, 2 * xMax);",
    "",
    "    yVel = g - rand(p, 2);",
    "    y = rem(yMax * rand2(p,3) + t * yVel, 2 * yMax);",
    "",
    "    zColProp = rand(p, 5); // z affects color and distance",
    "",
    "    glColor3f(0.5 + 0.5 * zColProp, 0.5 + 0.5 * zColProp, 0.5 + 0.5 * zColProp);",
    "    glVertex3f(x, y, zMax * zColProp);",
    "  }",
    "glEnd();",
    NULL
};

/* Stress test - exercises parser, code UI, nested structures,
 * multiple functions, recursion, conditionals, variables, tessellation,
 * GLU primitives, matrix stack, animation, and long line count. */
static const char *const g_example_stress[] = {
    "// @cfg axes = 4",
    "// @cfg vertex_outlines = 0",
    "// @cfg vertex_points = 0",
    "// @cfg backdrop = 1",
    "// @cfg grid = 7",
    "// camera",
    "glTranslatef(0.0f, 0.0f, -12.5f);",
    "glRotatef(27.5f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(-24.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(-0.6f, -0.1f, -0.4f);",
    "static float n, x, y, z, k;",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "// ===== Stress-test scene: parser + code UI torture =====",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT2);",
    "glEnable(GL_LIGHT3);",
    "glShadeModel(GL_SMOOTH);",
    "glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);",
    "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);",
    "",
    "// --- surface: parametric surface patch (nested for + math) ---",
    "surface(rows, cols, amp, phase) {",
        "for(i, 0, rows) {",
            "glBegin(GL_TRIANGLE_STRIP);",
            "for(j, 0, cols + 1) {",
                "x = -1.5 + 3.0*j/cols;",
                "z = -1.5 + 3.0*i/rows;",
                "y = amp*sin(x*2.5 + phase)*cos(z*2.5 + phase*0.7);",
                "n = 1.0/sqrt(1 + amp*amp*6.25*(cos(x*2.5 + phase)*cos(x*2.5 + phase)*cos(z*2.5 + phase*0.7)*cos(z*2.5 + phase*0.7) + sin(x*2.5 + phase)*sin(x*2.5 + phase)*sin(z*2.5 + phase*0.7)*sin(z*2.5 + phase*0.7)));",
                "glNormal3f(-amp*2.5*cos(x*2.5 + phase)*cos(z*2.5 + phase*0.7)*n, n, amp*2.5*sin(x*2.5 + phase)*sin(z*2.5 + phase*0.7)*n);",
                "glColor3f(0.35 + 0.35*sin(x + phase), 0.5 + 0.3*cos(z + phase*0.5), 0.65 + 0.25*sin(x*z + phase));",
                "glVertex3f(x, y, z);",
                "z = -1.5 + 3.0*(i + 1)/rows;",
                "y = amp*sin(x*2.5 + phase)*cos(z*2.5 + phase*0.7);",
                "n = 1.0/sqrt(1 + amp*amp*6.25*(cos(x*2.5 + phase)*cos(x*2.5 + phase)*cos(z*2.5 + phase*0.7)*cos(z*2.5 + phase*0.7) + sin(x*2.5 + phase)*sin(x*2.5 + phase)*sin(z*2.5 + phase*0.7)*sin(z*2.5 + phase*0.7)));",
                "glNormal3f(-amp*2.5*cos(x*2.5 + phase)*cos(z*2.5 + phase*0.7)*n, n, amp*2.5*sin(x*2.5 + phase)*sin(z*2.5 + phase*0.7)*n);",
                "glColor3f(0.35 + 0.35*sin(x + phase), 0.5 + 0.3*cos(z + phase*0.5), 0.65 + 0.25*sin(x*z + phase));",
                "glVertex3f(x, y, z);",
            "}",
            "glEnd();",
        "}",
    "}",
    "",
    "// --- tree: recursive branching tree (recursion + if + matrix stack) ---",
    "tree(depth, len, twist) {",
        "glColor3f(0.35 + depth*0.12, 0.25 + 0.15*sin(twist), 0.15);",
        "glBegin(GL_LINES);",
        "glNormal3f(0, 0, 1);",
        "glVertex3f(0, 0, 0);",
        "glVertex3f(0, len, 0);",
        "glEnd();",
        "if(depth > 0) {",
            "// Right branch",
            "glPushMatrix();",
            "glTranslatef(0, len, 0);",
            "glRotatef(25 + twist*8, 0, 0, 1);",
            "glScalef(0.72, 0.72, 0.72);",
            "tree(depth - 1, len, twist + 0.3);",
            "glPopMatrix();",
            "// Left branch",
            "glPushMatrix();",
            "glTranslatef(0, len, 0);",
            "glRotatef(-30 - twist*6, 0, 0, 1);",
            "glScalef(0.68, 0.68, 0.68);",
            "tree(depth - 1, len, twist + 0.25);",
            "glPopMatrix();",
            "// Forward branch (z-axis splay)",
            "glPushMatrix();",
            "glTranslatef(0, len*0.9, 0);",
            "glRotatef(15 + twist*4, 1, 0, 0);",
            "glScalef(0.6, 0.6, 0.6);",
            "tree(depth - 1, len*0.85, twist + 0.2);",
            "glPopMatrix();",
        "}",
        "if(depth <= 0) {",
            "// Leaf: small filled triangle",
            "glColor3f(0.2, 0.7 + 0.3*sin(twist*5), 0.15);",
            "glBegin(GL_TRIANGLES);",
            "glNormal3f(0, 0, 1);",
            "glVertex3f(0, len, 0);",
            "glVertex3f(-len*0.3, len*1.3, 0);",
            "glVertex3f(len*0.3, len*1.3, 0);",
            "glEnd();",
        "}",
    "}",
    "",
    "// --- starfloor: tessellated star floor with circular cutout ---",
    "starfloor(radius, hole) {",
        "glFrontFace(GL_CW);",
        "gluBegin(GLU_POLYGON);",
        "// Outer contour: 10-pointed star",
        "gluBegin(GLU_CONTOUR);",
        "gluNormal(0, 1, 0);",
        "for(i, 0, 20) {",
            "k = radius;",
            "if(fmod(i, 2) > 0) {",
                "k = radius*0.5;",
            "}",
            "gluColor(0.25 + 0.15*sin(i*0.6), 0.3 + 0.15*cos(i*0.8), 0.55 + 0.1*sin(i));",
            "gluVertex(k*cos(i*TAU/20), 0, k*sin(i*TAU/20));",
        "}",
        "gluEnd();",
        "// Inner contour: circular cutout (tessellator infers hole from overlap)",
        "gluBegin(GLU_CONTOUR);",
        "for(i, 0, 24) {",
            "gluColor(0.1, 0.1, 0.15);",
            "gluVertex(hole*cos(-i*TAU/24), 0, hole*sin(-i*TAU/24));",
        "}",
        "gluEnd();",
        "gluEnd();",
        "glFrontFace(GL_CCW);",
    "}",
    "",
    "// --- satellites: orbiting GLUT solid satellites ---",
    "satellites(count, orbit_r, sat_size, spin) {",
        "for(i, 0, count) {",
            "glPushMatrix();",
            "glRotatef(i*360/count + spin, 0, 1, 0);",
            "glTranslatef(orbit_r, 0, 0);",
            "// Sphere body",
            "glColor3f(0.8 + 0.2*sin(i + spin*0.02), 0.5, 0.3 + 0.2*cos(i));",
            "glutSolidSphere(sat_size, 12, 8);",
            "// Antenna cone",
            "glColor3f(0.5, 0.5, 0.6);",
            "glTranslatef(0, sat_size, 0);",
            "glRotatef(-90, 1, 0, 0);",
            "glutSolidCone(sat_size*0.15, sat_size*1.5, 8, 1);",
            "// Dish ring at top",
            "glTranslatef(0, 0, sat_size*1.5);",
            "glColor3f(0.9, 0.85, 0.5);",
            "glutSolidTorus(0.02*sat_size, sat_size*0.35, 12, 24);",
            "glPopMatrix();",
        "}",
    "}",
    "",
    "// ===== Main scene composition =====",
    "",
    "// Animated variables",
    "n = 8;",
    "x = 0.4*sin(t*0.6);",
    "y = 0.15*cos(t*0.8);",
    "",
    "// Ground: tessellated star floor",
    "glPushMatrix();",
    "glTranslatef(0, -1.8, 0);",
    "starfloor(3.0, 0.8);",
    "glPopMatrix();",
    "",
    "// Wavy parametric surface",
    "glPushMatrix();",
    "glTranslatef(0, x, 0);",
    "glRotatef(t*12, 0, 1, 0);",
    "surface(n, n, 0.35 + 0.15*sin(t), t*0.5);",
    "glPopMatrix();",
    "",
    "// Fractal tree (recursive)",
    "glPushMatrix();",
    "glTranslatef(-2.5, -1.8, 0);",
    "glRotatef(-5*sin(t*0.3), 0, 0, 1);",
    "tree(3, 0.7, t*0.2);",
    "glPopMatrix();",
    "",
    "// Second tree, mirrored",
    "glPushMatrix();",
    "glTranslatef(2.5, -1.8, 0);",
    "glScalef(-1, 1, 1);",
    "glRotatef(-5*cos(t*0.25), 0, 0, 1);",
    "tree(3, 0.65, t*0.15 + 1);",
    "glPopMatrix();",
    "",
    "// Orbiting satellites",
    "glPushMatrix();",
    "glTranslatef(0, y + 1.0, 0);",
    "satellites(5, 2.8, 0.18, t*35);",
    "glPopMatrix();",
    "",
    "// Central decoration: torus + sphere",
    "glPushMatrix();",
    "glTranslatef(0, 0.5 + 0.3*sin(t*1.2), 0);",
    "glRotatef(t*45, 0, 1, 0);",
    "glRotatef(t*20, 1, 0, 0);",
    "glColor3f(0.85, 0.6, 0.25);",
    "glutSolidTorus(0.12, 0.55, 16, 24);",
    "glColor3f(0.95, 0.90, 0.70);",
    "glutSolidSphere(0.25, 16, 12);",
    "glPopMatrix();",
    "",
    "// Wireframe accent ring",
    "glDisable(GL_LIGHTING);",
    "glBegin(GL_LINE_LOOP);",
    "for(i, 0, 64) {",
        "glColor3f(0.3 + 0.3*sin(i*TAU/64 + t), 0.5 + 0.2*cos(i*TAU/32 + t*0.7), 0.8);",
        "glVertex3f(3.5*cos(i*TAU/64), -1.75, 3.5*sin(i*TAU/64));",
    "}",
    "glEnd();",
    "glEnable(GL_LIGHTING);",
    "",
    "// Long expression stress: animated vertex spiral",
    "glDisable(GL_LIGHTING);",
    "glBegin(GL_LINE_STRIP);",
    "for(i, 0, 120) {",
        "x = (1.5 + 0.4*sin(i*TAU/30 + t*2))*cos(i*TAU/60 + t*0.3);",
        "y = -1.75 + i*0.025 + 0.08*sin(i*0.5 + t*3);",
        "z = (1.5 + 0.4*sin(i*TAU/30 + t*2))*sin(i*TAU/60 + t*0.3);",
        "glColor3f(0.1 + 0.5*sin(i*0.08 + t), 0.9 - 0.4*cos(i*0.06), 0.5 + 0.3*sin(i*0.12 + t*0.5));",
        "glVertex3f(x, y, z);",
    "}",
    "glEnd();",
    "glEnable(GL_LIGHTING);",
    NULL
};

/* Transformation stress test - exercises translate/rotate/scale guides in
 * varied contexts: nested push/pop, time-varying args (t), for-loop
 * unrolling, function scopes, and scale-of-origin (3-axis gizmo fallback).
 * Park the cursor on any transform line to see its guide overlay. */
static const char *const g_example_xform_stress[] = {
    "glClearColor(0.08, 0.08, 0.1, 1);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_LIGHT0);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_COLOR_MATERIAL);",
    "tri() {",
        "glBegin(GL_TRIANGLES);",
        "glNormal3f(0, 0, 1);",
        "glVertex3f(-0.3, -0.3, 0);",
        "glVertex3f(0.3, -0.3, 0);",
        "glVertex3f(0, 0.4, 0);",
        "glEnd();",
    "}",
    "glColor3f(1, 0.6, 0.3);",
    "glPushMatrix();",
    "glTranslatef(0, 0, 2);",
    "glRotatef(45, 0, 1, 0);",
    "glTranslatef(0, 0, -2);",
    "tri();",
    "glPopMatrix();",
    "glColor3f(0.4, 0.9, 0.6);",
    "glPushMatrix();",
    "glTranslatef(-2, 0, 0);",
    "glRotatef(3*t, 0, 1, 0);",
    "glScalef(1.5, 0.5, 1);",
    "tri();",
    "glPopMatrix();",
    "glColor3f(0.6, 0.7, 1);",
    "for(i, 0, 6) {",
        "glPushMatrix();",
        "glRotatef(i*60, 0, 1, 0);",
        "glTranslatef(2.5, 0, 0);",
        "glScalef(0.6, 0.6, 0.6);",
        "tri();",
        "glPopMatrix();",
    "}",
    "glColor3f(1, 0.4, 0.8);",
    "glPushMatrix();",
    "glTranslatef(0, 2, 0);",
    "glScalef(0.8, 0.8, 0.8);",
    "tri();",
    "glPopMatrix();",
    "glColor3f(1, 1, 0.4);",
    "glPushMatrix();",
    "glRotatef(t*30, 0, 1, 0);",
    "tri();",
    "glPopMatrix();",
    NULL
};

/* Scratch-array demo - cubic Bezier evaluation via de Casteljau updates.
 * A/B/C hold the x/y/z control-point lanes while collapse() implements one
 * in-place interpolation step using ordinary expression math. */
static const char *const g_example_scratch_casteljau[] = {
    "// camera",
    "glTranslatef(0.0f, 0.0f, -10.0f);",
    "glRotatef(18.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(-18.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(-0.0f, -0.0f, -0.0f);",
    "glClearColor(0.07, 0.08, 0.12, 1);",
    "glEnable(GL_DEPTH_TEST);",
    "glDisable(GL_LIGHTING);",
    "glLineWidth(3);",
    "static float u;",
    "// collapse(dst, lo, hi, blend): collapse one Bezier edge in place.",
    "collapse(dst, lo, hi, blend) {",
        "A[dst] = A[lo] + (A[hi] - A[lo])*blend;",
        "B[dst] = B[lo] + (B[hi] - B[lo])*blend;",
        "C[dst] = C[lo] + (C[hi] - C[lo])*blend;",
    "}",
    "// Seed A/B/C with control points, then overwrite them with collapse().",
    "glColor3f(1.0, 0.55, 0.2);",
    "glBegin(GL_LINE_STRIP);",
    "for(i, 0, 48) {",
        "A[0] = -3.2;",
        "B[0] = -1.8;",
        "C[0] = -0.8;",
        "A[1] = -1.1 + 0.4*sin(t*0.9);",
        "B[1] = 2.4;",
        "C[1] = -0.1;",
        "A[2] = 1.0;",
        "B[2] = -2.1 + 0.5*cos(t*0.7);",
        "C[2] = 0.5;",
        "A[3] = 3.1;",
        "B[3] = 1.6;",
        "C[3] = 1.0 + 0.25*sin(t*0.5);",
        "u = i/48.0;",
        "collapse(0, 0, 1, u);",
        "collapse(1, 1, 2, u);",
        "collapse(2, 2, 3, u);",
        "collapse(0, 0, 1, u);",
        "collapse(1, 1, 2, u);",
        "collapse(0, 0, 1, u);",
        "glVertex3f(A[0], B[0], C[0]);",
    "}",
    "glEnd();",
    "",
    "// Rebuild the same control polygon from scratch-array state.",
    "A[0] = -3.2;",
    "B[0] = -1.8;",
    "C[0] = -0.8;",
    "A[1] = -1.1 + 0.4*sin(t*0.9);",
    "B[1] = 2.4;",
    "C[1] = -0.1;",
    "A[2] = 1.0;",
    "B[2] = -2.1 + 0.5*cos(t*0.7);",
    "C[2] = 0.5;",
    "A[3] = 3.1;",
    "B[3] = 1.6;",
    "C[3] = 1.0 + 0.25*sin(t*0.5);",
    "glColor3f(0.25, 0.45, 0.95);",
    "glBegin(GL_LINE_STRIP);",
    "glVertex3f(A[0], B[0], C[0]);",
    "glVertex3f(A[1], B[1], C[1]);",
    "glVertex3f(A[2], B[2], C[2]);",
    "glVertex3f(A[3], B[3], C[3]);",
    "glEnd();",
    "glPointSize(9);",
    "glColor3f(1.0, 0.9, 0.35);",
    "glBegin(GL_POINTS);",
    "glVertex3f(A[0], B[0], C[0]);",
    "glVertex3f(A[1], B[1], C[1]);",
    "glVertex3f(A[2], B[2], C[2]);",
    "glVertex3f(A[3], B[3], C[3]);",
    "glEnd();",
    NULL
};

/* Text annotation demo - pairs glRasterPos3f with label() so scene data can
 * carry live numeric readouts instead of geometry alone. */
static const char *const g_example_annotated_orbit_plot[] = {
    "// @cfg vertex_outlines = 0",
    "// @cfg vertex_points = 0",
    "// @cfg light_indicators = 0",
    "// @cfg grid = 9",
    "// camera",
    "glTranslatef(0.0f, 0.0f, -6.0f);",
    "glRotatef(0.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(0.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    "static float n, r, x, y, phase, sample;",
    "glClearColor(0.06, 0.07, 0.09, 1);",
    "glDisable(GL_LIGHTING);",
    "glDisable(GL_DEPTH_TEST);",
    "glEnable(GL_BLEND);",
    "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);",
    "glLineWidth(1.4);",
    "// Annotation demo: geometry plus live numeric labels.",
    "phase = fmod(t*0.2, 1);",
    "n = 72;",
    "for(r, 1, 4) {",
        "glColor4f(0.2, 0.35, 0.55, 0.45);",
        "glBegin(GL_LINE_LOOP);",
        "for(i, 0, n) {",
            "glVertex3f(cos(i*TAU/n)*r*0.6, sin(i*TAU/n)*r*0.6, 0);",
        "}",
        "glEnd();",
        "glRasterPos3f(r*0.6 + 0.08, 0, 0);",
        "label(\"r=%f\", r*0.6);",
    "}",
    "glColor4f(0.45, 0.5, 0.65, 0.45);",
    "glBegin(GL_LINES);",
    "for(i, 0, 12) {",
        "glVertex3f(0, 0, 0);",
        "glVertex3f(cos(i*TAU/12)*2.4, sin(i*TAU/12)*2.4, 0);",
    "}",
    "glEnd();",
    "glColor3f(1.0, 0.75, 0.25);",
    "glLineWidth(3);",
    "glBegin(GL_LINE_STRIP);",
    "for(i, 0, n+1) {",
        "sample = i/n;",
        "r = 1.0 + 0.5*sin(sample*TAU*3 + t);",
        "glVertex3f(cos(sample*TAU)*r, sin(sample*TAU)*r, 0);",
    "}",
    "glEnd();",
    "sample = phase;",
    "r = 1.0 + 0.5*sin(sample*TAU*3 + t);",
    "x = cos(sample*TAU)*r;",
    "y = sin(sample*TAU)*r;",
    "glPointSize(11);",
    "glColor3f(0.1, 0.9, 1.0);",
    "glBegin(GL_POINTS);",
    "glVertex3f(x, y, 0);",
    "glEnd();",
    "glColor3f(1.0, 1.0, 1.0);",
    "glRasterPos3f(-2.35, 2.35, 0);",
    "label(\"phase=%f\", phase);",
    "glRasterPos3f(x + 0.12, y + 0.12, 0);",
    "label(\"r=%f\", r);",
    "glRasterPos3f(-2.35, -2.35, 0);",
    "label(\"label text follows glRasterPos3f\");",
    NULL
};

typedef unsigned int ReplExampleTagMask;

typedef struct {
    const char *name;
    const char *const *lines;
    ReplExampleTagMask tags;
} ReplExampleEntry;

/* REPL_EXAMPLE_TAG_ALL is a synthetic tag: every example is a member.
 * It is not listed in any g_example_entries[] mask literal; instead
 * repl_example_tag_mask() ORs its bit into every example's mask, so the
 * whole tag query API (has_tag / count_for_tag / index_for_tag /
 * visible_tag_*) — and therefore the Scene menu's "All" group and the
 * F12 cycle — pick it up with no per-entry bookkeeping. Kept at index 0
 * so "All" sorts first under "### EXAMPLES". */
enum {
    REPL_EXAMPLE_TAG_ALL = 0,
    REPL_EXAMPLE_TAG_2D,
    REPL_EXAMPLE_TAG_3D,
    REPL_EXAMPLE_TAG_POLYGONS,
    REPL_EXAMPLE_TAG_LINES,
    REPL_EXAMPLE_TAG_COUNT
};

#define EXAMPLE_TAG_BIT(tag) (1u << (tag))
#define EXAMPLE_TAG_ALL      EXAMPLE_TAG_BIT(REPL_EXAMPLE_TAG_ALL)
#define EXAMPLE_TAG_2D       EXAMPLE_TAG_BIT(REPL_EXAMPLE_TAG_2D)
#define EXAMPLE_TAG_3D       EXAMPLE_TAG_BIT(REPL_EXAMPLE_TAG_3D)
#define EXAMPLE_TAG_POLYGONS EXAMPLE_TAG_BIT(REPL_EXAMPLE_TAG_POLYGONS)
#define EXAMPLE_TAG_LINES    EXAMPLE_TAG_BIT(REPL_EXAMPLE_TAG_LINES)

static const char *const g_example_tag_labels[] = {
    "All",
    "2D",
    "3D",
    "Polygons",
    "Lines",
};

static const ReplExampleEntry g_example_entries[] = {
    { "Lit cube", g_example_cube,
      EXAMPLE_TAG_3D | EXAMPLE_TAG_POLYGONS },
    { "Animated ring (for + t)", g_example_ring,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_LINES },
    { "Function demo (named func)", g_example_func,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_LINES },
    { "Function polygons (args + for)", g_example_func_loop,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_POLYGONS },
    { "Function branching (args + if)", g_example_func_if,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_POLYGONS },
    { "Recursive triangle tree (func + recursion)", g_example_func_recurse,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_LINES },
    { "Conditional colors (if + t)", g_example_cond,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_POLYGONS },
    { "Parametric torus (nested for)", g_example_torus,
      EXAMPLE_TAG_3D | EXAMPLE_TAG_POLYGONS },
    { "GLU tessellator (concave arrow)", g_example_tess,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_POLYGONS },
    { "GLU tessellator (concave arrow cutout)", g_example_tess_cutout,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_POLYGONS },
    { "2D assignment sketch (vars only)", g_example_assign_2d,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_LINES },
    { "Glow sprites (blend + point attenuation)", g_example_glow_particles,
      EXAMPLE_TAG_3D },
    { "Procedural terrain (rand grid + sin ripple)", g_example_random_surface,
      EXAMPLE_TAG_3D | EXAMPLE_TAG_POLYGONS },
    { "Animated wave surface (analytic normals)", g_example_waves,
      EXAMPLE_TAG_3D | EXAMPLE_TAG_POLYGONS },
    { "Animated spirograph curve", g_example_spirograph_curve,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_LINES },
    { "Traveling ripple ring", g_example_traveling_ripple_ring,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_LINES },
    { "Bezier curve with guides", g_example_bezier,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_LINES },
    { "Snowfall demo (550 particles)", g_example_snowfall,
      EXAMPLE_TAG_3D },
    { "Transform stress (translate/rotate/scale guides)", g_example_xform_stress,
      EXAMPLE_TAG_3D | EXAMPLE_TAG_LINES },
    { "Stress test (all features)", g_example_stress,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_3D | EXAMPLE_TAG_POLYGONS | EXAMPLE_TAG_LINES },
    { "Scratch arrays (de Casteljau curve)", g_example_scratch_casteljau,
      EXAMPLE_TAG_2D | EXAMPLE_TAG_LINES },
    { "Annotated orbit plot (labels)", g_example_annotated_orbit_plot,
      EXAMPLE_TAG_3D | EXAMPLE_TAG_LINES },
};

int repl_examples_count(void) {
    return (int)(sizeof(g_example_entries) / sizeof(g_example_entries[0]));
}

const char *repl_examples_name(int idx) {
    if (idx < 0 || idx >= repl_examples_count())
        return NULL;
    return g_example_entries[idx].name;
}

const char *const *repl_examples_lines(int idx) {
    if (idx < 0 || idx >= repl_examples_count())
        return NULL;
    return g_example_entries[idx].lines;
}

int repl_example_tag_count(void) {
    return REPL_EXAMPLE_TAG_COUNT;
}

const char *repl_example_tag_label(int tag_idx) {
    if (tag_idx < 0 || tag_idx >= repl_example_tag_count())
        return NULL;
    return g_example_tag_labels[tag_idx];
}

unsigned int repl_example_tag_mask(int example_idx) {
    if (example_idx < 0 || example_idx >= repl_examples_count())
        return 0u;
    /* Fold in the synthetic "All" membership so every tag query derives
     * it uniformly; entry literals stay free of an explicit ALL bit. */
    return g_example_entries[example_idx].tags | EXAMPLE_TAG_ALL;
}

int repl_example_has_tag(int example_idx, int tag_idx) {
    unsigned int bit = repl_example_tag_bit(tag_idx);
    if (!bit)
        return 0;
    return (repl_example_tag_mask(example_idx) & bit) != 0u;
}

int repl_example_count_for_tag(int tag_idx) {
    int count = 0;
    if (!repl_example_tag_bit(tag_idx))
        return 0;
    for (int idx = 0; idx < repl_examples_count(); idx++)
        if (repl_example_has_tag(idx, tag_idx))
            count++;
    return count;
}

int repl_example_index_for_tag(int tag_idx, int ordinal) {
    int seen = 0;
    if (ordinal < 0 || !repl_example_tag_bit(tag_idx))
        return -1;
    for (int idx = 0; idx < repl_examples_count(); idx++) {
        if (!repl_example_has_tag(idx, tag_idx))
            continue;
        if (seen == ordinal)
            return idx;
        seen++;
    }
    return -1;
}

int repl_example_visible_tag_count(void) {
    int count = 0;
    for (int tag_idx = 0; tag_idx < repl_example_tag_count(); tag_idx++)
        if (repl_example_count_for_tag(tag_idx) > 0)
            count++;
    return count;
}

int repl_example_visible_tag_at(int dense_idx) {
    int seen = 0;
    if (dense_idx < 0)
        return -1;
    for (int tag_idx = 0; tag_idx < repl_example_tag_count(); tag_idx++) {
        if (repl_example_count_for_tag(tag_idx) <= 0)
            continue;
        if (seen == dense_idx)
            return tag_idx;
        seen++;
    }
    return -1;
}
