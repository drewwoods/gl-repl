/*
 * tools/compare_stroke_fonts/compare_stroke_fonts.c
 * (also available as tools/compare_stroke_fonts.c)
 *
 * Standalone OpenGL / GLUT application comparing the 4 stroke fonts
 * available in vendored freeglut:
 *
 *   1. GLUT_STROKE_ROMAN          - Proportional Hershey Roman (standard polyline)
 *   2. GLUT_STROKE_ROMAN_HI       - Proportional Hershey Roman (Catmull-Rom high-res)
 *   3. GLUT_STROKE_MONO_ROMAN     - Monospaced Hershey Roman   (standard polyline)
 *   4. GLUT_STROKE_MONO_ROMAN_HI  - Monospaced Hershey Roman   (Catmull-Rom high-res)
 *
 * Overview of Differences:
 *   - The standard stroke fonts (ROMAN, MONO_ROMAN) use coarse polyline
 *     approximations for curved glyphs (e.g. 'O', 'S', 'C', 'g', '8', '&', '@',
 *     and the rounded arches of 'm', 'n', 'h', 'u'), which appear noticeably
 *     angular when scaled up.
 *   - The high-resolution stroke fonts (ROMAN_HI, MONO_ROMAN_HI) share the EXACT
 *     same glyph metrics, advance widths, bounding boxes, baseline, and corner
 *     anchors, but have their curved strokes resampled via corner-preserving
 *     centripetal Catmull-Rom splines (~3x-5x line segment density). Punctuation
 *     dots ('.', ':', '!', '?', 'i', 'j') are generated as true circular marks.
 *   - MONO variants fix every character advance to 104.762 font units, whereas
 *     proportional variants vary advance per glyph (e.g. 'i' is ~26.6, 'm' is ~123.8,
 *     'W' is ~140.0). Words with consecutive letters like "command" highlight both
 *     the advance width difference and the spline smoothness on adjacent arches.
 *
 * Visualization Modes:
 *   [Tab / M] Cycle View Modes:
 *     - Mode 0: 4-Row Stacked Comparison (Overview of all 4 fonts simultaneously)
 *     - Mode 1: 2x2 Split Grid (ROMAN vs ROMAN_HI on top, MONO vs MONO_HI on bottom)
 *     - Mode 2: Direct Overlay / Diff (Standard in Red/Orange vs Hi-Res in Cyan/White)
 *     - Mode 3: Single Glyph Inspector (Magnified character view with vertex join dots)
 *     - Mode 4: Full ASCII Character Set Matrix (ASCII 32..126 grid)
 *     - Mode 5: 3D Vector Space Orbit (Perspective 3D rendering with animated camera)
 *
 * Interactive Controls:
 *   - Tab / M         : Cycle visualization modes (0..5)
 *   - 1, 2, 3, 4      : Select active font / isolate font in view
 *   - P               : Cycle sample text presets (command tests, pangrams, curves, code)
 *   - + / - (or Wheel): Zoom in / out
 *   - Arrow keys / Drag: Pan view (or select character in Glyph Inspector)
 *   - [ / ]           : Decrease / increase line width (1.0 .. 10.0 px)
 *   - A               : Toggle line antialiasing (GL_LINE_SMOOTH + GL_BLEND)
 *   - J               : Toggle vertex join dots (GLUT_STROKE_FONT_DRAW_JOIN_DOTS)
 *   - G               : Toggle typography metrics guide lines (baseline, cap, descender)
 *   - B               : Toggle blinking alternation (in Overlay / Diff mode)
 *   - Space           : Pause / resume 3D rotation or blinking
 *   - R               : Reset view / pan / zoom / rotation
 *   - H / F1          : Toggle on-screen HUD & keyboard help
 *   - Esc / Q         : Quit
 *
 * Build Instructions:
 *
 *   macOS (against vendored freeglut in this repo):
 *     clang -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE \
 *       -DGL_SILENCE_DEPRECATION -DFREEGLUT_STATIC \
 *       -Ithird_party/freeglut/include tools/compare_stroke_fonts/compare_stroke_fonts.c \
 *       third_party/freeglut/build/lib/libglut.a \
 *       -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -lm \
 *       -o build/compare_stroke_fonts
 *
 *   Linux (against vendored freeglut):
 *     gcc -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE -DFREEGLUT_STATIC \
 *       -Ithird_party/freeglut/include tools/compare_stroke_fonts/compare_stroke_fonts.c \
 *       third_party/freeglut/build/lib/libglut.a \
 *       -lGL -lGLU -lX11 -lXrandr -lXxf86vm -lXi -lm -lpthread \
 *       -o build/compare_stroke_fonts
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#ifdef __APPLE__
#  ifndef GL_SILENCE_DEPRECATION
#    define GL_SILENCE_DEPRECATION
#  endif
#  include <OpenGL/gl.h>
#  include <OpenGL/glu.h>
#endif

#include <GL/freeglut.h>
#include <GL/freeglut_ext.h>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------
 * Font Definitions and Metadata
 * ------------------------------------------------------------------------- */
typedef struct {
    void        *id;
    const char  *id_name;
    const char  *display_name;
    const char  *family;
    const char  *resolution;
    float        color[4];
    float        bg_color[4];
    bool         is_hi_res;
    bool         is_mono;
} StrokeFontDesc;

static const StrokeFontDesc g_fonts[4] = {
    {
        .id           = GLUT_STROKE_ROMAN,
        .id_name      = "GLUT_STROKE_ROMAN",
        .display_name = "Roman (Standard)",
        .family       = "Proportional",
        .resolution   = "Standard Polyline",
        .color        = { 0.98f, 0.76f, 0.25f, 1.0f }, /* Warm Gold */
        .bg_color     = { 0.98f, 0.76f, 0.25f, 0.12f },
        .is_hi_res    = false,
        .is_mono      = false,
    },
    {
        .id           = GLUT_STROKE_ROMAN_HI,
        .id_name      = "GLUT_STROKE_ROMAN_HI",
        .display_name = "Roman (High-Res)",
        .family       = "Proportional",
        .resolution   = "Catmull-Rom High-Res",
        .color        = { 0.22f, 0.85f, 0.96f, 1.0f }, /* Bright Cyan */
        .bg_color     = { 0.22f, 0.85f, 0.96f, 0.12f },
        .is_hi_res    = true,
        .is_mono      = false,
    },
    {
        .id           = GLUT_STROKE_MONO_ROMAN,
        .id_name      = "GLUT_STROKE_MONO_ROMAN",
        .display_name = "Mono Roman (Standard)",
        .family       = "Monospaced (104.76u)",
        .resolution   = "Standard Polyline",
        .color        = { 1.00f, 0.45f, 0.35f, 1.0f }, /* Coral / Salmon */
        .bg_color     = { 1.00f, 0.45f, 0.35f, 0.12f },
        .is_hi_res    = false,
        .is_mono      = true,
    },
    {
        .id           = GLUT_STROKE_MONO_ROMAN_HI,
        .id_name      = "GLUT_STROKE_MONO_ROMAN_HI",
        .display_name = "Mono Roman (High-Res)",
        .family       = "Monospaced (104.76u)",
        .resolution   = "Catmull-Rom High-Res",
        .color        = { 0.38f, 0.92f, 0.58f, 1.0f }, /* Mint Green */
        .bg_color     = { 0.38f, 0.92f, 0.58f, 0.12f },
        .is_hi_res    = true,
        .is_mono      = true,
    }
};

