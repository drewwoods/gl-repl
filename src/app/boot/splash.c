/*
 * splash.c - startup splash banner.
 *
 * A lower-third band along the bottom of the window: the open-cube mark
 * (lit-cube.glr's 4-faced cube, both X faces open - the logo) slowly
 * turning beside the app name and tagline.  Not a full-screen overlay:
 * the scene and UI render normally underneath from frame one; the band
 * fades out after a few seconds.  Frame-counted, not wall-clock, so
 * headless/record runs see deterministic timing.  Any keypress skips it.
 *
 * Drawn by display_func() (gl_repl.c) after glr_ctrl_display_frame() and
 * before glutSwapBuffers(), so it sits on top of the composited frame.
 *
 * The cube is projected in software (rotY -> rotX, drop Z - the same
 * projection as scripts/gen_rotating_cube.py and the baked SMIL logo) and
 * drawn as 2D fills inside a gl2d_begin/end scope.  Faces are two-sided:
 * quiet near-white exterior, azure wall + magenta floor interior showing
 * through the openings.  Painter order needs no sorting: for a convex
 * solid the back faces never overlap each other, nor do the front faces,
 * so the passes are simply interior fills, interior edges, exterior
 * fills, front edges.
 */
#include "app/boot/splash.h"

#include <math.h>
#include <string.h>

#include "accent_palette.h"  /* brand-mark anchors: face + edge colors */
#include "config.h"
#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"

/* ---- timing ----------------------------------------------------------- */

#define SPLASH_HOLD_FRAMES  150   /* ~2.5s at 60 fps: fully visible       */
#define SPLASH_FADE_FRAMES   36   /* ~0.6s fade-out (smoothstep)          */
#define SPLASH_TOTAL_FRAMES (SPLASH_HOLD_FRAMES + SPLASH_FADE_FRAMES)

static int g_splash_frame = 0;
static int g_splash_done  = 0;

int  splash_active(void) { return !g_splash_done; }
void splash_skip(void)   { g_splash_done = 1; }

/* ---- projection ------------------------------------------------------- */

/* Avoid M_PI - not guaranteed by C99. */
#define SPLASH_PI_F     3.14159265f
#define SPLASH_PITCH_F  0.6155f              /* atan(1/sqrt(2)) = 35.264deg */
#define SPLASH_REST_YAW (SPLASH_PI_F / 4.0f) /* the logo's resting angle    */
#define SPLASH_YAW_RATE 0.008f               /* rad/frame, ~27deg/s drift   */

/* rotY(yaw) then rotX(pitch); full 3D result (z kept for face culling). */
static void splash_rotate(const float v[3], float yaw, float out[3]) {
    float cy = cosf(yaw), sy = sinf(yaw);
    float x  = v[0] * cy + v[2] * sy;
    float z1 = -v[0] * sy + v[2] * cy;
    float cb = cosf(SPLASH_PITCH_F), sb = sinf(SPLASH_PITCH_F);
    out[0] = x;
    out[1] = v[1] * cb - z1 * sb;
    out[2] = v[1] * sb + z1 * cb;
}

