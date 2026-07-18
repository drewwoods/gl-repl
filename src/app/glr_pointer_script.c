/*
 * glr_pointer_script.c - scripted synthetic pointer/keyboard input for
 * headless/offline video capture (GLR_POINTER_SCRIPT). See the header for
 * the script grammar. Events dispatch through the public glr_ctrl_* GLUT
 * entry points so hover highlights, dropdown opens, flyouts, and clicks
 * behave exactly as live input; the overlay pass draws a cursor arrow,
 * a click ripple, and an optional highlight ring so the pointer is
 * visible in the recorded frames.
 */
#include "app/glr_pointer_script.h"
#include "app/glr_ctrl.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/core/gl_2d.h"   /* gl2d_begin/_end; pulls gl_includes.h */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PS_MAX_EVENTS   256
#define PS_MAX_KEY_TEXT 128
/* Rendered-frame clock rate: script seconds -> frames (GLR_FRAME_DT_SECS). */
#define PS_FPS          60.0f
/* Frames between a click's press and its synthesized release (~0.1s). */
#define PS_CLICK_RELEASE_FRAMES 6
/* Click ripple lifetime in frames. */
#define PS_RIPPLE_FRAMES 24

typedef enum {
    PS_MOVE,
    PS_GLIDE,
    PS_CLICK,
    PS_RIGHTCLICK,
    PS_DOWN,
    PS_UP,
    PS_WHEEL,
    PS_KEY,
    PS_SKEY,
    PS_RING,
    PS_ECHO
} PsVerb;

typedef struct {
    int    frame;                  /* fire frame (time * 60, rounded)   */
    PsVerb verb;
    int    has_xy;                 /* click/down/up took optional coords */
    int    x, y;                   /* target (move/glide/ring/click...)  */
    int    dur_frames;             /* glide/ring/echo duration           */
    int    wheel_dir;              /* wheel: +1 / -1                     */
    int    special;                /* skey: GLUT_KEY_* code              */
    float  size;                   /* echo: stroke cap height (px)       */
    char   text[PS_MAX_KEY_TEXT];  /* key: unescaped bytes to feed;      */
                                   /* echo: caption text to draw         */
} PsEvent;

static PsEvent g_events[PS_MAX_EVENTS];
static int     g_event_count = 0;
static int     g_active = 0;

static int g_frame = 0;        /* rendered-frame clock, first frame = 0  */
static int g_next_event = 0;   /* events fire in file order              */

static float g_px = 0.0f, g_py = 0.0f;   /* current pointer (window px) */

/* Active glide: ease g_px/g_py toward (to_x, to_y) with smoothstep. */
static int   g_glide_active = 0;
static int   g_glide_start = 0, g_glide_dur = 1;
static float g_glide_from_x = 0.0f, g_glide_from_y = 0.0f;
static float g_glide_to_x = 0.0f, g_glide_to_y = 0.0f;

/* Pending synthesized release for click/rightclick. */
static int g_release_frame = -1;
static int g_release_button = 0;

/* Overlay bookkeeping: last press (ripple) and active highlight ring. */
static int g_ripple_frame = -1;
static float g_ripple_x = 0.0f, g_ripple_y = 0.0f;
static int   g_ring_start = -1, g_ring_dur = 0;
static float g_ring_x = 0.0f, g_ring_y = 0.0f;

/* Active echo caption: stroke text shown at a fixed screen spot. */
static int   g_echo_start = -1, g_echo_dur = 0;
static float g_echo_x = 0.0f, g_echo_y = 0.0f, g_echo_size = 0.0f;
static char  g_echo_text[PS_MAX_KEY_TEXT] = "";