/* -------------------------------------------------------------------------
 * Sample Text Presets (Featuring double-m words like "command")
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *title;
    const char *text;
} TextPreset;

static const TextPreset g_presets[] = {
    {
        .title = "Command & Consecutive 'mm' Test",
        .text  = "Execute command: immediate symmetric gamma recommendation (mm / MM)"
    },
    {
        .title = "OpenGL Commands & Immediate Mode",
        .text  = "glCommands: command, glMultiDrawArrays, immediate summation"
    },
    {
        .title = "Pangram 1 (Fox + Command)",
        .text  = "The quick brown fox jumps over the lazy dog -- command OK"
    },
    {
        .title = "Curved Glyph Stress Test (Curves, Punctuation & mm)",
        .text  = "command O S C G Q 8 9 & % $ @ ~ ? { } ( ) 3 5 6 mm MM"
    },
    {
        .title = "Pangram 2 (Quartz Vow & Programmer)",
        .text  = "Sphinx of black quartz, judge my programmer vow!"
    },
    {
        .title = "Pangram 3 (Liquor Jugs & Commander)",
        .text  = "Commander, pack my box with five dozen liquor jugs."
    },
    {
        .title = "Numerics & Symbols",
        .text  = "0123456789 +-/*= <> () [] {} !@#$%^&* _~:;\"'`"
    },
    {
        .title = "Full Alphabet (Upper & Lower)",
        .text  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz"
    },
    {
        .title = "OpenGL C Code Snippet (glCmd)",
        .text  = "void exec_command(GLenum mode) { glBegin(mode); glVertex2f(x, y); glEnd(); }"
    },
    {
        .title = "Dots & Marks Alignment",
        .text  = "i. j: !? ... ,, ;; '' \"\" @@ ## $$ %% && ** (( )) command"
    }
};

static const int g_preset_count = (int)(sizeof(g_presets) / sizeof(g_presets[0]));

/* -------------------------------------------------------------------------
 * Application State
 * ------------------------------------------------------------------------- */
typedef enum {
    MODE_FOUR_ROWS   = 0, /* 4 horizontal rows comparing all fonts */
    MODE_GRID_2X2    = 1, /* 2x2 quadrant comparison */
    MODE_OVERLAY     = 2, /* Direct overlay / difference comparison */
    MODE_INSPECTOR   = 3, /* Magnified single glyph inspector */
    MODE_ASCII_TABLE = 4, /* Full ASCII 32..126 glyph matrix */
    MODE_3D_ORBIT    = 5, /* 3D vector space orbit showcase */
    MODE_COUNT       = 6
} ViewMode;

static const char *g_mode_names[MODE_COUNT] = {
    "4-Row Stacked Overview",
    "2x2 Split Grid",
    "Overlay / Difference",
    "Single Glyph Inspector",
    "ASCII Character Matrix",
    "3D Vector Space Orbit"
};

static struct {
    int   window_w;
    int   window_h;
    int   current_mode;
    int   current_preset;
    int   active_font_idx;      /* 0..3 */
    int   inspector_char;       /* ASCII 32..126 */
    int   overlay_pair;         /* 0 = Roman pair, 1 = Mono pair */

    /* View transform */
    float pan_x;
    float pan_y;
    float zoom;
    float rot_x;
    float rot_y;
    float rot_z;
    bool  animating_3d;

    /* Rendering options */
    float line_width;
    bool  antialiasing;
    bool  join_dots;
    bool  show_guides;
    bool  show_metrics_hud;
    bool  show_help;
    bool  blink_enabled;
    bool  blink_state;
    int   blink_timer_ms;

    /* Halo & Spacing options (inspired by glr_pointer_script.c) */
    bool  halo_enabled;
    float halo_size;           /* extra line width for dark halo pass */
    float char_spacing;        /* extra character spacing in font units */

    /* Mouse interaction */
    int   mouse_down_x;
    int   mouse_down_y;
    int   mouse_button;
    bool  mouse_dragging;
} g_app = {
    .window_w         = 1200,
    .window_h         = 820,
    .current_mode     = MODE_FOUR_ROWS,
    .current_preset   = 0,
    .active_font_idx  = 0,
    .inspector_char   = 'm', /* Default to 'm' for inspection */
    .overlay_pair     = 0,

    .pan_x            = 0.0f,
    .pan_y            = 0.0f,
    .zoom             = 1.0f,
    .rot_x            = 20.0f,
    .rot_y            = -30.0f,
    .rot_z            = 0.0f,
    .animating_3d     = true,

    .line_width       = 2.0f,
    .antialiasing     = true,
    .join_dots        = false,
    .show_guides      = true,
    .show_metrics_hud = true,
    .show_help        = true,
    .blink_enabled    = false,
    .blink_state      = false,
    .blink_timer_ms   = 500,

    .halo_enabled     = false,
    .halo_size        = 3.0f,
    .char_spacing     = 0.0f,

    .mouse_down_x     = 0,
    .mouse_down_y     = 0,
    .mouse_button     = -1,
    .mouse_dragging   = false
};

/* -------------------------------------------------------------------------
 * Utility 2D Drawing & Bitmap Text Helpers
 * ------------------------------------------------------------------------- */

static void draw_bitmap_string_at(float x, float y, void *font, const char *str)
{
    glRasterPos2f(x, y);
    while (*str) {
        glutBitmapCharacter(font, (unsigned char)*str++);
    }
}

static int get_bitmap_string_width(void *font, const char *str)
{
    if (!str) return 0;
    return glutBitmapLength(font, (const unsigned char *)str);
}

static void draw_truncated_bitmap_string(float x, float y, float max_w, void *font, const char *prefix, const char *str)
{
    char buf[256];
    if (!str || max_w <= 10.0f) return;
    int pref_w = prefix ? get_bitmap_string_width(font, prefix) : 0;
    if ((float)pref_w >= max_w) return;

    float remain_w = max_w - (float)pref_w;
    int full_w = get_bitmap_string_width(font, str);
    if ((float)full_w <= remain_w) {
        if (prefix) {
            draw_bitmap_string_at(x, y, font, prefix);
            draw_bitmap_string_at(x + (float)pref_w, y, font, str);
        } else {
            draw_bitmap_string_at(x, y, font, str);
        }
        return;
    }

    int dot_w = get_bitmap_string_width(font, "...");
    remain_w -= (float)dot_w;
    if (remain_w <= 0.0f) return;

    size_t len = strlen(str);
    size_t fit_len = 0;
    for (size_t i = 1; i <= len && i < sizeof(buf) - 5; ++i) {
        memcpy(buf, str, i);
        buf[i] = '\0';
        if ((float)get_bitmap_string_width(font, buf) > remain_w) {
            break;
        }
        fit_len = i;
    }
    memcpy(buf, str, fit_len);
    strcpy(buf + fit_len, "...");

    if (prefix) {
        draw_bitmap_string_at(x, y, font, prefix);
        draw_bitmap_string_at(x + (float)pref_w, y, font, buf);
    } else {
        draw_bitmap_string_at(x, y, font, buf);
    }
}

static void draw_filled_rect(float x0, float y0, float x1, float y1,
                             float r, float g, float b, float a)
{
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
}

