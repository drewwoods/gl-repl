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

#include <math.h>
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

static float smoothstep01(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return x * x * (3.0f - 2.0f * x);
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

static void render_cube(int cell_x, int cell_y, int cell_w, int cell_h,
                        float t) {
    if (cell_h <= 1 || cell_w <= 1) return;

    /* Render the cube into a centered SQUARE sub-region of the (wide) cell —
     * a square viewport + unit-box ortho keeps the cube un-stretched, so it
     * reads as a real turning cube rather than two labels spread apart. The
     * texture is baked over the same square (text centered) so a face-on
     * face matches the flat centered label. */
    int sq = cell_h;
    int sx = cell_x + (cell_w - sq) / 2;
    int sy = cell_y;

    ensure_baked(sx, sy, sq, sq);

    const float *bg = ui_rgba(UI_TOK_SURFACE);
    float umax = (float)sq / (float)g_baked_w;
    float vmax = (float)sq / (float)g_baked_h;

    glPushAttrib(GL_ALL_ATTRIB_BITS); /* viewport, scissor, enables, depth/color masks, matrix mode */

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    /* Aspect-corrected ortho over the FULL (wide) cell: a unit cube stays
     * square (un-stretched) and fills the cell height at rest, but the wide
     * cell now gives the spinning/tilting cube horizontal head-room so it is
     * never clipped mid-rotation (a cube needs ~1.41x its width at 45deg;
     * the cell is ~1.9x). y stays -1..1 so the resting face fills the
     * height; z range holds the tilt + depth. */
    double aspect = (double)cell_w / (double)cell_h;
    glOrtho(-aspect, aspect, -1.0, 1.0, -2.0, 2.0);

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

    /* Light from the camera (+Z eye), set at the identity modelview so it
     * rides the camera. Mostly ambient: a face pointing at the camera
     * reaches full brightness (the baked accent-on-surface texture shows
     * unmodulated -> matches the bar bg + flat accent text exactly), while
     * oblique faces only dim slightly (0.8..1.0) -> a subtle 3D cue that
     * stays in the bar's color family rather than reading as a dark box. */
    GLfloat lpos[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
    GLfloat lamb[4] = { 0.80f, 0.80f, 0.80f, 1.0f };
    GLfloat ldif[4] = { 0.20f, 0.20f, 0.20f, 1.0f };
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT7);
    glLightfv(GL_LIGHT7, GL_POSITION, lpos);
    glLightfv(GL_LIGHT7, GL_AMBIENT, lamb);
    glLightfv(GL_LIGHT7, GL_DIFFUSE, ldif);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_NORMALIZE);

    /* Rotate about Y: t=0 shows the +Z ("2D") face dead-on, t=1 brings the
     * +X ("3D") face dead-on (both flat -> seamless handoff to flat text). */
    glRotatef(-90.0f * t, 0.0f, 1.0f, 0.0f);

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
    glPopAttrib();
}

/* ---- public render ---------------------------------------------------- */

void ui_view_mode_swatch_render(int cell_x, int cell_y, int cell_w,
                                int cell_h, int ortho_mode,
                                float projection_mix) {
    float t = 0.0f;
    UiViewSwatchMode mode =
        ui_view_mode_swatch_state(ortho_mode, projection_mix, &t);

    const float *accent = ui_rgba(UI_TOK_ACCENT);

    switch (mode) {
    case UI_VIEW_SWATCH_FLAT_3D:
        draw_label_centered(cell_x, cell_y, cell_w, k_label_3d, accent, 1.0f);
        break;
    case UI_VIEW_SWATCH_FLAT_2D:
        draw_label_centered(cell_x, cell_y, cell_w, k_label_2d, accent, 1.0f);
        break;
    case UI_VIEW_SWATCH_CROSSFADE: {
        /* Sequential hand-off (NOT a simultaneous overlay — two 2-char
         * labels share the cell center, so drawing both at once garbles
         * the glyphs): "3D" fades out to the bar bg over the first ~half,
         * then "2D" fades in. Alpha-blended over the surface the menu bar
         * already drew, so each label cleanly dissolves into / out of the
         * background color. */
        float a3 = 1.0f - smoothstep01(t / 0.55f);
        float a2 = smoothstep01((t - 0.45f) / 0.55f);
        if (a3 > 0.004f)
            draw_label_centered(cell_x, cell_y, cell_w, k_label_3d, accent, a3);
        if (a2 > 0.004f)
            draw_label_centered(cell_x, cell_y, cell_w, k_label_2d, accent, a2);
        break;
    }
    case UI_VIEW_SWATCH_CUBE:
        render_cube(cell_x, cell_y, cell_w, cell_h, t);
        break;
    }
}
