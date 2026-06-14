/*
 * view_mode_swatch.c - see view_mode_swatch.h.
 *
 * Steady state and the 3D->2D cross-fade are flat 2D bitmap text. The
 * 2D->3D path bakes the two labels into cached GL textures (once per
 * theme/size) and renders a lit, rotating cube inside the cell's own
 * viewport. The texture-bake uses the only no-FBO texture-copy pattern in
 * the tree (glCopyTexSubImage2D, mirrored from src/scene/postprocess_filter.c):
 * the labels are drawn into the cell, copied to textures, then the cube is
 * drawn over them in the same frame, so the bake is never visible.
 */
#include "ui/app/view_mode_swatch.h"

#include <string.h>

#include "gl_includes.h"
#include "config.h"             /* FONT_SMALL, FONT_SMALL_W/H */
#include "ui/core/theme.h"
#include "ui/core/gl_2d.h"
#include "ui/core/metrics.h"    /* MENUBAR_TEXT_BASE_Y                    */

static const char *const k_label_3d = "3D";
static const char *const k_label_2d = "2D";

/* ---- pure selector ---------------------------------------------------- */

UiViewSwatchMode ui_view_mode_swatch_state(int ortho_mode,
                                           float projection_mix,
                                           float *out_t) {
    float t = 0.0f;
    UiViewSwatchMode mode;
    int target_2d = (ortho_mode != 0); /* SceneViewMode: 0 = 3D, 1 = 2D */

    if (target_2d) {
        if (projection_mix <= 0.0001f) {
            mode = UI_VIEW_SWATCH_FLAT_2D;
        } else {
            /* mix runs 1 -> 0 as we settle into 2D; fade-in progress = 1-mix. */
            mode = UI_VIEW_SWATCH_CROSSFADE;
            t = 1.0f - projection_mix;
        }
    } else {
        if (projection_mix >= 0.9999f) {
            mode = UI_VIEW_SWATCH_FLAT_3D;
        } else {
            /* mix runs 0 -> 1 as we settle into 3D; reveal progress = mix. */
            mode = UI_VIEW_SWATCH_CUBE;
            t = projection_mix;
        }
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (out_t) *out_t = t;
    return mode;
}

int ui_view_mode_swatch_label_width(void) {
    /* Both labels are 2 chars; reserve a little extra so the cube/text sit
     * comfortably and the divider has breathing room. */
    return 2 * FONT_SMALL_W + 26;
}

/* ---- shared 2D helpers ------------------------------------------------ */

/* Draw a 2-char label horizontally centered in the cell, vertically on the
 * Replay button's text baseline, in the given linear RGB. */
static void draw_label_centered(int cell_x, int cell_y, int cell_w,
                                const char *label, const float rgb[3],
                                float alpha) {
    int text_w = (int)strlen(label) * FONT_SMALL_W;
    int tx = cell_x + (cell_w - text_w) / 2;
    glColor4f(rgb[0], rgb[1], rgb[2], alpha);
    gl2d_draw_string((float)tx, (float)(cell_y + MENUBAR_TEXT_BASE_Y),
                     label, FONT_SMALL);
}

static void lerp3(const float *a, const float *b, float t, float out[3]) {
    out[0] = a[0] + (b[0] - a[0]) * t;
    out[1] = a[1] + (b[1] - a[1]) * t;
    out[2] = a[2] + (b[2] - a[2]) * t;
}

/* ---- cube textures + render ------------------------------------------ */

static int next_pow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* Cached label textures for the cube faces. Re-baked when the theme or the
 * cell size changes (singleton widget; GL is single-threaded). */
static GLuint g_tex_2d, g_tex_3d;
static int    g_baked_theme = -1;
static int    g_baked_w, g_baked_h;   /* POT texture dims */
static int    g_cell_w, g_cell_h;     /* cell px copied (umax/vmax basis) */

/* Render one label into the cell, then copy the cell pixels into `tex`. The
 * cell is in the menu bar's window-space 2D ortho (already active). */
static void bake_label_tex(GLuint tex, const char *label,
                           int cell_x, int cell_y, int cell_w, int cell_h,
                           int tex_w, int tex_h) {
    const float *bg = ui_rgba(UI_TOK_SURFACE);
    const float *fg = ui_rgba(UI_TOK_ACCENT);

    /* Opaque background == menu-bar surface so the lit, camera-facing face
     * dissolves into the bar; accent text on top. */
    glColor4f(bg[0], bg[1], bg[2], 1.0f);
    glRectf((float)cell_x, (float)cell_y,
            (float)(cell_x + cell_w), (float)(cell_y + cell_h));
    draw_label_centered(cell_x, cell_y, cell_w, label, fg, 1.0f);

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tex_w, tex_h, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, NULL);
    /* Copy the cell rect (window/bottom-left origin) into the texture. */
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cell_x, cell_y, cell_w, cell_h);
}