static void draw_rect_outline(float x0, float y0, float x1, float y1,
                              float r, float g, float b, float a, float width)
{
    glLineWidth(width);
    glColor4f(r, g, b, a);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
}

static void draw_dashed_horizontal_line(float x0, float x1, float y,
                                       float r, float g, float b, float a,
                                       float segment_len, float gap_len)
{
    glColor4f(r, g, b, a);
    glBegin(GL_LINES);
    float cur_x = x0;
    while (cur_x < x1) {
        float next_x = cur_x + segment_len;
        if (next_x > x1) next_x = x1;
        glVertex2f(cur_x, y);
        glVertex2f(next_x, y);
        cur_x = next_x + gap_len;
    }
    glEnd();
}

/* -------------------------------------------------------------------------
 * Guide Lines (Baseline, Cap Height, Descender, Top Ascent)
 * ------------------------------------------------------------------------- */
static void draw_stroke_metrics_guides(float start_x, float width,
                                      float scale, float base_y)
{
    if (!g_app.show_guides) return;

    glPushAttrib(GL_ENABLE_BIT | GL_LINE_BIT | GL_CURRENT_BIT);
    glDisable(GL_LIGHTING);
    glLineWidth(1.0f);

    float end_x = start_x + width;

    /* Baseline: y = 0 */
    float y_base = base_y;
    draw_dashed_horizontal_line(start_x, end_x, y_base, 0.8f, 0.85f, 0.9f, 0.45f, 8.0f, 4.0f);

    /* Cap Height / Upper ascent: y = ~119.05 * scale */
    float y_cap = base_y + 119.05f * scale;
    draw_dashed_horizontal_line(start_x, end_x, y_cap, 0.3f, 0.7f, 1.0f, 0.35f, 4.0f, 4.0f);

    /* Total Font Height: y = 152.381 * scale */
    float y_top = base_y + 152.381f * scale;
    draw_dashed_horizontal_line(start_x, end_x, y_top, 1.0f, 0.9f, 0.3f, 0.25f, 2.0f, 4.0f);

    /* Descender: y = -33.33 * scale */
    float y_desc = base_y - 33.33f * scale;
    draw_dashed_horizontal_line(start_x, end_x, y_desc, 1.0f, 0.4f, 0.4f, 0.30f, 4.0f, 4.0f);

    glPopAttrib();
}

static float compute_stroke_string_width(void *font, const char *str, float char_spacing)
{
    if (!str) return 0.0f;
    float w = 0.0f;
    size_t len = strlen(str);
    for (size_t i = 0; i < len; ++i) {
        w += glutStrokeWidthf(font, (int)(unsigned char)str[i]);
        if (i + 1 < len) {
            w += char_spacing;
        }
    }
    return w;
}

static void draw_stroke_string_with_spacing(void *font, const char *str, float char_spacing)
{
    if (!str) return;
    size_t len = strlen(str);
    for (size_t i = 0; i < len; ++i) {
        glutStrokeCharacter(font, (int)(unsigned char)str[i]);
        if (i + 1 < len && char_spacing != 0.0f) {
            glTranslatef(char_spacing, 0.0f, 0.0f);
        }
    }
}

static void render_stroke_text_styled(void *font, const char *str,
                                      float scale, float base_line_width,
                                      const float color[4])
{
    if (!str) return;

    if (g_app.halo_enabled && g_app.halo_size > 0.0f) {
        /* Pass 0: Dark Halo pass underneath (inspired by glr_pointer_script.c) */
        glPushMatrix();
        glScalef(scale, scale, scale);
        glLineWidth(base_line_width + g_app.halo_size);
        glColor4f(0.02f, 0.03f, 0.05f, 0.92f);
        draw_stroke_string_with_spacing(font, str, g_app.char_spacing);
        glPopMatrix();
    }

    /* Pass 1 (or single pass): Glyph body */
    glPushMatrix();
    glScalef(scale, scale, scale);
    glLineWidth(base_line_width);
    glColor4fv(color);
    draw_stroke_string_with_spacing(font, str, g_app.char_spacing);
    glPopMatrix();
}

/* -------------------------------------------------------------------------
 * Render Modes
 * ------------------------------------------------------------------------- */

/* Mode 0: 4 Horizontal Rows Stacked Comparison */
static void render_mode_four_rows(void)
{
    const char *text = g_presets[g_app.current_preset].text;
    int w = g_app.window_w;
    int h = g_app.window_h;

    float header_h = 100.0f;
    float footer_h = 40.0f;
    float available_h = (float)h - header_h - footer_h;
    float row_h = available_h / 4.0f;

    for (int i = 0; i < 4; ++i) {
        float y_top = (float)h - header_h - (float)i * row_h;
        float y_bot = y_top - row_h;
        const StrokeFontDesc *desc = &g_fonts[i];

        /* Row background and border */
        bool is_active = (g_app.active_font_idx == i);
        if (is_active) {
            draw_filled_rect(20.0f, y_bot + 6.0f, (float)w - 20.0f, y_top - 6.0f,
                             desc->color[0], desc->color[1], desc->color[2], 0.08f);
            draw_rect_outline(20.0f, y_bot + 6.0f, (float)w - 20.0f, y_top - 6.0f,
                              desc->color[0], desc->color[1], desc->color[2], 0.6f, 1.5f);
        } else {
            draw_filled_rect(20.0f, y_bot + 6.0f, (float)w - 20.0f, y_top - 6.0f,
                             0.12f, 0.14f, 0.18f, 0.75f);
            draw_rect_outline(20.0f, y_bot + 6.0f, (float)w - 20.0f, y_top - 6.0f,
                              0.25f, 0.28f, 0.35f, 0.4f, 1.0f);
        }

        /* Badge and Title */
        float badge_w = 125.0f;
        draw_filled_rect(30.0f, y_top - 32.0f, 30.0f + badge_w, y_top - 12.0f,
                         desc->color[0], desc->color[1], desc->color[2], 0.25f);
        glColor3f(desc->color[0], desc->color[1], desc->color[2]);
        char badge[64];
        snprintf(badge, sizeof(badge), "[%d] %s", i + 1, desc->is_hi_res ? "HI-RES" : "STANDARD");
        draw_bitmap_string_at(36.0f, y_top - 26.0f, GLUT_BITMAP_HELVETICA_12, badge);

        /* Font description string */
        glColor3f(0.9f, 0.9f, 0.95f);
        float title_x = 30.0f + badge_w + 10.0f;
        draw_bitmap_string_at(title_x, y_top - 26.0f, GLUT_BITMAP_HELVETICA_12, desc->id_name);
        int title_w = get_bitmap_string_width(GLUT_BITMAP_HELVETICA_12, desc->id_name);

        /* Type & Advance details (dynamically fitted / right-aligned) */
        glColor3f(0.6f, 0.65f, 0.75f);
        char details[128];
        GLfloat str_len = compute_stroke_string_width(desc->id, text, g_app.char_spacing);
        if ((float)w >= 1080.0f) {
            snprintf(details, sizeof(details), "Family: %s | Spline: %s | Len: %.1fu | H: %.1fu",
                     desc->family, desc->resolution, (double)str_len, (double)glutStrokeHeight(desc->id));
        } else {
            snprintf(details, sizeof(details), "%s | Len: %.1fu | H: %.1fu",
                     desc->resolution, (double)str_len, (double)glutStrokeHeight(desc->id));
        }
        int det_w = get_bitmap_string_width(GLUT_BITMAP_8_BY_13, details);
        float det_x = (float)w - 30.0f - (float)det_w;
        if (det_x > title_x + (float)title_w + 15.0f) {
            draw_bitmap_string_at(det_x, y_top - 26.0f, GLUT_BITMAP_8_BY_13, details);
        }

        /* Measure text and determine baseline position */
        float text_scale = 0.38f * g_app.zoom;
        float base_x = 40.0f + g_app.pan_x;
        float base_y = y_bot + 38.0f + g_app.pan_y;

        /* Draw metric guide lines */
        draw_stroke_metrics_guides(base_x - 10.0f, (float)w - 60.0f, text_scale, base_y);

        /* Render Stroke Text with styled halo and spacing */
        glPushMatrix();
        glTranslatef(base_x, base_y, 0.0f);
        render_stroke_text_styled(desc->id, text, text_scale, g_app.line_width, desc->color);
        glPopMatrix();
    }
}

