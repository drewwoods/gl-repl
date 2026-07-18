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
#include "repl/host_effects.h"   /* repl_set_status_error (tour target loss) */
#include "ui/app/layout.h"       /* ui_layout_scene_rect (scene: targets) */
#include "ui/app/menu_bar.h"     /* ui_menu_bar_target_* (menu/item/sub/pin) */
#include "ui/app/state.h"        /* ui_state_viewport (GL->mouse y flip) */

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
#define PS_MAX_TARGET   64
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
    int    has_xy;                 /* event carries a point (literal or  */
                                   /* symbolic)                          */
    int    x, y;                   /* literal point (move/glide/ring...) */
    char   target[PS_MAX_TARGET];  /* symbolic point token ("" = use     */
                                   /* x/y); resolved at fire time        */
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
static int     g_tour = 0;     /* runtime-started (auto-stop, cancelable) */

static int g_frame = 0;        /* rendered-frame clock, first frame = 0  */
static int g_next_event = 0;   /* events fire in file order              */

static float g_px = 0.0f, g_py = 0.0f;   /* current pointer (window px) */

/* Scripted button currently held (a `down` awaiting its `up`, or a click's
 * synthesized-release window), so moves route through glr_ctrl_motion and
 * drags — camera orbit, slider drags — track the pointer like a real one. */
static int g_button_held = -1;           /* GLUT_*_BUTTON, or -1 = none  */

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

/* Scan a point from the front of `args`: either literal "<x> <y>" pixels
 * or one symbolic target token (see the header's grammar). Fills the
 * event's x/y or target[] and sets has_xy. Returns a pointer just past the
 * point (for trailing args like a glide's duration), or NULL when `args`
 * does not start with a point. Symbolic tokens are prefix-validated here
 * (and `scene:` fractions fully parsed) so a typo fails at load, not
 * mid-run; label existence can only be checked at fire time. */
