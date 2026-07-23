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

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

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

#if defined(__EMSCRIPTEN__)
/* Resolve browser-shell chrome into the canvas's framebuffer coordinate
 * space. The New button sits above the canvas, so y is normally negative;
 * the scripted cursor glides toward the canvas edge while activation is
 * forwarded to the real DOM control below. */
EM_JS(int, ps_web_shell_target, (const char *name_ptr, int *mx, int *my), {
    var name = UTF8ToString(name_ptr);
    if (name !== 'new' && name !== 'new_scene') return 0;
    var button = document.getElementById('newSceneButton');
    var canvas = Module['canvas'] || document.getElementById('canvas');
    if (!button || !canvas || button.disabled) return 0;
    var br = button.getBoundingClientRect();
    var cr = canvas.getBoundingClientRect();
    if (!cr.width || !cr.height) return 0;
    var x = Math.round((br.left + br.width * 0.5 - cr.left) *
                       canvas.width / cr.width);
    var y = Math.round((br.top + br.height * 0.5 - cr.top) *
                       canvas.height / cr.height);
    if (mx) HEAP32[mx >> 2] = x;
    if (my) HEAP32[my >> 2] = y;
    return 1;
});

/* Queue the real DOM click after the current Wasm frame returns. The shell's
 * existing listener owns the New-scene bridge and restores canvas focus. */
EM_JS(int, ps_web_shell_click, (const char *name_ptr), {
    var name = UTF8ToString(name_ptr);
    if (name !== 'new' && name !== 'new_scene') return 0;
    var button = document.getElementById('newSceneButton');
    if (!button || button.disabled) return 0;
    setTimeout(function() { button.click(); }, 0);
    return 1;
});
#endif

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
    PS_CHORD,
    PS_RING,
    PS_ECHO,
    PS_PAUSE
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
    int    special;                /* skey/chord: GLUT_KEY_* code, or    */
                                   /* -1 for a chord's printable path    */
    int    mods;                   /* chord: GLUT_ACTIVE_* override mask  */
    unsigned char key_byte;        /* chord: control byte (special < 0)  */
    float  size;                   /* echo: requested cap height (px);   */
                                   /* picks the nearest GLUT bitmap font */
    float  cps;                    /* key@N: chars/sec (0 = all at once) */
    int    source_line;            /* physical script line (1-based) for */
                                   /* the tour HUD; 0 when unknown       */
    char   text[PS_MAX_KEY_TEXT];  /* key: unescaped bytes to feed;      */
                                   /* echo: caption text to draw         */
} PsEvent;

static PsEvent g_events[PS_MAX_EVENTS];
static int     g_event_count = 0;
static int     g_active = 0;
static int     g_tour = 0;     /* runtime-started (auto-stop, cancelable) */

static int g_frame = 0;        /* rendered-frame clock, first frame = 0  */
static int g_next_event = 0;   /* events fire in file order              */
static int g_sequential = 0;   /* untimed grammar: wait for each step    */

/* An explicit pause blocks event dispatch in either grammar. */
static int g_pause_until = -1;

/* Untimed scripts wait for the current step's intrinsic work to finish
 * before starting the next one. Immediate verbs complete on their fire
 * frame; dispatch still advances by at most one step per frame. */
typedef enum {
    PS_WAIT_NONE,
    PS_WAIT_GLIDE,
    PS_WAIT_CLICK,
    PS_WAIT_TYPE,
    PS_WAIT_RING,
    PS_WAIT_PAUSE
} PsWait;
static PsWait g_step_wait = PS_WAIT_NONE;

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

/* Active paced typing (`key@N`): remaining text feeds through the keyboard
 * dispatch one character at a time on the frame clock, like the tutorial
 * comment reveal — instead of the whole payload landing on one frame. */
static int   g_type_active = 0;
static int   g_type_start = 0;               /* frame of the first char   */
static int   g_type_sent = 0;
static float g_type_cps = 0.0f;
static char  g_type_text[PS_MAX_KEY_TEXT] = "";

/* Overlay bookkeeping: last press (ripple) and active highlight ring. */
static int g_ripple_frame = -1;
static float g_ripple_x = 0.0f, g_ripple_y = 0.0f;
static int   g_ring_start = -1, g_ring_dur = 0;
static float g_ring_x = 0.0f, g_ring_y = 0.0f;

/* Active echo caption: bitmap text shown at a fixed screen spot. */
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

/* Parse a `+`-joined modifier token ("ctrl", "shift", "alt", combos like
 * "ctrl+shift", order-free, case-insensitive) into a GLUT_ACTIVE_* mask for
 * the `chord` verb. Returns -1 on any empty component (leading/trailing/doubled
 * `+`), a repeated modifier, or an unknown name — so a typo fails at load. */