/* Mode 1: 2x2 Quadrant Grid Split View */
static void render_mode_grid_2x2(void)
{
    const char *text = g_presets[g_app.current_preset].text;
    int w = g_app.window_w;
    int h = g_app.window_h;

    float header_h = 90.0f;
    float footer_h = 35.0f;
    float usable_w = (float)w - 40.0f;
    float usable_h = (float)h - header_h - footer_h;
    float quad_w   = usable_w * 0.5f - 8.0f;
    float quad_h   = usable_h * 0.5f - 8.0f;

    /* 4 quadrants coordinates: Top-Left, Top-Right, Bottom-Left, Bottom-Right */
    float quad_x[4] = { 20.0f, 20.0f + quad_w + 16.0f, 20.0f, 20.0f + quad_w + 16.0f };
    float quad_y[4] = {
        (float)h - header_h - quad_h,
        (float)h - header_h - quad_h,
        footer_h + 10.0f,
        footer_h + 10.0f
    };

    for (int i = 0; i < 4; ++i) {
        float qx0 = quad_x[i];
        float qy0 = quad_y[i];
        float qx1 = qx0 + quad_w;
        float qy1 = qy0 + quad_h;
        const StrokeFontDesc *desc = &g_fonts[i];

        /* Panel Box */
        draw_filled_rect(qx0, qy0, qx1, qy1, 0.12f, 0.14f, 0.18f, 0.85f);
        draw_rect_outline(qx0, qy0, qx1, qy1, desc->color[0], desc->color[1], desc->color[2], 0.5f, 1.5f);

        /* Header in quadrant */
        draw_filled_rect(qx0, qy1 - 30.0f, qx1, qy1,
                         desc->color[0], desc->color[1], desc->color[2], 0.2f);
        glColor3f(desc->color[0], desc->color[1], desc->color[2]);
        draw_bitmap_string_at(qx0 + 12.0f, qy1 - 20.0f, GLUT_BITMAP_HELVETICA_12, desc->display_name);

        glColor3f(0.7f, 0.75f, 0.85f);
        char info[64];
        snprintf(info, sizeof(info), "%s | %s", desc->family, desc->resolution);
        int info_w = get_bitmap_string_width(GLUT_BITMAP_8_BY_13, info);
        float info_x = qx1 - 12.0f - (float)info_w;
        if (info_x > qx0 + 12.0f + (float)get_bitmap_string_width(GLUT_BITMAP_HELVETICA_12, desc->display_name) + 10.0f) {
            draw_bitmap_string_at(info_x, qy1 - 20.0f, GLUT_BITMAP_8_BY_13, info);
        }

        /* Render Text inside quadrant */
        float text_scale = 0.42f * g_app.zoom;
        float base_x = qx0 + 20.0f + g_app.pan_x;
        float base_y = qy0 + quad_h * 0.40f + g_app.pan_y;

        draw_stroke_metrics_guides(qx0 + 10.0f, quad_w - 20.0f, text_scale, base_y);

        glPushMatrix();
        glTranslatef(base_x, base_y, 0.0f);
        render_stroke_text_styled(desc->id, text, text_scale, g_app.line_width, desc->color);
        glPopMatrix();
    }
}

/* Mode 2: Direct Overlay / Difference Comparison */
static void render_mode_overlay(void)
{
    const char *text = g_presets[g_app.current_preset].text;
    int w = g_app.window_w;
    int h = g_app.window_h;

    int idx_std = (g_app.overlay_pair == 0) ? 0 : 2; /* ROMAN or MONO_ROMAN */
    int idx_hi  = (g_app.overlay_pair == 0) ? 1 : 3; /* ROMAN_HI or MONO_ROMAN_HI */

    const StrokeFontDesc *font_std = &g_fonts[idx_std];
    const StrokeFontDesc *font_hi  = &g_fonts[idx_hi];

    float box_x0 = 30.0f;
    float box_y0 = 60.0f;
    float box_x1 = (float)w - 30.0f;
    float box_y1 = (float)h - 110.0f;

    draw_filled_rect(box_x0, box_y0, box_x1, box_y1, 0.10f, 0.12f, 0.16f, 0.9f);
    draw_rect_outline(box_x0, box_y0, box_x1, box_y1, 0.35f, 0.4f, 0.5f, 0.6f, 1.5f);

    /* Legend / Overlay Header */
    char pair_info[128];
    snprintf(pair_info, sizeof(pair_info), "Overlay: %s (1: Roman, 2: Mono, B: Blink)",
             g_app.overlay_pair == 0 ? "Proportional Roman" : "Monospaced Roman");
    glColor3f(1.0f, 1.0f, 1.0f);
    draw_bitmap_string_at(box_x0 + 20.0f, box_y1 - 30.0f, GLUT_BITMAP_HELVETICA_18, pair_info);

    /* Legend boxes dynamically positioned */
    float leg1_x = box_x0 + 20.0f;
    draw_filled_rect(leg1_x, box_y1 - 65.0f, leg1_x + 20.0f, box_y1 - 50.0f,
                     font_std->color[0], font_std->color[1], font_std->color[2], 1.0f);
    glColor3fv(font_std->color);
    char legend_std[64];
    snprintf(legend_std, sizeof(legend_std), "Standard (%s)", font_std->display_name);
    draw_bitmap_string_at(leg1_x + 28.0f, box_y1 - 62.0f, GLUT_BITMAP_HELVETICA_12, legend_std);
    int leg1_w = get_bitmap_string_width(GLUT_BITMAP_HELVETICA_12, legend_std);

    float leg2_x = leg1_x + 28.0f + (float)leg1_w + 30.0f;
    draw_filled_rect(leg2_x, box_y1 - 65.0f, leg2_x + 20.0f, box_y1 - 50.0f,
                     font_hi->color[0], font_hi->color[1], font_hi->color[2], 1.0f);
    glColor3fv(font_hi->color);
    char legend_hi[64];
    snprintf(legend_hi, sizeof(legend_hi), "High-Res (%s)", font_hi->display_name);
    draw_bitmap_string_at(leg2_x + 28.0f, box_y1 - 62.0f, GLUT_BITMAP_HELVETICA_12, legend_hi);

    float text_scale = 0.55f * g_app.zoom;
    float base_x = box_x0 + 40.0f + g_app.pan_x;
    float base_y = box_y0 + (box_y1 - box_y0) * 0.45f + g_app.pan_y;

    draw_stroke_metrics_guides(base_x - 20.0f, (box_x1 - box_x0) - 40.0f, text_scale, base_y);

    /* Render Standard Font */
    if (!g_app.blink_enabled || !g_app.blink_state) {
        glPushMatrix();
        glTranslatef(base_x, base_y, 0.0f);
        float std_clr[4] = { font_std->color[0], font_std->color[1], font_std->color[2], 0.75f };
        render_stroke_text_styled(font_std->id, text, text_scale, g_app.line_width + 1.5f, std_clr);
        glPopMatrix();
    }

    /* Render High-Res Font */
    if (!g_app.blink_enabled || g_app.blink_state) {
        glPushMatrix();
        glTranslatef(base_x, base_y, 0.0f);
        float hi_clr[4] = { font_hi->color[0], font_hi->color[1], font_hi->color[2], 0.95f };
        render_stroke_text_styled(font_hi->id, text, text_scale, g_app.line_width, hi_clr);
        glPopMatrix();
    }
}