static const char *ps_scan_point(const char *args, PsEvent *ev) {
    static const char *const k_prefixes[] = {
        "menu:", "item:", "subenter:", "sub:", "pin:", "scene:"
    };
    int n = 0;

    while (*args == ' ' || *args == '\t') args++;
    if (sscanf(args, "%d %d%n", &ev->x, &ev->y, &n) == 2) {
        ev->has_xy = 1;
        return args + n;
    }

    char word[PS_MAX_TARGET];
    if (sscanf(args, "%63s%n", word, &n) != 1)
        return NULL;
    for (size_t i = 0; i < sizeof(k_prefixes) / sizeof(k_prefixes[0]); i++) {
        size_t plen = strlen(k_prefixes[i]);
        if (strncmp(word, k_prefixes[i], plen) != 0 || word[plen] == '\0')
            continue;
        if (strcmp(k_prefixes[i], "scene:") == 0) {
            float fx, fy;
            if (sscanf(word + plen, "%f,%f", &fx, &fy) != 2)
                return NULL;
        }
        snprintf(ev->target, sizeof(ev->target), "%s", word);
        ev->has_xy = 1;
        return args + n;
    }
    return NULL;
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
        return ps_scan_point(args, ev) ? 1 : -1;
    }
    if (strcmp(verb, "glide") == 0) {
        float dur = 0.0f;
        ev->verb = PS_GLIDE;
        args = ps_scan_point(args, ev);
        if (!args || sscanf(args, "%f", &dur) != 1 || dur <= 0.0f)
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
        /* Point is optional (press/release at the current pointer), but a
         * present-yet-malformed one is an error, not a bare click: only
         * blank args may fall through. */
        if (!ps_scan_point(args, ev)) {
            const char *rest = args;
            while (*rest == ' ' || *rest == '\t' ||
                   *rest == '\n' || *rest == '\r')
                rest++;
            if (*rest != '\0')
                return -1;
        }
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
        args = ps_scan_point(args, ev);
        if (!args || sscanf(args, "%f", &dur) != 1 || dur <= 0.0f)
            return -1;
        ev->dur_frames = (int)(dur * PS_FPS + 0.5f);
        return 1;
    }
    if (strcmp(verb, "echo") == 0) {
        float size = 0.0f, dur = 0.0f;
        int nread = 0;
        ev->verb = PS_ECHO;
        /* echo <point> <size> <dur> <text...> — position + cap height +
         * on-screen lifetime, then the rest of the line is the caption. */
        args = ps_scan_point(args, ev);
        if (!args ||
            sscanf(args, "%f %f %n", &size, &dur, &nread) < 2 ||
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

int glr_pointer_script_tour_active(void) {
    return g_active && g_tour;
}

/* Reset the per-run state (clocks, glide, held button, overlays) without
 * touching the loaded event list. The env loader runs once at startup so it
 * never needed this; runtime tour starts reuse the statics mid-session. */
static void ps_reset_runtime(void) {
    g_frame = 0;
    g_next_event = 0;
    g_glide_active = 0;
    g_release_frame = -1;
    g_button_held = -1;
    g_ripple_frame = -1;
    g_ring_start = -1;
    g_echo_start = -1;
    g_echo_text[0] = '\0';
}

int glr_pointer_script_start_lines(const char *const *lines, int count) {
    glr_pointer_script_stop();
    g_event_count = 0;
    int last_frame = 0;
    for (int i = 0; i < count; i++) {
        PsEvent ev;
        int r = ps_parse_line(lines[i], &ev);
        if (r < 0 || (r > 0 && ev.frame < last_frame)) {
            fprintf(stderr, "gl-repl: tour script line %d: %s: %s\n", i + 1,
                    r < 0 ? "bad line" : "out of order", lines[i]);
            g_event_count = 0;
            return 0;
        }
        if (r == 0) continue;
        if (g_event_count >= PS_MAX_EVENTS) {
            fprintf(stderr, "gl-repl: tour script: too many events (max %d)\n",
                    PS_MAX_EVENTS);
            g_event_count = 0;
            return 0;
        }
        last_frame = ev.frame;
        g_events[g_event_count++] = ev;
    }
    if (g_event_count == 0) return 0;
    ps_reset_runtime();
    g_active = 1;
    g_tour = 1;
    return 1;
}

static void ps_dispatch_move(float x, float y) {
    g_px = x;
    g_py = y;
    /* While a scripted button is held this is a drag: route through the
     * motion callback (orbit/slider handlers), not passive motion. */
    if (g_button_held >= 0)
        glr_ctrl_motion((int)(x + 0.5f), (int)(y + 0.5f));
    else
        glr_ctrl_passive_motion((int)(x + 0.5f), (int)(y + 0.5f));
}

static void ps_press(int button) {
    glr_ctrl_mouse(button, GLUT_DOWN, (int)(g_px + 0.5f), (int)(g_py + 0.5f));
    g_button_held = button;
    g_ripple_frame = g_frame;
    g_ripple_x = g_px;
    g_ripple_y = g_py;
}

static void ps_release(int button) {
    glr_ctrl_mouse(button, GLUT_UP, (int)(g_px + 0.5f), (int)(g_py + 0.5f));
    g_button_held = -1;
}

void glr_pointer_script_stop(void) {
    if (!g_active) return;
    /* Never leave the app mid-drag: complete a pending synthesized click
     * release, then any explicitly held button. */
    if (g_release_frame >= 0) {
        g_release_frame = -1;
        ps_release(g_release_button);
    }
    if (g_button_held >= 0)
        ps_release(g_button_held);
    g_active = 0;
    g_tour = 0;
    ps_reset_runtime();
}

int glr_pointer_script_resolve_target(const char *target, int *mx, int *my) {
    if (!target || !*target)
        return 0;
    if (strncmp(target, "menu:", 5) == 0)
        return ui_menu_bar_target_menu(target + 5, mx, my);
    if (strncmp(target, "pin:", 4) == 0)
        return ui_menu_bar_target_pin(target + 4, mx, my);
    if (strncmp(target, "item:", 5) == 0)
        return ui_menu_bar_target_open_row(target + 5, mx, my);
    if (strncmp(target, "subenter:", 9) == 0)
        return ui_menu_bar_target_flyout_entry(target + 9, mx, my);
    if (strncmp(target, "sub:", 4) == 0) {
        char parent[PS_MAX_TARGET];
        const char *rest = target + 4;
        const char *colon = strchr(rest, ':');
        size_t len;
        if (!colon || colon == rest || colon[1] == '\0')
            return 0;
        len = (size_t)(colon - rest);
        if (len >= sizeof(parent))
            return 0;
        memcpy(parent, rest, len);
        parent[len] = '\0';
        return ui_menu_bar_target_flyout_row(parent, colon + 1, mx, my);
    }
    if (strncmp(target, "scene:", 6) == 0) {
        float fx = 0.0f, fy = 0.0f;
        int sx, sy, sw, sh;
        if (sscanf(target + 6, "%f,%f", &fx, &fy) != 2)
            return 0;
        ui_layout_scene_rect(&sx, &sy, &sw, &sh);
        if (sw <= 0 || sh <= 0)
            return 0;
        /* Scene rect is GL space (y up from the bottom); the fraction is
         * measured from the rect's top-left like every mouse coordinate. */
        if (mx) *mx = sx + (int)(fx * (float)sw + 0.5f);
        if (my) *my = ui_state_viewport().window_h - (sy + sh) +
                      (int)(fy * (float)sh + 0.5f);
        return 1;
    }
    return 0;
}

/* Resolve an event's point for firing (a literal point passes through).
 * On a failed symbolic resolve, a capture run exits nonzero — recording
 * the wrong interaction is worse than failing — while a tour stops with a
 * status message. Returns 0 when the event must be dropped. */
static int ps_fire_point(const PsEvent *ev, float *x, float *y) {
    int mx, my;
    if (!ev->target[0]) {
        *x = (float)ev->x;
        *y = (float)ev->y;
        return 1;
    }
    if (glr_pointer_script_resolve_target(ev->target, &mx, &my)) {
        *x = (float)mx;
        *y = (float)my;
        return 1;
    }
    fprintf(stderr, "gl-repl: pointer script: cannot resolve target '%s'\n",
            ev->target);
    if (!g_tour)
        exit(1);
    glr_pointer_script_stop();
    repl_set_status_error("Tour stopped (target not found)");
    return 0;
}

static void ps_fire(const PsEvent *ev) {
    float x = 0.0f, y = 0.0f;
    switch (ev->verb) {
    case PS_MOVE:
        if (!ps_fire_point(ev, &x, &y)) break;
        g_glide_active = 0;
        ps_dispatch_move(x, y);
        break;
    case PS_GLIDE:
        if (!ps_fire_point(ev, &x, &y)) break;
        g_glide_active = 1;
        g_glide_start = g_frame;
        g_glide_dur = ev->dur_frames;
        g_glide_from_x = g_px;
        g_glide_from_y = g_py;
        g_glide_to_x = x;
        g_glide_to_y = y;
        break;
    case PS_CLICK:
    case PS_RIGHTCLICK: {
        int button = (ev->verb == PS_CLICK) ? GLUT_LEFT_BUTTON
                                            : GLUT_RIGHT_BUTTON;
        if (ev->has_xy) {
            if (!ps_fire_point(ev, &x, &y)) break;
            ps_dispatch_move(x, y);
        }
        ps_press(button);
        g_release_frame = g_frame + PS_CLICK_RELEASE_FRAMES;
        g_release_button = button;
        break;
    }
    case PS_DOWN:
        if (ev->has_xy) {
            if (!ps_fire_point(ev, &x, &y)) break;
            ps_dispatch_move(x, y);
        }
        ps_press(GLUT_LEFT_BUTTON);
        break;
    case PS_UP:
        /* Drag release: the button is still held, so a coordinate move
         * routes through motion and drag handlers see the final position.
         * (A failed resolve in tour mode stops the script, which releases
         * the held button itself — no explicit release needed then.) */
        if (ev->has_xy) {
            if (!ps_fire_point(ev, &x, &y)) break;
            ps_dispatch_move(x, y);
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
        if (!ps_fire_point(ev, &x, &y)) break;
        g_ring_start = g_frame;
        g_ring_dur = ev->dur_frames;
        g_ring_x = x;
        g_ring_y = y;
        break;
    case PS_ECHO:
        if (!ps_fire_point(ev, &x, &y)) break;
        g_echo_start = g_frame;
        g_echo_dur = ev->dur_frames;
        g_echo_x = x;
        g_echo_y = y;
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

    /* g_active can drop mid-loop: a tour whose symbolic target fails to
     * resolve stops itself from inside ps_fire. */
    while (g_active && g_next_event < g_event_count &&
           g_events[g_next_event].frame <= g_frame)
        ps_fire(&g_events[g_next_event++]);
    if (!g_active) return;

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

    /* Tour auto-stop: once every event has fired and every in-flight effect
     * (glide, pending release, ring/echo/ripple) has run out, hand the app
     * back. Env-driven capture runs keep the overlay up until the recorder
     * exits instead — the clip should never end cursor-less. */
    if (g_tour && g_next_event >= g_event_count &&
        g_release_frame < 0 && !g_glide_active && g_button_held < 0 &&
        (g_ring_start < 0 || g_frame - g_ring_start >= g_ring_dur) &&
        (g_echo_start < 0 || g_frame - g_echo_start >= g_echo_dur) &&
        (g_ripple_frame < 0 || g_frame - g_ripple_frame >= PS_RIPPLE_FRAMES))
        glr_pointer_script_stop();
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