static void ensure_baked(int cell_x, int cell_y, int cell_w, int cell_h) {
    int tex_w = next_pow2(cell_w);
    int tex_h = next_pow2(cell_h);
    if (g_baked_theme == g_ui_theme && g_cell_w == cell_w && g_cell_h == cell_h)
        return;
    if (g_tex_2d == 0) glGenTextures(1, &g_tex_2d);
    if (g_tex_3d == 0) glGenTextures(1, &g_tex_3d);
    bake_label_tex(g_tex_2d, k_label_2d, cell_x, cell_y, cell_w, cell_h, tex_w, tex_h);
    bake_label_tex(g_tex_3d, k_label_3d, cell_x, cell_y, cell_w, cell_h, tex_w, tex_h);
    g_baked_theme = g_ui_theme;
    g_baked_w = tex_w;
    g_baked_h = tex_h;
    g_cell_w = cell_w;
    g_cell_h = cell_h;
}

/* One cube face: a unit quad at +Z (rotated into place by the caller),
 * textured 0..umax/0..vmax with the cell subregion, lit via its normal. */
static void cube_textured_face(float umax, float vmax) {
    glBegin(GL_QUADS);
        glNormal3f(0.0f, 0.0f, 1.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(-1.0f, -1.0f, 1.0f);
        glTexCoord2f(umax, 0.0f); glVertex3f( 1.0f, -1.0f, 1.0f);
        glTexCoord2f(umax, vmax); glVertex3f( 1.0f,  1.0f, 1.0f);
        glTexCoord2f(0.0f, vmax); glVertex3f(-1.0f,  1.0f, 1.0f);
    glEnd();
}

/* The four "side/back" faces (everything except the +Z face the caller
 * orients toward the camera) as plain lit quads in the surface color, so
 * the cube body shades 3D without carrying text. */
static void cube_solid_body(void) {
    glBegin(GL_QUADS);
        /* -Z */ glNormal3f(0,0,-1);
        glVertex3f( 1,-1,-1); glVertex3f(-1,-1,-1); glVertex3f(-1,1,-1); glVertex3f(1,1,-1);
        /* +Y */ glNormal3f(0,1,0);
        glVertex3f(-1,1,1); glVertex3f(1,1,1); glVertex3f(1,1,-1); glVertex3f(-1,1,-1);
        /* -Y */ glNormal3f(0,-1,0);
        glVertex3f(-1,-1,-1); glVertex3f(1,-1,-1); glVertex3f(1,-1,1); glVertex3f(-1,-1,1);
        /* -X */ glNormal3f(-1,0,0);
        glVertex3f(-1,-1,-1); glVertex3f(-1,-1,1); glVertex3f(-1,1,1); glVertex3f(-1,1,-1);
    glEnd();
}

static void render_cube(int cell_x, int cell_y, int cell_w, int cell_h,
                        float t) {
    if (cell_w <= 1 || cell_h <= 1) return;

    ensure_baked(cell_x, cell_y, cell_w, cell_h);

    const float *bg = ui_rgba(UI_TOK_SURFACE);
    float umax = (float)cell_w / (float)g_baked_w;
    float vmax = (float)cell_h / (float)g_baked_h;
    float aspect = (float)cell_w / (float)cell_h;

    GLint saved_mm = 0;
    glGetIntegerv(GL_MATRIX_MODE, &saved_mm);
    glPushAttrib(GL_ALL_ATTRIB_BITS); /* viewport, scissor, enables, depth/color masks */

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluPerspective(34.0, (double)aspect, 0.5, 12.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glViewport(cell_x, cell_y, cell_w, cell_h);
    glScissor(cell_x, cell_y, cell_w, cell_h);
    glEnable(GL_SCISSOR_TEST);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);     /* scissored to the cell */

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* Headlight: directional light from the camera (+Z eye), set at the
     * identity modelview so it rides the camera. Tuned so a face normal
     * pointing at the camera (N.L = 1) renders at full brightness — the
     * baked texture (accent on surface bg) then shows unmodulated, so the
     * camera-facing face reads as flat text on the bar. Oblique faces fall
     * off -> the rotation reads as a 3D cube. */
    GLfloat lpos[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
    GLfloat lamb[4] = { 0.32f, 0.32f, 0.32f, 1.0f };
    GLfloat ldif[4] = { 0.68f, 0.68f, 0.68f, 1.0f };
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT7);
    glLightfv(GL_LIGHT7, GL_POSITION, lpos);
    glLightfv(GL_LIGHT7, GL_AMBIENT, lamb);
    glLightfv(GL_LIGHT7, GL_DIFFUSE, ldif);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_NORMALIZE);

    /* Pull the cube back so a 2x2 face nearly fills the cell, then rotate
     * about Y so t=0 shows the +Z ("2D") face dead-on and t=1 brings the
     * +X ("3D") face dead-on (flat -> seamless handoff to the flat text). */
    glTranslatef(0.0f, 0.0f, -3.35f);
    glScalef(0.92f, 0.92f, 0.92f);
    glRotatef(-90.0f * t, 0.0f, 1.0f, 0.0f);

    /* Solid body (surface color) first. */
    glDisable(GL_TEXTURE_2D);
    glColor4f(bg[0], bg[1], bg[2], 1.0f);
    cube_solid_body();

    /* Textured faces modulate the lit brightness, keeping the accent hue. */
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    /* +Z face: "2D". */
    glBindTexture(GL_TEXTURE_2D, g_tex_2d);
    cube_textured_face(umax, vmax);

    /* +X face: "3D" — same +Z quad rotated -90 about Y into the +X slot. */
    glPushMatrix();
    glRotatef(90.0f, 0.0f, 1.0f, 0.0f);
    glBindTexture(GL_TEXTURE_2D, g_tex_3d);
    cube_textured_face(umax, vmax);
    glPopMatrix();

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode((GLenum)saved_mm);
    glPopAttrib();
}