/* Mode 3: Single Glyph Inspector / Magnifier */
static void render_mode_inspector(void)
{
    int w = g_app.window_w;
    int h = g_app.window_h;
    int c = g_app.inspector_char;

    float header_h = 100.0f;
    float footer_h = 45.0f;
    float usable_w = (float)w - 40.0f;
    float col_w    = usable_w * 0.25f - 10.0f;
    float panel_h  = (float)h - header_h - footer_h;

    /* Info Banner */
    char banner[128];
    snprintf(banner, sizeof(banner),
             "Glyph Inspector: ASCII %d (0x%02X) - '%c'  [Use Left/Right or , / . to change glyph]",
             c, c, (c >= 32 && c <= 126) ? c : ' ');
    glColor3f(1.0f, 1.0f, 1.0f);
    draw_bitmap_string_at(30.0f, (float)h - 60.0f, GLUT_BITMAP_HELVETICA_18, banner);

    for (int i = 0; i < 4; ++i) {
        float cx0 = 20.0f + (float)i * (col_w + 12.0f);
        float cy0 = footer_h + 10.0f;
        float cx1 = cx0 + col_w;
        float cy1 = cy0 + panel_h;
        const StrokeFontDesc *desc = &g_fonts[i];

        /* Panel Box */
        draw_filled_rect(cx0, cy0, cx1, cy1, 0.12f, 0.14f, 0.18f, 0.9f);
        draw_rect_outline(cx0, cy0, cx1, cy1, desc->color[0], desc->color[1], desc->color[2], 0.5f, 1.5f);

        /* Column Header */
        draw_filled_rect(cx0, cy1 - 40.0f, cx1, cy1,
                         desc->color[0], desc->color[1], desc->color[2], 0.2f);
        glColor3fv(desc->color);
        draw_bitmap_string_at(cx0 + 10.0f, cy1 - 25.0f, GLUT_BITMAP_HELVETICA_12, desc->display_name);

        /* Character Metrics Readout */
        GLfloat advance = glutStrokeWidthf(desc->id, c);
        char metric_info[128];
        snprintf(metric_info, sizeof(metric_info), "Advance: %.2fu", (double)advance);
        glColor3f(0.8f, 0.85f, 0.95f);
        draw_bitmap_string_at(cx0 + 10.0f, cy1 - 58.0f, GLUT_BITMAP_8_BY_13, metric_info);

        /* Render Magnified Glyph */
        float glyph_scale = 1.35f * g_app.zoom;
        float center_x = cx0 + col_w * 0.5f - (advance * glyph_scale * 0.5f) + g_app.pan_x;
        float base_y   = cy0 + panel_h * 0.40f + g_app.pan_y;

        draw_stroke_metrics_guides(cx0 + 10.0f, col_w - 20.0f, glyph_scale, base_y);

        char single_char[2] = { (char)c, '\0' };
        glPushMatrix();
        glTranslatef(center_x, base_y, 0.0f);
        render_stroke_text_styled(desc->id, single_char, glyph_scale, g_app.line_width + 1.0f, desc->color);
        glPopMatrix();
    }
}

/* Mode 4: ASCII Character Set Matrix (ASCII 32..126) */
static void render_mode_ascii_table(void)
{
    int w = g_app.window_w;
    int h = g_app.window_h;

    const StrokeFontDesc *desc = &g_fonts[g_app.active_font_idx];

    float top_y  = (float)h - 90.0f;
    float bot_y  = 50.0f;
    float left_x = 30.0f;
    float right_x = (float)w - 30.0f;

    draw_filled_rect(left_x, bot_y, right_x, top_y, 0.10f, 0.12f, 0.16f, 0.9f);
    draw_rect_outline(left_x, bot_y, right_x, top_y, desc->color[0], desc->color[1], desc->color[2], 0.5f, 1.5f);

    /* Matrix Banner */
    char banner[128];
    snprintf(banner, sizeof(banner), "ASCII Printable Glyphs Matrix (32..126) - Font: [%d] %s",
             g_app.active_font_idx + 1, desc->id_name);
    glColor3fv(desc->color);
    draw_bitmap_string_at(left_x + 20.0f, top_y - 30.0f, GLUT_BITMAP_HELVETICA_18, banner);

    /* Grid configuration: 16 columns x 6 rows (95 characters: 32..126) */
    int cols = 16;
    int rows = 6;
    float grid_w = (right_x - left_x - 40.0f) / (float)cols;
    float grid_h = (top_y - bot_y - 60.0f) / (float)rows;
    float glyph_scale = 0.22f * g_app.zoom;

    for (int idx = 0; idx < 95; ++idx) {
        int code = 32 + idx;
        int col = idx % cols;
        int row = idx / cols;

        float cell_x = left_x + 20.0f + (float)col * grid_w;
        float cell_y = top_y - 60.0f - (float)(row + 1) * grid_h;

        /* Highlight selected inspector glyph */
        if (code == g_app.inspector_char) {
            draw_filled_rect(cell_x + 2.0f, cell_y + 2.0f, cell_x + grid_w - 2.0f, cell_y + grid_h - 2.0f,
                             desc->color[0], desc->color[1], desc->color[2], 0.25f);
            draw_rect_outline(cell_x + 2.0f, cell_y + 2.0f, cell_x + grid_w - 2.0f, cell_y + grid_h - 2.0f,
                              desc->color[0], desc->color[1], desc->color[2], 0.9f, 1.5f);
        }

        /* ASCII code label */
        char code_str[16];
        snprintf(code_str, sizeof(code_str), "%d", code);
        glColor3f(0.5f, 0.55f, 0.65f);
        draw_bitmap_string_at(cell_x + 4.0f, cell_y + grid_h - 14.0f, GLUT_BITMAP_8_BY_13, code_str);

        /* Render stroke character */
        GLfloat char_w = glutStrokeWidthf(desc->id, code);
        float draw_x = cell_x + (grid_w * 0.5f) - (char_w * glyph_scale * 0.5f);
        float draw_y = cell_y + 12.0f;

        char single_char[2] = { (char)code, '\0' };
        glPushMatrix();
        glTranslatef(draw_x, draw_y, 0.0f);
        render_stroke_text_styled(desc->id, single_char, glyph_scale, g_app.line_width, desc->color);
        glPopMatrix();
    }
}