static float ps_smoothstep(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Unescape `\n` (Enter, the CR GLUT delivers), `\e` (Esc), `\t` (Tab),
 * `\cX` (the Ctrl+X control byte, e.g. `\cT` toggles animation) and `\\`
 * in a key-text payload; everything else copies verbatim. */
static void ps_unescape(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (; *src && o + 1 < cap; src++) {
        char c = *src;
        if (c == '\\' && src[1]) {
            src++;
            switch (*src) {
            case 'n':  c = '\r'; break;
            case 'e':  c = 27;   break;
            case 't':  c = '\t'; break;
            case '\\': c = '\\'; break;
            case 'c':
                if (src[1]) {
                    src++;
                    c = (char)(toupper((unsigned char)*src) & 0x1F);
                } else
                    c = *src;
                break;
            default:   c = *src; break;
            }
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
}

/* Special-key names for the `skey` verb -> GLUT_KEY_* codes. */
static int ps_special_from_name(const char *name) {
    static const struct { const char *name; int key; } k_map[] = {
        { "f1", GLUT_KEY_F1 },   { "f2", GLUT_KEY_F2 },
        { "f3", GLUT_KEY_F3 },   { "f4", GLUT_KEY_F4 },
        { "f5", GLUT_KEY_F5 },   { "f6", GLUT_KEY_F6 },
        { "f7", GLUT_KEY_F7 },   { "f8", GLUT_KEY_F8 },
        { "f9", GLUT_KEY_F9 },   { "f10", GLUT_KEY_F10 },
        { "f11", GLUT_KEY_F11 }, { "f12", GLUT_KEY_F12 },
        { "up", GLUT_KEY_UP },       { "down", GLUT_KEY_DOWN },
        { "left", GLUT_KEY_LEFT },   { "right", GLUT_KEY_RIGHT },
        { "home", GLUT_KEY_HOME },   { "end", GLUT_KEY_END },
        { "pageup", GLUT_KEY_PAGE_UP },
        { "pagedown", GLUT_KEY_PAGE_DOWN },
    };
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++)
        if (strcmp(name, k_map[i].name) == 0)
            return k_map[i].key;
    return -1;
}

/* Parse one script line into *ev. Returns 1 on an event, 0 on a blank or
 * comment line, -1 on a malformed line. */
static int ps_parse_line(const char *line, PsEvent *ev) {
    char verb[24];
    float secs;
    int consumed = 0;

    /* Skip leading whitespace; blank and #-comment lines are no-ops. */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '\n' || *line == '#') return 0;

    if (sscanf(line, "%f %23s %n", &secs, verb, &consumed) < 2 || secs < 0.0f)
        return -1;

    memset(ev, 0, sizeof(*ev));
    ev->frame = (int)(secs * PS_FPS + 0.5f);
    const char *args = line + consumed;

    if (strcmp(verb, "move") == 0) {
        ev->verb = PS_MOVE;
        return (sscanf(args, "%d %d", &ev->x, &ev->y) == 2) ? 1 : -1;
    }
    if (strcmp(verb, "glide") == 0) {
        float dur = 0.0f;
        ev->verb = PS_GLIDE;
        if (sscanf(args, "%d %d %f", &ev->x, &ev->y, &dur) != 3 || dur <= 0.0f)
            return -1;
        ev->dur_frames = (int)(dur * PS_FPS + 0.5f);
        if (ev->dur_frames < 1) ev->dur_frames = 1;
        return 1;
    }
    if (strcmp(verb, "click") == 0 || strcmp(verb, "rightclick") == 0 ||
        strcmp(verb, "down") == 0 || strcmp(verb, "up") == 0) {
        ev->verb = (verb[0] == 'c') ? PS_CLICK
                 : (verb[0] == 'r') ? PS_RIGHTCLICK
                 : (verb[0] == 'd') ? PS_DOWN : PS_UP;
        ev->has_xy = (sscanf(args, "%d %d", &ev->x, &ev->y) == 2);
        return 1;
    }
    if (strcmp(verb, "wheel") == 0) {
        ev->verb = PS_WHEEL;
        return (sscanf(args, "%d", &ev->wheel_dir) == 1 &&
                ev->wheel_dir != 0) ? 1 : -1;
    }
    if (strcmp(verb, "key") == 0) {
        ev->verb = PS_KEY;
        /* Payload is the rest of the line, newline stripped, escapes
         * resolved — so `key glColor3f(` keeps its punctuation. */
        char raw[PS_MAX_KEY_TEXT];
        size_t n = strlen(args);
        while (n > 0 && (args[n - 1] == '\n' || args[n - 1] == '\r'))
            n--;
        if (n == 0 || n >= sizeof(raw)) return -1;
        memcpy(raw, args, n);
        raw[n] = '\0';
        ps_unescape(raw, ev->text, sizeof(ev->text));
        return 1;
    }
    if (strcmp(verb, "skey") == 0) {
        char name[16];
        ev->verb = PS_SKEY;
        if (sscanf(args, "%15s", name) != 1) return -1;
        for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
        ev->special = ps_special_from_name(name);
        return (ev->special >= 0) ? 1 : -1;
    }
    if (strcmp(verb, "ring") == 0) {
        float dur = 0.0f;
        ev->verb = PS_RING;
        if (sscanf(args, "%d %d %f", &ev->x, &ev->y, &dur) != 3 || dur <= 0.0f)
            return -1;
        ev->dur_frames = (int)(dur * PS_FPS + 0.5f);
        return 1;
    }
    if (strcmp(verb, "echo") == 0) {
        float size = 0.0f, dur = 0.0f;
        int nread = 0;
        ev->verb = PS_ECHO;
        /* echo <x> <y> <size> <dur> <text...> — position + cap height +
         * on-screen lifetime, then the rest of the line is the caption. */
        if (sscanf(args, "%d %d %f %f %n",
                   &ev->x, &ev->y, &size, &dur, &nread) < 4 ||
            size <= 0.0f || dur <= 0.0f)
            return -1;
        ev->size = size;
        ev->dur_frames = (int)(dur * PS_FPS + 0.5f);
        if (ev->dur_frames < 1) ev->dur_frames = 1;
        /* Caption is the remainder verbatim (newline stripped) so a label
         * like `Ctrl+K` keeps its punctuation and spacing. */
        const char *txt = args + nread;
        size_t n = strlen(txt);
        while (n > 0 && (txt[n - 1] == '\n' || txt[n - 1] == '\r')) n--;
        if (n == 0 || n >= sizeof(ev->text)) return -1;
        memcpy(ev->text, txt, n);
        ev->text[n] = '\0';
        return 1;
    }
    return -1;
}

int glr_pointer_script_load_env(void) {
    const char *path = getenv("GLR_POINTER_SCRIPT");
    if (!path || !*path) return 0;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "gl-repl: GLR_POINTER_SCRIPT: cannot open %s\n", path);
        exit(1);
    }

    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        PsEvent ev;
        int r = ps_parse_line(line, &ev);
        if (r < 0) {
            fprintf(stderr, "gl-repl: GLR_POINTER_SCRIPT: %s:%d: bad line: %s",
                    path, lineno, line);
            exit(1);
        }
        if (r == 0) continue;
        if (g_event_count >= PS_MAX_EVENTS) {
            fprintf(stderr, "gl-repl: GLR_POINTER_SCRIPT: %s: too many events "
                    "(max %d)\n", path, PS_MAX_EVENTS);
            exit(1);
        }
        g_events[g_event_count++] = ev;
    }
    fclose(fp);

    g_active = 1;
    fprintf(stderr, "gl-repl: pointer script: %d event(s) from %s\n",
            g_event_count, path);
    return 1;
}