static int ps_parse_mods(const char *tok) {
    int mask = 0;
    const char *p = tok;
    if (!*p) return -1;
    while (*p) {
        char name[16];
        size_t n = 0;
        while (*p && *p != '+' && n + 1 < sizeof(name))
            name[n++] = (char)tolower((unsigned char)*p++);
        name[n] = '\0';
        if (n == 0) return -1;                 /* empty component: "a++b", "+a" */
        int bit;
        if      (strcmp(name, "ctrl") == 0)  bit = GLUT_ACTIVE_CTRL;
        else if (strcmp(name, "shift") == 0) bit = GLUT_ACTIVE_SHIFT;
        else if (strcmp(name, "alt") == 0)   bit = GLUT_ACTIVE_ALT;
        else return -1;                        /* unknown modifier name        */
        if (mask & bit) return -1;             /* duplicate: "ctrl+ctrl"        */
        mask |= bit;
        if (*p == '+') {
            p++;
            if (*p == '\0') return -1;         /* trailing '+'                  */
        }
    }
    return mask;
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
        "menu:", "item:", "subenter:", "sub:", "pin:", "shell:",
        "scene:"
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
static int ps_parse_line(const char *line, PsEvent *ev, int *timed) {
    char verb[24];
    float secs = 0.0f;
    int consumed = 0;
    int verb_consumed = 0;
    char *time_end;

    /* Skip leading whitespace; blank and #-comment lines are no-ops. */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '\n' || *line == '#') return 0;

    /* A leading timestamp selects the legacy absolute-time grammar. With
     * no timestamp, the first token is the verb and the script runs as a
     * completion-driven sequence. Loaders reject mixing the two forms. */
    secs = strtof(line, &time_end);
    if (time_end != line) {
        if (secs < 0.0f || (*time_end != ' ' && *time_end != '\t'))
            return -1;
        if (sscanf(time_end, " %23s %n", verb, &verb_consumed) < 1)
            return -1;
        consumed = (int)(time_end - line) + verb_consumed;
        *timed = 1;
    } else {
        if (sscanf(line, "%23s %n", verb, &consumed) < 1)
            return -1;
        *timed = 0;
    }

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
         * blank args or a trailing `#` comment may fall through. */
        if (!ps_scan_point(args, ev)) {
            const char *rest = args;
            while (*rest == ' ' || *rest == '\t' ||
                   *rest == '\n' || *rest == '\r')
                rest++;
            if (*rest != '\0' && *rest != '#')
                return -1;
        }
        return 1;
    }
    if (strcmp(verb, "wheel") == 0) {
        ev->verb = PS_WHEEL;
        return (sscanf(args, "%d", &ev->wheel_dir) == 1 &&
                ev->wheel_dir != 0) ? 1 : -1;
    }
    if (strncmp(verb, "key", 3) == 0 &&
        (verb[3] == '\0' || verb[3] == '@')) {
        ev->verb = PS_KEY;
        /* `key@<cps>` paces the payload at chars/sec on the frame clock
         * (bare `key` feeds every byte on the fire frame, as before). */
        if (verb[3] == '@') {
            int n = 0;
            if (sscanf(verb + 4, "%f%n", &ev->cps, &n) != 1 ||
                verb[4 + n] != '\0' || ev->cps <= 0.0f)
                return -1;
        }
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
    if (strcmp(verb, "chord") == 0) {
        /* chord <mods> <key> — one modified key press. <key> is a special-key
         * name (skey vocabulary) or a single printable char; ctrl on a
         * printable folds to its control byte (like \cX). A shift/alt-only
         * printable has no keymap meaning and is rejected (type it via `key`). */
        char modtok[32], keytok[16];
        int nread = 0;
        ev->verb = PS_CHORD;
        ev->special = -1;               /* printable path unless a name matches */
        if (sscanf(args, "%31s %15s%n", modtok, keytok, &nread) != 2)
            return -1;
        /* Only whitespace or a trailing comment may follow the key token. */
        const char *rest = args + nread;
        while (*rest == ' ' || *rest == '\t' || *rest == '\n' || *rest == '\r')
            rest++;
        if (*rest != '\0' && *rest != '#')
            return -1;
        ev->mods = ps_parse_mods(modtok);
        if (ev->mods < 0)
            return -1;
        char lowered[16];
        size_t i = 0;
        for (; keytok[i] && i + 1 < sizeof(lowered); i++)
            lowered[i] = (char)tolower((unsigned char)keytok[i]);
        lowered[i] = '\0';
        int sp = ps_special_from_name(lowered);
        if (sp >= 0) {
            ev->special = sp;
            return 1;
        }
        /* Not a special-key name: require exactly one printable char + ctrl. */
        if (keytok[1] != '\0' || !(ev->mods & GLUT_ACTIVE_CTRL))
            return -1;
        ev->key_byte = (unsigned char)(toupper((unsigned char)keytok[0]) & 0x1F);
        return 1;
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
    if (strcmp(verb, "pause") == 0) {
        float dur = 0.0f;
        int nread = 0;
        const char *rest;
        ev->verb = PS_PAUSE;
        if (sscanf(args, "%f%n", &dur, &nread) != 1 || dur <= 0.0f)
            return -1;
        rest = args + nread;
        while (*rest == ' ' || *rest == '\t' ||
               *rest == '\n' || *rest == '\r')
            rest++;
        if (*rest != '\0' && *rest != '#') return -1;
        ev->dur_frames = (int)(dur * PS_FPS + 0.5f);
        if (ev->dur_frames < 1) ev->dur_frames = 1;
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
    int mode = -1;
    g_event_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        PsEvent ev;
        int timed = 0;
        int r = ps_parse_line(line, &ev, &timed);
        if (r < 0 || (r > 0 && mode >= 0 && timed != mode)) {
            fprintf(stderr, "gl-repl: GLR_POINTER_SCRIPT: %s:%d: bad line: %s",
                    path, lineno, line);
            exit(1);
        }
        if (r == 0) continue;
        if (mode < 0) mode = timed;
        if (g_event_count >= PS_MAX_EVENTS) {
            fprintf(stderr, "gl-repl: GLR_POINTER_SCRIPT: %s: too many events "
                    "(max %d)\n", path, PS_MAX_EVENTS);
            exit(1);
        }
        ev.source_line = lineno;
        g_events[g_event_count++] = ev;
    }
    fclose(fp);

    g_sequential = (mode == 0);
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
    g_pause_until = -1;
    g_step_wait = PS_WAIT_NONE;
    g_glide_active = 0;
    g_release_frame = -1;
    g_button_held = -1;
    g_ripple_frame = -1;
    g_ring_start = -1;
    g_echo_start = -1;
    g_echo_text[0] = '\0';
    /* A canceled tour drops unsent paced-typing text — the user took over;
     * more synthetic keystrokes would fight their input. */
    g_type_active = 0;
    g_type_text[0] = '\0';
}

int glr_pointer_script_start_lines(const char *const *lines, int count) {
    glr_pointer_script_stop();
    g_event_count = 0;
    int last_frame = 0;
    int mode = -1;
    for (int i = 0; i < count; i++) {
        PsEvent ev;
        int timed = 0;
        int r = ps_parse_line(lines[i], &ev, &timed);
        if (r < 0 ||
            (r > 0 && mode >= 0 && timed != mode) ||
            (r > 0 && timed && ev.frame < last_frame)) {
            fprintf(stderr, "gl-repl: tour script line %d: %s: %s\n", i + 1,
                    (r < 0 || (mode >= 0 && timed != mode))
                        ? "bad line" : "out of order",
                    lines[i]);
            g_event_count = 0;
            return 0;
        }
        if (r == 0) continue;
        if (mode < 0) mode = timed;
        if (g_event_count >= PS_MAX_EVENTS) {
            fprintf(stderr, "gl-repl: tour script: too many events (max %d)\n",
                    PS_MAX_EVENTS);
            g_event_count = 0;
            return 0;
        }
        if (timed) last_frame = ev.frame;
        ev.source_line = i + 1;
        g_events[g_event_count++] = ev;
    }
    if (g_event_count == 0) return 0;
    g_sequential = (mode == 0);
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

/* Feed paced-typing characters up to index `upto` (exclusive). */
static void ps_type_send(int upto) {
    while (g_type_text[g_type_sent] && g_type_sent < upto) {
        glr_ctrl_keyboard((unsigned char)g_type_text[g_type_sent++],
                          (int)(g_px + 0.5f), (int)(g_py + 0.5f));
    }
    if (!g_type_text[g_type_sent])
        g_type_active = 0;
}

/* Complete any in-flight paced typing immediately. Called before another
 * key/skey event dispatches so a too-tight schedule never interleaves or
 * drops payload text — the remainder snaps in, then the new event fires. */
static void ps_type_flush(void) {
    if (g_type_active)
        ps_type_send((int)sizeof(g_type_text));
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
    if (strncmp(target, "shell:", 6) == 0) {
#if defined(__EMSCRIPTEN__)
        return ps_web_shell_target(target + 6, mx, my);
#else
        return 0;
#endif
    }
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

static int ps_fire_shell_click(const PsEvent *ev) {
#if defined(__EMSCRIPTEN__)
    if (ps_web_shell_click(ev->target + 6))
        return 1;
#endif
    fprintf(stderr, "gl-repl: pointer script: cannot activate target '%s'\n",
            ev->target);
    if (!g_tour)
        exit(1);
    glr_pointer_script_stop();
    repl_set_status_error("Tour stopped (target unavailable)");
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
        /* Browser-shell controls live outside GLUT's canvas. Resolve them
         * for pointer motion as normal, then activate the real DOM button
         * instead of sending an impossible negative-y canvas click. */
        if (ev->verb == PS_CLICK &&
            strncmp(ev->target, "shell:", 6) == 0) {
            if (!ps_fire_shell_click(ev)) break;
            g_ripple_frame = g_frame;
            g_ripple_x = g_px;
            g_ripple_y = g_py;
            break;
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
        ps_type_flush();
        if (ev->cps > 0.0f) {
            /* Paced: stash the payload; the per-frame step below the event
             * loop sends the first character this same frame. */
            g_type_active = 1;
            g_type_start = g_frame;
            g_type_sent = 0;
            g_type_cps = ev->cps;
            snprintf(g_type_text, sizeof(g_type_text), "%s", ev->text);
        } else {
            for (const char *c = ev->text; *c; c++)
                glr_ctrl_keyboard((unsigned char)*c,
                                  (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        }
        break;
    case PS_SKEY:
        ps_type_flush();
        glr_ctrl_special(ev->special,
                         (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        break;
    case PS_CHORD:
        ps_type_flush();
        if (ev->special >= 0)
            glr_ctrl_special_with_modifiers(ev->special,
                                            (int)(g_px + 0.5f),
                                            (int)(g_py + 0.5f), ev->mods);
        else
            glr_ctrl_keyboard_with_modifiers(ev->key_byte,
                                             (int)(g_px + 0.5f),
                                             (int)(g_py + 0.5f), ev->mods);
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
    case PS_PAUSE:
        g_pause_until = g_frame + ev->dur_frames;
        break;
    }
}

/* Select the completion condition for one untimed step. A ring is itself an
 * action and completes with its overlay; an echo is a non-blocking caption
 * whose on-screen lifetime may overlap later actions. A click completes at
 * synthesized release, while its decorative ripple may overlap too. */
static void ps_wait_for_event(const PsEvent *ev) {
    switch (ev->verb) {
    case PS_GLIDE:      g_step_wait = PS_WAIT_GLIDE; break;
    case PS_CLICK:
    case PS_RIGHTCLICK: g_step_wait = PS_WAIT_CLICK; break;
    case PS_KEY:
        g_step_wait = ev->cps > 0.0f ? PS_WAIT_TYPE : PS_WAIT_NONE;
        break;
    case PS_RING:       g_step_wait = PS_WAIT_RING; break;
    case PS_ECHO:       g_step_wait = PS_WAIT_NONE; break;
    case PS_PAUSE:      g_step_wait = PS_WAIT_PAUSE; break;
    default:            g_step_wait = PS_WAIT_NONE; break;
    }
}

static int ps_step_complete(void) {
    switch (g_step_wait) {
    case PS_WAIT_NONE:  return 1;
    case PS_WAIT_GLIDE: return !g_glide_active;
    case PS_WAIT_CLICK: return g_release_frame < 0;
    case PS_WAIT_TYPE:  return !g_type_active;
    case PS_WAIT_RING:
        return g_ring_start < 0 || g_frame - g_ring_start >= g_ring_dur;
    case PS_WAIT_PAUSE: return g_pause_until < 0;
    }
    return 1;
}

/* Advance the script one virtual tour frame: fire due events, step glide +
 * paced typing, tick the frame clock, and run the auto-stop check. A rendered
 * frame maps to exactly one virtual frame for env-capture and legacy runtime
 * scripts; controlled tours will drive this zero-or-more times per rendered
 * frame under the virtual clock (speed control). */
static void ps_advance_one_virtual_frame(void) {
    /* Synthesized click release first, so a click's press and release
     * bracket any events scheduled between them. */
    if (g_release_frame >= 0 && g_frame >= g_release_frame) {
        g_release_frame = -1;
        ps_release(g_release_button);
    }

    if (g_pause_until >= 0 && g_frame >= g_pause_until)
        g_pause_until = -1;

    /* Untimed scripts are completion-driven and start at most one new step
     * per rendered frame. Timestamped scripts retain their absolute-time
     * behavior; an explicit pause temporarily gates dispatch. g_active can
     * drop inside ps_fire when a tour target fails to resolve. */
    if (g_sequential) {
        if (ps_step_complete()) {
            g_step_wait = PS_WAIT_NONE;
            if (g_next_event < g_event_count) {
                const PsEvent *ev = &g_events[g_next_event++];
                ps_fire(ev);
                if (g_active) ps_wait_for_event(ev);
            }
        }
    } else if (g_pause_until < 0) {
        while (g_active && g_next_event < g_event_count &&
               g_events[g_next_event].frame <= g_frame) {
            const PsEvent *ev = &g_events[g_next_event++];
            ps_fire(ev);
            if (g_pause_until >= 0) break;
        }
    }
    if (!g_active) return;

    /* Step paced typing: characters due by now = elapsed * cps, plus one
     * so the first character lands on the event's own fire frame. */
    if (g_type_active)
        ps_type_send((int)((float)(g_frame - g_type_start) *
                           g_type_cps / PS_FPS) + 1);

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
        g_release_frame < 0 && g_pause_until < 0 &&
        !g_glide_active && !g_type_active &&
        g_button_held < 0 &&
        (g_ring_start < 0 || g_frame - g_ring_start >= g_ring_dur) &&
        (g_echo_start < 0 || g_frame - g_echo_start >= g_echo_dur) &&
        (g_ripple_frame < 0 || g_frame - g_ripple_frame >= PS_RIPPLE_FRAMES))
        glr_pointer_script_stop();
}

void glr_pointer_script_frame(void) {
    if (!g_active) return;
    ps_advance_one_virtual_frame();
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

/* Echo captions render in a fixed-size GLUT bitmap font (bitmap glyphs
 * don't scale, so the script's requested cap height only selects the
 * nearest font from this ladder). */
static void *ps_echo_font(float cap_px) {
    static const struct { float px; void *font; } k_fonts[] = {
        { 10.0f, GLUT_BITMAP_HELVETICA_10 },
        { 12.0f, GLUT_BITMAP_HELVETICA_12 },
        { 18.0f, GLUT_BITMAP_HELVETICA_18 },
        { 24.0f, GLUT_BITMAP_TIMES_ROMAN_24 },
    };
    size_t best = 0;
    float best_d = -1.0f;
    for (size_t i = 0; i < sizeof(k_fonts) / sizeof(k_fonts[0]); i++) {
        float d = fabsf(cap_px - k_fonts[i].px);
        if (best_d < 0.0f || d < best_d) { best_d = d; best = i; }
    }
    return k_fonts[best].font;
}

/* Draw a NUL-terminated string with its baseline left edge at (x, y) in
 * window pixels (2D ortho is already active). */
static void ps_bitmap_text(float x, float y, void *font, const char *s) {
    glRasterPos2f(x, y);
    for (; *s; s++)
        glutBitmapCharacter(font, (unsigned char)*s);
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

    /* Echo caption: bitmap text (e.g. "Ctrl+K") pinned at a screen spot to
     * label how the next action was triggered. A 1px dark outline (eight
     * offset passes) is laid down first, then the bright glyphs on top, so
     * it reads over any scene; the whole caption eases in/out. */
    if (g_echo_start >= 0 && g_frame - g_echo_start < g_echo_dur &&
        g_echo_text[0]) {
        float age = (float)(g_frame - g_echo_start);
        float alpha = 1.0f;
        if (age < 9.0f) alpha *= age / 9.0f;          /* ease in ~0.15s   */
        float left = (float)g_echo_dur - age;
        if (left < 30.0f) alpha *= left / 30.0f;       /* ease out ~0.5s   */
        void *font = ps_echo_font(g_echo_size);
        float cy = (float)win_h - g_echo_y;
        static const float k_halo[][2] = {
            { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 },
            {  1,  0 }, { -1, 1 }, { 0,  1 }, {  1, 1 },
        };
        int i;
        glColor4f(0.06f, 0.07f, 0.09f, alpha * 0.85f);
        for (i = 0; i < (int)(sizeof(k_halo) / sizeof(k_halo[0])); i++)
            ps_bitmap_text(g_echo_x + k_halo[i][0], cy + k_halo[i][1],
                           font, g_echo_text);
        glColor4f(0.98f, 0.98f, 0.99f, alpha);
        ps_bitmap_text(g_echo_x, cy, font, g_echo_text);
    }

    ps_draw_cursor(g_px, (float)win_h - g_py);

    glLineWidth(1.0f);
    glDisable(GL_BLEND);
    gl2d_end();
}