/* Mode 5: 3D Vector Space Orbit Showcase */
static void render_mode_3d_orbit(void)
{
    int w = g_app.window_w;
    int h = g_app.window_h;

    /* Switch to 3D Perspective Projection */
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluPerspective(45.0, (double)w / (double)h, 1.0, 2000.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);

    /* Camera position */
    glTranslatef(0.0f, 0.0f, -450.0f * (1.0f / g_app.zoom));
    glRotatef(g_app.rot_x, 1.0f, 0.0f, 0.0f);
    glRotatef(g_app.rot_y, 0.0f, 1.0f, 0.0f);
    glRotatef(g_app.rot_z, 0.0f, 0.0f, 1.0f);

    /* 3D Coordinate Grid in vector space */
    if (g_app.show_guides) {
        glLineWidth(1.0f);
        glColor4f(0.3f, 0.35f, 0.45f, 0.4f);
        glBegin(GL_LINES);
        for (int i = -200; i <= 200; i += 40) {
            glVertex3f((float)i, -100.0f, -200.0f);
            glVertex3f((float)i, -100.0f, 200.0f);
            glVertex3f(-200.0f, -100.0f, (float)i);
            glVertex3f(200.0f, -100.0f, (float)i);
        }
        glEnd();
    }

    const char *text = g_presets[g_app.current_preset].text;
    float text_scale = 0.25f;

    /* Render all 4 fonts at staggered depths along the Z axis */
    float z_offsets[4] = { 90.0f, 30.0f, -30.0f, -90.0f };
    float y_offsets[4] = { 60.0f, 20.0f, -20.0f, -60.0f };

    for (int i = 0; i < 4; ++i) {
        const StrokeFontDesc *desc = &g_fonts[i];
        GLfloat len = compute_stroke_string_width(desc->id, text, g_app.char_spacing);

        glPushMatrix();
        glTranslatef(-len * text_scale * 0.5f, y_offsets[i], z_offsets[i]);
        render_stroke_text_styled(desc->id, text, text_scale, g_app.line_width + 1.0f, desc->color);
        glPopMatrix();
    }

    glDisable(GL_DEPTH_TEST);
    glPopMatrix();

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

/* -------------------------------------------------------------------------
 * Header & Footer HUD Overlay
 * ------------------------------------------------------------------------- */
static void render_hud_overlay(void)
{
    int w = g_app.window_w;
    int h = g_app.window_h;

    /* Top Navigation Bar: 56px tall */
    float top_bar_h = 56.0f;
    float top_y0 = (float)h - top_bar_h;
    float top_y1 = (float)h;

    draw_filled_rect(0.0f, top_y0, (float)w, top_y1, 0.08f, 0.09f, 0.12f, 0.95f);
    draw_rect_outline(0.0f, top_y0, (float)w, top_y1, 0.22f, 0.25f, 0.32f, 0.8f, 1.0f);

    /* --- TOP ROW (y = h - 22.0f) --- */
    float row1_y = (float)h - 22.0f;

    /* Title on Left */
    glColor3f(1.0f, 1.0f, 1.0f);
    const char *title = (w >= 750) ? "freeglut Stroke Font Comparison" : "Stroke Font Comparison";
    draw_bitmap_string_at(20.0f, row1_y, GLUT_BITMAP_HELVETICA_18, title);
    int title_w = get_bitmap_string_width(GLUT_BITMAP_HELVETICA_18, title);

    /* Mode and Preset Badges on Right */
    char mode_str[64];
    snprintf(mode_str, sizeof(mode_str), "Mode [%d/6]: %s", g_app.current_mode + 1, g_mode_names[g_app.current_mode]);
    int mode_w = get_bitmap_string_width(GLUT_BITMAP_HELVETICA_12, mode_str);

    char preset_str[64];
    snprintf(preset_str, sizeof(preset_str), "Preset (P): %s", g_presets[g_app.current_preset].title);
    int preset_w = get_bitmap_string_width(GLUT_BITMAP_HELVETICA_12, preset_str);

    float cur_right = (float)w - 20.0f;
    float avail_r1 = (float)w - (20.0f + (float)title_w + 30.0f);

    if (avail_r1 >= (float)(mode_w + preset_w + 25)) {
        /* Draw Preset badge */
        float p_x = cur_right - (float)preset_w;
        glColor3f(0.95f, 0.80f, 0.30f);
        draw_bitmap_string_at(p_x, row1_y, GLUT_BITMAP_HELVETICA_12, preset_str);
        cur_right = p_x - 18.0f;

        /* Draw Mode badge */
        float m_x = cur_right - (float)mode_w;
        glColor3f(0.30f, 0.85f, 1.00f);
        draw_bitmap_string_at(m_x, row1_y, GLUT_BITMAP_HELVETICA_12, mode_str);
    } else if (avail_r1 >= (float)mode_w + 10) {
        /* Draw just Mode badge */
        float m_x = cur_right - (float)mode_w;
        glColor3f(0.30f, 0.85f, 1.00f);
        draw_bitmap_string_at(m_x, row1_y, GLUT_BITMAP_HELVETICA_12, mode_str);
    }

    /* --- SECOND ROW (y = h - 44.0f) --- */
    float row2_y = (float)h - 44.0f;

    /* Status Indicators on Right */
    char status_str[192];
    if (w >= 1200) {
        snprintf(status_str, sizeof(status_str),
                 "AA:%s(A) | Dots:%s(J) | Halo:%s(O)[%.1f({/})] | Space:%+.0fu(s/S) | W:%.1f([/]) | Z:%.2fx",
                 g_app.antialiasing ? "ON" : "OFF",
                 g_app.join_dots ? "ON" : "OFF",
                 g_app.halo_enabled ? "ON" : "OFF",
                 (double)g_app.halo_size,
                 (double)g_app.char_spacing,
                 (double)g_app.line_width,
                 (double)g_app.zoom);
    } else if (w >= 880) {
        snprintf(status_str, sizeof(status_str),
                 "AA:%s | Dots:%s | Halo:%s(O) | Space:%+.0fu(s/S) | W:%.1f | Z:%.2fx",
                 g_app.antialiasing ? "ON" : "OFF",
                 g_app.join_dots ? "ON" : "OFF",
                 g_app.halo_enabled ? "ON" : "OFF",
                 (double)g_app.char_spacing,
                 (double)g_app.line_width,
                 (double)g_app.zoom);
    } else {
        snprintf(status_str, sizeof(status_str),
                 "AA:%s | Halo:%s | Space:%+.0fu | Z:%.1fx",
                 g_app.antialiasing ? "ON" : "OFF",
                 g_app.halo_enabled ? "ON" : "OFF",
                 (double)g_app.char_spacing,
                 (double)g_app.zoom);
    }
    int status_w = get_bitmap_string_width(GLUT_BITMAP_8_BY_13, status_str);
    float status_x = (float)w - 20.0f - (float)status_w;

    glColor3f(0.70f, 0.75f, 0.85f);
    draw_bitmap_string_at(status_x, row2_y, GLUT_BITMAP_8_BY_13, status_str);

    /* Sample Text Preview on Left (dynamically truncated so it never collides with status) */
    float text_max_w = status_x - 40.0f;
    if (text_max_w > 80.0f) {
        glColor3f(0.55f, 0.60f, 0.70f);
        draw_truncated_bitmap_string(20.0f, row2_y, text_max_w, GLUT_BITMAP_8_BY_13,
                                     "Sample: \"", g_presets[g_app.current_preset].text);
    }

    /* --- BOTTOM BAR (y = 0 to 30.0f) --- */
    if (g_app.show_help) {
        draw_filled_rect(0.0f, 0.0f, (float)w, 30.0f, 0.08f, 0.09f, 0.12f, 0.95f);
        draw_rect_outline(0.0f, 0.0f, (float)w, 30.0f, 0.22f, 0.25f, 0.32f, 0.8f, 1.0f);

        glColor3f(0.65f, 0.70f, 0.80f);
        const char *help = NULL;
        if (w >= 1220) {
            help = "Tab/M: Mode | 1..4: Font | P: Preset | +/-/Wheel: Zoom | Drag: Pan | O: Halo | {/}: HaloSz | s/S: Space | J: Dots | A: AA | G: Guides | Q: Quit";
        } else if (w >= 900) {
            help = "Tab: Mode | P: Preset | 1..4: Font | O: Halo | {/}: HaloSz | s/S: Space | J: Dots | A: AA | G: Guides | Q: Quit";
        } else {
            help = "Tab: Mode | P: Preset | O: Halo | s/S: Space | A: AA | G: Guides | Q: Quit";
        }
        draw_bitmap_string_at(20.0f, 9.0f, GLUT_BITMAP_8_BY_13, help);

        /* HUD toggle hint on far right */
        if (w >= 650) {
            glColor3f(0.45f, 0.50f, 0.60f);
            draw_bitmap_string_at((float)w - 90.0f, 9.0f, GLUT_BITMAP_8_BY_13, "[H] HUD");
        }
    }
}

/* -------------------------------------------------------------------------
 * GLUT Callbacks
 * ------------------------------------------------------------------------- */
static void display_cb(void)
{
    /* Clear color buffer */
    glClearColor(0.086f, 0.098f, 0.122f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Blending setup (always enabled for alpha-transparent UI highlights/panels) */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Antialiasing / Line smoothing toggle */
    if (g_app.antialiasing) {
        glEnable(GL_LINE_SMOOTH);
        glEnable(GL_POINT_SMOOTH);
        glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
        glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
    } else {
        glDisable(GL_LINE_SMOOTH);
        glDisable(GL_POINT_SMOOTH);
    }

    /* Set Join Dots option in freeglut */
    glutSetOption(GLUT_STROKE_FONT_DRAW_JOIN_DOTS, g_app.join_dots ? 1 : 0);
    glPointSize(g_app.line_width + 2.0f);

    /* Setup 2D Orthographic Projection for UI */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, (double)g_app.window_w, 0.0, (double)g_app.window_h, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Render Active View Mode */
    switch (g_app.current_mode) {
        case MODE_FOUR_ROWS:
            render_mode_four_rows();
            break;
        case MODE_GRID_2X2:
            render_mode_grid_2x2();
            break;
        case MODE_OVERLAY:
            render_mode_overlay();
            break;
        case MODE_INSPECTOR:
            render_mode_inspector();
            break;
        case MODE_ASCII_TABLE:
            render_mode_ascii_table();
            break;
        case MODE_3D_ORBIT:
            render_mode_3d_orbit();
            break;
        default:
            break;
    }

    /* Draw HUD Overlay */
    if (g_app.show_metrics_hud) {
        /* Ensure orthographic mode is active for HUD */
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, (double)g_app.window_w, 0.0, (double)g_app.window_h, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        render_hud_overlay();
    }

    glutSwapBuffers();
}

static void reshape_cb(int width, int height)
{
    if (height <= 0) height = 1;
    g_app.window_w = width;
    g_app.window_h = height;
    glViewport(0, 0, width, height);
    glutPostRedisplay();
}

static void timer_cb(int value)
{
    (void)value;
    /* 3D Orbit Animation */
    if (g_app.current_mode == MODE_3D_ORBIT && g_app.animating_3d) {
        g_app.rot_y += 0.4f;
        if (g_app.rot_y > 360.0f) g_app.rot_y -= 360.0f;
        glutPostRedisplay();
    }

    /* Blink timer for Overlay mode */
    if (g_app.current_mode == MODE_OVERLAY && g_app.blink_enabled) {
        static int elapsed = 0;
        elapsed += 16;
        if (elapsed >= g_app.blink_timer_ms) {
            g_app.blink_state = !g_app.blink_state;
            elapsed = 0;
            glutPostRedisplay();
        }
    }

    glutTimerFunc(16, timer_cb, 0);
}

static void keyboard_cb(unsigned char key, int x, int y)
{
    (void)x;
    (void)y;
    switch (key) {
        case 27: /* Esc */
        case 'q':
        case 'Q':
            exit(0);
            break;

        case 9:  /* Tab */
        case 'm':
        case 'M':
            g_app.current_mode = (g_app.current_mode + 1) % MODE_COUNT;
            glutPostRedisplay();
            break;

        case '1':
            g_app.active_font_idx = 0;
            g_app.overlay_pair = 0;
            glutPostRedisplay();
            break;
        case '2':
            g_app.active_font_idx = 1;
            g_app.overlay_pair = 1;
            glutPostRedisplay();
            break;
        case '3':
            g_app.active_font_idx = 2;
            glutPostRedisplay();
            break;
        case '4':
            g_app.active_font_idx = 3;
            glutPostRedisplay();
            break;

        case 'p':
        case 'P':
            g_app.current_preset = (g_app.current_preset + 1) % g_preset_count;
            glutPostRedisplay();
            break;

        case '+':
        case '=':
            g_app.zoom *= 1.15f;
            if (g_app.zoom > 15.0f) g_app.zoom = 15.0f;
            glutPostRedisplay();
            break;
        case '-':
        case '_':
            g_app.zoom /= 1.15f;
            if (g_app.zoom < 0.1f) g_app.zoom = 0.1f;
            glutPostRedisplay();
            break;

        case '[':
            if (g_app.line_width > 1.0f) g_app.line_width -= 0.5f;
            glutPostRedisplay();
            break;
        case ']':
            if (g_app.line_width < 10.0f) g_app.line_width += 0.5f;
            glutPostRedisplay();
            break;

        case '0':
        case 'o':
        case 'O':
            g_app.halo_enabled = !g_app.halo_enabled;
            glutPostRedisplay();
            break;

        case '{':
            if (g_app.halo_size > 1.0f) g_app.halo_size -= 0.5f;
            glutPostRedisplay();
            break;
        case '}':
            if (g_app.halo_size < 12.0f) g_app.halo_size += 0.5f;
            glutPostRedisplay();
            break;

        case 's':
        case '(':
            if (g_app.char_spacing > -20.0f) g_app.char_spacing -= 2.0f;
            glutPostRedisplay();
            break;
        case 'S':
        case ')':
            if (g_app.char_spacing < 80.0f) g_app.char_spacing += 2.0f;
            glutPostRedisplay();
            break;

        case 'a':
        case 'A':
            g_app.antialiasing = !g_app.antialiasing;
            glutPostRedisplay();
            break;

        case 'j':
        case 'J':
            g_app.join_dots = !g_app.join_dots;
            glutPostRedisplay();
            break;

        case 'g':
        case 'G':
            g_app.show_guides = !g_app.show_guides;
            glutPostRedisplay();
            break;

        case 'b':
        case 'B':
            g_app.blink_enabled = !g_app.blink_enabled;
            glutPostRedisplay();
            break;

        case ' ':
            if (g_app.current_mode == MODE_3D_ORBIT) {
                g_app.animating_3d = !g_app.animating_3d;
            } else if (g_app.current_mode == MODE_OVERLAY) {
                g_app.blink_state = !g_app.blink_state;
            }
            glutPostRedisplay();
            break;

        case 'r':
        case 'R':
            g_app.pan_x         = 0.0f;
            g_app.pan_y         = 0.0f;
            g_app.zoom          = 1.0f;
            g_app.rot_x         = 20.0f;
            g_app.rot_y         = -30.0f;
            g_app.rot_z         = 0.0f;
            g_app.char_spacing  = 0.0f;
            g_app.halo_size     = 3.0f;
            glutPostRedisplay();
            break;

        case 'h':
        case 'H':
            g_app.show_help = !g_app.show_help;
            glutPostRedisplay();
            break;

        case ',':
        case '<':
            if (g_app.inspector_char > 32) {
                g_app.inspector_char--;
                glutPostRedisplay();
            }
            break;
        case '.':
        case '>':
            if (g_app.inspector_char < 126) {
                g_app.inspector_char++;
                glutPostRedisplay();
            }
            break;

        default:
            break;
    }
}

static void special_cb(int key, int x, int y)
{
    (void)x;
    (void)y;
    switch (key) {
        case GLUT_KEY_LEFT:
            if (g_app.current_mode == MODE_INSPECTOR) {
                if (g_app.inspector_char > 32) g_app.inspector_char--;
            } else {
                g_app.pan_x -= 30.0f;
            }
            glutPostRedisplay();
            break;
        case GLUT_KEY_RIGHT:
            if (g_app.current_mode == MODE_INSPECTOR) {
                if (g_app.inspector_char < 126) g_app.inspector_char++;
            } else {
                g_app.pan_x += 30.0f;
            }
            glutPostRedisplay();
            break;
        case GLUT_KEY_UP:
            g_app.pan_y += 30.0f;
            glutPostRedisplay();
            break;
        case GLUT_KEY_DOWN:
            g_app.pan_y -= 30.0f;
            glutPostRedisplay();
            break;
        case GLUT_KEY_PAGE_UP:
            g_app.zoom *= 1.25f;
            glutPostRedisplay();
            break;
        case GLUT_KEY_PAGE_DOWN:
            g_app.zoom /= 1.25f;
            glutPostRedisplay();
            break;
        case GLUT_KEY_F1:
            g_app.show_help = !g_app.show_help;
            glutPostRedisplay();
            break;
        default:
            break;
    }
}

static void mouse_cb(int button, int state, int x, int y)
{
    g_app.mouse_down_x = x;
    g_app.mouse_down_y = y;
    g_app.mouse_button = button;

    if (state == GLUT_DOWN) {
        g_app.mouse_dragging = true;

        /* Wheel scroll events (buttons 3 and 4 in GLUT) */
        if (button == 3) {
            g_app.zoom *= 1.08f;
            if (g_app.zoom > 15.0f) g_app.zoom = 15.0f;
            glutPostRedisplay();
        } else if (button == 4) {
            g_app.zoom /= 1.08f;
            if (g_app.zoom < 0.1f) g_app.zoom = 0.1f;
            glutPostRedisplay();
        }
    } else if (state == GLUT_UP) {
        g_app.mouse_dragging = false;
    }
}

static void motion_cb(int x, int y)
{
    if (!g_app.mouse_dragging) return;

    int dx = x - g_app.mouse_down_x;
    int dy = y - g_app.mouse_down_y;
    g_app.mouse_down_x = x;
    g_app.mouse_down_y = y;

    if (g_app.current_mode == MODE_3D_ORBIT) {
        /* Rotate in 3D */
        g_app.rot_y += (float)dx * 0.5f;
        g_app.rot_x += (float)dy * 0.5f;
    } else {
        /* Pan 2D */
        g_app.pan_x += (float)dx;
        g_app.pan_y -= (float)dy; /* Invert Y delta for OpenGL bottom-left origin */
    }

    glutPostRedisplay();
}

static void mouse_wheel_cb(int wheel, int direction, int x, int y)
{
    (void)wheel;
    (void)x;
    (void)y;
    if (direction > 0) {
        g_app.zoom *= 1.12f;
        if (g_app.zoom > 15.0f) g_app.zoom = 15.0f;
    } else {
        g_app.zoom /= 1.12f;
        if (g_app.zoom < 0.1f) g_app.zoom = 0.1f;
    }
    glutPostRedisplay();
}

/* -------------------------------------------------------------------------
 * Main Entry Point
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    printf("=================================================================\n");
    printf(" freeglut 4-Stroke Font Comparison Tool\n");
    printf("=================================================================\n");
    printf(" Stroke Fonts in Comparison:\n");
    printf("   [1] GLUT_STROKE_ROMAN         - Proportional Standard Polyline\n");
    printf("   [2] GLUT_STROKE_ROMAN_HI      - Proportional Catmull-Rom High-Res\n");
    printf("   [3] GLUT_STROKE_MONO_ROMAN    - Monospaced Standard Polyline\n");
    printf("   [4] GLUT_STROKE_MONO_ROMAN_HI - Monospaced Catmull-Rom High-Res\n");
    printf("-----------------------------------------------------------------\n");
    printf(" Shortcuts:\n");
    printf("   Tab / M         : Cycle visualization modes (0..5)\n");
    printf("   1, 2, 3, 4      : Select active font\n");
    printf("   P               : Cycle sample presets / pangrams\n");
    printf("   + / - (or Wheel): Zoom in / out\n");
    printf("   Arrow keys/Drag : Pan view (or navigate glyph in Inspector)\n");
    printf("   [ / ]           : Adjust stroke line width (1.0 .. 10.0 px)\n");
    printf("   0 / O           : Toggle two-pass dark halo effect underneath strokes\n");
    printf("   { / }           : Adjust halo size / thickness (1.0 .. 12.0 px)\n");
    printf("   s / S (or ( / )): Adjust character spacing / tracking (-20 .. +80 units)\n");
    printf("   J               : Toggle vertex join dots (GLUT_STROKE_FONT_DRAW_JOIN_DOTS)\n");
    printf("   A               : Toggle line antialiasing (GL_LINE_SMOOTH)\n");
    printf("   G               : Toggle typography metrics guide lines\n");
    printf("   B               : Toggle blinking alternation (Overlay mode)\n");
    printf("   H               : Toggle on-screen HUD overlay\n");
    printf("   R               : Reset pan / zoom / rotation / halo / spacing\n");
    printf("   Esc / Q         : Quit\n");
    printf("=================================================================\n");
    fflush(stdout);

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH | GLUT_MULTISAMPLE);
    glutInitWindowSize(g_app.window_w, g_app.window_h);
    glutInitWindowPosition(80, 60);
    glutCreateWindow("freeglut Stroke Fonts Comparison");

    /* Register Callbacks */
    glutDisplayFunc(display_cb);
    glutReshapeFunc(reshape_cb);
    glutKeyboardFunc(keyboard_cb);
    glutSpecialFunc(special_cb);
    glutMouseFunc(mouse_cb);
    glutMotionFunc(motion_cb);
    glutMouseWheelFunc(mouse_wheel_cb);
    glutTimerFunc(16, timer_cb, 0);

    glutMainLoop();
    return 0;
}