static float splash_smoothstep(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

/* ---- cube model (lit-cube.glr: faces +Y -Y +Z -Z; +X/-X open) --------- */

static const float k_verts[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};

/* Outward-CCW vertex order, same as the SVG generator. */
static const int k_faces[4][4] = {
    {4, 5, 6, 7},   /* +z wall  */
    {0, 3, 2, 1},   /* -z wall  */
    {2, 3, 7, 6},   /* +y top   */
    {0, 1, 5, 4},   /* -y floor */
};

/* Face base colors come from the shared brand-mark anchors
 * (accent_palette.h) - the same vocabulary the SVG twins are checked
 * against - so retuning the mark there re-themes this splash too. The
 * two faces the vector mark never shows lit (the exterior underside,
 * the dim interior ceiling) are splash-only shading nuances and stay
 * local named constants. */
static const float k_ext_bottom[3]  = {0.725f, 0.737f, 0.776f};
static const float k_inn_ceiling[3] = {0.561f, 0.588f, 0.733f};

/* Base color for face f (k_faces order: +z, -z, +y, -y). */
static const float *splash_face_base(int f, int interior) {
    if (interior)
        return (f == 2) ? k_inn_ceiling
             : palette_anchor_rgb(f == 3 ? PAL_MARK_FLOOR : PAL_MARK_WALL);
    if (f == 3) return k_ext_bottom;
    return palette_anchor_rgb(f == 2 ? PAL_MARK_TOP : PAL_MARK_SIDE);
}

/* 12 edges: vertex pair + the (up to 2) existing faces each borders,
 * as indices into k_faces (-1 = the adjacent face is an opening).  An
 * edge renders in the front pass while any adjacent face is
 * front-facing; otherwise it is an interior seam seen through an
 * opening (drawn dimmer, under the exterior fills). */
static const int k_edges[12][4] = {
    {0, 1, 3, 1}, {1, 2, 1, -1}, {2, 3, 2, 1}, {3, 0, 1, -1},
    {4, 5, 3, 0}, {5, 6, 0, -1}, {6, 7, 2, 0}, {7, 4, 0, -1},
    {0, 4, 3, -1}, {1, 5, 3, -1}, {2, 6, 2, -1}, {3, 7, 2, -1},
};

/* ---- drawing ---------------------------------------------------------- */

static void splash_fill_face(const float px[8], const float py[8],
                             const int face[4],
                             const float rgb[3], float shade, float alpha) {
    int i;
    glColor4f(rgb[0] * shade, rgb[1] * shade, rgb[2] * shade, alpha);
    glBegin(GL_QUADS);
    for (i = 0; i < 4; i++) glVertex2f(px[face[i]], py[face[i]]);
    glEnd();
}

static void splash_edge_pass(const float px[8], const float py[8],
                             const int front[4], int want_front,
                             float width, float alpha) {
    int e;
    const float *edge = palette_anchor_rgb(PAL_MARK_EDGE);
    glLineWidth(width);
    glColor4f(edge[0], edge[1], edge[2], alpha);
    glBegin(GL_LINES);
    for (e = 0; e < 12; e++) {
        int fa = k_edges[e][2], fb = k_edges[e][3];
        int is_front = (fa >= 0 && front[fa]) || (fb >= 0 && front[fb]);
        if (is_front != want_front) continue;
        glVertex2f(px[k_edges[e][0]], py[k_edges[e][0]]);
        glVertex2f(px[k_edges[e][1]], py[k_edges[e][1]]);
    }
    glEnd();
    glLineWidth(1.0f);
}

/* Draw the open cube at (cx, cy), unit scale s px, in gl2d (y-up) coords. */
static void splash_draw_cube(float cx, float cy, float s,
                             float yaw, float alpha) {
    float rv[8][3], px[8], py[8], nz[4];
    int front[4];
    int i, f;

    for (i = 0; i < 8; i++) {
        splash_rotate(k_verts[i], yaw, rv[i]);
        px[i] = cx + rv[i][0] * s;
        py[i] = cy + rv[i][1] * s;
    }
    for (f = 0; f < 4; f++) {
        /* Face normal from two rotated edge vectors (cross product z). */
        const float *a = rv[k_faces[f][0]];
        const float *b = rv[k_faces[f][1]];
        const float *c = rv[k_faces[f][2]];
        float e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
        float e2x = c[0] - b[0], e2y = c[1] - b[1], e2z = c[2] - b[2];
        float x = e1y * e2z - e1z * e2y;
        float y = e1z * e2x - e1x * e2z;
        float z = e1x * e2y - e1y * e2x;
        float len = sqrtf(x * x + y * y + z * z);
        nz[f] = (len > 0.0f) ? z / len : 0.0f;
        front[f] = nz[f] > 0.0f;
    }

    /* Interior surfaces (seen through the openings), then their seams. */
    for (f = 0; f < 4; f++)
        if (!front[f])
            splash_fill_face(px, py, k_faces[f], splash_face_base(f, 1),
                             0.78f + 0.22f * -nz[f], alpha);
    splash_edge_pass(px, py, front, 0, 1.0f, alpha * 0.55f);

    /* Quiet exterior on top, then the white front edges. */
    for (f = 0; f < 4; f++)
        if (front[f])
            splash_fill_face(px, py, k_faces[f], splash_face_base(f, 0),
                             0.86f + 0.14f * nz[f], alpha);
    splash_edge_pass(px, py, front, 1, 1.5f, alpha * 0.95f);
}

/* ---- public API ------------------------------------------------------- */

void splash_render(int win_w, int win_h) {
    const char *title   = "gl-repl";
    /* ASCII only: the GLUT bitmap fonts draw one glyph per byte, so the
     * wordmark's em dash becomes a hyphen here. */
    const char *tagline = "OpenGL REPL - immediate mode, immediately.";
    const float band_h  = 64.0f;
    const float cube_s  = 13.0f;             /* projected h = 3.27*s px    */
    float alpha, yaw;
    float cube_w, title_w, tag_w, total_w, x;

    (void)win_h;
    if (g_splash_done) return;
    if (g_splash_frame >= SPLASH_TOTAL_FRAMES) {
        g_splash_done = 1;
        return;
    }

    if (g_splash_frame < SPLASH_HOLD_FRAMES) {
        alpha = 1.0f;
    } else {
        float t = (float)(g_splash_frame - SPLASH_HOLD_FRAMES)
                / (float)SPLASH_FADE_FRAMES;
        alpha = 1.0f - splash_smoothstep(t);
    }

    yaw = SPLASH_REST_YAW + (float)g_splash_frame * SPLASH_YAW_RATE;

    /* Centered single-line lockup: [cube] gap [gl-repl] gap [tagline]. */
    cube_w  = 2.0f * 1.414f * cube_s;
    title_w = (float)strlen(title) * FONT_W;
    tag_w   = (float)strlen(tagline) * FONT_SMALL_W;
    total_w = cube_w + 18.0f + title_w + 14.0f + tag_w;
    x = ((float)win_w - total_w) * 0.5f;
    if (x < 8.0f) x = 8.0f;

    gl2d_begin(win_w, win_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Band along the bottom + a faint top hairline. */
    {
        const float *bg = ui_rgba(UI_TOK_SUNKEN);
        glColor4f(bg[0], bg[1], bg[2], alpha * 0.92f);
        glRectf(0.0f, 0.0f, (float)win_w, band_h);
        glColor4f(1.0f, 1.0f, 1.0f, alpha * 0.08f);
        glRectf(0.0f, band_h - 1.0f, (float)win_w, band_h);
    }

    splash_draw_cube(x + cube_w * 0.5f, band_h * 0.5f, cube_s, yaw, alpha);

    glColor4f(0.90f, 0.93f, 0.97f, alpha);
    gl2d_draw_string(x + cube_w + 18.0f,
                     (band_h - (float)FONT_H) * 0.5f + 3.0f,
                     title, FONT_MONO);
    ui_clr_a(UI_TOK_TEXT_MUTED, alpha);
    gl2d_draw_string(x + cube_w + 18.0f + title_w + 14.0f,
                     (band_h - (float)FONT_H) * 0.5f + 3.0f,
                     tagline, FONT_SMALL);

    glDisable(GL_BLEND);
    gl2d_end();

    g_splash_frame++;
}