int glr_pointer_script_active(void) {
    return g_active;
}

static void ps_dispatch_move(float x, float y) {
    g_px = x;
    g_py = y;
    glr_ctrl_passive_motion((int)(x + 0.5f), (int)(y + 0.5f));
}

static void ps_press(int button) {
    glr_ctrl_mouse(button, GLUT_DOWN, (int)(g_px + 0.5f), (int)(g_py + 0.5f));
    g_ripple_frame = g_frame;
    g_ripple_x = g_px;
    g_ripple_y = g_py;
}

static void ps_release(int button) {
    glr_ctrl_mouse(button, GLUT_UP, (int)(g_px + 0.5f), (int)(g_py + 0.5f));
}

static void ps_fire(const PsEvent *ev) {
    switch (ev->verb) {
    case PS_MOVE:
        g_glide_active = 0;
        ps_dispatch_move((float)ev->x, (float)ev->y);
        break;
    case PS_GLIDE:
        g_glide_active = 1;
        g_glide_start = g_frame;
        g_glide_dur = ev->dur_frames;
        g_glide_from_x = g_px;
        g_glide_from_y = g_py;
        g_glide_to_x = (float)ev->x;
        g_glide_to_y = (float)ev->y;
        break;
    case PS_CLICK:
    case PS_RIGHTCLICK: {
        int button = (ev->verb == PS_CLICK) ? GLUT_LEFT_BUTTON
                                            : GLUT_RIGHT_BUTTON;
        if (ev->has_xy)
            ps_dispatch_move((float)ev->x, (float)ev->y);
        ps_press(button);
        g_release_frame = g_frame + PS_CLICK_RELEASE_FRAMES;
        g_release_button = button;
        break;
    }
    case PS_DOWN:
        if (ev->has_xy)
            ps_dispatch_move((float)ev->x, (float)ev->y);
        ps_press(GLUT_LEFT_BUTTON);
        break;
    case PS_UP:
        if (ev->has_xy) {
            /* Drag release: route through motion (button held), not
             * passive motion, so drag handlers see the final position. */
            g_px = (float)ev->x;
            g_py = (float)ev->y;
            glr_ctrl_motion(ev->x, ev->y);
        }
        ps_release(GLUT_LEFT_BUTTON);
        break;
    case PS_WHEEL:
        glr_ctrl_mousewheel(0, ev->wheel_dir,
                            (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        break;
    case PS_KEY:
        for (const char *c = ev->text; *c; c++)
            glr_ctrl_keyboard((unsigned char)*c,
                              (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        break;
    case PS_SKEY:
        glr_ctrl_special(ev->special,
                         (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        break;
    case PS_RING:
        g_ring_start = g_frame;
        g_ring_dur = ev->dur_frames;
        g_ring_x = (float)ev->x;
        g_ring_y = (float)ev->y;
        break;
    case PS_ECHO:
        g_echo_start = g_frame;
        g_echo_dur = ev->dur_frames;
        g_echo_x = (float)ev->x;
        g_echo_y = (float)ev->y;
        g_echo_size = ev->size;
        snprintf(g_echo_text, sizeof(g_echo_text), "%s", ev->text);
        break;
    }
}

void glr_pointer_script_frame(void) {
    if (!g_active) return;

    /* Synthesized click release first, so a click's press and release
     * bracket any events scheduled between them. */
    if (g_release_frame >= 0 && g_frame >= g_release_frame) {
        g_release_frame = -1;
        ps_release(g_release_button);
    }

    while (g_next_event < g_event_count &&
           g_events[g_next_event].frame <= g_frame)
        ps_fire(&g_events[g_next_event++]);

    /* Step the active glide AFTER event fire so the glide's own start
     * frame emits its first (near-source) sample this same frame. */
    if (g_glide_active) {
        float t = (float)(g_frame - g_glide_start) / (float)g_glide_dur;
        float s = ps_smoothstep(t);
        ps_dispatch_move(g_glide_from_x + (g_glide_to_x - g_glide_from_x) * s,
                         g_glide_from_y + (g_glide_to_y - g_glide_from_y) * s);
        if (t >= 1.0f)
            g_glide_active = 0;
    }

    g_frame++;
}

/* --- overlay rendering -------------------------------------------------- */

static void ps_circle(float cx, float cy, float radius, int filled) {
    int i;
    glBegin(filled ? GL_TRIANGLE_FAN : GL_LINE_LOOP);
    if (filled) glVertex2f(cx, cy);
    for (i = 0; i <= 40; i++) {
        float a = (float)i * (float)(2.0 * M_PI / 40.0);
        glVertex2f(cx + cosf(a) * radius, cy + sinf(a) * radius);
    }
    glEnd();
}

/* GLUT stroke-roman glyphs are ~119 units cap height; scaling by
 * size/PS_STROKE_CAP maps the requested cap height to window pixels. */
#define PS_STROKE_CAP 119.05f

static void ps_stroke_text(const char *s) {
    for (; *s; s++)
        glutStrokeCharacter(GLUT_STROKE_MONO_ROMAN, (int)(unsigned char)*s);
}

/* Classic pointer arrow at the current position. Local coords are y-down
 * (mouse space); px/py convert to the gl2d y-up ortho at the call site. */
static void ps_draw_cursor(float px, float py) {
    /* Arrow outline, y-down, ~19px tall: tip, left edge down, notch,
     * tail right side, back up the hypotenuse. */
    static const float pts[][2] = {
        { 0.0f,  0.0f }, { 0.0f, 16.5f }, { 3.9f, 12.9f }, { 6.6f, 19.0f },
        { 9.3f, 17.8f }, { 6.6f, 11.9f }, { 11.9f, 11.9f },
    };
    const int n = (int)(sizeof(pts) / sizeof(pts[0]));
    int i;

    /* Fill: the arrow polygon is star-shaped from a point inside the
     * head, so a triangle fan anchored there fills it correctly. */
    glColor4f(0.97f, 0.97f, 0.98f, 0.96f);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(px + 2.6f, py - 8.5f);
    for (i = 0; i < n; i++)
        glVertex2f(px + pts[i][0], py - pts[i][1]);
    glVertex2f(px + pts[0][0], py - pts[0][1]);
    glEnd();

    /* Dark outline so the cursor reads on light and dark panels alike. */
    glColor4f(0.08f, 0.09f, 0.11f, 0.9f);
    glLineWidth(1.5f);
    glBegin(GL_LINE_LOOP);
    for (i = 0; i < n; i++)
        glVertex2f(px + pts[i][0], py - pts[i][1]);
    glEnd();
}

void glr_pointer_script_render_overlay(int win_w, int win_h) {
    if (!g_active) return;
    (void)win_w;

    /* The overlay pass runs after glr_ctrl_display_frame, mirroring
     * splash_render: 2D ortho, blending on, y flipped from mouse space. */
    gl2d_begin(win_w, win_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Highlight ring: two concentric pulsing circles in warm amber. */
    if (g_ring_start >= 0 && g_frame - g_ring_start < g_ring_dur) {
        float age = (float)(g_frame - g_ring_start);
        float pulse = 0.5f + 0.5f * sinf(age * (float)(2.0 * M_PI) / 45.0f);
        float alpha = 0.35f + 0.4f * pulse;
        /* Ease out over the last half second. */
        float left = (float)g_ring_dur - age;
        if (left < 30.0f) alpha *= left / 30.0f;
        float cy = (float)win_h - g_ring_y;
        glLineWidth(2.5f);
        glColor4f(1.0f, 0.72f, 0.30f, alpha);
        ps_circle(g_ring_x, cy, 17.0f + 2.0f * pulse, 0);
        glColor4f(1.0f, 0.72f, 0.30f, alpha * 0.5f);
        ps_circle(g_ring_x, cy, 23.0f + 3.0f * pulse, 0);
    }

    /* Click ripple: one expanding, fading circle at the press point. */
    if (g_ripple_frame >= 0 && g_frame - g_ripple_frame < PS_RIPPLE_FRAMES) {
        float t = (float)(g_frame - g_ripple_frame) / (float)PS_RIPPLE_FRAMES;
        glLineWidth(2.0f);
        glColor4f(0.55f, 0.78f, 1.0f, 0.8f * (1.0f - t));
        ps_circle(g_ripple_x, (float)win_h - g_ripple_y,
                  4.0f + 18.0f * t, 0);
    }

    /* Echo caption: stroke text (e.g. "Ctrl+K") pinned at a screen spot to
     * label how the next action was triggered. Drawn twice — a dark halo,
     * then bright glyphs — so it reads over any scene, then eased in/out. */
    if (g_echo_start >= 0 && g_frame - g_echo_start < g_echo_dur &&
        g_echo_text[0]) {
        float age = (float)(g_frame - g_echo_start);
        float alpha = 1.0f;
        if (age < 9.0f) alpha *= age / 9.0f;          /* ease in ~0.15s   */
        float left = (float)g_echo_dur - age;
        if (left < 30.0f) alpha *= left / 30.0f;       /* ease out ~0.5s   */
        float s = g_echo_size / PS_STROKE_CAP;
        float cy = (float)win_h - g_echo_y;
        float halo = g_echo_size * 0.09f + 2.0f;
        float body = g_echo_size * 0.05f + 1.0f;
        int pass;
        for (pass = 0; pass < 2; pass++) {
            if (pass == 0) {
                glColor4f(0.06f, 0.07f, 0.09f, alpha * 0.85f);
                glLineWidth(halo);
            } else {
                glColor4f(0.98f, 0.98f, 0.99f, alpha);
                glLineWidth(body);
            }
            glPushMatrix();
            glTranslatef(g_echo_x, cy, 0.0f);
            glScalef(s, s, 1.0f);
            ps_stroke_text(g_echo_text);
            glPopMatrix();
        }
    }

    ps_draw_cursor(g_px, (float)win_h - g_py);

    glLineWidth(1.0f);
    glDisable(GL_BLEND);
    gl2d_end();
}