/* ---- public render ---------------------------------------------------- */

void ui_view_mode_swatch_render(int cell_x, int cell_y, int cell_w,
                                int cell_h, int ortho_mode,
                                float projection_mix) {
    float t = 0.0f;
    UiViewSwatchMode mode =
        ui_view_mode_swatch_state(ortho_mode, projection_mix, &t);

    const float *accent  = ui_rgba(UI_TOK_ACCENT);
    const float *surface = ui_rgba(UI_TOK_SURFACE);

    switch (mode) {
    case UI_VIEW_SWATCH_FLAT_3D:
        draw_label_centered(cell_x, cell_y, cell_w, k_label_3d, accent, 1.0f);
        break;
    case UI_VIEW_SWATCH_FLAT_2D:
        draw_label_centered(cell_x, cell_y, cell_w, k_label_2d, accent, 1.0f);
        break;
    case UI_VIEW_SWATCH_CROSSFADE: {
        /* "3D" dissolves toward the bar bg; "2D" emerges from it. Drawn
         * opaque as color lerps so the text literally melts into / out of
         * the surface color (no order-dependent alpha). */
        float c3[3], c2[3];
        lerp3(accent, surface, t, c3);  /* 3D: accent -> bg     */
        lerp3(surface, accent, t, c2);  /* 2D: bg     -> accent */
        draw_label_centered(cell_x, cell_y, cell_w, k_label_3d, c3, 1.0f);
        draw_label_centered(cell_x, cell_y, cell_w, k_label_2d, c2, 1.0f);
        break;
    }
    case UI_VIEW_SWATCH_CUBE:
        render_cube(cell_x, cell_y, cell_w, cell_h, t);
        break;
    }
}
